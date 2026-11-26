#!/usr/bin/env python3
"""
Run the paper's experiment matrix in gem5 and collect results.

  benchmarks x configurations, one gem5 run each (gem5 is deterministic,
  so the paper's 20 repetitions are not needed).

Configurations (paper Figure 7):
  ws-numactl    work-stealing,  every page interleaved ("numactl --interleave=all",
                the paper's Figure 7 baseline)
  ws-standard   work-stealing,  first-touch            (machine default)
  ws-fine       work-stealing,  fine   (~ numactl --interleave, the paper's baseline)
  ws-coarse     work-stealing,  coarse
  la-fine       locality-aware, fine
  la-coarse     locality-aware, coarse
  la-standard   locality-aware, first-touch (extra; runtime has no page info)

Improvements (beyond the paper):
  la-fine-cost      locality-aware, fine,     cost-aware stealing   (Improvement 1)
  la-coarse-cost    locality-aware, coarse,   cost-aware stealing   (Improvement 1)
  la-adaptive       locality-aware, adaptive distribution, hop rule (Improvement 2)
  la-adaptive-cost  locality-aware, adaptive + cost-aware stealing  (both)
  "adaptive" reads profiles/<bench>.prof (made by improvements_experiments.sh)

Example:
  python3 scripts/run_experiments.py --gem5 ~/gem5/build/X86_MOESI_hammer/gem5.opt \
      --bench map --jobs 3
  python3 scripts/run_experiments.py --gem5 ... --bench all --configs ws-fine,la-coarse

Results go to results/<bench>/<config>/ and results/summary.csv
"""
import argparse
import csv
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CONFIGS = {
    "ws-numactl": ("ws", "standard"),   # + NUMA_INTERLEAVE_ALL=1
    "ws-standard": ("ws", "standard"),
    "ws-fine": ("ws", "fine"),
    "ws-coarse": ("ws", "coarse"),
    "la-fine": ("locality", "fine"),
    "la-coarse": ("locality", "coarse"),
    "la-standard": ("locality", "standard"),
    # Improvement 1 and 2
    "la-fine-cost": ("locality", "fine"),
    "la-coarse-cost": ("locality", "coarse"),
    "la-adaptive": ("locality", "adaptive"),
    "la-adaptive-cost": ("locality", "adaptive"),
}
IMPROVEMENT_CONFIGS = ["la-fine-cost", "la-coarse-cost", "la-adaptive", "la-adaptive-cost"]
PAPER_CONFIGS = ["ws-numactl", "ws-fine", "ws-coarse", "la-fine", "la-coarse"]

# Scaled-down inputs for the 48-core gem5 machine (see README for the reasoning)
BENCH_ARGS = {
    "map": "-n 48 -s 128",
    "vecmul": "-n 48 -s 64",
    "reduction": "-m 8 -d 6",
    "jacobi": "-n 1024 -b 128 -i 2",
    "matmul": "-n 384 -b 48",
    "sparselu": "-n 16 -b 32",
}
ALL_BENCHES = list(BENCH_ARGS)


def first_stats_block(path):
    """Stats of the first dump = the region of interest (rt_roi_end)."""
    stats = {}
    if not os.path.exists(path):
        return stats
    inside = False
    with open(path) as f:
        for line in f:
            if line.startswith("---------- Begin Simulation Statistics"):
                inside = True
                continue
            if line.startswith("---------- End Simulation Statistics"):
                break
            if inside:
                parts = line.split()
                if len(parts) >= 2:
                    stats[parts[0]] = parts[1]
    return stats


def mean_of(stats, suffix):
    for k, v in stats.items():
        if k.endswith(suffix):
            try:
                return float(v)
            except ValueError:
                pass
    return None


