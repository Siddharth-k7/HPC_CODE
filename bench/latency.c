/*
 * latency.c : memory latency from node 0 to every node.
 *
 * This is our version of the paper's Figure 2 ("Latencies measured while
 * accessing 4 MB of data allocated on different NUMA nodes from node 0").
 * A buffer is placed on node k, then a random pointer chase walks it so that
 * every load misses in the caches. Cycles per load = memory latency.
 *
 * Run with RT_THREADS=1 (only the main thread is needed).
 *
 * Options:
 *   -s <KiB>    buffer size (default 1024; must be much bigger than L2)
 *   -l <loads>  loads per node (default 20000)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../runtime/numa_rt.h"

#define LINE 64

int main(int argc, char **argv)
{
    int kib = 1024, loads = 20000, opt;
    while ((opt = getopt(argc, argv, "s:l:")) != -1) {
        if (opt == 's') kib = atoi(optarg);
        else if (opt == 'l') loads = atoi(optarg);
    }
    rt_init();
    size_t bytes = (size_t)kib * 1024, lines = bytes / LINE;
    int nodes = rt_num_nodes();
    printf("latency: from node %d, buffer %d KiB, %d loads per node\n",
           rt_thread_node(), kib, loads);

    /* one random cyclic permutation of cache lines, shared by all buffers */
    size_t *perm = malloc(sizeof(size_t) * lines);
    for (size_t i = 0; i < lines; i++) perm[i] = i;
    srand(12345);
    for (size_t i = lines - 1; i > 0; i--) {
        size_t j = (size_t)rand() % (i + 1), t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }

    rt_roi_begin();
    for (int k = 0; k < nodes; k++) {
        char *buf = rt_malloc_on_node(bytes, k);
        for (size_t i = 0; i < lines; i++)  /* build the chain */
            *(void **)(buf + perm[i] * LINE) =
                buf + perm[(i + 1) % lines] * LINE;
        int where = rt_page_node(buf);
        /* warm-up walk over a few lines, then the timed walk */
        void **p = (void **)(buf + perm[0] * LINE);
        for (int i = 0; i < 64; i++) p = *p;
        uint64_t t0 = rt_rdtsc();
        for (int i = 0; i < loads; i++) p = *p;
        uint64_t t1 = rt_rdtsc();
        printf("node %d (page on node %d, distance %d): %.1f cycles/load%s\n",
               k, where, rt_distance(rt_thread_node(), k),
               (double)(t1 - t0) / loads, p ? "" : " ");
    }
    rt_roi_end();
    rt_finalize();
    return 0;
}
