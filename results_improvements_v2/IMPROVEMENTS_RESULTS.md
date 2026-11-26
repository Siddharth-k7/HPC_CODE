# Results: the two improvements

Execution time of the region of interest, divided by ws-numactl (work-stealing + `numactl --interleave=all`, the paper's baseline). Below 1.00x = faster than the baseline. One gem5 run per cell (gem5 is deterministic).

## 1. Normalized execution time

| Benchmark | ws-numactl | la-fine | la-coarse | la-fine-cost | la-coarse-cost | la-adaptive | la-adaptive-cost |
|---|---|---|---|---|---|---|---|
| map | 1.00x | 1.16x | 0.87x | 1.17x | 1.32x | 0.87x | 0.93x |
| vecmul | 1.00x | 1.13x | 1.03x | 1.19x | 1.42x | 1.03x | 1.50x |
| matmul | 1.00x | 1.49x | 0.99x | 1.03x | 1.00x | 0.99x | 1.00x |
| jacobi | 1.00x | 1.44x | 1.39x | 1.00x | 1.02x | 1.44x | 1.00x |
| reduction | 1.00x | 1.10x | 1.01x | 1.00x | 0.95x | 1.10x | 1.00x |
| sparselu | 1.00x | 1.23x | 1.24x | 1.05x | 1.06x | 1.21x | 1.06x |

Correctness: every run printed `result OK`.

## 2. Improvement 1: cost-aware stealing vs the paper's rule

| Benchmark | la-fine (hops) | la-fine-cost | change | la-coarse (hops) | la-coarse-cost | change | steals accepted / rejected (fine, coarse) |
|---|---|---|---|---|---|---|---|
| map | 1.16x | 1.17x | 1.2% | 0.87x | 1.32x | 52.0% | 42 / 0, 2 / 276 |
| vecmul | 1.13x | 1.19x | 5.3% | 1.03x | 1.42x | 38.3% | 42 / 0, 9 / 204 |
| matmul | 1.49x | 1.03x | -31.1% | 0.99x | 1.00x | 1.0% | 53 / 78, 18 / 90 |
| jacobi | 1.44x | 1.00x | -30.4% | 1.39x | 1.02x | -26.3% | 106 / 0, 105 / 97 |
| reduction | 1.10x | 1.00x | -9.0% | 1.01x | 0.95x | -6.0% | 38 / 0, 41 / 71 |
| sparselu | 1.23x | 1.05x | -14.6% | 1.24x | 1.06x | -14.5% | 332 / 28, 334 / 11 |

## 3. Improvement 2: adaptive distribution vs choosing by hand

| Benchmark | adaptive chose | la-adaptive | best hand-picked (la-fine / la-coarse) | worse hand-picked | gap to best |
|---|---|---|---|---|---|
| map | fine=0 coarse=48 | 0.87x | coarse 0.87x | fine 1.16x | -0.2% |
| vecmul | fine=0 coarse=144 | 1.03x | coarse 1.03x | fine 1.13x | 0.3% |
| matmul | fine=0 coarse=192 | 0.99x | coarse 0.99x | fine 1.49x | -0.0% |
| jacobi | fine=2 coarse=0 | 1.44x | coarse 1.39x | fine 1.44x | 3.7% |
| reduction | fine=2 coarse=0 | 1.10x | coarse 1.01x | fine 1.10x | 8.3% |
| sparselu | fine=0 coarse=144 | 1.21x | fine 1.23x | coarse 1.24x | -1.7% |

## 4. Both together

| Benchmark | ws-numactl | best paper config (la-fine / la-coarse) | la-adaptive-cost | avg miss latency (cycles): best paper -> adaptive-cost |
|---|---|---|---|---|
| map | 1.00x | la-coarse 0.87x | 0.93x | 158 -> 219 |
| vecmul | 1.00x | la-coarse 1.03x | 1.50x | 255 -> 329 |
| matmul | 1.00x | la-coarse 0.99x | 1.00x | 204 -> 270 |
| jacobi | 1.00x | la-coarse 1.39x | 1.00x | 273 -> 296 |
| reduction | 1.00x | la-coarse 1.01x | 1.00x | 263 -> 266 |
| sparselu | 1.00x | la-fine 1.23x | 1.06x | 654 -> 1130 |
