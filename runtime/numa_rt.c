/*
 * numa_rt.c : task runtime with NUMA data distribution and
 *             locality-aware scheduling (see numa_rt.h).
 */
#define _GNU_SOURCE
#include "numa_rt.h"

#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAX_NODES   64
#define MAX_THREADS 256
#define MAX_ALLOCS  4096
#define PAGE        4096UL
#define CACHELINE   64
#define MAX_SPIN    256          /* pause iterations before sleeping */

/* Linux mempolicy values (same numbers the gem5 patch understands) */
#define MPOL_DEFAULT     0
#define MPOL_BIND        2
#define MPOL_INTERLEAVE  3
#define MPOL_F_NODE      1
#define MPOL_F_ADDR      2

enum { SCHED_WS = 0, SCHED_LOCALITY = 1 };
enum { STEAL_HOPS = 0, STEAL_OS = 1, STEAL_NONE = 2, STEAL_COST = 3 };
enum { TAKE_OWN = 0, TAKE_LOCAL_STEAL = 1, TAKE_REMOTE_STEAL = 2 };

/* ------------------------------------------------------------ gem5 ops */
#define M5OP(func) \
    __asm__ volatile(".byte 0x0F, 0x04\n\t.word " #func \
                     : : "D"(0UL), "S"(0UL) : "rax", "memory")

/* ------------------------------------------------------------ spinlock */
typedef struct { atomic_int v; } spin_t;

static inline void cpu_pause(void) { __builtin_ia32_pause(); }

static inline void spin_lock(spin_t *l)
{
    for (;;) {
        if (!atomic_exchange_explicit(&l->v, 1, memory_order_acquire))
            return;
        while (atomic_load_explicit(&l->v, memory_order_relaxed))
            cpu_pause();
    }
}
static inline void spin_unlock(spin_t *l)
{
    atomic_store_explicit(&l->v, 0, memory_order_release);
}

/* ------------------------------------------------------------ tasks */
typedef struct task_s {
    rt_task_fn fn;
    struct task_s *parent;
    atomic_int children;          /* children not yet finished          */
    struct task_s *prev, *next;   /* queue links                        */
    uint64_t id;
    int ndeps;
    int queue_node;               /* node / core queue it was put in    */
    rt_dep_t *deps;
    void *arg;
    uint64_t *hopcost;            /* cost mode: sum_j lines_j x hops[a][j] */
    uint64_t est;                 /* cost mode: lines x local latency     */
    _Alignas(16) unsigned char data[]; /* deps then arg                 */
} task_t;

/* doubly linked queue with a lock; one cache line per queue header */
typedef struct {
    _Alignas(CACHELINE) spin_t lock;
    task_t *head, *tail;
    atomic_int size;
} queue_t;

static void q_push_front(queue_t *q, task_t *t)
{
    spin_lock(&q->lock);
    t->prev = NULL;
    t->next = q->head;
    if (q->head) q->head->prev = t; else q->tail = t;
    q->head = t;
    atomic_fetch_add_explicit(&q->size, 1, memory_order_relaxed);
    spin_unlock(&q->lock);
}

static task_t *q_pop_front(queue_t *q)
{
    if (atomic_load_explicit(&q->size, memory_order_relaxed) == 0)
        return NULL;
    spin_lock(&q->lock);
    task_t *t = q->head;
    if (t) {
        q->head = t->next;
        if (q->head) q->head->prev = NULL; else q->tail = NULL;
        atomic_fetch_sub_explicit(&q->size, 1, memory_order_relaxed);
    }
    spin_unlock(&q->lock);
    return t;
}

static task_t *q_pop_back(queue_t *q)
{
    if (atomic_load_explicit(&q->size, memory_order_relaxed) == 0)
        return NULL;
    spin_lock(&q->lock);
    task_t *t = q->tail;
    if (t) {
        q->tail = t->prev;
        if (q->tail) q->tail->next = NULL; else q->head = NULL;
        atomic_fetch_sub_explicit(&q->size, 1, memory_order_relaxed);
    }
    spin_unlock(&q->lock);
    return t;
}

/* ------------------------------------------------------------ workers */
typedef struct {
    uint64_t task, start, end;
    int queue_node;
    int kind;
} trace_t;

typedef struct {
    _Alignas(CACHELINE) int id, node, cpu;
    task_t *current;
    unsigned rr;
    uint64_t executed, own, local_steals, remote_steals;
    uint64_t dealt_local, dealt_remote, dealt_analysed;
    uint64_t busy_cycles;
    uint64_t cost_accepted, cost_rejected, cost_fallback;
    int last_kind;
    trace_t *trace;
    size_t ntrace, trace_cap;
} worker_t;

typedef struct {
    uintptr_t start;
    size_t bytes;
    rt_dist_t pol;
    int node;
    int idx;          /* main-program allocation number, -1 if made in a task */
} alloc_t;

/* Improvement 2: what tasks did with each main-program allocation */
typedef struct {
    size_t bytes;
    uint64_t ntasks;        /* tasks that listed it in their depend list */
    uint64_t touched;       /* bytes of it those tasks listed, summed    */
} prof_t;

