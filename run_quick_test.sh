#!/usr/bin/env bash
# run_quick_test.sh : first check after setup_gem5.sh
#
#  1. latency probe (paper Figure 2): node 0 -> every node, 1 thread
#  2. Map on the full 48-core machine, locality-aware + coarse, with a
#     placement check (pages of vector 0 should be on node 0, vector 1 on node 1)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
G="${GEM5:-$HOME/gem5/build/X86_MOESI_hammer/gem5.opt}"
CFG="$HERE/gem5-config/numa_machine.py"
mkdir -p "$HERE/results/quick"

echo "== latency probe (about a minute)"
printf "RT_THREADS=1\n" > "$HERE/results/quick/env_lat.txt"
"$G" -d "$HERE/results/quick/latency" "$CFG" \
    --cmd "$HERE/bin/latency" --options="-s 1024 -l 5000" \
    --env "$HERE/results/quick/env_lat.txt" | grep -E "node|NUMA|Exiting"

echo
echo "== map, 48 cores, locality-aware + coarse"
printf "RT_SCHED=locality\nOMP_DATA_DISTRIBUTION=coarse\n" > "$HERE/results/quick/env_map.txt"
"$G" -d "$HERE/results/quick/map" "$CFG" \
    --cmd "$HERE/bin/map" --options="-n 48 -s 128 -c" \
    --env "$HERE/results/quick/env_map.txt" | grep -E "numa_rt|placement|map:|ROI|tasks per node|NUMA pages|Exiting"
