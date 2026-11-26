#!/usr/bin/env bash
# rerun_cost_fix.sh : reruns the cost-aware stealing configs after the cold-start fix.
#
# Fix 2 (this round): the tail task is checked under the victim's lock and only
# removed when the steal is accepted (before, a rejected check popped it and
# pushed it back, so the victim's own cores briefly saw an empty queue and went
# to sleep), and a node is only a victim when it has more queued tasks than
# idle cores. The per-task cost numbers are computed once, at task creation.
# Fix 1 (previous round): before the first task finishes, the cost rule now estimates task time from
# the task's own footprint (cache lines x local latency) instead of falling back
# to the paper's hop rule, and idle workers are woken once the first real
# measurement exists. This removes the Map la-fine-cost slowdown (1.54x).
#
#   1. keeps the first results as results_improvements_v1/
#   2. rebuilds, reruns the 3 *-cost configs (18 gem5 runs) into results_improvements/
#   3. rewrites IMPROVEMENTS_RESULTS.md and improvements.png
#
# Usage:  bash rerun_cost_fix.sh
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
G="${GEM5:-$HOME/gem5/build/X86_MOESI_hammer/gem5.opt}"
OUT="$HERE/results_improvements"
# keep every earlier round: v1 = first run, v2 = after the cold-start fix
if [ ! -d results_improvements_v1 ]; then cp -r "$OUT" results_improvements_v1
elif [ ! -d results_improvements_v2 ]; then cp -r "$OUT" results_improvements_v2; fi
exec > >(tee -a "$OUT/run_cost_fix.log") 2>&1
CORES=$(nproc)
MEM_GB=$(awk '/MemAvailable/ {print int($2/1024/1024)}' /proc/meminfo)
RUN_JOBS="${RUN_JOBS:-$(( MEM_GB / 3 < CORES ? MEM_GB / 3 : CORES ))}"
[ "$RUN_JOBS" -lt 1 ] && RUN_JOBS=1
echo "===== $(date '+%F %T')  rebuild"
bash build.sh || { echo "FAILED: build.sh"; exit 1; }
echo "===== $(date '+%F %T')  18 gem5 runs, $RUN_JOBS at a time"
python3 scripts/run_experiments.py --gem5 "$G" \
    --bench matmul,sparselu,jacobi,reduction,vecmul,map \
    --configs la-fine-cost,la-coarse-cost,la-adaptive-cost \
    --jobs "$RUN_JOBS" --out "$OUT" || echo "WARNING: some gem5 runs failed"
python3 scripts/summarize_improvements.py "$OUT"
echo "===== $(date '+%F %T')  ALL DONE. Earlier rounds kept in results_improvements_v1/ and _v2/. Tell Claude."