static struct {
    int nthreads, nnodes, cpn;
    int sched;
    rt_dist_t dist;
    int steal_mode;
    size_t llc_bytes;
    int gem5;
    int hops[MAX_NODES][MAX_NODES];
    int dist_tab[MAX_NODES][MAX_NODES];
    int steal_order[MAX_NODES][MAX_NODES]; /* other nodes, nearest first */

    queue_t *nodeq[MAX_NODES];     /* locality-aware: one per node  */
    queue_t *coreq[MAX_THREADS];   /* work-stealing: one per core   */

    worker_t workers[MAX_THREADS];
    pthread_t threads[MAX_THREADS];
    task_t root;

    atomic_int stop;
    atomic_int epoch;              /* futex word for sleeping workers */
    atomic_int sleepers;
    atomic_uint_least64_t next_id;
    atomic_int coarse_next;
    atomic_int running[MAX_NODES]; /* tasks running now, per node        */

    spin_t alloc_lock;
    alloc_t allocs[MAX_ALLOCS];    /* sorted by start */
    int nallocs;

    uint64_t roi_start, roi_cycles;
    const char *trace_file;
    int started;

    /* Improvement 1: cost-aware stealing */
    uint64_t lat_local, lat_hop;
    atomic_uint_least64_t task_cycles, task_count;

    /* Improvement 2: adaptive distribution */
    int next_idx;                  /* main-program allocations so far   */
    const char *prof_out;
    prof_t prof[MAX_ALLOCS];       /* filled when profiling             */
    prof_t prof_in[MAX_ALLOCS];    /* read from RT_PROFILE_IN           */
    int nprof_in;
    int ad_fine, ad_coarse, ad_size_rule, ad_profile_rule, ad_no_evidence;
} G;

static __thread worker_t *me;

/* ------------------------------------------------------------ syscalls */
static long sys_mbind(void *a, unsigned long len, int mode,
                      const unsigned long *mask, unsigned long maxnode,
                      unsigned flags)
{
    return syscall(SYS_mbind, a, len, mode, mask, maxnode, flags);
}

static void futex_wait(atomic_int *addr, int val)
{
    syscall(SYS_futex, (int *)addr, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0);
}

static void futex_wake_all(atomic_int *addr)
{
    syscall(SYS_futex, (int *)addr, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
}

static void wake_sleepers(void)
{
    if (atomic_load(&G.sleepers) > 0) {
        atomic_fetch_add(&G.epoch, 1);
        futex_wake_all(&G.epoch);
    }
}

/* ------------------------------------------------------------ helpers */
static int env_int(const char *name, int def)
{
    const char *s = getenv(name);
    return (s && *s) ? atoi(s) : def;
}

static void *page_alloc_on_node(size_t bytes, int node)
{
    size_t sz = (bytes + PAGE - 1) & ~(PAGE - 1);
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    if (node >= 0 && G.nnodes > 1) {
        unsigned long mask = 1UL << node;
        sys_mbind(p, sz, MPOL_BIND, &mask, G.nnodes + 1, 0);
    }
    memset(p, 0, sz); /* touch now, so pages land where we asked */
    return p;
}

/* Router links: the two dies of a socket, plus all even dies and all odd
 * dies. Must match gem5-config/numa_machine.py. */
static int linked(int a, int b)
{
    return (a / 2 == b / 2) || ((a % 2) == (b % 2));
}

static void build_topology(void)
{
    int n = G.nnodes;
    for (int s = 0; s < n; s++) {
        int dist[MAX_NODES], q[MAX_NODES], qh = 0, qt = 0;
        for (int j = 0; j < n; j++) dist[j] = -1;
        dist[s] = 0; q[qt++] = s;
        while (qh < qt) {
            int u = q[qh++];
            for (int v = 0; v < n; v++)
                if (v != u && dist[v] < 0 && linked(u, v)) {
                    dist[v] = dist[u] + 1;
                    q[qt++] = v;
                }
        }
        for (int j = 0; j < n; j++) {
            G.hops[s][j] = dist[j];
            G.dist_tab[s][j] = dist[j] == 0 ? 10 : 10 + 6 * dist[j];
        }
    }
    /* optional override: NUMA_DISTANCE="10,16,22,..." (n*n values) */
    const char *s = getenv("NUMA_DISTANCE");
    if (s && *s) {
        for (int i = 0; i < n * n && *s; i++) {
            G.dist_tab[i / n][i % n] = atoi(s);
            G.hops[i / n][i % n] = (G.dist_tab[i / n][i % n] - 10 + 5) / 6;
            while (*s && *s != ',') s++;
            if (*s == ',') s++;
        }
    }
    /* steal order for Algorithm 2: other nodes sorted by distance */
    for (int a = 0; a < n; a++) {
        int k = 0;
        for (int d = 1; d <= 64 && k < n - 1; d++)
            for (int off = 1; off < n; off++) {
                int b = (a + off) % n;
                if (G.hops[a][b] == d) G.steal_order[a][k++] = b;
            }
    }
}

/* ------------------------------------------------------------ allocation */
rt_dist_t rt_global_dist(void) { return G.dist; }

const char *rt_dist_name(rt_dist_t d)
{
    return d == RT_DIST_FINE ? "fine" : d == RT_DIST_COARSE ? "coarse"
         : d == RT_DIST_ADAPTIVE ? "adaptive" : "standard";
}

static rt_dist_t parse_dist(const char *s)
{
    if (!s) return RT_DIST_STANDARD;
    if (!strcmp(s, "fine")) return RT_DIST_FINE;
    if (!strcmp(s, "coarse")) return RT_DIST_COARSE;
    if (!strcmp(s, "adaptive")) return RT_DIST_ADAPTIVE;
    return RT_DIST_STANDARD;
}

static void record_alloc(uintptr_t start, size_t bytes, rt_dist_t pol, int node,
                         int idx)
{
    spin_lock(&G.alloc_lock);
    if (G.nallocs >= MAX_ALLOCS) {
        fprintf(stderr, "numa_rt: too many allocations\n");
        exit(1);
    }
    int i = G.nallocs++;
    while (i > 0 && G.allocs[i - 1].start > start) {
        G.allocs[i] = G.allocs[i - 1];
        i--;
    }
    G.allocs[i] = (alloc_t){start, bytes, pol, node, idx};
    if (idx >= 0 && idx < MAX_ALLOCS) G.prof[idx].bytes = bytes;
    spin_unlock(&G.alloc_lock);
}

/* caller holds alloc_lock */
static const alloc_t *find_alloc(uintptr_t a)
{
    int lo = 0, hi = G.nallocs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const alloc_t *x = &G.allocs[mid];
        if (a < x->start) hi = mid - 1;
        else if (a >= x->start + x->bytes) lo = mid + 1;
        else return x;
    }
    return NULL;
}

