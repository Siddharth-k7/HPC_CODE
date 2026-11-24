/*
 * gpu_locality.cu : the NUMA-locality question, replayed on a CPU<->GPU system.
 *
 * A discrete GPU over PCIe is an extreme 2-node NUMA machine: host DRAM is
 * "node 0", GPU VRAM is "node 1", and the link between them (PCIe) is far
 * slower and far higher-latency than any link gem5's NumaOpteron model used.
 * This program runs the same "Map" pattern as bench/map.c (each task scales
 * its own vector) under three data-placement policies, which map onto the
 * NUMA project's three distributions:
 *
 *   explicit   - cudaMalloc + cudaMemcpy up front, like "coarse": the data is
 *                placed on its node (the GPU) before any task touches it.
 *   managed    - cudaMallocManaged, first touched by the CPU (init loop),
 *                then read by the GPU: like Linux "first-touch" / "standard"
 *                distribution. The driver migrates pages across PCIe lazily,
 *                on fault, exactly like first-touch migrates across NUMA
 *                nodes lazily, on access.
 *   prefetched - cudaMallocManaged + cudaMemPrefetchAsync to the GPU before
 *                the kernel runs: like "locality-aware" scheduling, which
 *                moves data (or tasks) to the right node ahead of time
 *                instead of waiting for a fault.
 *
 * It also runs a scheduling analogue of Algorithm 2 (work-stealing vs.
 * locality-aware): splitting one big array into chunks ("tasks") and
 * either running them all on one CUDA stream (serial, no overlap -- the
 * "put everything in one queue" failure mode from Jacobi/SparseLU) or
 * round-robin across several streams so copy and compute overlap.
 *
 * Build : nvcc -O3 -arch=sm_89 -o gpu_locality gpu_locality.cu
 *         (sm_89 = Ada Lovelace, e.g. RTX 4000 Ada; drop -arch for a generic build)
 * Run   : ./gpu_locality [elements_millions] [n_tasks] [n_streams]
 */
#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>

#define CHECK(call) do { \
    cudaError_t e = (call); \
    if (e != cudaSuccess) { \
        fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(e)); \
        exit(1); \
    } \
} while (0)

__global__ void scale_kernel(float *data, size_t n, float factor)
{
    size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x;
    if (i < n) data[i] = data[i] * factor + 1.0f;
}

static double now_ms(cudaEvent_t a, cudaEvent_t b)
{
    float ms = 0;
    cudaEventElapsedTime(&ms, a, b);
    return (double)ms;
}

/* ---- experiment 1: data placement policy (coarse / first-touch / locality-aware) ---- */
static void run_placement(const char *name, size_t n, int device,
                           void (*setup)(float **, size_t, int),
                           void (*prelaunch)(float *, size_t, int))
{
    cudaEvent_t t0, t1, t2, t3;
    cudaEventCreate(&t0); cudaEventCreate(&t1); cudaEventCreate(&t2); cudaEventCreate(&t3);

    float *data = nullptr;
    cudaEventRecord(t0);
    setup(&data, n, device);           /* allocate + CPU-side first touch / copy */
    cudaEventRecord(t1);
    if (prelaunch) prelaunch(data, n, device);   /* prefetch, if this policy uses it */
    cudaEventRecord(t2);

    int threads = 256;
    int blocks = (int)((n + threads - 1) / threads);
    scale_kernel<<<blocks, threads>>>(data, n, 1.0001f);
    CHECK(cudaGetLastError());
    cudaEventRecord(t3);
    CHECK(cudaDeviceSynchronize());

    double setup_ms = now_ms(t0, t1);
    double prefetch_ms = now_ms(t1, t2);
    double kernel_ms = now_ms(t2, t3);
    double total_ms = now_ms(t0, t3);
    double gbps = (n * sizeof(float) / 1e9) / (total_ms / 1000.0);

    printf("%-12s n=%8zu  setup=%8.3f ms  prefetch=%8.3f ms  kernel=%8.3f ms  total=%8.3f ms  %.2f GB/s\n",
           name, n, setup_ms, prefetch_ms, kernel_ms, total_ms, gbps);

    cudaFree(data);
    cudaEventDestroy(t0); cudaEventDestroy(t1); cudaEventDestroy(t2); cudaEventDestroy(t3);
}

static void setup_explicit(float **data, size_t n, int device)
{
    float *h = (float *)malloc(n * sizeof(float));
    for (size_t i = 0; i < n; i++) h[i] = (float)(i % 97) * 0.01f;
    CHECK(cudaMalloc(data, n * sizeof(float)));
    CHECK(cudaMemcpy(*data, h, n * sizeof(float), cudaMemcpyHostToDevice));
    free(h);
}

