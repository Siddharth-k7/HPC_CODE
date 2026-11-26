#!/usr/bin/env bash
# improvements_experiments.sh : builds and measures the two proposed improvements.
#
#   Improvement 1  NUMA cost-aware work stealing   (RT_STEAL_DIST=cost)
#   Improvement 2  adaptive data distribution      (OMP_DATA_DISTRIBUTION=adaptive)
#
#   1. rebuild the benchmarks with the new runtime
#   2. profile run of every benchmark (native, a few seconds each): records, per
#      allocation, how many tasks use it and how much of it each task uses.
#      Written to profiles/<bench>.prof, read by the adaptive policy.
#   3. gem5: all 6 benchmarks x 7 configs (42 runs) into results_improvements/
#        ws-numactl la-fine la-coarse                 (baselines, same build)
#        la-fine-cost la-coarse-cost                  (Improvement 1)
#        la-adaptive                                  (Improvement 2)
#        la-adaptive-cost                             (both)
#   4. results table + figure: results_improvements/IMPROVEMENTS_RESULTS.md
#
# Usage (from the project folder, in a terminal you keep open):
#     bash improvements_experiments.sh
# Optional:  RUN_JOBS=12 bash improvements_experiments.sh
#            PAPERSCALE=1 bash improvements_experiments.sh   (also Jacobi 4096^2)
# Everything also goes to results_improvements/run.log
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
G="${GEM5:-$HOME/gem5/build/X86_MOESI_hammer/gem5.opt}"
OUT="$HERE/results_improvements"
mkdir -p "$OUT" profiles
exec > >(tee -a "$OUT/run.log") 2>&1

CORES=$(nproc)
MEM_GB=$(awk '/MemAvailable/ {print int($2/1024/1024)}' /proc/meminfo)
RUN_JOBS="${RUN_JOBS:-$(( MEM_GB / 3 < CORES ? MEM_GB / 3 : CORES ))}"
[ "$RUN_JOBS" -lt 1 ] && RUN_JOBS=1
stamp() { echo; echo "===== $(date '+%F %T')  $*"; }

stamp "machine: $CORES cores, ${MEM_GB} GB free -> $RUN_JOBS parallel gem5 runs"
if [ ! -x "$G" ]; then echo "FAILED: gem5 binary not found at $G"; exit 1; fi

# ---------------------------------------------------------------- 1. build
stamp "STEP 1/4  build benchmarks with the new runtime"
bash build.sh || { echo "FAILED: build.sh"; exit 1; }

# ---------------------------------------------------------------- 2. profiles
stamp "STEP 2/4  profile runs (native, for the adaptive policy)"
declare -A ARGS=(
  [map]="-n 48 -s 128" [vecmul]="-n 48 -s 64" [reduction]="-m 8 -d 6"
  [jacobi]="-n 1024 -b 128 -i 2" [matmul]="-n 384 -b 48" [sparselu]="-n 16 -b 32"
)
for b in map vecmul reduction jacobi matmul sparselu; do
  NUMA_NODES=8 NUMA_CORES_PER_NODE=6 RT_SCHED=locality OMP_DATA_DISTRIBUTION=fine \
  RT_PROFILE_OUT="profiles/$b.prof" timeout 600 "bin/$b" ${ARGS[$b]} \
      | grep -E "result|profile:" || echo "WARNING: profile run of $b failed"
done
# native correctness check of the new modes (same checksums as before)
stamp "native check: adaptive + cost on every benchmark"
for b in map vecmul reduction jacobi matmul sparselu; do
  NUMA_NODES=8 NUMA_CORES_PER_NODE=6 RT_SCHED=locality OMP_DATA_DISTRIBUTION=adaptive \
  RT_PROFILE_IN="profiles/$b.prof" RT_STEAL_DIST=cost timeout 600 "bin/$b" ${ARGS[$b]} \
      | grep -E "result|adaptive:" || echo "WARNING: native check of $b failed"
done

# ---------------------------------------------------------------- 3. gem5
stamp "STEP 3/4  gem5: 6 benchmarks x 7 configs"
python3 scripts/run_experiments.py --gem5 "$G" \
    --bench matmul,sparselu,jacobi,reduction,vecmul,map \
    --configs ws-numactl,la-fine,la-coarse,la-fine-cost,la-coarse-cost,la-adaptive,la-adaptive-cost \
    --jobs "$RUN_JOBS" --out "$OUT" \
    || echo "WARNING: some gem5 runs failed"

if [ "${PAPERSCALE:-0}" = "1" ]; then
  stamp "STEP 3b  Jacobi at paper scale (4096^2)"
  # its own profile (different allocation sizes); replaces jacobi.prof after step 3
  NUMA_NODES=8 NUMA_CORES_PER_NODE=6 RT_SCHED=locality OMP_DATA_DISTRIBUTION=fine \
  RT_PROFILE_OUT="profiles/jacobi.prof" timeout 600 bin/jacobi -n 4096 -b 128 -i 2 \
      | grep -E "result|profile:"
  python3 scripts/run_experiments.py --gem5 "$G" --bench jacobi \
      --bench-args="-n 4096 -b 128 -i 2" \
      --configs ws-numactl,la-fine,la-fine-cost,la-adaptive-cost \
      --jobs 4 --out "$OUT/paperscale" || echo "WARNING: paper-scale runs failed"
fi

# ---------------------------------------------------------------- 4. results
stamp "STEP 4/4  results table and figure"
python3 scripts/summarize_improvements.py "$OUT" || echo "WARNING: summary failed"

stamp "ALL DONE"
echo "Results: $OUT/IMPROVEMENTS_RESULTS.md and $OUT/improvements.png"
echo "Tell Claude it's finished."
