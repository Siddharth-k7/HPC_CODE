/*
 * map.c : the "Map" pattern from the paper (Listing 1).
 *
 *   for (i = 0; i < N; i++) { list[i] = malloc(SZ); initialize(list[i]); }
 *   for (i = 0; i < N; i++)
 *       #pragma omp task depend(in: list[i][0:SZ-1])
 *       map(list[i], SZ);
 *   #pragma omp taskwait
 *
 * Each task scales its own vector, so the best schedule runs task i on the
 * node that holds vector i. Data distribution guidance (Table 4): coarse.
 *
 * Options:
 *   -n <vectors>   number of vectors / tasks           (default 48)
 *   -s <KiB>       size of one vector in KiB           (default 128)
 *   -r <reps>      passes over the vector inside a task (default 1)
 *   -c             print where pages ended up (checks NUMA placement)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../runtime/numa_rt.h"

typedef struct {
    float *v;
    size_t len;
    int reps;
} map_arg_t;

static void map_task(void *p)
{
    map_arg_t *a = p;
    float *v = a->v;
    for (int r = 0; r < a->reps; r++)
        for (size_t j = 0; j < a->len; j++)
            v[j] = v[j] * 1.0001f + 0.5f;
}

int main(int argc, char **argv)
{
    int n = 48, kib = 128, reps = 1, check = 0, opt;
    while ((opt = getopt(argc, argv, "n:s:r:c")) != -1) {
        switch (opt) {
        case 'n': n = atoi(optarg); break;
        case 's': kib = atoi(optarg); break;
        case 'r': reps = atoi(optarg); break;
        case 'c': check = 1; break;
        default:
            fprintf(stderr, "usage: %s [-n vectors] [-s KiB] [-r reps] [-c]\n",
                    argv[0]);
            return 1;
        }
    }
    rt_init();
    size_t len = (size_t)kib * 1024 / sizeof(float);
    printf("map: %d vectors x %d KiB, reps=%d\n", n, kib, reps);

    float **list = malloc(sizeof(float *) * n);
    for (int i = 0; i < n; i++) {
        list[i] = rt_malloc(len * sizeof(float));
        for (size_t j = 0; j < len; j++)      /* initialize (first touch) */
            list[i][j] = (float)(i + j % 7);
    }
    if (check) {
        rt_print_placement("vector0", list[0], len * sizeof(float));
        if (n > 1)
            rt_print_placement("vector1", list[1], len * sizeof(float));
    }

    rt_roi_begin();
    for (int i = 0; i < n; i++) {
        map_arg_t a = {list[i], len, reps};
        rt_dep_t d = {list[i], len * sizeof(float)};
        rt_task(map_task, &a, sizeof a, &d, 1);
    }
    rt_taskwait();
    rt_roi_end();

    /* verify */
    int bad = 0;
    for (int i = 0; i < n && !bad; i++)
        for (size_t j = 0; j < len; j += 97) {
            float x = (float)(i + j % 7);
            for (int r = 0; r < reps; r++) x = x * 1.0001f + 0.5f;
            if (list[i][j] != x) { bad = 1; break; }
        }
    printf("map: %s\n", bad ? "WRONG RESULT" : "result OK");
    rt_finalize();
    return bad;
}
