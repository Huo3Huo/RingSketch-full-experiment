"""Recompute every accuracy group and validate timing identities before plotting."""
import csv
import hashlib
import json
import math
import statistics as st
import sys
from collections import defaultdict
from contextlib import ExitStack
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
METHODS = ["baseline", "optimized"]
PHASES = ["all", "development", "outside_development"]
COHORTS = ["overall", "young_1_256", "stale_257_8192", "age_1_16", "age_17_64",
           "age_65_255", "age_256", "age_257_1024", "age_1025_4096", "age_4097_8192",
           *[f"survivors_{i}" for i in range(6)]]
MASK = (1 << 64) - 1


def require(ok, message):
    if not ok:
        raise ValueError(message)


def read(path):
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def write(path, rows):
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)


def close(a, b):
    return math.isclose(float(a), float(b), rel_tol=1e-9, abs_tol=1e-12)


def describe(values, prefix):
    return {prefix: st.mean(values), prefix + "_sd": st.stdev(values) if len(values) > 1 else 0,
            prefix + "_median": st.median(values)}


def main(out):
    manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    require(manifest["status"] in ["MEASUREMENTS_COMPLETE", "COMPLETE"], "Measurements incomplete")
    cfg, n = manifest["config"], manifest["packets"]
    require(cfg.get("comparison") == "equal_memory", "Use frozen verifier for old equal-bucket results")
    seeds, memories = cfg["seeds"], cfg["memory_bytes"]
    expected_rows = len(seeds) * ((n-1)//cfg["query_every"])
    report = dict(status="PASS", points=[], timing="NOT_REQUESTED")
    summaries, sample_hashes, memory_rows = [], [], []
    # Completed-stage fingerprints also protect standalone verification/replotting.
    for stage, entry in manifest["completed_stages"].items():
        point = out / stage.split("/")[1]
        for name, wanted in entry["outputs"].items():
            h = hashlib.sha256()
            with (point / name).open("rb") as f:
                for chunk in iter(lambda: f.read(8*1024*1024), b""):
                    h.update(chunk)
            require(h.hexdigest() == wanted, "Changed measurement artifact: " + str(point / name))
    for memory in memories:
        point = out / str(memory)
        layout = json.loads((point / f"layout_{memory}.json").read_text())
        quantum = math.lcm(cfg["optimized_hashes"], cfg["slow_period"], 4)
        width = ((memory//8)*64//cfg["optimized_bits"])//quantum*quantum
        baseline_width = memory*8//cfg["baseline_bits"]
        require(layout["width"] == width and layout["payload"] == layout["allocated_payload"] == memory
                and layout["baseline_payload"] == memory
                and layout["baseline_width"] == baseline_width
                and layout["packed_counter_bytes"] == (width*12+63)//64*8
                and layout["optimized_hashes"] == cfg["optimized_hashes"]
                and layout["baseline_hashes"] == cfg["baseline_hashes"]
                and layout["optimized_partition_buckets"] == width//cfg["optimized_hashes"]
                and layout["baseline_partition_buckets"] == baseline_width//cfg["baseline_hashes"]
                and layout["fast_speed"] == 65535*width//cfg["window"]
                and layout["slow_speed"] == width//cfg["slow_period"]
                and layout["baseline_speed"] == 65535*baseline_width//cfg["window"], "Memory layout mismatch")
        require(layout["packets"] == n and layout["queries"] == expected_rows, "Layout count mismatch")
        memory_rows.append(dict(memory=memory, baseline_buckets=baseline_width,
                                optimized_partition_buckets=layout["optimized_partition_buckets"],
                                baseline_partition_buckets=layout["baseline_partition_buckets"],
                                packed_counter_bytes=layout["packed_counter_bytes"],
                                tail_padding_bytes=memory-layout["packed_counter_bytes"],
                                optimized_payload_bytes=layout["payload"], baseline_payload_bytes=layout["baseline_payload"],
                                buckets=width, optimized_object_bytes=layout["optimized_object_bytes"],
                                baseline_object_bytes=layout["baseline_object_bytes"],
                                optimized_known_persistent_bytes=layout["payload"]+layout["optimized_object_bytes"],
                                baseline_known_persistent_bytes=layout["baseline_payload"]+layout["baseline_object_bytes"]))
        stats = defaultdict(lambda: [0, 0, 0.0, 0])
        reference_path = ROOT / "reference" / "equal-memory-d5" / f"queries_{memory}.csv"
        prefix, mismatches, rows = 0, 0, 0
        sample_hash = hashlib.sha256()
        with ExitStack() as stack:
            stream = csv.DictReader(stack.enter_context((point / f"queries_{memory}.csv").open(newline="")))
            reference = (iter(csv.DictReader(stack.enter_context(reference_path.open(newline=""))))
                         if reference_path.exists() else None)
            for row in stream:
                t, seed, index, truth = [int(row[k]) for k in ["t", "seed", "index", "truth"]]
                require(seed == seeds[rows % len(seeds)] and t == (rows//len(seeds)+1)*cfg["query_every"],
                        "Query order mismatch")
                require(max(0, t-cfg["window"]) <= index < t and 1 <= truth <= cfg["window"], "Invalid query")
                sample_hash.update(f"{t},{seed},{index},{truth}\n".encode())
                if t < cfg["development_packets"] and reference is not None:
                    wanted = next(reference, None)
                    require(wanted is not None and all(row[k] == wanted[k] for k in
                            ["t", "seed", "index", "truth", "baseline", "optimized"]), "Frozen trace mismatch")
                    prefix += 1
                if truth <= cfg["young_max_age"]:
                    mismatches += row["baseline"] != row["optimized"]
                    require(row["fine"] == "1", "Young query not in fine mode")
                require(0 <= int(row["survivors"]) <= cfg["optimized_hashes"], "Invalid optimized survivor count")
                require(0 <= int(row["baseline_survivors"]) <= cfg["baseline_hashes"], "Invalid baseline survivor count")
                require(0 <= int(row["mask"]) < 1 << cfg["optimized_hashes"], "Invalid collision mask")
                if row["fine"] == "1":
                    require(row["optimized"] == row["fine_reference"], "Same-layout fine decoder mismatch")
                require(int(row["lower"]) <= truth, "Age lower bound mismatch")
                if int(row["survivors"]):
                    require(truth <= int(row["upper"]), "Age upper bound mismatch")
                require(int(row["lower"]) <= int(row["optimized"]) <= int(row["upper"]), "Estimate outside envelope")
                age = ("age_1_16" if truth <= 16 else "age_17_64" if truth <= 64 else
                       "age_65_255" if truth <= 255 else "age_256" if truth == 256 else
                       "age_257_1024" if truth <= 1024 else "age_1025_4096" if truth <= 4096 else "age_4097_8192")
                groups = ["overall", "young_1_256" if truth <= cfg["young_max_age"] else "stale_257_8192",
                          age]
                for phase in ["all", "development" if t < cfg["development_packets"] else "outside_development"]:
                    for method in METHODS:
                        error = abs(int(row[method])-truth)
                        for group in groups + ["survivors_" + row["survivors" if method == "optimized" else "baseline_survivors"]]:
                            m = stats[(phase, str(seed), method, group)]
                            m[0] += 1; m[1] += error; m[2] += error/truth; m[3] += error == 0
                rows += 1
            if reference is not None and n >= cfg["development_packets"]:
                require(next(reference, None) is None, "Incomplete frozen prefix")
        require(rows == expected_rows, "Query count mismatch")
        sample_hashes.append(sample_hash.hexdigest())
        metrics = read(point / f"metrics_{memory}.csv")
        keys = [(r["phase"], r["seed"], r["method"], r["cohort"]) for r in metrics]
        expected_keys = {(p, str(s), m, g) for p in PHASES for s in seeds for m in METHODS for g in COHORTS}
        require(len(keys) == len(expected_keys) and set(keys) == expected_keys, "Missing/duplicate metric groups")
        for row, key in zip(metrics, keys):
            m = stats[key]
            require(int(row["n"]) == m[0] and int(row["abs_sum"]) == m[1] and int(row["hits"]) == m[3],
                    "Metric counts/sums mismatch")
            require(close(row["relative_sum"], m[2]), "ARE sum mismatch")
            if m[0]:
                require(close(row["aae"], m[1]/m[0]) and close(row["are"], m[2]/m[0]), "Metric mean mismatch")
            else:
                require(row["aae"] == row["are"] == "", "Empty group must not report zero error")
        for phase in PHASES:
            for cohort in COHORTS:
                for method in METHODS:
                    values = [stats[(phase, str(seed), method, cohort)] for seed in seeds]
                    nonempty = [m for m in values if m[0]]
                    total = sum(m[0] for m in values)
                    summary = dict(memory=memory, width=width if method == "optimized" else baseline_width, phase=phase, cohort=cohort, method=method,
                                   n=total, nonempty_seeds=len(nonempty))
                    for metric, field in [("aae", 1), ("are", 2), ("hit_rate", 3)]:
                        if nonempty:
                            summary.update(describe([m[field]/m[0] for m in nonempty], metric))
                            summary[metric+"_pooled"] = sum(m[field] for m in values)/total
                        else:
                            summary.update({metric: "", metric+"_sd": "", metric+"_median": "", metric+"_pooled": ""})
                    summaries.append(summary)
        report["points"].append(dict(memory=memory, rows=rows, frozen_prefix_rows_matched=prefix,
                                     frozen_reference_available=reference_path.exists(), young_between_method_differences=mismatches, fine_same_layout_equivalence="PASS",
                                     metrics_recomputed=len(metrics)))
    require(len(set(sample_hashes)) == 1, "Accuracy samples differ across memory points")
    report["accuracy_sample_sha256"] = sample_hashes[0]
    write(out / "summary.csv", summaries)
    write(out / "memory.csv", memory_rows)
    if manifest["identity"]["stages"] == "all":
        timing = cfg["throughput"]["smoke" if manifest["profile"] == "smoke" else "full"]
        batches = manifest["query_batches"]
        expected_repeats = set(range(-timing["warmups"], timing["repeats"]))
        expected_keys = {(method, repeat) for method in METHODS for repeat in expected_repeats}
        throughput, snapshot_times = [], []
        common_indices = {}
        for memory in memories:
            point = out / str(memory)
            checksums = json.loads((point / "final_checksums.json").read_text())
            for phase in ["update", "query"]:
                raw = read(point / f"{phase}_raw.csv")
                keys = [(r["method"], int(r["repeat"])) for r in raw]
                require(len(keys) == len(expected_keys) and set(keys) == expected_keys, "Timing repeats missing/duplicate")
                for r in raw:
                    operations = n if phase == "update" else timing["snapshots"]*timing["keys_per_snapshot"]*batches
                    seconds = float(r["seconds"])
                    require(int(r["memory"]) == memory and int(r["n"]) == n and r["status"] == "OK",
                            "Timing identity mismatch")
                    require(int(r["operations"]) == operations and math.isfinite(seconds) and seconds > 0,
                            "Invalid timing count/duration")
                    require(close(r["mops"], operations/seconds/1e6), "Throughput mismatch")
                    require(int(r["final_state_checksum"]) == checksums[r["method"]],
                            "Timing implementation final state/query differs from accuracy implementation")
                for method in METHODS:
                    selected = [r for r in raw if r["method"] == method]
                    require(len({r["checksum"] for r in selected}) == 1, "Repeated query/update results changed")
                    measured = [r for r in selected if int(r["repeat"]) >= 0]
                    item = dict(memory=memory, width=next(r["buckets" if method == "optimized" else "baseline_buckets"] for r in memory_rows if r["memory"] == memory),
                                method=method, phase=phase, repeats=len(measured))
                    item.update(describe([float(r["mops"]) for r in measured], "mops"))
                    item["mops_pooled"] = sum(int(r["operations"]) for r in measured)/sum(float(r["seconds"]) for r in measured)/1e6
                    throughput.append(item)
                if phase == "query":
                    snapshots = read(point / "query_snapshots.csv")
                    snapshot_keys = [(r["method"], int(r["repeat"]), int(r["snapshot"])) for r in snapshots]
                    wanted = {(method, repeat, j) for method, repeat in expected_keys for j in range(timing["snapshots"])}
                    require(len(snapshot_keys) == len(wanted) and set(snapshot_keys) == wanted, "Invalid snapshot coverage")
                    by_round, result_checks = defaultdict(list), {}
                    for r in snapshots:
                        j = int(r["snapshot"])
                        require(int(r["memory"]) == memory and int(r["q"]) == (j+1)*n//timing["snapshots"]
                                and int(r["keys"]) == timing["keys_per_snapshot"] and int(r["batches"]) == batches
                                and int(r["operations"]) == batches*timing["keys_per_snapshot"], "Invalid snapshot protocol")
                        seconds = float(r["seconds"])
                        require(math.isfinite(seconds) and seconds > 0, "Invalid snapshot time")
                        idx = int(r["index_checksum"])
                        require(common_indices.setdefault(j, idx) == idx, "Snapshot samples differ between methods/configurations")
                        key = (r["method"], j)
                        require(result_checks.setdefault(key, r["checksum"]) == r["checksum"], "Snapshot query results changed")
                        by_round[(r["method"], int(r["repeat"]))].append(r)
                        if int(r["repeat"]) >= 0:
                            snapshot_times.append(seconds)
                    for r in raw:
                        members = by_round[(r["method"], int(r["repeat"]))]
                        require(close(sum(float(s["seconds"]) for s in members), r["seconds"]), "Snapshot time sum mismatch")
                        require(sum(int(s["checksum"]) for s in members) & MASK == int(r["checksum"]), "Snapshot checksum sum mismatch")
                        require(sum(int(s["operations"]) for s in members) == int(r["operations"]), "Snapshot operation sum mismatch")
        write(out / "throughput_summary.csv", throughput)
        report["timing"] = dict(status="PASS", summary_rows=len(throughput), measured_rounds=len(memories)*2*2*timing["repeats"],
                                warmup_rounds=len(memories)*2*2*timing["warmups"],
                                snapshot_min_seconds=min(snapshot_times), snapshot_median_seconds=st.median(snapshot_times),
                                query_batches=batches, snapshot_seed=cfg["snapshot_seed"])
    (out / "verification.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve())
