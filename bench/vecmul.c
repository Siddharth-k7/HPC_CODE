/*
 * vecmul.c : "Vector cross product" (paper Table 4, guidance: coarse).
 *
 * Task i computes c[i][k] = a[i][k] x b[i][k] for its own arrays of 3D
 * vectors. Every array is a separate allocation, so with coarse distribution
 * a[i], b[i], c[i] land on three different nodes (round-robin per malloc).
 * The locality-aware scheduler then picks the node with the least weighted
 * distance to all three.
 *
 * Options:
 *   -n <tasks>   number of tasks (default 48)
 *   -s <KiB>     size of each of the 3 arrays of a task (default 64)
 *   -r <reps>    passes inside a task (default 1)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../runtime/numa_rt.h"

typedef struct { float x, y, z; } vec3;

typedef struct {
    vec3 *a, *b, *c;
    size_t len;
    int reps;
} arg_t;

static void cross_task(void *p)
{
    arg_t *t = p;
    for (int r = 0; r < t->reps; r++)
        for (size_t k = 0; k < t->len; k++) {
            vec3 a = t->a[k], b = t->b[k];
            t->c[k].x = a.y * b.z - a.z * b.y;
            t->c[k].y = a.z * b.x - a.x * b.z;
            t->c[k].z = a.x * b.y - a.y * b.x;
        }
}

int main(int argc, char **argv)
{
    int n = 48, kib = 64, reps = 1, opt;
    while ((opt = getopt(argc, argv, "n:s:r:")) != -1) {
        if (opt == 'n') n = atoi(optarg);
        else if (opt == 's') kib = atoi(optarg);
        else if (opt == 'r') reps = atoi(optarg);
    }
    rt_init();
    size_t len = (size_t)kib * 1024 / sizeof(vec3);
    printf("vecmul: %d tasks x 3 arrays x %d KiB, reps=%d\n", n, kib, reps);

    vec3 **A = malloc(sizeof(vec3 *) * n), **B = malloc(sizeof(vec3 *) * n),
         **C = malloc(sizeof(vec3 *) * n);
    for (int i = 0; i < n; i++) {
        A[i] = rt_malloc(len * sizeof(vec3));
        B[i] = rt_malloc(len * sizeof(vec3));
        C[i] = rt_malloc(len * sizeof(vec3));
        for (size_t k = 0; k < len; k++) {
            A[i][k] = (vec3){(float)(k % 5), 1.0f, (float)(i % 3)};
            B[i][k] = (vec3){2.0f, (float)(k % 3), 1.0f};
            C[i][k] = (vec3){0, 0, 0};
        }
    }

    rt_roi_begin();
    for (int i = 0; i < n; i++) {
        arg_t a = {A[i], B[i], C[i], len, reps};
        rt_dep_t d[3] = {{A[i], len * sizeof(vec3)},
                         {B[i], len * sizeof(vec3)},
                         {C[i], len * sizeof(vec3)}};
        rt_task(cross_task, &a, sizeof a, d, 3);
    }
    rt_taskwait();
    rt_roi_end();

    int bad = 0;
    for (int i = 0; i < n && !bad; i++)
        for (size_t k = 0; k < len; k += 31) {
            vec3 a = A[i][k], b = B[i][k], c = C[i][k];
            if (c.x != a.y * b.z - a.z * b.y || c.z != a.x * b.y - a.y * b.x) {
                bad = 1;
                break;
            }
        }
    printf("vecmul: %s\n", bad ? "WRONG RESULT" : "result OK");
    rt_finalize();
    return bad;
}
