"""Plot verified equal-budget comparisons; keep protocol notes in figures/README.md."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

METHODS = [("baseline", "Original: 16-bit, d=4", "#2264a5", "o"),
           ("optimized", "Optimized: 12-bit, d=5", "#d16b25", "s")]
COHORTS = [("overall", "Overall"), ("young_1_256", "Young: age 1-256"),
           ("stale_257_8192", "Stale: age 257-8192")]

def read(path):
    with path.open(newline="") as f:
        return list(csv.DictReader(f))

def main(out):
    manifest=json.loads((out/"manifest.json").read_text())
    verified=json.loads((out/"verification.json").read_text())
    if manifest["status"] not in ["MEASUREMENTS_COMPLETE", "COMPLETE"] or verified["status"] != "PASS":
        raise ValueError("Measurements must pass verification")
    if manifest["config"]["comparison"] != "equal_memory":
        raise ValueError("Use the frozen plotter for the previous equal-bucket experiment")
    has_timing=isinstance(verified["timing"],dict) and verified["timing"]["status"]=="PASS"
    accuracy=read(out/"summary.csv"); memory=read(out/"memory.csv")
    throughput=read(out/"throughput_summary.csv") if has_timing else []
    profile=manifest["profile"]; n=manifest["packets"]
    tag="FULL CAIDA" if profile=="full" else f"CAIDA fixed prefix: {n:,} packets (sample)"
    if profile=="smoke": tag += " - pipeline validation"
    figures=out/"figures"; figures.mkdir(exist_ok=True)
    plt.rcParams.update({"font.family":"DejaVu Sans", "font.size":10,
                        "axes.spines.top":False,"axes.spines.right":False,"svg.fonttype":"none"})
    ticks=[m/1000 for m in manifest["config"]["memory_bytes"]]
    caption="Equal allocated counter-array budgets; 1 KB = 1000 bytes.\nMean +/- sample SD across four query seeds; fixed hash seeds (not independent hash trials)."
    notes=["# Figure notes", "", tag, "", caption.replace("\n", " "), "",
           "- accuracy_all: overall, young (age 1-256) and stale (age 257-8192), using all evaluated queries.",
           "- overall_accuracy, overall_aae and overall_are: the same all-query overall statistics.",
           "- CSV development groups use query time < 65,536; outside_development uses query time >= 65,536. The stream state is never reset. These are auxiliary diagnostics, not an independent held-out test, and no outside_development figure is generated.",
           "- AAE is measured in packet time units; ARE is dimensionless. Both use the latest occurrence of the queried key as truth.",
           "- Linear vertical scales are used, including zero. All-zero panels use a 0-1 display range; this is an axis convention, not measured variation.",
           "- Each point averages four fixed query seeds; error bars are their sample standard deviation, not confidence intervals or independent hash trials.",
           "- Plots compare equal allocated counter-array budgets. Object overhead is reported separately in memory.csv; padding bytes do not participate in the algorithm.", ""]
    def style(ax,ylabel):
        ax.set(xlabel="Memory (KB)",ylabel=ylabel)
        ax.set_xticks(ticks);ax.set_ylim(bottom=0);ax.grid(alpha=.2)
        ax.ticklabel_format(axis="y",style="sci",scilimits=(-3,4))
    def panel(ax,cohort,metric,phase):
        for method,label,color,marker in METHODS:
            rows=sorted((r for r in accuracy if r["method"]==method and r["cohort"]==cohort and r["phase"]==phase and int(r["n"])),key=lambda r:int(r["memory"]))
            ax.errorbar([int(r["memory"])/1000 for r in rows],[float(r[metric]) for r in rows],
                        yerr=[float(r[metric+"_sd"]) for r in rows],label=label,color=color,marker=marker,
                        markerfacecolor="white",capsize=3,lw=1.5,linestyle="-" if method=="baseline" else "--")
        style(ax,"AAE" if metric=="aae" else "ARE")
        ax.set_title(dict(COHORTS)[cohort])
        rows=[r for r in accuracy if r["cohort"]==cohort and r["phase"]==phase and int(r["n"])]
        if rows and all(float(r[metric])==0 for r in rows):
            ax.set_ylim(0,1)
            notes.append(f"- {phase}/{cohort}/{metric.upper()}: both methods have zero error on all evaluated queries in this group.")
    def save(fig,name,note):
        h,l=fig.axes[0].get_legend_handles_labels()
        fig.legend(h,l,loc="upper center",bbox_to_anchor=(.5,1),ncol=2,frameon=False)
        fig.tight_layout(rect=[0,0,1,.93])
        notes.append(f"\n## {name}\n\n"+note.replace("\n", " "))
        for ext in ("png","svg"): fig.savefig(figures/f"{name}.{ext}",dpi=180)
        plt.close(fig)
    phases=["all"]
    for phase in phases:
        fig,axes=plt.subplots(3,2,figsize=(12,10))
        for i,(cohort,_) in enumerate(COHORTS):
            for j,metric in enumerate(("aae","are")): panel(axes[i,j],cohort,metric,phase)
        save(fig,"accuracy_"+phase,caption+("\nExcludes first 65,536 packets; state is not reset." if phase!="all" else ""))
    fig,axes=plt.subplots(1,2,figsize=(12,5))
    for ax,metric in zip(axes,("aae","are")): panel(ax,"overall",metric,"all")
    save(fig,"overall_accuracy",caption)
    for metric in ("aae","are"):
        fig,ax=plt.subplots(figsize=(8,5))
        panel(ax,"overall",metric,"all")
        save(fig,"overall_"+metric,caption)
    if has_timing:
        timing=manifest["config"]["throughput"]["smoke" if profile=="smoke" else "full"]
        fig,axes=plt.subplots(1,2,figsize=(12,5))
        for ax,phase in zip(axes,("update","query")):
            for method,label,color,marker in METHODS:
                rows=sorted((r for r in throughput if r["method"]==method and r["phase"]==phase),key=lambda r:int(r["memory"]))
                ax.errorbar([int(r["memory"])/1000 for r in rows],[float(r["mops"]) for r in rows],
                            yerr=[float(r["mops_sd"]) for r in rows],label=label,color=color,marker=marker,capsize=3)
            style(ax,"Throughput (Mops/s)");ax.set_title("Insert + maintenance" if phase=="update" else "Complete query")
        save(fig,"throughput",f"{timing['repeats']} measured runs + {timing['warmups']} warmup; serial, alternating order, one pinned CPU.\nQuery: {timing['snapshots']} snapshots x {timing['keys_per_snapshot']} keys x {manifest['query_batches']} common batches.")
    report=["# Equal-memory RingSketch accuracy experiment","",tag,"",caption.replace("\n"," "),"",
            "Accuracy only; no throughput was measured." if not has_timing else "Accuracy and throughput measured.", "",
            "| Budget B | Original buckets | Optimized buckets | Original per part | Optimized per part | Optimized packed B | Tail padding B |",
            "| ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for r in memory:
        report.append("| "+" | ".join(r[k] for k in ("memory","baseline_buckets","buckets","baseline_partition_buckets","optimized_partition_buckets","packed_counter_bytes","tail_padding_bytes"))+" |")
    report += ["","| KB | Method | Overall AAE | Overall ARE | Exact-hit rate |","| ---: | --- | ---: | ---: | ---: |"]
    for r in accuracy:
        if r["phase"]=="all" and r["cohort"]=="overall":
            report.append(f"| {int(r['memory'])/1000:g} | {r['method']} | {float(r['aae']):.9g} | {float(r['are']):.9g} | {float(r['hit_rate']):.9g} |")
    # Derive comparisons from verified values; never reuse a conclusion from another run.
    report += ["", "## Comparison from verified overall means", ""]
    for metric in ("aae","are","hit_rate"):
        categories={"lower":[],"equal":[],"higher":[]}
        for budget in manifest["config"]["memory_bytes"]:
            pair={r["method"]:float(r[metric]) for r in accuracy if r["phase"]=="all" and r["cohort"]=="overall" and int(r["memory"])==budget}
            a,b=pair["optimized"],pair["baseline"]
            categories["lower" if a<b else "higher" if a>b else "equal"].append(f"{budget/1000:g}")
        for relation,budgets in categories.items():
            if budgets: report.append(f"- Optimized {metric.upper()} is {relation} than baseline at {', '.join(budgets)} KB.")
    report += ["", "Lower AAE/ARE is better; higher exact-hit rate is better. These metrics describe different aspects of the error distribution."]
    report += ["", "- Original: 16-bit/d=4, hash seeds 100-103. Optimized: 12-bit/d=5, hash seeds 100-104.",
        "- Same query occurrences and truth for both methods. Four query seeds 131-134; window 8192; query before insert every 10 packets.",
        "- Optimized threshold 1984, slow period 4, fast-stage budget 2111. Original trajectory-candidate rules retained.",
        "- Survivor cohorts use each method's own counters. Young estimates are measured against truth; fine decoding is independently checked against a diagnostic same-layout 16-bit instance.",
        "- Object sizes are separate; allocator metadata, input, truth/oracle arrays and transient query allocations are excluded from counter-array budgets.",
        "- Both implementations use synchronous single-thread AVX2 maintenance.",
        ("- The full input stream was replayed. Queries remain sampled with four fixed query seeds; no independent hash repetitions were performed." if profile=="full" else "- Sample-prefix results do not establish full-stream performance or independent-hash uncertainty."),
        "", "![Accuracy](figures/accuracy_all.png)"]
    if has_timing:
        report.insert(-2, "- Query timing includes complete estimation; diagnostics and preparation are excluded.")
    (out/"REPORT.md").write_text("\n".join(report)+"\n",encoding="utf-8")
    (figures/"README.md").write_text("\n".join(notes)+"\n",encoding="utf-8")
    names=["summary.csv","memory.csv"]+(["throughput_summary.csv"] if has_timing else [])
    metadata=dict(matplotlib=matplotlib.__version__,plotter_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        summary_sha256={name:hashlib.sha256((out/name).read_bytes()).hexdigest() for name in names},figures=[p.name for p in sorted(figures.iterdir())])
    (out/"plot_manifest.json").write_text(json.dumps(metadata,indent=2))
    print(f"Plots and report: {out}")

if __name__=="__main__":
    parser=argparse.ArgumentParser();parser.add_argument("results",type=Path)
    main(parser.parse_args().results.resolve())
