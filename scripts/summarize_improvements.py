#!/usr/bin/env python3
"""
Results table + figure for the two improvements.

  python3 scripts/summarize_improvements.py results_improvements

Reads <dir>/summary.csv (from run_experiments.py), writes
  <dir>/IMPROVEMENTS_RESULTS.md   tables in plain English
  <dir>/improvements.png          normalized execution time, per benchmark

Time is ROI cycles divided by ws-numactl (the paper's Figure 7 baseline):
below 1.00 is faster than the baseline, above 1.00 is slower.
"""
import csv
import os
import sys

BENCHES = ["map", "vecmul", "matmul", "jacobi", "reduction", "sparselu"]
CONFIGS = ["ws-numactl", "la-fine", "la-coarse", "la-fine-cost",
           "la-coarse-cost", "la-adaptive", "la-adaptive-cost"]


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "results_improvements"
    rows = {}
    with open(os.path.join(out, "summary.csv")) as f:
        for r in csv.DictReader(f):
            rows[(r["bench"], r["config"])] = r

    def t(b, c):
        r = rows.get((b, c))
        return num(r["roi_cycles"]) if r else None

    def norm(b, c):
        base, x = t(b, "ws-numactl"), t(b, c)
        return x / base if base and x else None

    def fmt(v, d=2, suf="x"):
        return f"{v:.{d}f}{suf}" if v is not None else "n/a"

    benches = [b for b in BENCHES if any((b, c) in rows for c in CONFIGS)]
    L = []
    L.append("# Results: the two improvements\n")
    L.append("Execution time of the region of interest, divided by ws-numactl "
             "(work-stealing + `numactl --interleave=all`, the paper's baseline). "
             "Below 1.00x = faster than the baseline. One gem5 run per cell "
             "(gem5 is deterministic).\n")

    L.append("## 1. Normalized execution time\n")
    L.append("| Benchmark | " + " | ".join(CONFIGS) + " |")
    L.append("|---" * (len(CONFIGS) + 1) + "|")
    for b in benches:
        L.append(f"| {b} | " + " | ".join(fmt(norm(b, c)) for c in CONFIGS) + " |")

    # all OK?
    bad = [f"{b}/{c}" for (b, c), r in rows.items()
           if c in CONFIGS and r.get("ok") != "True"]
    L.append("")
    L.append("Correctness: " + ("every run printed `result OK`."
                                if not bad else "NOT OK: " + ", ".join(bad)))

    L.append("\n## 2. Improvement 1: cost-aware stealing vs the paper's rule\n")
    L.append("| Benchmark | la-fine (hops) | la-fine-cost | change | "
             "la-coarse (hops) | la-coarse-cost | change | steals accepted / rejected (fine, coarse) |")
    L.append("|---|---|---|---|---|---|---|---|")
    for b in benches:
        cells = []
        for d in ("fine", "coarse"):
            h, c = t(b, f"la-{d}"), t(b, f"la-{d}-cost")
            ch = (c / h - 1) * 100 if h and c else None
            cells += [fmt(norm(b, f"la-{d}")), fmt(norm(b, f"la-{d}-cost")),
                      fmt(ch, 1, "%")]
        st = []
        for d in ("fine", "coarse"):
            r = rows.get((b, f"la-{d}-cost"), {})
            st.append(f"{r.get('cost_accepted') or '?'} / {r.get('cost_rejected') or '?'}")
        L.append(f"| {b} | " + " | ".join(cells) + " | " + ", ".join(st) + " |")

    L.append("\n## 3. Improvement 2: adaptive distribution vs choosing by hand\n")
    L.append("| Benchmark | adaptive chose | la-adaptive | best hand-picked (la-fine / la-coarse) | worse hand-picked | gap to best |")
    L.append("|---|---|---|---|---|---|")
    for b in benches:
        r = rows.get((b, "la-adaptive"), {})
        f_, c_ = norm(b, "la-fine"), norm(b, "la-coarse")
        a_ = norm(b, "la-adaptive")
        if f_ is None or c_ is None:
            continue
        best, worst = (("fine", f_), ("coarse", c_)) if f_ <= c_ else (("coarse", c_), ("fine", f_))
        gap = (a_ / best[1] - 1) * 100 if a_ else None
        L.append(f"| {b} | {r.get('adaptive') or '?'} | {fmt(a_)} | "
                 f"{best[0]} {fmt(best[1])} | {worst[0]} {fmt(worst[1])} | {fmt(gap, 1, '%')} |")

    L.append("\n## 4. Both together\n")
    L.append("| Benchmark | ws-numactl | best paper config (la-fine / la-coarse) | la-adaptive-cost | avg miss latency (cycles): best paper -> adaptive-cost |")
    L.append("|---|---|---|---|---|")
    for b in benches:
        f_, c_ = norm(b, "la-fine"), norm(b, "la-coarse")
        if f_ is None or c_ is None:
            continue
        bc = "la-fine" if f_ <= c_ else "la-coarse"
        ml = lambda c: num(rows.get((b, c), {}).get("avg_miss_latency"))
        L.append(f"| {b} | 1.00x | {bc} {fmt(norm(b, bc))} | {fmt(norm(b, 'la-adaptive-cost'))} | "
                 f"{fmt(ml(bc), 0, '')} -> {fmt(ml('la-adaptive-cost'), 0, '')} |")

    with open(os.path.join(out, "IMPROVEMENTS_RESULTS.md"), "w") as f:
        f.write("\n".join(L) + "\n")
    print("\n".join(L))

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed: skipped the figure")
        return
    show = ["la-fine", "la-coarse", "la-fine-cost", "la-coarse-cost",
            "la-adaptive", "la-adaptive-cost"]
    colors = ["#bdbdbd", "#737373", "#9ecae1", "#3182bd", "#a1d99b", "#31a354"]
    fig, ax = plt.subplots(figsize=(11, 4.5))
    w = 0.8 / len(show)
    for k, c in enumerate(show):
        ys = [norm(b, c) or 0 for b in benches]
        xs = [i + (k - (len(show) - 1) / 2) * w for i in range(len(benches))]
        ax.bar(xs, ys, w, label=c, color=colors[k])
    ax.axhline(1.0, color="black", lw=1)
    ax.text(len(benches) - 0.5, 1.01, "ws-numactl = 1.00", ha="right",
            va="bottom", fontsize=8)
    ax.set_xticks(range(len(benches)))
    ax.set_xticklabels(benches)
    ax.set_ylabel("time / ws-numactl (lower is better)")
    ax.set_title("Locality-aware scheduler: paper rules vs Improvement 1 (cost) "
                 "and Improvement 2 (adaptive)")
    ax.spines[["top", "right"]].set_visible(False)
    ax.legend(ncol=3, fontsize=8, frameon=False)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "improvements.png"), dpi=150)
    print("figure:", os.path.join(out, "improvements.png"))


if __name__ == "__main__":
    main()