/* ---------------------------------------------- Improvement 2: adaptive
 * Start from the baseline (fine, the paper's numactl-like default) and
 * switch an allocation to coarse only when there is evidence for it:
 *   size rule    : smaller than one page per node, so fine cannot spread it
 *                  evenly anyway (it would pile on the first few nodes);
 *   profile rule : a profile run (RT_PROFILE_OUT) showed that the tasks
 *                  using it each list at least half of it, i.e. a task works
 *                  on the whole allocation ("one malloc per task" in the
 *                  paper's Table 4), so keeping it on one node lets
 *                  Algorithm 1 send the task to its data.
 * Everything else stays fine ("one malloc, many tasks"). Pages are never
 * moved: the choice is made when the allocation is created. */
static rt_dist_t adaptive_decide(int idx, size_t sz)
{
    if (sz < (size_t)G.nnodes * PAGE) { G.ad_size_rule++; return RT_DIST_COARSE; }
    if (idx >= 0 && idx < G.nprof_in && G.prof_in[idx].bytes == sz &&
        G.prof_in[idx].ntasks > 0) {
        G.ad_profile_rule++;
        double frac = (double)G.prof_in[idx].touched /
                      (double)G.prof_in[idx].ntasks / (double)sz;
        return frac >= 0.5 ? RT_DIST_COARSE : RT_DIST_FINE;
    }
    G.ad_no_evidence++;
    return RT_DIST_FINE;
}

static void load_profile(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "numa_rt: cannot read profile %s\n", path); return; }
    int idx; unsigned long long b, nt, tb;
    while (fscanf(f, "%d %llu %llu %llu", &idx, &b, &nt, &tb) == 4)
        if (idx >= 0 && idx < MAX_ALLOCS) {
            G.prof_in[idx] = (prof_t){(size_t)b, nt, tb};
            if (idx + 1 > G.nprof_in) G.nprof_in = idx + 1;
        }
    fclose(f);
}

/* count, for each main-program allocation, which tasks list it */
static void profile_task(const rt_dep_t *deps, int ndeps)
{
    int seen[RT_MAX_DEPS], nseen = 0;
    spin_lock(&G.alloc_lock);
    for (int k = 0; k < ndeps; k++) {
        uintptr_t a = (uintptr_t)deps[k].addr;
        size_t len = deps[k].bytes;
        while (len > 0) {
            const alloc_t *al = find_alloc(a);
            if (!al) break;
            size_t avail = al->start + al->bytes - a;
            size_t chunk = len < avail ? len : avail;
            if (al->idx >= 0 && al->idx < MAX_ALLOCS) {
                G.prof[al->idx].touched += chunk;
                int dup = 0;
                for (int s = 0; s < nseen; s++) if (seen[s] == al->idx) dup = 1;
                if (!dup) {
                    G.prof[al->idx].ntasks++;
                    if (nseen < RT_MAX_DEPS) seen[nseen++] = al->idx;
                }
            }
            a += chunk;
            len -= chunk;
        }
    }
    spin_unlock(&G.alloc_lock);
}

void *rt_malloc_specific(size_t bytes, rt_dist_t pol)
{
    if (!G.nnodes) rt_init();
    size_t sz = (bytes + PAGE - 1) & ~(PAGE - 1);
    /* main-program allocation (not inside a task): give it a number, so a
     * profile run and the real run can talk about the same allocation */
    int idx = -1;
    if (!me || !me->current) {
        spin_lock(&G.alloc_lock);
        idx = G.next_idx++;
        spin_unlock(&G.alloc_lock);
    }
    if (pol == RT_DIST_ADAPTIVE) {
        spin_lock(&G.alloc_lock);
        pol = adaptive_decide(idx, sz);
        if (pol == RT_DIST_COARSE) G.ad_coarse++; else G.ad_fine++;
        spin_unlock(&G.alloc_lock);
    }
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    int node = -1;
    if (G.nnodes > 1) {
        if (pol == RT_DIST_FINE) {
            unsigned long mask = (G.nnodes >= 64) ? ~0UL : ((1UL << G.nnodes) - 1);
            sys_mbind(p, sz, MPOL_INTERLEAVE, &mask, G.nnodes + 1, 0);
        } else if (pol == RT_DIST_COARSE) {
            node = atomic_fetch_add(&G.coarse_next, 1) % G.nnodes;
            unsigned long mask = 1UL << node;
            sys_mbind(p, sz, MPOL_BIND, &mask, G.nnodes + 1, 0);
        }
    }
    record_alloc((uintptr_t)p, sz, pol, node, idx);
    return p;
}

