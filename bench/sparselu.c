/*
 * sparselu.c : LU factorization of a sparse blocked matrix, derived from
 *              BOTS SparseLU (paper Table 4, guidance: coarse).
 *
 * The matrix is NB x NB blocks of BS x BS floats; many blocks are empty
 * (NULL). Every block is its own allocation (coarse -> round-robin over
 * nodes). For each kk:
 *     lu0(diag)                       (serial)
 *     fwd(kk,jj)  for non-empty row blocks       } tasks, taskwait
 *     bdiv(ii,kk) for non-empty column blocks    }
 *     bmod(ii,jj) for every pair                 } tasks, taskwait
 * bmod may create a new block (allocated inside the task), like BOTS.
 *
 * Options:
 *   -n <blocks>  blocks per side (default 16)
 *   -b <size>    block size      (default 32)
 *   -v           compare with a serial run (native testing)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../runtime/numa_rt.h"

static int NB, BS;

static int is_null(int ii, int jj)
{
    int null_entry = 0;
    if ((ii < jj) && (ii % 3 != 0)) null_entry = 1;
    if ((ii > jj) && (jj % 3 != 0)) null_entry = 1;
    if (ii % 2 == 1) null_entry = 1;
    if (jj % 2 == 1) null_entry = 1;
    if (ii == jj) null_entry = 0;
    if (ii == jj - 1) null_entry = 0;
    if (ii - 1 == jj) null_entry = 0;
    return null_entry;
}

static float **genmat(int use_rt)
{
    float **M = calloc((size_t)NB * NB, sizeof(float *));
    int init_val = 1325;
    for (int ii = 0; ii < NB; ii++)
        for (int jj = 0; jj < NB; jj++) {
            if (is_null(ii, jj)) continue;
            size_t bytes = sizeof(float) * BS * BS;
            float *p = use_rt ? rt_malloc(bytes) : malloc(bytes);
            for (int k = 0; k < BS * BS; k++) {
                init_val = (3125 * init_val) % 65536;
                p[k] = (float)((init_val - 32768.0) / 16384.0);
            }
            /* LU here does no pivoting (like BOTS). With BS=32 this generator
             * gives an exactly singular leading minor (zero pivot -> NaN), so
             * make the matrix diagonally dominant: every |entry| <= 2 and a row
             * has at most NB*BS entries, so adding 2*NB*BS to the diagonal
             * keeps every pivot safe. Same work per task, only values change. */
            if (ii == jj)
                for (int d = 0; d < BS; d++)
                    p[d * BS + d] += (float)(2.0 * NB * BS);
            M[ii * NB + jj] = p;
        }
    return M;
}

static void lu0(float *diag)
{
    for (int k = 0; k < BS; k++)
        for (int i = k + 1; i < BS; i++) {
            diag[i * BS + k] /= diag[k * BS + k];
            for (int j = k + 1; j < BS; j++)
                diag[i * BS + j] -= diag[i * BS + k] * diag[k * BS + j];
        }
}

static void fwd(const float *diag, float *col)
{
    for (int k = 0; k < BS; k++)
        for (int i = k + 1; i < BS; i++)
            for (int j = 0; j < BS; j++)
                col[i * BS + j] -= diag[i * BS + k] * col[k * BS + j];
}

static void bdiv(const float *diag, float *row)
{
    for (int i = 0; i < BS; i++)
        for (int k = 0; k < BS; k++) {
            row[i * BS + k] /= diag[k * BS + k];
            for (int j = k + 1; j < BS; j++)
                row[i * BS + j] -= row[i * BS + k] * diag[k * BS + j];
        }
}

static void bmod(const float *row, const float *col, float *inner)
{
    for (int i = 0; i < BS; i++)
        for (int k = 0; k < BS; k++) {
            float r = row[i * BS + k];
            for (int j = 0; j < BS; j++)
                inner[i * BS + j] -= r * col[k * BS + j];
        }
}

/* ---- tasks ---- */
static float **A;
typedef struct { int ii, jj, kk; } arg_t;

static void fwd_task(void *p)
{
    arg_t *a = p;
    fwd(A[a->kk * NB + a->kk], A[a->kk * NB + a->jj]);
}
static void bdiv_task(void *p)
{
    arg_t *a = p;
    bdiv(A[a->kk * NB + a->kk], A[a->ii * NB + a->kk]);
}
static void bmod_task(void *p)
{
    arg_t *a = p;
    float **dst = &A[a->ii * NB + a->jj];
    if (*dst == NULL) {   /* fill-in: new block, allocated inside the task */
        float *b = rt_malloc(sizeof(float) * BS * BS);
        memset(b, 0, sizeof(float) * BS * BS);
        *dst = b;
    }
    bmod(A[a->ii * NB + a->kk], A[a->kk * NB + a->jj], *dst);
}

