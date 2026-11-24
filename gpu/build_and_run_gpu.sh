#!/usr/bin/env bash
# build_and_run_gpu.sh : build and run the GPU locality experiment.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

if ! command -v nvcc >/dev/null 2>&1; then
    echo "nvcc not found. Install the CUDA toolkit first, e.g.:"
    echo "    sudo apt-get install -y nvidia-cuda-toolkit"
    echo "or, for the newer toolkit matching driver 595.x:"
    echo "    https://developer.nvidia.com/cuda-downloads  (Linux x86_64, deb network install)"
    exit 1
fi

echo "== building (sm_89 = Ada Lovelace, RTX 4000 Ada)"
nvcc -O3 -arch=sm_89 -o gpu_locality gpu_locality.cu

echo "== running"
mkdir -p results
./gpu_locality 64 48 8 | tee results/gpu_locality.txt

echo
echo "Wrote results/gpu_locality.txt"
echo "Tell Claude it's finished."