void *rt_malloc(size_t bytes) { return rt_malloc_specific(bytes, G.dist); }

void *rt_malloc_on_node(size_t bytes, int node)
{
    if (!G.nnodes) rt_init();
    size_t sz = (bytes + PAGE - 1) & ~(PAGE - 1);
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    if (G.nnodes > 1) {
        unsigned long mask = 1UL << (node % G.nnodes);
        sys_mbind(p, sz, MPOL_BIND, &mask, G.nnodes + 1, 0);
    }
    record_alloc((uintptr_t)p, sz, RT_DIST_COARSE, node % (G.nnodes ? G.nnodes : 1), -1);
    return p;
}

void rt_free(void *p)
{
    spin_lock(&G.alloc_lock);
    for (int i = 0; i < G.nallocs; i++)
        if (G.allocs[i].start == (uintptr_t)p) {
            size_t sz = G.allocs[i].bytes;
            memmove(&G.allocs[i], &G.allocs[i + 1],
                    (G.nallocs - i - 1) * sizeof(alloc_t));
            G.nallocs--;
            spin_unlock(&G.alloc_lock);
            sys_mbind(p, sz, MPOL_DEFAULT, NULL, 0, 0); /* forget policy */
            munmap(p, sz);
            return;
        }
    spin_unlock(&G.alloc_lock);
}

int rt_page_node(const void *addr)
{
    int node = -1;
    long r = syscall(SYS_get_mempolicy, &node, NULL, 0, addr,
                     MPOL_F_NODE | MPOL_F_ADDR);
    return r == 0 ? node : -1;
}

void rt_print_placement(const char *name, const void *p, size_t bytes)
{
    long cnt[MAX_NODES] = {0}, unknown = 0;
    for (size_t off = 0; off < bytes; off += PAGE) {
        int n = rt_page_node((const char *)p + off);
        if (n >= 0 && n < MAX_NODES) cnt[n]++; else unknown++;
    }
    printf("placement %-10s:", name);
    for (int i = 0; i < G.nnodes; i++) printf(" n%d=%ld", i, cnt[i]);
    if (unknown) printf(" unknown=%ld", unknown);
    printf("  (pages)\n");
}

/* Bytes of the task footprint on each node, from the allocator's records.
 * Pages of "standard" allocations are unknown to the runtime (first touch),
 * like in the paper, so they are not counted. */
static void footprint(const task_t *t, uint64_t *D)
{
    int n = G.nnodes;
    memset(D, 0, sizeof(uint64_t) * n);
    spin_lock(&G.alloc_lock);
    for (int k = 0; k < t->ndeps; k++) {
        uintptr_t a = (uintptr_t)t->deps[k].addr;
        size_t len = t->deps[k].bytes;
        while (len > 0) {
            const alloc_t *al = find_alloc(a);
            if (!al) break;
            size_t avail = al->start + al->bytes - a;
            size_t chunk = len < avail ? len : avail;
            if (al->pol == RT_DIST_COARSE) {
                D[al->node] += chunk;
            } else if (al->pol == RT_DIST_FINE) {
                uintptr_t x = a, end = a + chunk;
                while (x < end) {
                    uintptr_t off = x - al->start;
                    uintptr_t page_end = al->start + (off / PAGE + 1) * PAGE;
                    uintptr_t e = page_end < end ? page_end : end;
                    D[(off / PAGE) % n] += e - x;
                    x = e;
                }
            }
            a += chunk;
            len -= chunk;
        }
    }
    spin_unlock(&G.alloc_lock);
}

/* ------------------------------------------------------------ scheduling */

/* Algorithm 1: work-dealing for NUMA systems (paper p.3).
 * Returns the node queue that should get task t. */
static int deal_work(worker_t *w, task_t *t, const uint64_t *Dpre)
{
    int n = G.nnodes;
    if (n <= 1 || t->ndeps == 0) return w->node;
    uint64_t D[MAX_NODES];
    if (Dpre) memcpy(D, Dpre, sizeof(uint64_t) * n); else footprint(t, D);
    uint64_t sum = 0;
    int uneven = 0;
    for (int i = 0; i < n; i++) {
        sum += D[i];
        if (D[i] != D[0]) uneven = 1;       /* StandardDeviation(D) > 0 */
    }
    if (!(sum > G.llc_bytes / G.cpn && uneven))
        return w->node;                     /* not worth it: local queue */
    w->dealt_analysed++;
    /* queue with least NUMA-distance-weighted cost to D */
    int best = w->node;
    uint64_t best_cost = UINT64_MAX;
    for (int l = 0; l < n; l++) {
        uint64_t c = 0;
        for (int j = 0; j < n; j++) c += D[j] * (uint64_t)G.dist_tab[l][j];
        if (c < best_cost || (c == best_cost && l == w->node)) {
            best_cost = c;
            best = l;
        }
    }
    return best;
}