static void sparselu_serial(float **M)
{
    for (int kk = 0; kk < NB; kk++) {
        lu0(M[kk * NB + kk]);
        for (int jj = kk + 1; jj < NB; jj++)
            if (M[kk * NB + jj]) fwd(M[kk * NB + kk], M[kk * NB + jj]);
        for (int ii = kk + 1; ii < NB; ii++)
            if (M[ii * NB + kk]) bdiv(M[kk * NB + kk], M[ii * NB + kk]);
        for (int ii = kk + 1; ii < NB; ii++)
            if (M[ii * NB + kk])
                for (int jj = kk + 1; jj < NB; jj++)
                    if (M[kk * NB + jj]) {
                        if (!M[ii * NB + jj])
                            M[ii * NB + jj] = calloc(BS * BS, sizeof(float));
                        bmod(M[ii * NB + kk], M[kk * NB + jj], M[ii * NB + jj]);
                    }
    }
}

int main(int argc, char **argv)
{
    int verify = 0, opt;
    NB = 16; BS = 32;
    while ((opt = getopt(argc, argv, "n:b:v")) != -1) {
        if (opt == 'n') NB = atoi(optarg);
        else if (opt == 'b') BS = atoi(optarg);
        else if (opt == 'v') verify = 1;
    }
    rt_init();
    size_t bbytes = sizeof(float) * BS * BS;
    printf("sparselu: %d x %d blocks of %d x %d\n", NB, NB, BS, BS);
    A = genmat(1);

    rt_roi_begin();
    for (int kk = 0; kk < NB; kk++) {
        lu0(A[kk * NB + kk]);
        for (int jj = kk + 1; jj < NB; jj++)
            if (A[kk * NB + jj]) {
                arg_t a = {0, jj, kk};
                rt_dep_t d[2] = {{A[kk * NB + kk], bbytes}, {A[kk * NB + jj], bbytes}};
                rt_task(fwd_task, &a, sizeof a, d, 2);
            }
        for (int ii = kk + 1; ii < NB; ii++)
            if (A[ii * NB + kk]) {
                arg_t a = {ii, 0, kk};
                rt_dep_t d[2] = {{A[kk * NB + kk], bbytes}, {A[ii * NB + kk], bbytes}};
                rt_task(bdiv_task, &a, sizeof a, d, 2);
            }
        rt_taskwait();
        for (int ii = kk + 1; ii < NB; ii++)
            if (A[ii * NB + kk])
                for (int jj = kk + 1; jj < NB; jj++)
                    if (A[kk * NB + jj]) {
                        arg_t a = {ii, jj, kk};
                        rt_dep_t d[3];
                        int nd = 0;
                        d[nd++] = (rt_dep_t){A[ii * NB + kk], bbytes};
                        d[nd++] = (rt_dep_t){A[kk * NB + jj], bbytes};
                        if (A[ii * NB + jj])
                            d[nd++] = (rt_dep_t){A[ii * NB + jj], bbytes};
                        rt_task(bmod_task, &a, sizeof a, d, nd);
                    }
        rt_taskwait();
    }
    rt_roi_end();

    double sum = 0;
    int bad = 0;
    for (int i = 0; i < NB * NB; i++)
        if (A[i])
            for (int k = 0; k < BS * BS; k += 13) {
                if (!isfinite(A[i][k])) bad = 1;
                sum += A[i][k];
            }
    if (verify) {
        float **R = genmat(0);
        sparselu_serial(R);
        for (int i = 0; i < NB * NB && !bad; i++) {
            if (!A[i] != !R[i]) { bad = 1; break; }
            if (A[i])
                for (int k = 0; k < BS * BS; k++)
                    if (fabsf(A[i][k] - R[i][k]) > 1e-3f * (fabsf(R[i][k]) + 1)) {
                        bad = 1;
                        break;
                    }
        }
    }
    printf("sparselu: checksum %.4f, %s%s\n", sum, bad ? "WRONG RESULT" : "result OK",
           verify ? " (verified vs serial)" : "");
    rt_finalize();
    return bad;
}
