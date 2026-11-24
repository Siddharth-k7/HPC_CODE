/*
 * reduction.c : iterative merge phase of BOTS Sort (paper Table 4,
 *               guidance: fine).
 *
 * One big array (single malloc -> fine) holds 2^depth sorted runs. Each
 * level merges neighbouring runs pairwise into a second array, one task per
 * pair, with a taskwait between levels. After `depth` levels the array is
 * fully sorted.
 *
 * Options:
 *   -m <MiB>    array size in MiB (default 8)
 *   -d <depth>  number of merge levels (default 6 -> 64 initial runs)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../runtime/numa_rt.h"

typedef struct {
    const int32_t *src;
    int32_t *dst;
    size_t lo, mid, hi;   /* merge src[lo,mid) and src[mid,hi) into dst[lo,hi) */
} arg_t;

static void merge_task(void *p)
{
    arg_t *a = p;
    size_t i = a->lo, j = a->mid, k = a->lo;
    while (i < a->mid && j < a->hi)
        a->dst[k++] = (a->src[i] <= a->src[j]) ? a->src[i++] : a->src[j++];
    while (i < a->mid) a->dst[k++] = a->src[i++];
    while (j < a->hi) a->dst[k++] = a->src[j++];
}

int main(int argc, char **argv)
{
    int mib = 8, depth = 6, opt;
    while ((opt = getopt(argc, argv, "m:d:")) != -1) {
        if (opt == 'm') mib = atoi(optarg);
        else if (opt == 'd') depth = atoi(optarg);
    }
    rt_init();
    size_t n = (size_t)mib * 1024 * 1024 / sizeof(int32_t);
    size_t runs = (size_t)1 << depth;
    n -= n % runs;
    size_t run_len = n / runs;
    printf("reduction: %zu ints (%d MiB), %zu runs, %d levels\n", n, mib, runs, depth);

    int32_t *buf[2] = {rt_malloc(n * sizeof(int32_t)), rt_malloc(n * sizeof(int32_t))};
    /* run r holds r, r+runs, r+2*runs, ... (sorted), all runs interleave */
    for (size_t r = 0; r < runs; r++)
        for (size_t k = 0; k < run_len; k++)
            buf[0][r * run_len + k] = (int32_t)(r + k * runs);
    for (size_t k = 0; k < n; k++) buf[1][k] = 0;

    rt_roi_begin();
    size_t width = run_len;
    int src = 0;
    for (int lvl = 0; lvl < depth; lvl++) {
        for (size_t lo = 0; lo < n; lo += 2 * width) {
            size_t mid = lo + width, hi = lo + 2 * width;
            if (hi > n) hi = n;
            if (mid > n) mid = n;
            arg_t a = {buf[src], buf[1 - src], lo, mid, hi};
            rt_dep_t d[2] = {{buf[src] + lo, (hi - lo) * sizeof(int32_t)},
                             {buf[1 - src] + lo, (hi - lo) * sizeof(int32_t)}};
            rt_task(merge_task, &a, sizeof a, d, 2);
        }
        rt_taskwait();
        src = 1 - src;
        width *= 2;
    }
    rt_roi_end();

    int bad = 0;
    for (size_t k = 0; k < n; k++)
        if (buf[src][k] != (int32_t)k) { bad = 1; break; }
    printf("reduction: %s\n", bad ? "WRONG RESULT" : "result OK");
    rt_finalize();
    return bad;
}