static int steal_dist(int i, int n)
{
    switch (G.steal_mode) {
    case STEAL_OS:   return G.dist_tab[i][n];
    case STEAL_NONE: return 0;
    default:         return G.hops[i][n];   /* hops, and cost's fallback */
    }
}

/* ------------------------------------------ Improvement 1: cost-aware steal
 * Computed once per task, when it is created (from the footprint that
 * Algorithm 1 uses anyway):
 *   hopcost[a] = sum over nodes j of (cache lines of its data on j) x hops(a,j)
 *   est        = all its lines x local latency  (task-time guess)
 * Extra cycles of running it on node `thief` instead of node `victim`:
 *   lat_hop x (hopcost[thief] - hopcost[victim])   (92 cycles per hop)
 * Negative when the thief is closer to the data. */
static void cost_prepare(task_t *t, const uint64_t *D)
{
    uint64_t lines_all = 0;
    for (int a = 0; a < G.nnodes; a++) t->hopcost[a] = 0;
    for (int j = 0; j < G.nnodes; j++) {
        if (!D[j]) continue;
        uint64_t lines = (D[j] + CACHELINE - 1) / CACHELINE;
        lines_all += lines;
        for (int a = 0; a < G.nnodes; a++)
            t->hopcost[a] += lines * (uint64_t)G.hops[a][j];
    }
    t->est = lines_all * G.lat_local;
}

/* Steal from node i only when
 *   (1) node i really is busy: more tasks queued than it has free cores
 *       (backlog = queued - idle cores of i), and
 *   (2) the time that backlog takes there (backlog x task time / cores per
 *       node) is larger than the extra memory time the task pays here.
 * The tail task is checked under the victim's lock and only removed if the
 * steal is accepted, so a rejected check never hides work from node i.
 * Task time = measured average once a task has finished, before that the
 * footprint estimate. A task with neither falls back to the hop rule.
 * Unlike the paper's rule, a rejected victim does not stop the search. */
static task_t *find_work_cost(worker_t *w)
{
    int n = w->node;
    uint64_t cnt = atomic_load_explicit(&G.task_count, memory_order_relaxed);
    uint64_t avg = cnt ? atomic_load_explicit(&G.task_cycles,
                                              memory_order_relaxed) / cnt : 0;
    for (int k = 0; k < G.nnodes - 1; k++) {
        int i = G.steal_order[n][k];
        queue_t *q = G.nodeq[i];
        int sz = atomic_load_explicit(&q->size, memory_order_relaxed);
        if (sz <= 0) continue;
        int idle = G.cpn - atomic_load_explicit(&G.running[i], memory_order_relaxed);
        if (idle < 0) idle = 0;
        int backlog = sz - idle;
        if (backlog <= 0) continue;              /* its own cores will take it */
        spin_lock(&q->lock);
        task_t *t = q->tail;
        if (!t) { spin_unlock(&q->lock); continue; }
        uint64_t tt = avg ? avg : t->est;
        int take;
        if (!tt) {
            take = sz > G.hops[i][n] * G.cpn;     /* nothing known: hop rule */
            if (take) w->cost_fallback++;
        } else {
            int64_t pen = (int64_t)G.lat_hop *
                          ((int64_t)t->hopcost[n] - (int64_t)t->hopcost[i]);
            int64_t benefit = (int64_t)((uint64_t)backlog * tt / (uint64_t)G.cpn);
            take = benefit > pen;
            if (take) w->cost_accepted++; else w->cost_rejected++;
        }
        if (take) {
            q->tail = t->prev;
            if (q->tail) q->tail->next = NULL; else q->head = NULL;
            atomic_fetch_sub_explicit(&q->size, 1, memory_order_relaxed);
        }
        spin_unlock(&q->lock);
        if (take) { w->last_kind = TAKE_REMOTE_STEAL; return t; }
    }
    return NULL;
}

/* Algorithm 2: work-finding for NUMA systems (paper p.3). */
static task_t *find_work_locality(worker_t *w)
{
    int n = w->node;
    task_t *t = q_pop_back(G.nodeq[n]);
    if (t) { w->last_kind = TAKE_OWN; return t; }
    if (G.steal_mode == STEAL_COST) return find_work_cost(w);
    for (int k = 0; k < G.nnodes - 1; k++) {
        int i = G.steal_order[n][k];
        int sz = atomic_load_explicit(&G.nodeq[i]->size, memory_order_relaxed);
        if (sz > steal_dist(i, n) * G.cpn) {
            t = q_pop_back(G.nodeq[i]);
            if (t) { w->last_kind = TAKE_REMOTE_STEAL; return t; }
            break;  /* "Run dequeue(Qi); break" */
        }
    }
    return NULL;
}

