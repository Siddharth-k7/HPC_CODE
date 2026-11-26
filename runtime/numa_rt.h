/*
 * numa_rt.h : a small task runtime that implements
 *
 *   "Locality-Aware Task Scheduling and Data Distribution for OpenMP
 *    Programs on NUMA Systems and Manycore Processors"
 *   A. Muddukrishna, P. A. Jonsson, M. Brorsson, Scientific Programming 2015
 *
 * It plays the role of the paper's MIR runtime:
 *   - runtime-assisted data distribution: rt_malloc / rt_malloc_specific
 *     with the policies standard / fine / coarse (paper Table 1, Listing 2)
 *   - two schedulers:
 *       ws        work-stealing baseline: one deque per core, steal round-robin
 *       locality  locality-aware: one queue per NUMA node,
 *                 work-dealing (Algorithm 1) + work-finding (Algorithm 2)
 *
 * Tasks carry a "depend" list of (address, bytes) ranges. Like the paper,
 * the list is only used to estimate the task's data footprint. Ordering
 * between tasks is done with rt_taskwait(), as in the BOTS benchmarks.
 *
 * Environment variables:
 *   OMP_DATA_DISTRIBUTION = standard | fine | coarse | adaptive (default standard)
 *   RT_SCHED              = ws | locality              (default ws)
 *   NUMA_NODES, NUMA_CORES_PER_NODE                    (set by gem5 config)
 *   RT_LLC_BYTES          last-level cache size of one node (default 384 KiB)
 *   RT_STEAL_DIST         hops | os | none | cost : rule used in Algorithm 2
 *                         (cost = Improvement 1, NUMA cost-aware stealing)
 *   RT_LAT_LOCAL, RT_LAT_HOP  memory latency model for "cost", in cycles
 *                         (default 151 + 92 per hop, measured by the probe)
 *   RT_PROFILE_OUT        file: write per-allocation task-access profile
 *   RT_PROFILE_IN         file: profile read by OMP_DATA_DISTRIBUTION=adaptive
 *   RT_TRACE              file name: write a per-task CSV (thread timelines)
 */
#ifndef NUMA_RT_H
#define NUMA_RT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- data distribution (paper Section 3) ---------------- */
typedef enum {
    RT_DIST_STANDARD = 0, /* leave it to the OS: first touch                */
    RT_DIST_FINE     = 1, /* pages round-robin over all nodes               */
    RT_DIST_COARSE   = 2, /* whole allocation on one node, next allocation   */
                          /* on the next node (round-robin per allocation)  */
    RT_DIST_ADAPTIVE = 3  /* Improvement 2: the runtime picks fine or coarse */
                          /* per allocation from evidence (size rule +      */
                          /* RT_PROFILE_IN). Never stored: each allocation  */
                          /* ends up fine or coarse.                        */
} rt_dist_t;

/* omp_malloc in the paper: uses the global policy (OMP_DATA_DISTRIBUTION) */
void *rt_malloc(size_t bytes);
/* omp_malloc_specific in the paper: overrides the global policy */
void *rt_malloc_specific(size_t bytes, rt_dist_t policy);
void  rt_free(void *p);
/* Put a whole allocation on one given node (used by the latency probe) */
void *rt_malloc_on_node(size_t bytes, int node);
rt_dist_t rt_global_dist(void);
const char *rt_dist_name(rt_dist_t d);

/* ---------------- tasks ---------------- */
#define RT_MAX_DEPS 40

typedef struct {
    const void *addr;
    size_t bytes;
} rt_dep_t;

typedef void (*rt_task_fn)(void *arg);

/* Create a task. deps may be NULL when ndeps == 0.
 * arg is copied (arg_bytes) into the task, so it can live on the stack. */
void rt_task(rt_task_fn fn, const void *arg, size_t arg_bytes,
             const rt_dep_t *deps, int ndeps);
/* Wait for all child tasks of the current task (or of main). */
void rt_taskwait(void);

/* ---------------- runtime ---------------- */
void rt_init(void);      /* start worker threads (one per core)           */
void rt_finalize(void);  /* stop workers, print statistics               */

int rt_num_threads(void);
int rt_num_nodes(void);
int rt_cores_per_node(void);
int rt_thread_id(void);           /* 0 .. threads-1 (main thread = 0)     */
int rt_thread_node(void);
int rt_distance(int a, int b);    /* OS-style distance 10 / 16 / 22       */
const char *rt_sched_name(void);

/* Region of interest: resets gem5 stats at begin, dumps them at end,
 * and measures cycles. Use around the parallel part of a benchmark. */
void rt_roi_begin(void);
void rt_roi_end(void);
uint64_t rt_roi_cycles(void);

/* Ask the OS (or patched gem5) which node a page lives on (-1 = unknown) */
int rt_page_node(const void *addr);
/* Print how the pages of [p, p+bytes) are spread over nodes */
void rt_print_placement(const char *name, const void *p, size_t bytes);

static inline uint64_t rt_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#ifdef __cplusplus
}
#endif
#endif
