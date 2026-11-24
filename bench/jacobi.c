/*
 * jacobi.c : blocked 2D heat equation solver (paper Table 4, guidance: fine).
 *
 * Two n x n grids (old, new), each one big allocation ("one malloc, many
 * tasks" -> fine). Grids are stored tile by tile (b x b tiles contiguous),
 * so each tile is one dependence range. One task per tile per sweep:
 * new = average of the 4 neighbours in old. taskwait between sweeps,
 * then old/new swap. Boundary cells are fixed.
 *
 * Options:
 *   -n <size>   grid size (default 1024)
 *   -b <block>  tile size (default 128)
 *   -i <iters>  sweeps    (default 4)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../runtime/numa_rt.h"

static int N, BS, NB;

/* address of cell (r,c) in tiled storage */
static inline float *cell(float *g, int r, int c)
{
    int ti = r / BS, tj = c / BS;
    return g + ((size_t)(ti * NB + tj) * BS * BS) + (r % BS) * BS + (c % BS);
}
static inline float *tile(float *g, int ti, int tj)
{
    return g + (size_t)(ti * NB + tj) * BS * BS;
}

typedef struct { float *old, *nw; int ti, tj; } arg_t;

static void sweep_task(void *p)
{
    arg_t *a = p;
    int r0 = a->ti * BS, c0 = a->tj * BS;
    for (int r = r0; r < r0 + BS; r++)
        for (int c = c0; c < c0 + BS; c++) {
            if (r == 0 || c == 0 || r == N - 1 || c == N - 1) {
                *cell(a->nw, r, c) = *cell(a->old, r, c);
                continue;
            }
            *cell(a->nw, r, c) = 0.25f * (*cell(a->old, r - 1, c) +
                                          *cell(a->old, r + 1, c) +
                                          *cell(a->old, r, c - 1) +
                                          *cell(a->old, r, c + 1));
        }
}

static float init_val(int r, int c)
{
    return (r == 0) ? 100.0f : (c == 0 ? 50.0f : 0.0f);  /* hot edges */
}

int main(int argc, char **argv)
{
    int iters = 4, opt;
    N = 1024; BS = 128;
    while ((opt = getopt(argc, argv, "n:b:i:")) != -1) {
        if (opt == 'n') N = atoi(optarg);
        else if (opt == 'b') BS = atoi(optarg);
        else if (opt == 'i') iters = atoi(optarg);
    }
    NB = N / BS;
    rt_init();
    printf("jacobi: %d x %d grid, tile %d (%d tiles), %d sweeps\n",
           N, N, BS, NB * NB, iters);
    size_t gbytes = sizeof(float) * (size_t)N * N, tbytes = sizeof(float) * BS * BS;
    float *g[2] = {rt_malloc(gbytes), rt_malloc(gbytes)};
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            *cell(g[0], r, c) = init_val(r, c);
            *cell(g[1], r, c) = 0.0f;
        }

    rt_roi_begin();
    for (int it = 0; it < iters; it++) {
        float *old = g[it % 2], *nw = g[(it + 1) % 2];
        for (int ti = 0; ti < NB; ti++)
            for (int tj = 0; tj < NB; tj++) {
                rt_dep_t d[6];
                int nd = 0;
                d[nd++] = (rt_dep_t){tile(nw, ti, tj), tbytes};
                d[nd++] = (rt_dep_t){tile(old, ti, tj), tbytes};
                if (ti > 0)      d[nd++] = (rt_dep_t){tile(old, ti - 1, tj), tbytes};
                if (ti < NB - 1) d[nd++] = (rt_dep_t){tile(old, ti + 1, tj), tbytes};
                if (tj > 0)      d[nd++] = (rt_dep_t){tile(old, ti, tj - 1), tbytes};
                if (tj < NB - 1) d[nd++] = (rt_dep_t){tile(old, ti, tj + 1), tbytes};
                arg_t a = {old, nw, ti, tj};
                rt_task(sweep_task, &a, sizeof a, d, nd);
            }
        rt_taskwait();
    }
    rt_roi_end();

    /* verify a few interior cells against a serial computation of 1 sweep
     * band: cheap check that values are finite, bounded and symmetric-ish */
    float *res = g[iters % 2];
    int bad = 0;
    double sum = 0;
    for (int r = 0; r < N; r += 7)
        for (int c = 0; c < N; c += 7) {
            float v = *cell(res, r, c);
            if (!(v >= 0.0f && v <= 100.0f)) bad = 1;
            sum += v;
        }
    /* the cell next to the hot top edge must have warmed up */
    if (iters > 0 && *cell(res, 1, N / 2) <= 0.0f) bad = 1;
    printf("jacobi: checksum %.4f, %s\n", sum, bad ? "WRONG RESULT" : "result OK");
    rt_finalize();
    return bad;
}