/* Work-stealing baseline: own deque first (LIFO), then victims round-robin. */
static task_t *find_work_ws(worker_t *w)
{
    task_t *t = q_pop_front(G.coreq[w->id]);
    if (t) { w->last_kind = TAKE_OWN; return t; }
    int T = G.nthreads;
    for (int k = 0; k < T - 1; k++) {
        int v = (w->id + 1 + (w->rr + k) % (T - 1)) % T;
        t = q_pop_back(G.coreq[v]);
        if (t) {
            w->rr = (w->rr + k + 1) % (T - 1);
            w->last_kind = (G.workers[v].node == w->node) ? TAKE_LOCAL_STEAL
                                                          : TAKE_REMOTE_STEAL;
            return t;
        }
    }
    return NULL;
}

static task_t *find_work(worker_t *w)
{
    return G.sched == SCHED_LOCALITY ? find_work_locality(w) : find_work_ws(w);
}

static void wait_children(worker_t *w, task_t *cur);

static void run_task(worker_t *w, task_t *t)
{
    task_t *prev = w->current;
    w->current = t;
    int kind = w->last_kind;
    atomic_fetch_add_explicit(&G.running[w->node], 1, memory_order_relaxed);
    uint64_t s = rt_rdtsc();
    t->fn(t->arg);
    if (atomic_load(&t->children) > 0) wait_children(w, t); /* implicit wait */
    uint64_t e = rt_rdtsc();
    atomic_fetch_sub_explicit(&G.running[w->node], 1, memory_order_relaxed);
    w->current = prev;
    w->busy_cycles += e - s;
    w->executed++;
    if (G.steal_mode == STEAL_COST) {     /* average task time for the cost rule */
        atomic_fetch_add_explicit(&G.task_cycles, e - s, memory_order_relaxed);
        /* first measurement: wake idle workers so they re-check with it */
        if (atomic_fetch_add_explicit(&G.task_count, 1, memory_order_relaxed) == 0)
            wake_sleepers();
    }
    if (kind == TAKE_OWN) w->own++;
    else if (kind == TAKE_LOCAL_STEAL) w->local_steals++;
    else w->remote_steals++;
    if (w->trace && w->ntrace < w->trace_cap)
        w->trace[w->ntrace++] = (trace_t){t->id, s, e, t->queue_node, kind};
    task_t *p = t->parent;
    free(t);
    if (atomic_fetch_sub(&p->children, 1) == 1)
        wake_sleepers();  /* someone may be sleeping in rt_taskwait */
}

static void idle_pause(int n)
{
    for (int i = 0; i < n; i++) cpu_pause();
}

/* Execute other tasks until `cur` has no running children. */
static void wait_children(worker_t *w, task_t *cur)
{
    int backoff = 1;
    while (atomic_load(&cur->children) > 0) {
        task_t *t = find_work(w);
        if (t) { run_task(w, t); backoff = 1; continue; }
        if (backoff < MAX_SPIN) { idle_pause(backoff); backoff *= 2; continue; }
        int e = atomic_load(&G.epoch);
        atomic_fetch_add(&G.sleepers, 1);
        if (atomic_load(&cur->children) > 0) {
            t = find_work(w);
            if (t) {
                atomic_fetch_sub(&G.sleepers, 1);
                run_task(w, t);
                backoff = 1;
                continue;
            }
            futex_wait(&G.epoch, e);
        }
        atomic_fetch_sub(&G.sleepers, 1);
        backoff = 1;
    }
}

void rt_taskwait(void)
{
    if (!G.started) return;
    worker_t *w = me;
    wait_children(w, w->current ? w->current : &G.root);
}

void rt_task(rt_task_fn fn, const void *arg, size_t arg_bytes,
             const rt_dep_t *deps, int ndeps)
{
    if (!G.started) rt_init();
    worker_t *w = me;
    if (ndeps > RT_MAX_DEPS) ndeps = RT_MAX_DEPS;
    size_t dep_bytes = sizeof(rt_dep_t) * (size_t)ndeps;
    size_t arg_pad = (arg_bytes + 15) & ~15UL;
    int cost = (G.sched == SCHED_LOCALITY && G.steal_mode == STEAL_COST);
    size_t hc_bytes = cost ? sizeof(uint64_t) * (size_t)G.nnodes : 0;
    task_t *t = malloc(sizeof(task_t) + dep_bytes + arg_pad + hc_bytes);
    t->fn = fn;
    t->parent = w->current ? w->current : &G.root;
    atomic_init(&t->children, 0);
    t->id = atomic_fetch_add(&G.next_id, 1);
    t->ndeps = ndeps;
    t->deps = (rt_dep_t *)t->data;
    t->arg = t->data + dep_bytes;
    t->hopcost = cost ? (uint64_t *)(t->data + dep_bytes + arg_pad) : NULL;
    t->est = 0;
    if (ndeps) memcpy(t->deps, deps, dep_bytes);
    if (arg_bytes) memcpy(t->arg, arg, arg_bytes);
    if (G.prof_out && ndeps) profile_task(t->deps, ndeps);
    atomic_fetch_add(&t->parent->children, 1);

    if (G.sched == SCHED_LOCALITY) {
        int l;
        if (cost) {                /* one footprint for both algorithms */
            uint64_t D[MAX_NODES];
            footprint(t, D);
            cost_prepare(t, D);
            l = deal_work(w, t, D);
        } else {
            l = deal_work(w, t, NULL);
        }
        if (l == w->node) w->dealt_local++; else w->dealt_remote++;
        t->queue_node = l;
        q_push_front(G.nodeq[l], t);
    } else {
        t->queue_node = w->node;
        q_push_front(G.coreq[w->id], t);
    }
    wake_sleepers();
}

