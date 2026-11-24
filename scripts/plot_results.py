#!/usr/bin/env python3
"""
Make the paper-style figures from results/.

  1. Figure 7 style: execution time per benchmark and configuration,
     normalized to "work-stealing numactl-interleave" like the paper.
  2. Figure 8 style: thread timelines (one row per thread, one bar per task)
     for one benchmark, one panel per configuration.

  python3 scripts/plot_results.py                 # bar chart of summary.csv
  python3 scripts/plot_results.py --timeline map  # timelines for map

Needs matplotlib (pip install matplotlib).
"""
import argparse
import csv
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ORDER = ["ws-numactl", "ws-standard", "ws-fine", "ws-coarse", "la-fine", "la-coarse", "la-standard"]
LABEL = {
    "ws-numactl": "Work-stealing numactl-interleave",
    "ws-standard": "Work-stealing first-touch",
    "ws-fine": "Work-stealing fine",
    "ws-coarse": "Work-stealing coarse",
    "la-fine": "Locality-aware fine",
    "la-coarse": "Locality-aware coarse",
    "la-standard": "Locality-aware first-touch",
}


def bar_chart(results, baseline, out):
    rows = list(csv.DictReader(open(os.path.join(results, "summary.csv"))))
    benches = sorted({r["bench"] for r in rows})
    cfgs = [c for c in ORDER if any(r["config"] == c for r in rows)]
    t = {(r["bench"], r["config"]): float(r["sim_seconds"]) for r in rows
         if r["sim_seconds"] not in ("", "nan")}
    fig, ax = plt.subplots(figsize=(max(7.0, 1.6 + 1.5 * len(benches)), 4.8))
    width = 0.8 / max(1, len(cfgs))
    for k, c in enumerate(cfgs):
        xs, ys = [], []
        for i, b in enumerate(benches):
            if (b, c) in t and (b, baseline) in t:
                xs.append(i + k * width)
                ys.append(t[(b, c)] / t[(b, baseline)])
        ax.bar(xs, ys, width, label=LABEL[c])
    ax.axhline(1.0, color="black", linewidth=0.8)
    ax.set_xticks([i + width * (len(cfgs) - 1) / 2 for i in range(len(benches))])
    ax.set_xticklabels([b.capitalize() for b in benches])
    ax.set_ylabel("Normalized execution time")
    ax.legend(fontsize=7, ncol=3, loc="upper center", bbox_to_anchor=(0.5, -0.1))
    ax.set_title(f"48-core, 8-node NUMA machine (gem5), baseline = {LABEL[baseline]}",
                 fontsize=9)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    print("wrote", out)


def timelines(results, bench, out):
    cfgs = [c for c in ORDER if os.path.exists(os.path.join(results, bench, c, "trace.csv"))]
    if not cfgs:
        print("no traces for", bench)
        return
    traces = {}
    t0, t1 = None, 0
    for c in cfgs:
        rows = list(csv.DictReader(open(os.path.join(results, bench, c, "trace.csv"))))
        traces[c] = rows
        s = min(int(r["start"]) for r in rows)
        e = max(int(r["end"]) for r in rows)
        traces[c + "_t0"] = s
        t1 = max(t1, e - s)
    colors = {"own": "tab:green", "local_steal": "tab:olive", "remote_steal": "tab:blue"}
    fig, axes = plt.subplots(len(cfgs), 1, figsize=(9, 1.6 * len(cfgs)), sharex=True)
    if len(cfgs) == 1:
        axes = [axes]
    for ax, c in zip(axes, cfgs):
        s0 = traces[c + "_t0"]
        for r in traces[c]:
            ax.barh(int(r["thread"]), int(r["end"]) - int(r["start"]),
                    left=int(r["start"]) - s0, height=0.8,
                    color=colors.get(r["kind"], "gray"))
        ax.set_ylabel(LABEL[c], rotation=0, ha="right", fontsize=8)
        ax.set_yticks([])
        ax.set_xlim(0, t1)
    axes[-1].set_xlabel("cycles (green = own/local queue, blue = taken from a remote node)")
    fig.suptitle(f"Thread timelines: {bench}")
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    print("wrote", out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=os.path.join(ROOT, "results"))
    ap.add_argument("--baseline", default="ws-numactl")
    ap.add_argument("--timeline", default=None, help="benchmark for thread timelines")
    a = ap.parse_args()
    if a.timeline:
        timelines(a.results, a.timeline,
                  os.path.join(a.results, f"timeline_{a.timeline}.png"))
    else:
        bar_chart(a.results, a.baseline, os.path.join(a.results, "fig7_style.png"))


if __name__ == "__main__":
    main()
