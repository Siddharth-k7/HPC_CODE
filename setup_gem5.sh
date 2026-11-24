#!/usr/bin/env bash
# setup_gem5.sh : one-time setup inside WSL2 Ubuntu (or any Ubuntu 22.04/24.04)
#
#   1. installs build tools
#   2. downloads gem5 (stable branch) into ~/gem5
#   3. applies our NUMA-SE patch
#   4. builds gem5 for x86 + MOESI_hammer (takes 1-2 hours on a laptop)
#   5. builds our benchmarks
#
# Usage (from this project folder):
#     bash setup_gem5.sh            # uses 2 build jobs
#     JOBS=4 bash setup_gem5.sh     # 16 GB RAM or more: 4 jobs is fine
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
GEM5="${GEM5_DIR:-$HOME/gem5}"
JOBS="${JOBS:-2}"

echo "== 1/5 installing packages (needs sudo)"
sudo apt-get update
sudo apt-get install -y build-essential git m4 scons zlib1g zlib1g-dev \
    libprotobuf-dev protobuf-compiler libprotoc-dev libgoogle-perftools-dev \
    python3-dev python3-pip libboost-all-dev pkg-config python3-matplotlib lld

echo "== 2/5 getting gem5 into $GEM5"
if [ ! -d "$GEM5/.git" ]; then
    git clone --depth 1 -b stable https://github.com/gem5/gem5.git "$GEM5"
fi

echo "== 3/5 applying the NUMA-SE patch"
python3 "$HERE/gem5-patch/apply_numa_patch.py" "$GEM5"
cp "$HERE/gem5-patch/X86_MOESI_hammer" "$GEM5/build_opts/X86_MOESI_hammer"

echo "== 4/5 building gem5 (this is the long step)"
cd "$GEM5"
scons defconfig build/X86_MOESI_hammer build_opts/X86_MOESI_hammer
# lld needs far less RAM than the default linker (which needs > 7 GB here)
scons build/X86_MOESI_hammer/gem5.opt -j"$JOBS" --linker=lld

echo "== 5/5 building benchmarks"
cd "$HERE"
bash build.sh

echo
echo "Done. gem5 binary: $GEM5/build/X86_MOESI_hammer/gem5.opt"
echo "Quick test:  bash run_quick_test.sh"