/* ------------------------------------------------------------ threads */
static void bind_worker(worker_t *w)
{
    if (G.gem5) {
        /* gem5 gives the k-th thread core k; patched getcpu gives its node */
        unsigned cpu = 0, node = 0;
        syscall(SYS_getcpu, &cpu, &node, NULL);
        w->cpu = cpu;
        w->node = (G.nnodes > 1) ? (int)node : 0;
        if ((int)cpu != w->id)
            fprintf(stderr, "numa_rt: warning: worker %d runs on cpu %u\n",
                    w->id, cpu);
    } else {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(w->id % (ncpu > 0 ? ncpu : 1), &set);
        sched_setaffinity(0, sizeof(set), &set);
        w->cpu = w->id;
        w->node = w->id / G.cpn;   /* logical node when not in gem5 */
    }
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    me = w;
    bind_worker(w);
    int backoff = 1;
    while (!atomic_load(&G.stop)) {
        task_t *t = find_work(w);
        if (t) { run_task(w, t); backoff = 1; continue; }
        if (backoff < MAX_SPIN) { idle_pause(backoff); backoff *= 2; continue; }
        int e = atomic_load(&G.epoch);
        atomic_fetch_add(&G.sleepers, 1);
        t = find_work(w);
        if (t) {
            atomic_fetch_sub(&G.sleepers, 1);
            run_task(w, t);
            backoff = 1;
            continue;
        }
        if (!atomic_load(&G.stop)) futex_wait(&G.epoch, e);
        atomic_fetch_sub(&G.sleepers, 1);
        backoff = 1;
    }
    return NULL;
}

void rt_init(void)
{
    if (G.started) return;
    G.gem5 = env_int("NUMA_GEM5", 0);
    G.nnodes = env_int("NUMA_NODES", 1);
    G.cpn = env_int("NUMA_CORES_PER_NODE", 0);
    if (G.cpn <= 0) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        G.cpn = (int)((ncpu > 0 ? ncpu : 1) / G.nnodes);
        if (G.cpn < 1) G.cpn = 1;
    }
    if (G.nnodes > MAX_NODES) G.nnodes = MAX_NODES;
    G.nthreads = env_int("RT_THREADS", G.nnodes * G.cpn);
    if (G.nthreads > MAX_THREADS) G.nthreads = MAX_THREADS;
    G.dist = parse_dist(getenv("OMP_DATA_DISTRIBUTION"));
    const char *s = getenv("RT_SCHED");
    G.sched = (s && !strcmp(s, "locality")) ? SCHED_LOCALITY : SCHED_WS;
    s = getenv("RT_STEAL_DIST");
    G.steal_mode = (s && !strcmp(s, "os")) ? STEAL_OS
                 : (s && !strcmp(s, "none")) ? STEAL_NONE
                 : (s && !strcmp(s, "cost")) ? STEAL_COST : STEAL_HOPS;
    G.lat_local = (uint64_t)env_int("RT_LAT_LOCAL", 151);
    G.lat_hop = (uint64_t)env_int("RT_LAT_HOP", 92);
    G.prof_out = getenv("RT_PROFILE_OUT");
    if (G.prof_out && !*G.prof_out) G.prof_out = NULL;
    s = getenv("RT_PROFILE_IN");
    if (s && *s) load_profile(s);
    G.llc_bytes = (size_t)env_int("RT_LLC_BYTES", 6 * 64 * 1024);
    G.trace_file = getenv("RT_TRACE");
    build_topology();

    /* NUMA_INTERLEAVE_ALL=1 is "numactl --interleave=all": every page of the
     * process (heap, stacks, task records) is interleaved over all nodes.
     * This is the paper's Figure 7 baseline. */
    if (env_int("NUMA_INTERLEAVE_ALL", 0) && G.nnodes > 1) {
        unsigned long mask = (G.nnodes >= 64) ? ~0UL : ((1UL << G.nnodes) - 1);
        syscall(SYS_set_mempolicy, MPOL_INTERLEAVE, &mask, G.nnodes + 1);
    }

    /* queues live on their own node */
    for (int nd = 0; nd < G.nnodes; nd++) {
        G.nodeq[nd] = page_alloc_on_node(sizeof(queue_t), nd);
        int first = nd * G.cpn, cnt = 0;
        for (int c = first; c < G.nthreads && c < first + G.cpn; c++) cnt++;
        if (cnt) {
            queue_t *qs = page_alloc_on_node(sizeof(queue_t) * cnt, nd);
            for (int k = 0; k < cnt; k++) G.coreq[first + k] = &qs[k];
        }
    }
    for (int c = G.nnodes * G.cpn; c < G.nthreads; c++)  /* leftovers */
        G.coreq[c] = page_alloc_on_node(sizeof(queue_t), c % G.nnodes);

    for (int i = 0; i < G.nthreads; i++) {
        worker_t *w = &G.workers[i];
        w->id = i;
        if (G.trace_file) {
            w->trace_cap = 1 << 16;
            w->trace = malloc(sizeof(trace_t) * w->trace_cap);
        }
    }
    atomic_init(&G.root.children, 0);
    G.started = 1;

    me = &G.workers[0];
    bind_worker(me);
    for (int i = 1; i < G.nthreads; i++)
        pthread_create(&G.threads[i], NULL, worker_main, &G.workers[i]);

    printf("numa_rt: %d threads, %d nodes x %d cores, sched=%s, dist=%s, "
           "LLC/node=%zu KiB, steal-distance=%s%s\n",
           G.nthreads, G.nnodes, G.cpn, rt_sched_name(), rt_dist_name(G.dist),
           G.llc_bytes / 1024,
           G.steal_mode == STEAL_OS ? "os" : G.steal_mode == STEAL_NONE ? "none"
           : G.steal_mode == STEAL_COST ? "cost" : "hops",
           G.gem5 ? ", gem5" : "");
}

