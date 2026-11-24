/*
 * matmul.c : blocked matrix multiplication (paper Table 4, guidance: coarse).
 *
 * C = A * B with n x n float matrices split into b x b blocks. Every block is
 * its own allocation, so coarse distribution places blocks round-robin over
 * the nodes. Task (i,j) computes C[i][j] += sum_k A[i][k] * B[k][j], which
 * reads a chain of blocks from A and B (paper: "Each task ... updates a block
 * in the output matrix using a chain of blocks from two input matrices").
 *
 * Options:
 *   -n <size>   matrix size (default 512)
 *   -b <block>  block size  (default 64)
 *   -v          verify against a simple serial multiply (slow; native only)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../runtime/numa_rt.h"

static int N, BS, NB;
static float **Ab, **Bb, **Cb;   /* NB*NB block pointers */
#define BLK(M, i, j) M[(i) * NB + (j)]

typedef struct { int i, j; } arg_t;

/* C += A * B for one pair of blocks (i-k-j order, like a small sgemm) */
static void block_gemm(const float *a, const float *b, float *c)
{
    for (int i = 0; i < BS; i++)
        for (int k = 0; k < BS; k++) {
            float aik = a[i * BS + k];
            const float *brow = b + k * BS;
            float *crow = c + i * BS;
            for (int j = 0; j < BS; j++) crow[j] += aik * brow[j];
        }
}

static void mm_task(void *p)
{
    arg_t *t = p;
    for (int k = 0; k < NB; k++)
        block_gemm(BLK(Ab, t->i, k), BLK(Bb, k, t->j), BLK(Cb, t->i, t->j));
}

static float aval(int r, int c) { return (float)((r * 7 + c * 3) % 11) * 0.1f; }
static float bval(int r, int c) { return (float)((r * 5 + c) % 13) * 0.1f; }

int main(int argc, char **argv)
{
    int verify = 0, opt;
    N = 512; BS = 64;
    while ((opt = getopt(argc, argv, "n:b:v")) != -1) {
        if (opt == 'n') N = atoi(optarg);
        else if (opt == 'b') BS = atoi(optarg);
        else if (opt == 'v') verify = 1;
    }
    NB = N / BS;
    rt_init();
    printf("matmul: %d x %d, block %d (%d x %d blocks, %d tasks)\n",
           N, N, BS, NB, NB, NB * NB);
    size_t bbytes = sizeof(float) * BS * BS;
    Ab = malloc(sizeof(float *) * NB * NB);
    Bb = malloc(sizeof(float *) * NB * NB);
    Cb = malloc(sizeof(float *) * NB * NB);
    /* allocation order A, B, C per block -> coarse spreads them */
    for (int i = 0; i < NB; i++)
        for (int j = 0; j < NB; j++) {
            BLK(Ab, i, j) = rt_malloc(bbytes);
            BLK(Bb, i, j) = rt_malloc(bbytes);
            BLK(Cb, i, j) = rt_malloc(bbytes);
            for (int r = 0; r < BS; r++)
                for (int c = 0; c < BS; c++) {
                    BLK(Ab, i, j)[r * BS + c] = aval(i * BS + r, j * BS + c);
                    BLK(Bb, i, j)[r * BS + c] = bval(i * BS + r, j * BS + c);
                    BLK(Cb, i, j)[r * BS + c] = 0.0f;
                }
        }

    rt_roi_begin();
    for (int i = 0; i < NB; i++)
        for (int j = 0; j < NB; j++) {
            rt_dep_t d[RT_MAX_DEPS];
            int nd = 0;
            d[nd++] = (rt_dep_t){BLK(Cb, i, j), bbytes};
            for (int k = 0; k < NB && nd + 2 <= RT_MAX_DEPS; k++) {
                d[nd++] = (rt_dep_t){BLK(Ab, i, k), bbytes};
                d[nd++] = (rt_dep_t){BLK(Bb, k, j), bbytes};
            }
            arg_t a = {i, j};
            rt_task(mm_task, &a, sizeof a, d, nd);
        }
    rt_taskwait();
    rt_roi_end();

    /* check a few entries (always), or all of them with -v */
    int bad = 0;
    for (int s = 0; s < (verify ? N * N : 64) && !bad; s++) {
        int r = verify ? s / N : (s * 37) % N, c = verify ? s % N : (s * 91) % N;
        double ref = 0;
        for (int k = 0; k < N; k++) ref += (double)aval(r, k) * bval(k, c);
        float got = BLK(Cb, r / BS, c / BS)[(r % BS) * BS + (c % BS)];
        if (fabs(got - ref) > 1e-3 * fabs(ref) + 1e-3) bad = 1;
    }
    printf("matmul: %s\n", bad ? "WRONG RESULT" : "result OK");
    rt_finalize();
    return bad;
}
