#!/usr/bin/env bash
# build.sh : builds the runtime + benchmarks as static binaries in bin/
# (static linking is what gem5 SE mode handles best)
#   bash build.sh          build all
#   bash build.sh clean    remove bin/
set -euo pipefail
cd "$(dirname "$0")"
CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -pthread}"
BENCHES="map vecmul matmul jacobi reduction sparselu latency"

if [ "${1:-}" = "clean" ]; then rm -rf bin; exit 0; fi
mkdir -p bin
for b in $BENCHES; do
    echo "  CC  bin/$b"
    $CC $CFLAGS -o "bin/$b" "bench/$b.c" runtime/numa_rt.c -static -pthread -lm
done