void rt_finalize(void)
{
    if (!G.started) return;
    atomic_store(&G.stop, 1);
    atomic_fetch_add(&G.epoch, 1);
    futex_wake_all(&G.epoch);
    for (int i = 1; i < G.nthreads; i++) pthread_join(G.threads[i], NULL);

    uint64_t ex = 0, own = 0, ls = 0, rs = 0, dl = 0, dr = 0, da = 0, busy = 0;
    uint64_t ca = 0, cr = 0, cf = 0;
    for (int i = 0; i < G.nthreads; i++) {
        worker_t *w = &G.workers[i];
        ex += w->executed; own += w->own; ls += w->local_steals;
        rs += w->remote_steals; dl += w->dealt_local; dr += w->dealt_remote;
        da += w->dealt_analysed; busy += w->busy_cycles;
        ca += w->cost_accepted; cr += w->cost_rejected; cf += w->cost_fallback;
    }
    printf("numa_rt stats: tasks=%lu own=%lu local_steals=%lu remote_steals=%lu"
           " | dealt_local=%lu dealt_remote=%lu analysed=%lu"
           " | busy_cycles=%lu roi_cycles=%lu\n",
           ex, own, ls, rs, dl, dr, da, busy, G.roi_cycles);
    if (G.steal_mode == STEAL_COST)
        printf("cost steals: accepted=%lu rejected=%lu fallback=%lu "
               "avg_task_cycles=%lu\n", ca, cr, cf,
               G.task_count ? (unsigned long)(G.task_cycles / G.task_count) : 0UL);
    if (G.dist == RT_DIST_ADAPTIVE)
        printf("adaptive: fine=%d coarse=%d | size_rule=%d profile_rule=%d "
               "no_evidence=%d | profile_entries=%d\n", G.ad_fine, G.ad_coarse,
               G.ad_size_rule, G.ad_profile_rule, G.ad_no_evidence, G.nprof_in);
    if (G.prof_out) {
        FILE *f = fopen(G.prof_out, "w");
        if (f) {
            for (int i = 0; i < G.next_idx && i < MAX_ALLOCS; i++)
                fprintf(f, "%d %lu %lu %lu\n", i, (unsigned long)G.prof[i].bytes,
                        (unsigned long)G.prof[i].ntasks,
                        (unsigned long)G.prof[i].touched);
            fclose(f);
            printf("profile: %d allocations written to %s\n", G.next_idx, G.prof_out);
        }
    }
    printf("tasks per node:");
    for (int nd = 0; nd < G.nnodes; nd++) {
        uint64_t c = 0;
        for (int i = 0; i < G.nthreads; i++)
            if (G.workers[i].node == nd) c += G.workers[i].executed;
        printf(" n%d=%lu", nd, c);
    }
    printf("\n");

    if (G.trace_file) {
        FILE *f = fopen(G.trace_file, "w");
        if (f) {
            fprintf(f, "thread,node,task,start,end,queue_node,kind\n");
            for (int i = 0; i < G.nthreads; i++) {
                worker_t *w = &G.workers[i];
                for (size_t k = 0; k < w->ntrace; k++) {
                    trace_t *r = &w->trace[k];
                    fprintf(f, "%d,%d,%lu,%lu,%lu,%d,%s\n", i, w->node,
                            r->task, r->start, r->end, r->queue_node,
                            r->kind == TAKE_OWN ? "own"
                            : r->kind == TAKE_LOCAL_STEAL ? "local_steal"
                                                          : "remote_steal");
                }
            }
            fclose(f);
        }
    }
    fflush(stdout);
}

/* ------------------------------------------------------------ info */
int rt_num_threads(void) { return G.nthreads; }
int rt_num_nodes(void) { return G.nnodes; }
int rt_cores_per_node(void) { return G.cpn; }
int rt_thread_id(void) { return me ? me->id : 0; }
int rt_thread_node(void) { return me ? me->node : 0; }
int rt_distance(int a, int b) { return G.dist_tab[a][b]; }
const char *rt_sched_name(void)
{
    return G.sched == SCHED_LOCALITY ? "locality" : "ws";
}

void rt_roi_begin(void)
{
    if (G.gem5) M5OP(0x40);          /* m5_reset_stats */
    G.roi_start = rt_rdtsc();
}

void rt_roi_end(void)
{
    G.roi_cycles = rt_rdtsc() - G.roi_start;
    if (G.gem5) M5OP(0x41);          /* m5_dump_stats */
    printf("ROI cycles: %lu\n", G.roi_cycles);
}

uint64_t rt_roi_cycles(void) { return G.roi_cycles; }
