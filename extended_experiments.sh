#!/usr/bin/env bash
# extended_experiments.sh : the "Next steps" from the report, run for real.
#
#   0. real-hardware NUMA baseline (numactl --hardware, lscpu)
#   1. Jacobi at the paper's task count (4096^2 grid, block 128 -> 1024 tiles/sweep)
#   2. scaling study: the same machine shrunk to 4 nodes and 2 nodes
#   3. steal distance: raw OS NUMA distance vs hop count
#   4. a slower-network machine: hop-latency doubled (45 -> 90 cycles)
#
# Usage (from the project folder, in a terminal you keep open):
#     bash extended_experiments.sh
# Everything also goes to results/extended_experiments.log
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
G="${GEM5:-$HOME/gem5/build/X86_MOESI_hammer/gem5.opt}"
mkdir -p results
LOG="$HERE/results/extended_experiments.log"
exec > >(tee -a "$LOG") 2>&1

CORES=$(nproc)
stamp() { echo; echo "===== $(date '+%F %T')  $*"; }

stamp "machine: $CORES cores"
if [ ! -x "$G" ]; then echo "FAILED: gem5 binary not found at $G"; exit 1; fi

# ---------------------------------------------------------------- 0. hardware baseline
stamp "STEP 0/4  real-hardware NUMA baseline"
{
  echo "## numactl --hardware"
  numactl --hardware 2>&1 || echo "(numactl not installed: sudo apt-get install -y numactl)"
  echo
  echo "## lscpu (NUMA + cache lines)"
  lscpu 2>&1 | grep -iE "numa|model name|socket|core|thread|cache"
} | tee results/hardware_baseline.txt

# ---------------------------------------------------------------- 1. Jacobi at paper scale
stamp "STEP 1/4  Jacobi at paper scale (4096^2, block 128, ~1024 tiles/sweep)"
python3 scripts/run_experiments.py --gem5 "$G" --bench jacobi \
    --bench-args="-n 4096 -b 128 -i 2" --configs paper \
    --jobs 5 --out results_jacobi_paperscale \
    || echo "WARNING: some paper-scale Jacobi runs failed"

# ---------------------------------------------------------------- 2. scaling study
stamp "STEP 2/4  scaling study: 4 nodes, then 2 nodes (same 48 cores total)"
python3 scripts/run_experiments.py --gem5 "$G" --bench map,jacobi,matmul \
    --configs ws-numactl,la-fine,la-coarse --jobs 6 \
    --gem5-arg=--nodes=4 --gem5-arg=--cores-per-node=12 \
    --out results_scaling_n4 \
    || echo "WARNING: some 4-node scaling runs failed"

python3 scripts/run_experiments.py --gem5 "$G" --bench map,jacobi,matmul \
    --configs ws-numactl,la-fine,la-coarse --jobs 6 \
    --gem5-arg=--nodes=2 --gem5-arg=--cores-per-node=24 \
    --out results_scaling_n2 \
    || echo "WARNING: some 2-node scaling runs failed"

# ---------------------------------------------------------------- 3. steal distance: os vs hops
stamp "STEP 3/4  steal distance: raw OS NUMA distance (vs the hop count already in results/summary.csv)"
python3 scripts/run_experiments.py --gem5 "$G" --bench jacobi,matmul,sparselu \
    --configs la-fine,la-coarse --env RT_STEAL_DIST=os --jobs 6 \
    --out results_steal_os \
    || echo "WARNING: some steal-distance-os runs failed"

# ---------------------------------------------------------------- 4. slower network (2x hop latency)
stamp "STEP 4/4  slower inter-node links (hop-latency 45 -> 90 cycles)"
python3 scripts/run_experiments.py --gem5 "$G" --bench all --configs paper \
    --jobs "$CORES" --gem5-arg=--hop-latency=90 \
    --out results_hoplat90 \
    || echo "WARNING: some hop-latency-90 runs failed"

stamp "ALL DONE"
echo "New result folders: results_jacobi_paperscale  results_scaling_n4  results_scaling_n2  results_steal_os  results_hoplat90"
echo "Tell Claude it's finished."
