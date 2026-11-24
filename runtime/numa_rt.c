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
enum { STEAL_HOPS = 0, STEAL_OS = 1, STEAL_NONE = 2 };
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
    int last_kind;
    trace_t *trace;
    size_t ntrace, trace_cap;
} worker_t;

typedef struct {
    uintptr_t start;
    size_t bytes;
    rt_dist_t pol;
    int node;
} alloc_t;

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

    spin_t alloc_lock;
    alloc_t allocs[MAX_ALLOCS];    /* sorted by start */
    int nallocs;

    uint64_t roi_start, roi_cycles;
    const char *trace_file;
    int started;
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
                                                            : "standard";
}

static rt_dist_t parse_dist(const char *s)
{
    if (!s) return RT_DIST_STANDARD;
    if (!strcmp(s, "fine")) return RT_DIST_FINE;
    if (!strcmp(s, "coarse")) return RT_DIST_COARSE;
    return RT_DIST_STANDARD;
}

static void record_alloc(uintptr_t start, size_t bytes, rt_dist_t pol, int node)
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
    G.allocs[i] = (alloc_t){start, bytes, pol, node};
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

void *rt_malloc_specific(size_t bytes, rt_dist_t pol)
{
    if (!G.nnodes) rt_init();
    size_t sz = (bytes + PAGE - 1) & ~(PAGE - 1);
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
    record_alloc((uintptr_t)p, sz, pol, node);
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
    record_alloc((uintptr_t)p, sz, RT_DIST_COARSE, node % (G.nnodes ? G.nnodes : 1));
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
static int deal_work(worker_t *w, task_t *t)
{
    int n = G.nnodes;
    if (n <= 1 || t->ndeps == 0) return w->node;
    uint64_t D[MAX_NODES];
    footprint(t, D);
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
    default:         return G.hops[i][n];
    }
}

/* Algorithm 2: work-finding for NUMA systems (paper p.3). */
static task_t *find_work_locality(worker_t *w)
{
    int n = w->node;
    task_t *t = q_pop_back(G.nodeq[n]);
    if (t) { w->last_kind = TAKE_OWN; return t; }
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
    uint64_t s = rt_rdtsc();
    t->fn(t->arg);
    if (atomic_load(&t->children) > 0) wait_children(w, t); /* implicit wait */
    uint64_t e = rt_rdtsc();
    w->current = prev;
    w->busy_cycles += e - s;
    w->executed++;
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
    task_t *t = malloc(sizeof(task_t) + dep_bytes + arg_pad);
    t->fn = fn;
    t->parent = w->current ? w->current : &G.root;
    atomic_init(&t->children, 0);
    t->id = atomic_fetch_add(&G.next_id, 1);
    t->ndeps = ndeps;
    t->deps = (rt_dep_t *)t->data;
    t->arg = t->data + dep_bytes;
    if (ndeps) memcpy(t->deps, deps, dep_bytes);
    if (arg_bytes) memcpy(t->arg, arg, arg_bytes);
    atomic_fetch_add(&t->parent->children, 1);

    if (G.sched == SCHED_LOCALITY) {
        int l = deal_work(w, t);
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
                 : (s && !strcmp(s, "none")) ? STEAL_NONE : STEAL_HOPS;
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
           G.steal_mode == STEAL_OS ? "os" : G.steal_mode == STEAL_NONE ? "none" : "hops",
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
    for (int i = 0; i < G.nthreads; i++) {
        worker_t *w = &G.workers[i];
        ex += w->executed; own += w->own; ls += w->local_steals;
        rs += w->remote_steals; dl += w->dealt_local; dr += w->dealt_remote;
        da += w->dealt_analysed; busy += w->busy_cycles;
    }
    printf("numa_rt stats: tasks=%lu own=%lu local_steals=%lu remote_steals=%lu"
           " | dealt_local=%lu dealt_remote=%lu analysed=%lu"
           " | busy_cycles=%lu roi_cycles=%lu\n",
           ex, own, ls, rs, dl, dr, da, busy, G.roi_cycles);
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