def run_one(gem5, bench, cfg, outroot, extra_args, gem5_args, bench_args):
    sched, dist = CONFIGS[cfg]
    outdir = os.path.join(outroot, bench, cfg)
    os.makedirs(outdir, exist_ok=True)
    envfile = os.path.join(outdir, "env.txt")
    with open(envfile, "w") as f:
        f.write(f"RT_SCHED={sched}\n")
        f.write(f"OMP_DATA_DISTRIBUTION={dist}\n")
        f.write(f"RT_TRACE={os.path.join(outdir, 'trace.csv')}\n")
        if cfg == "ws-numactl":
            f.write("NUMA_INTERLEAVE_ALL=1\n")
        if cfg.endswith("-cost"):
            f.write("RT_STEAL_DIST=cost\n")
        if dist == "adaptive":
            prof = os.path.join(ROOT, "profiles", f"{bench}.prof")
            if os.path.exists(prof):
                f.write(f"RT_PROFILE_IN={prof}\n")
        for kv in extra_args:
            f.write(kv + "\n")
    cmd = [
        gem5, "-d", outdir, "--stats-file=stats.txt",
        os.path.join(ROOT, "gem5-config", "numa_machine.py"),
        "--cmd", os.path.join(ROOT, "bin", bench),
        f"--options={bench_args}",
        "--env", envfile,
    ] + gem5_args
    t0 = time.time()
    with open(os.path.join(outdir, "run.log"), "w") as log:
        rc = subprocess.call(cmd, stdout=log, stderr=subprocess.STDOUT)
    wall = time.time() - t0
    text = open(os.path.join(outdir, "run.log")).read()
    stats = first_stats_block(os.path.join(outdir, "stats.txt"))

    def grab(pattern, cast=int):
        m = re.search(pattern, text)
        return cast(m.group(1)) if m else None

    row = {
        "bench": bench,
        "config": cfg,
        "sched": sched,
        "dist": dist,
        "ok": "result OK" in text,
        "rc": rc,
        "roi_cycles": grab(r"ROI cycles: (\d+)"),
        "sim_seconds": float(stats.get("simSeconds", "nan")),
        "remote_steals": grab(r"remote_steals=(\d+)"),
        "local_steals": grab(r"local_steals=(\d+)"),
        "dealt_remote": grab(r"dealt_remote=(\d+)"),
        "avg_miss_latency": mean_of(stats, "m_missLatencyHistSeqr::mean"),
        "avg_latency": mean_of(stats, "m_latencyHistSeqr::mean"),
        "pages": grab(r"NUMA pages placed:([^\n]*)", str),
        "cost_accepted": grab(r"cost steals: accepted=(\d+)"),
        "cost_rejected": grab(r"cost steals: accepted=\d+ rejected=(\d+)"),
        "adaptive": grab(r"adaptive: (fine=\d+ coarse=\d+)", str),
        "wall_seconds": round(wall, 1),
    }
    print(f"[{bench:9s} {cfg:12s}] rc={rc} ok={row['ok']} "
          f"roi_cycles={row['roi_cycles']} miss_lat={row['avg_miss_latency']} "
          f"wall={wall:.0f}s", flush=True)
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gem5", required=True, help="path to gem5.opt / gem5.fast")
    ap.add_argument("--bench", default="map", help="name, comma list, or all")
    ap.add_argument("--configs", default="paper",
                    help="paper, improvements, all, or a comma list, e.g. ws-fine,la-coarse")
    ap.add_argument("--jobs", type=int, default=2, help="gem5 runs in parallel")
    ap.add_argument("--out", default=os.path.join(ROOT, "results"))
    ap.add_argument("--bench-args", default=None,
                    help="override benchmark options (one benchmark only)")
    ap.add_argument("--env", action="append", default=[],
                    help="extra runtime env, e.g. --env RT_STEAL_DIST=os")
    ap.add_argument("--gem5-arg", action="append", default=[],
                    help="extra numa_machine.py option, e.g. --gem5-arg=--nodes=4")
    a = ap.parse_args()

    benches = ALL_BENCHES if a.bench == "all" else a.bench.split(",")
    cfgs = (PAPER_CONFIGS if a.configs == "paper"
            else IMPROVEMENT_CONFIGS if a.configs == "improvements"
            else list(CONFIGS) if a.configs == "all" else a.configs.split(","))
    jobs = []
    for b in benches:
        bargs = a.bench_args if a.bench_args else BENCH_ARGS[b]
        for c in cfgs:
            jobs.append((a.gem5, b, c, a.out, a.env, a.gem5_arg, bargs))
    print(f"{len(jobs)} gem5 runs, {a.jobs} at a time")
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        rows = list(ex.map(lambda j: run_one(*j), jobs))

    os.makedirs(a.out, exist_ok=True)
    summary = os.path.join(a.out, "summary.csv")
    old = []
    if os.path.exists(summary):
        with open(summary) as f:
            old = [r for r in csv.DictReader(f)
                   if (r["bench"], r["config"]) not in {(x["bench"], x["config"]) for x in rows}]
    with open(summary, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        for r in old + rows:
            w.writerow({k: r.get(k) for k in rows[0].keys()})
    print("summary:", summary)


if __name__ == "__main__":
    sys.exit(main())
