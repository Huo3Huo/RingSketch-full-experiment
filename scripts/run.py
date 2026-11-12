"""Run paired accuracy and separate, serial timing measurements with fixed seeds."""
import argparse
import ctypes
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def source_hashes(folders):
    return {p.relative_to(ROOT).as_posix(): digest(p) for base in folders
            for p in sorted((ROOT / base).rglob("*"))
            if p.is_file() and "__pycache__" not in p.parts and p.suffix in {".h", ".cpp", ".py", ".ps1", ".json"}}


def now():
    # Wall clock is used ONLY for provenance/output naming, never random sampling.
    return dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).isoformat()


def pin_cpu():
    """Pin launcher before spawning children, so affinity applies before timing."""
    if os.name != "nt":
        raise RuntimeError("This runner currently requires Windows CPU affinity support")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.GetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
    kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    process = kernel.GetCurrentProcess()
    original, system = ctypes.c_size_t(), ctypes.c_size_t()
    if not kernel.GetProcessAffinityMask(process, ctypes.byref(original), ctypes.byref(system)):
        raise ctypes.WinError(ctypes.get_last_error())
    length = ctypes.c_ulong()
    kernel.GetSystemCpuSetInformation(None, 0, ctypes.byref(length), None, 0)
    buffer = ctypes.create_string_buffer(length.value)
    if not kernel.GetSystemCpuSetInformation(buffer, length, ctypes.byref(length), None, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    data, offset, topology = buffer.raw, 0, []
    while offset < length.value:
        size, kind = struct.unpack_from("II", data, offset)
        if size < 8 or offset + size > length.value:
            raise RuntimeError("Invalid CPU topology record")
        if kind == 0:
            topology.append(dict(group=struct.unpack_from("H", data, offset + 12)[0],
                                 logical=data[offset + 14], core=data[offset + 15],
                                 efficiency_class=data[offset + 18]))
        offset += size
    eligible = [r for r in topology if r["group"] == 0 and original.value & (1 << r["logical"])]
    if not eligible:
        raise RuntimeError("No eligible CPU in processor group zero")
    highest = max(r["efficiency_class"] for r in eligible)
    selected = min((r for r in eligible if r["efficiency_class"] == highest), key=lambda r: r["logical"])
    if not kernel.SetProcessAffinityMask(process, 1 << selected["logical"]):
        raise ctypes.WinError(ctypes.get_last_error())

    def restore():
        if not kernel.SetProcessAffinityMask(process, original.value):
            raise ctypes.WinError(ctypes.get_last_error())
    return dict(selected=selected, topology=topology, execution_threads=1,
                frequency="OS-controlled", priority="unchanged"), restore


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=["smoke", "full"], default="smoke")
    parser.add_argument("--stages", choices=["all", "accuracy"], default="all")
    parser.add_argument("--out", type=Path, help="New result directory, or existing directory with --resume")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--runtime-dir", default="C:/msys64/ucrt64/bin")
    args = parser.parse_args()
    if args.resume and args.out is None:
        parser.error("--resume requires --out")
    config = json.loads((ROOT / "configs/experiment.json").read_text())
    env = dict(os.environ, PATH=args.runtime_dir + os.pathsep + os.environ.get("PATH", ""))
    build = json.loads((ROOT / "build/build_manifest.json").read_text(encoding="utf-8-sig"))
    if build["sources"] != source_hashes(["src", "tests"]):
        raise ValueError("Sources changed since build; run scripts/build.ps1")
    executables = {name: ROOT / "build" / f"{name}.exe" for name in ["core", "experiment", "benchmark"]}
    for name, exe in executables.items():
        if digest(exe) != build["executables"][name]:
            raise ValueError("Executable changed since build")
    for name in ["experiment", "benchmark"]:
        protocol = json.loads(subprocess.check_output([executables[name], "--describe"], env=env, text=True))
        if protocol != {key: config[key] for key in protocol}:
            raise ValueError(f"Config/{name} protocol mismatch")
    if args.stages == "all":
        import matplotlib  # Fail before measurements if plotting dependency is absent.
    data = ROOT / config["dataset"]
    if data.stat().st_size != config["dataset_bytes"] or digest(data) != config["dataset_sha256"]:
        raise ValueError("Dataset identity mismatch")
    if data.stat().st_size % 21:
        raise ValueError("Partial input record")
    count = config["development_packets"] if args.profile == "smoke" else 0
    n = count or data.stat().st_size // 21
    stamp = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).strftime("%Y%m%d-%H%M%S")
    out = (args.out or ROOT / "results" / f"{args.profile}-{stamp}").resolve()
    out.mkdir(parents=True, exist_ok=args.resume)
    resources, restore_affinity = pin_cpu()
    identity = dict(profile=args.profile, stages=args.stages, config=config, packets=n,
                    build=build, sources=source_hashes(["src", "tests", "scripts", "configs"]),
                    references={p.name: digest(p) for p in sorted((ROOT / "reference").glob("*.csv"))},
                    resource_control=resources, platform=platform.platform(), processor=platform.processor(),
                    python=sys.version, runtime_dir=args.runtime_dir)
    mf = out / "manifest.json"
    if args.resume:
        manifest = json.loads(mf.read_text(encoding="utf-8"))
        if manifest["identity"] != identity:
            restore_affinity()
            raise ValueError("Resume identity mismatch; use a new directory")
    else:
        manifest = dict(status="RUNNING", profile=args.profile, config=config, packets=n,
                        paired_queries_per_point=len(config["seeds"]) * ((n-1)//config["query_every"]),
                        identity=identity, started=now(), completed_stages={})

    def save():
        temp = mf.with_suffix(".tmp")
        temp.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
        temp.replace(mf)

    def run_stage(key, command, folder, files):
        done = manifest["completed_stages"].get(key)
        if done:
            if any(not (folder / name).is_file() or digest(folder / name) != checksum
                   for name, checksum in done["outputs"].items()):
                raise ValueError(f"Completed stage files changed: {key}")
            return
        folder.mkdir(exist_ok=True)
        start = time.monotonic()
        with (folder / f"{key.replace('/', '_')}.log").open("w", encoding="utf-8") as log:
            process = subprocess.Popen(list(map(str, command)), env=env, cwd=ROOT,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
            try:
                for line in process.stdout:
                    log.write(line); log.flush(); print(f"[{key}] {line}", end="", flush=True)
                if process.wait():
                    raise RuntimeError(f"Stage failed: {key}")
            except BaseException:
                if process.poll() is None:
                    process.terminate(); process.wait()
                raise
        manifest["completed_stages"][key] = dict(elapsed_seconds=time.monotonic()-start,
                                                outputs={name: digest(folder / name) for name in files})
        save()

    try:
        manifest["status"] = "RUNNING"; manifest.pop("error", None); save()
        for memory in config["memory_bytes"]:
            point = out / str(memory)
            run_stage(f"accuracy/{memory}", [executables["experiment"], data, memory, point, count], point,
                      [f"layout_{memory}.json", f"metrics_{memory}.csv", f"queries_{memory}.csv", "final_checksums.json"])
        if args.stages == "all":
            def command(memory, phase, batches=1):
                return [executables["benchmark"], "--data", data, "--out", out / str(memory),
                        "--count", count, "--memory", memory, "--profile", args.profile,
                        "--phase", phase, "--batches", batches]
            for memory in config["memory_bytes"]:
                run_stage(f"calibrate/{memory}", command(memory, "calibrate"), out / str(memory),
                          ["calibration.csv", "batches.txt"])
            batches = max(int((out / str(m) / "batches.txt").read_text()) for m in config["memory_bytes"])
            manifest["query_batches"] = batches; save()
            for memory in config["memory_bytes"]:
                for phase in ["update", "query"]:
                    files = [f"{phase}_raw.csv"] + (["query_snapshots.csv"] if phase == "query" else [])
                    run_stage(f"{phase}/{memory}", command(memory, phase, batches), out / str(memory), files)
        if digest(data) != config["dataset_sha256"]:
            raise ValueError("Dataset changed during run")
        if source_hashes(["src", "tests", "scripts", "configs"]) != identity["sources"]:
            raise ValueError("Source/config changed during run; results cannot be finalized")
        manifest["status"] = "MEASUREMENTS_COMPLETE"; save()
        subprocess.run([sys.executable, str(ROOT / "scripts/verify.py"), str(out)], check=True, env=env)
        if args.stages == "all":
            subprocess.run([sys.executable, str(ROOT / "scripts/plot.py"), str(out)], check=True, env=env)
        manifest["status"] = "COMPLETE"
    except BaseException as error:
        manifest["status"] = "FAILED"; manifest["error"] = repr(error)
        raise
    finally:
        manifest["finished"] = now(); save(); restore_affinity()
        print(f"Results: {out}", flush=True)


if __name__ == "__main__":
    main()
