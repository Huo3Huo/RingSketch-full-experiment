"""Plot only verified measurements; keep smoke and full results visibly distinct."""
import argparse
import csv
import hashlib
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

METHODS = [("baseline", "Baseline (16-bit, AVX2 update)", "#2264a5", "o"),
           ("optimized", "Optimized (12-bit, AVX2 update)", "#d16b25", "s")]
COHORTS = [("overall", "Overall"), ("young_1_256", "Young: age 1-256"),
           ("stale_257_8192", "Stale: age 257-8192")]


def read(path):
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def main(out):
    manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    verified = json.loads((out / "verification.json").read_text())
    if manifest["status"] not in ["MEASUREMENTS_COMPLETE", "COMPLETE"] or verified["status"] != "PASS":
        raise ValueError("Results must pass verification before plotting")
    if not isinstance(verified["timing"], dict) or verified["timing"]["status"] != "PASS":
        raise ValueError("Throughput measurements have not passed verification")
    accuracy, throughput, memory = [read(out / name) for name in ["summary.csv", "throughput_summary.csv", "memory.csv"]]
    profile = manifest["profile"]
    tag = "FULL CAIDA" if profile == "full" else "SMOKE PREFIX - pipeline validation only"
    timing = manifest["config"]["throughput"][profile]
    figures = out / "figures"
    figures.mkdir(exist_ok=True)
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10,
                         "axes.spines.top": False, "axes.spines.right": False, "svg.fonttype": "none"})
    ticks = [int(r["buckets"])/1000 for r in memory]

    def axes_style(ax, ylabel):
        ax.set(xlabel="Total buckets (thousands; equal for both methods)", ylabel=ylabel)
        ax.set_xticks(ticks, [f"{v:g}" for v in ticks])
        ax.set_ylim(bottom=0)
        ax.grid(alpha=0.2)

    def accuracy_panel(ax, cohort, metric, phase="all"):
        for method, label, color, marker in METHODS:
            rows = sorted((r for r in accuracy if r["method"] == method and r["cohort"] == cohort
                           and r["phase"] == phase and int(r["n"]) > 0), key=lambda r: int(r["width"]))
            ax.errorbar([int(r["width"])/1000 for r in rows], [float(r[metric]) for r in rows],
                        yerr=[float(r[metric+"_sd"]) for r in rows], color=color, marker=marker,
                        linestyle="-" if method == "baseline" else "--", label=label, capsize=3,
                        markersize=6 if method == "baseline" else 4, markerfacecolor="white", lw=1.6)
        axes_style(ax, "AAE (packets)" if metric == "aae" else "ARE")
        ax.set_title(dict(COHORTS)[cohort])
        ax.ticklabel_format(axis="y", style="sci", scilimits=(-3, 4))
        if all(float(r[metric]) == 0 for r in accuracy if r["phase"] == phase and r["cohort"] == cohort and r[metric] != ""):
            ax.set_ylim(0, 1)
            ax.text(0.5, 0.45, "Both methods: zero error on sampled queries", transform=ax.transAxes,
                    ha="center", fontsize=9)

    def throughput_panel(ax, phase):
        for method, label, color, marker in METHODS:
            rows = sorted((r for r in throughput if r["method"] == method and r["phase"] == phase), key=lambda r: int(r["width"]))
            ax.errorbar([int(r["width"])/1000 for r in rows], [float(r["mops"]) for r in rows],
                        yerr=[float(r["mops_sd"]) for r in rows], color=color, marker=marker,
                        linestyle="-" if method == "baseline" else "--", label=label, capsize=3, lw=1.6)
        axes_style(ax, "Throughput (Mops/s)")
        ax.set_title("Update: insert + synchronous maintenance" if phase == "update" else "Query: complete estimate, frozen snapshots")

    def save(fig, name, caption):
        handles, labels = fig.axes[0].get_legend_handles_labels()
        fig.suptitle(tag, fontsize=13, y=0.995)
        fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 0.96), ncol=2, frameon=False)
        fig.text(0.5, 0.015, caption, ha="center", fontsize=9)
        fig.tight_layout(rect=[0, 0.09, 1, 0.89])
        fig.savefig(figures / f"{name}.png", dpi=180)
        fig.savefig(figures / f"{name}.svg")
        plt.close(fig)

    accuracy_caption = "Mean +/- sample SD across four query seeds; fixed hash layout.\nEqual buckets: optimized payload is about 25% smaller; overlapping young curves are intentional."
    speed_caption = (f"Mean +/- sample SD across {timing['repeats']} runs; one full warmup per configuration.\n"
                     f"Query: {timing['snapshots']} snapshots x {timing['keys_per_snapshot']} prepared keys x "
                     f"{manifest['query_batches']} common batches; one pinned execution thread.")
    for phase in (["all", "outside_development"] if profile == "full" else ["all"]):
        fig, axes = plt.subplots(3, 2, figsize=(13, 11))
        for i, (cohort, _) in enumerate(COHORTS):
            for j, metric in enumerate(["aae", "are"]):
                accuracy_panel(axes[i, j], cohort, metric, phase)
        save(fig, "accuracy_"+phase, accuracy_caption + ("\nExcludes the development prefix." if phase != "all" else ""))
    for cohort, _ in COHORTS:
        for metric in ["aae", "are"]:
            fig, ax = plt.subplots(figsize=(8, 5.8))
            accuracy_panel(ax, cohort, metric)
            save(fig, cohort+"_"+metric, accuracy_caption)
    fig, axes = plt.subplots(1, 2, figsize=(13, 5.8))
    for ax, phase in zip(axes, ["update", "query"]):
        throughput_panel(ax, phase)
    save(fig, "throughput", speed_caption)
    for phase in ["update", "query"]:
        fig, ax = plt.subplots(figsize=(8, 5.8))
        throughput_panel(ax, phase)
        save(fig, phase+"_throughput", speed_caption)
    fig, axes = plt.subplots(2, 2, figsize=(13, 9))
    accuracy_panel(axes[0, 0], "overall", "aae")
    accuracy_panel(axes[0, 1], "overall", "are")
    throughput_panel(axes[1, 0], "update")
    throughput_panel(axes[1, 1], "query")
    save(fig, "core_2x2", f"Mean +/- sample SD: accuracy 4 query seeds; throughput {timing['repeats']} runs.\nEqual buckets, different payload; synchronous single-thread implementations.")

    report = ["# RingSketch 配对比较结果", "", f"**{tag}**", "",
              f"数据量：{manifest['packets']:,} 条；精度查询合计 {manifest['paired_queries_per_point']*len(memory):,} 条配对记录。",
              "", "## 内存口径", "", "KB = 1000 B；两种方法相同 bucket 数，图横轴为总 bucket 数。",
              "对象占用另列；不包括 allocator 元数据、评测输入、真值、核验数组和临时查询分配。临时分配的执行时间保留。", "",
              "| 总 bucket | 优化版 payload (B) | 基线 payload (B) | 优化版对象 (B) | 基线对象 (B) |",
              "| ---: | ---: | ---: | ---: | ---: |"]
    for row in memory:
        report.append("| " + " | ".join(row[k] for k in ["buckets", "optimized_payload_bytes", "baseline_payload_bytes", "optimized_object_bytes", "baseline_object_bytes"]) + " |")
    report += ["", "## 总体指标", "", "曲线和下表使用重复间算术均值 ± 样本标准差；CSV 另保留 pooled 和中位数。",
               "精度的四次重复仅改变查询抽样，不改变 hash 布局；新鲜/陈旧子组样本数不同，均值与 pooled 值可能不同。", "",
               "| 优化版 payload (KB) | 方法 | 总体 AAE | 总体 ARE | 更新 Mops/s | 查询 Mops/s |",
               "| ---: | --- | ---: | ---: | ---: | ---: |"]
    for mem in manifest["config"]["memory_bytes"]:
        for method, label, _, _ in METHODS:
            a = next(r for r in accuracy if int(r["memory"]) == mem and r["method"] == method and r["phase"] == "all" and r["cohort"] == "overall")
            values = [f"{float(a[k]):.6g} ± {float(a[k+'_sd']):.3g}" for k in ["aae", "are"]]
            for phase in ["update", "query"]:
                r = next(r for r in throughput if int(r["memory"]) == mem and r["method"] == method and r["phase"] == phase)
                values.append(f"{float(r['mops']):.6g} ± {float(r['mops_sd']):.3g}")
            report.append(f"| {mem/1000:g} | {label} | " + " | ".join(values) + " |")
    report += ["", "## 实验边界", "",
               "- 新鲜为距最近出现 1–256 packet，陈旧为 257–8192 packet。新鲜样本逐条要求两种估计一致，不预设误差为零。",
               "- 更新速度包含插入和完整同步时钟维护；查询速度包含优化版轨迹恢复。准备状态、真值和文件写入不计入吞吐率。",
               "- 两种实现顺序测量，交替先后次序；各配置完整预热一次。保留全部正式轮次，不删除慢轮。",
               f"- 查询快照种子 {manifest['config']['snapshot_seed']}；精度种子 131–134；hash 种子 100–103，均不由日期或时钟生成。",
               "- 快照查询反复访问准备好的 key，属于缓存较热的批量查询，不代表在线混合流量的端到端延迟。",
               "- 两种方法均使用 AVX2 时钟维护。优化版批量处理压缩的 12-bit counter，阈值跨越保留精确标量处理；两种完整查询均未新增 SIMD。",
               "- 基线是 repro 的同步单线程 SIMD 实现，不是论文后台线程实现；这里的速度差异仍包含打包方式、双指针和当前实现成本。",
               "- AAE/ARE 的可接受阈值、论文主指标及最终结论仍需研究者确定。",
               f"- 最短正式查询快照计时 {verified['timing']['snapshot_min_seconds']*1000:.3f} ms；校准目标不是对每个快照时长的保证。",
               "", "![核心指标](figures/core_2x2.png)", "", "![精度分组](figures/accuracy_all.png)", "", "![吞吐率](figures/throughput.png)"]
    if profile == "smoke":
        report += ["", "本报告仅证明短前缀端到端流程已通过；不能作为 CAIDA 全量精度或性能结论。"]
    (out / "REPORT.md").write_text("\n".join(report)+"\n", encoding="utf-8")
    metadata = dict(matplotlib=matplotlib.__version__, plotter_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    summary_sha256={name: hashlib.sha256((out/name).read_bytes()).hexdigest()
                                    for name in ["summary.csv", "throughput_summary.csv", "memory.csv"]},
                    figures=[p.name for p in sorted(figures.iterdir())])
    (out / "plot_manifest.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(f"Plots and report: {out}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    main(parser.parse_args().results.resolve())
