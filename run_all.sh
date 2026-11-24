#!/usr/bin/env bash
# run_all.sh : does the whole job in one go, on this machine.
#
#   1. builds gem5 + benchmarks (skipped if gem5.opt already exists)
#   2. quick test: latency probe + Map (stops here if Map is not "result OK")
#   3. full experiment matrix, paper configs (ws-numactl ws-fine ws-coarse la-fine la-coarse)
#   4. extra configs (ws-standard la-standard)
#   5. figures: fig7_style.png + timeline_<bench>.png for every benchmark
#
# Usage (from this project folder, in a terminal you keep open):
#     bash run_all.sh
# Optional:  RUN_JOBS=6 bash run_all.sh     (force the number of parallel gem5 runs)
#            SKIP_EXTRA=1 bash run_all.sh   (skip step 4)
#
# Everything is also written to results/run_all.log
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
GEM5_DIR="${GEM5_DIR:-$HOME/gem5}"
G="$GEM5_DIR/build/X86_MOESI_hammer/gem5.opt"
mkdir -p results
LOG="$HERE/results/run_all.log"
exec > >(tee -a "$LOG") 2>&1

CORES=$(nproc)
MEM_GB=$(awk '/MemAvailable/ {print int($2/1024/1024)}' /proc/meminfo)
# about 3 GB RAM per gem5 run, about 2 GB per compile job
RUN_JOBS="${RUN_JOBS:-$(( MEM_GB / 3 < CORES ? MEM_GB / 3 : CORES ))}"
BUILD_JOBS=$(( MEM_GB / 2 < CORES ? MEM_GB / 2 : CORES ))
[ "$RUN_JOBS" -lt 1 ] && RUN_JOBS=1
[ "$BUILD_JOBS" -lt 1 ] && BUILD_JOBS=1

stamp() { echo; echo "===== $(date '+%F %T')  $*"; }

stamp "machine: $CORES cores, ${MEM_GB} GB free RAM -> $BUILD_JOBS build jobs, $RUN_JOBS parallel gem5 runs"

# keep the Map results from the old weak VM, to check gem5 gives the same numbers
if [ -f results/summary.csv ] && [ ! -d results_weakvm ]; then
    mkdir -p results_weakvm
    cp results/summary.csv results/*.png results/latency_probe.log results_weakvm/ 2>/dev/null
    echo "old results copied to results_weakvm/"
fi

stamp "STEP 1/5  build gem5 + benchmarks"
if [ -x "$G" ]; then
    echo "gem5 already built at $G, only rebuilding benchmarks"
    bash build.sh || { echo "FAILED: build.sh"; exit 1; }
else
    JOBS=$BUILD_JOBS bash setup_gem5.sh || { echo "FAILED: setup_gem5.sh"; exit 1; }
fi

stamp "STEP 2/5  quick test (latency probe + Map)"
GEM5="$G" bash run_quick_test.sh | tee results/quick_test.log
if ! grep -q "result OK" results/quick_test.log; then
    echo "FAILED: quick test did not print 'result OK'. Stopping here. Send results/run_all.log to Claude."
    exit 1
fi

# longest benchmarks first, so the parallel slots stay busy until the end
ORDER="matmul,sparselu,jacobi,reduction,vecmul,map"

stamp "STEP 3/5  full matrix, paper configs"
python3 scripts/run_experiments.py --gem5 "$G" --bench "$ORDER" \
    --configs paper --jobs "$RUN_JOBS" || echo "WARNING: some paper-config runs failed"

if [ -z "${SKIP_EXTRA:-}" ]; then
    stamp "STEP 4/5  extra configs (first-touch)"
    python3 scripts/run_experiments.py --gem5 "$G" --bench "$ORDER" \
        --configs ws-standard,la-standard --jobs "$RUN_JOBS" || echo "WARNING: some extra runs failed"
fi

stamp "STEP 5/5  figures"
python3 scripts/plot_results.py
for b in map vecmul matmul jacobi reduction sparselu; do
    python3 scripts/plot_results.py --timeline "$b"
done

stamp "ALL DONE"
column -s, -t < results/summary.csv | cut -c1-150
echo
echo "Tell Claude it's finished."