static void setup_managed_firsttouch(float **data, size_t n, int device)
{
    CHECK(cudaMallocManaged(data, n * sizeof(float)));
    /* CPU first-touch: like Linux's default "standard" placement, this
     * binds the pages to the host until something else touches them. */
    for (size_t i = 0; i < n; i++) (*data)[i] = (float)(i % 97) * 0.01f;
}

static void prefetch_to_gpu(float *data, size_t n, int device)
{
    CHECK(cudaMemPrefetchAsync(data, n * sizeof(float), device, 0));
}

/* ---- experiment 2: scheduling across streams (one queue vs. many) ---- */
static void run_scheduling(size_t total_n, int n_tasks, int n_streams)
{
    size_t chunk = total_n / n_tasks;
    float *d_data;
    CHECK(cudaMalloc(&d_data, total_n * sizeof(float)));
    float *h_data;
    CHECK(cudaMallocHost(&h_data, total_n * sizeof(float))); /* pinned, so copies can overlap */
    for (size_t i = 0; i < total_n; i++) h_data[i] = (float)(i % 97) * 0.01f;

    cudaStream_t *streams = (cudaStream_t *)malloc(n_streams * sizeof(cudaStream_t));
    for (int s = 0; s < n_streams; s++) CHECK(cudaStreamCreate(&streams[s]));

    cudaEvent_t t0, t1;
    cudaEventCreate(&t0); cudaEventCreate(&t1);
    int threads = 256;

    /* one queue: every task on stream 0, strictly serial copy-then-compute */
    cudaEventRecord(t0);
    for (int task = 0; task < n_tasks; task++) {
        size_t off = (size_t)task * chunk;
        CHECK(cudaMemcpyAsync(d_data + off, h_data + off, chunk * sizeof(float),
                               cudaMemcpyHostToDevice, streams[0]));
        int blocks = (int)((chunk + threads - 1) / threads);
        scale_kernel<<<blocks, threads, 0, streams[0]>>>(d_data + off, chunk, 1.0001f);
    }
    cudaEventRecord(t1);
    CHECK(cudaDeviceSynchronize());
    printf("one-queue    tasks=%3d streams=1   %.3f ms\n", n_tasks, now_ms(t0, t1));

    /* many queues: tasks round-robin across streams, so copy(k+1) overlaps compute(k) */
    cudaEventRecord(t0);
    for (int task = 0; task < n_tasks; task++) {
        size_t off = (size_t)task * chunk;
        cudaStream_t s = streams[task % n_streams];
        CHECK(cudaMemcpyAsync(d_data + off, h_data + off, chunk * sizeof(float),
                               cudaMemcpyHostToDevice, s));
        int blocks = (int)((chunk + threads - 1) / threads);
        scale_kernel<<<blocks, threads, 0, s>>>(d_data + off, chunk, 1.0001f);
    }
    cudaEventRecord(t1);
    CHECK(cudaDeviceSynchronize());
    printf("many-queues  tasks=%3d streams=%d   %.3f ms\n", n_tasks, n_streams, now_ms(t0, t1));

    for (int s = 0; s < n_streams; s++) cudaStreamDestroy(streams[s]);
    free(streams);
    cudaFreeHost(h_data);
    cudaFree(d_data);
}

int main(int argc, char **argv)
{
    int device = 0;
    cudaDeviceProp prop;
    CHECK(cudaGetDevice(&device));
    CHECK(cudaGetDeviceProperties(&prop, device));
    printf("GPU: %s  (%d SMs, %.1f GB VRAM)\n\n", prop.name, prop.multiProcessorCount,
           prop.totalGlobalMem / 1e9);

    size_t n = (argc > 1 ? (size_t)atol(argv[1]) : 64) * 1000000;  /* elements */
    int n_tasks = argc > 2 ? atoi(argv[2]) : 48;    /* 48, to match the CPU project's Map */
    int n_streams = argc > 3 ? atoi(argv[3]) : 8;    /* 8, to match the CPU project's nodes */

    printf("-- experiment 1: data placement (like coarse / first-touch / locality-aware) --\n");
    run_placement("explicit",   n, device, setup_explicit, nullptr);
    run_placement("first-touch",n, device, setup_managed_firsttouch, nullptr);
    run_placement("prefetched", n, device, setup_managed_firsttouch, prefetch_to_gpu);

    printf("\n-- experiment 2: scheduling across streams (like work-stealing vs. locality-aware) --\n");
    run_scheduling(n, n_tasks, n_streams);

    return 0;
}
