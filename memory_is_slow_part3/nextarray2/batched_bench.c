/*
 * Batched single-consumer driver for nextarray with a prefetch helper.
 *
 * The consumer (cpu 0) resolves each operation's extent `window` operations
 * ahead of executing it.  With --prefetch helper a thread on cpu 0's
 * hyperthread sibling pulls those extents into the L2 the two threads share,
 * so by the time the consumer's binary search and memmove run, the extent is
 * warm.  --prefetch self has the consumer issue the prefetches itself;
 * --prefetch none is the same batched loop with no prefetching, the baseline.
 */
#include "nextarray.h"
#include "benchmark_helpers.h"
#include "gen_nodes.h"

#include <getopt.h>
#include <immintrin.h>
#include <x86intrin.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_EXTENT_SIZE 512
#define DEFAULT_OVERPROVISIONING 2.0
#define DEFAULT_WINDOW 128          // operations resolved (and prefetched) ahead of execution

enum { PF_NONE, PF_SELF, PF_HELPER, PF_SEARCH, PF_PIPELINE, PF_WARM };
#define MAX_GROUP 64

char* g_inputfile = NULL;
char* g_listfile = NULL;
uint64_t g_extent_size = DEFAULT_EXTENT_SIZE;
double g_overprovisioning = DEFAULT_OVERPROVISIONING;
uint64_t g_count = 0;
int g_window = DEFAULT_WINDOW;
int g_prefetch = PF_HELPER;
int g_helper_cpu = -1;              // default: cpu 0's hyperthread sibling
size_t g_prefetch_bytes = 0;        // 0 = whole extent
int g_hint = _MM_HINT_T1;           // prefetch hint: t0 | t1 | t2 | nta
size_t g_stride = 64;               // bytes between touched lines (128: let the adjacent-line prefetcher do pairs)
int g_load = 0;                     // helper uses demand loads instead of prefetch instructions
int g_group = 8;                    // search mode: operations resolved together so their misses overlap
int g_helpers = 1;                  // search mode: helper threads; command i goes to helper i % g_helpers
int g_warm = 0;                     // search mode: ops ahead at which the sibling warmer pulls the tail into L2 (0 = off)
int g_selfwarm = 0;                 // search mode: consumer prefetches the tail of the op this many ahead itself (0 = off)
int g_tail_ahead = 4;               // pipeline mode: tails prefetched this many ops ahead inside the group
size_t g_evict_kib = 0;             // warm mode: helper streams this much junk after each group to push its L2 into L3
int g_warm_index_only = 0;          // warm mode: helpers walk only the max index, not the extents
#define MAX_HELPERS 8
static volatile long g_sink;
int debug = 0;
int verbose = 0;

/* ---- cpu helpers --------------------------------------------------------- */

static int ht_sibling(int cpu) {
    char path[128];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int a = -1, b = -1;
    int n = fscanf(f, "%d,%d", &a, &b);
    fclose(f);
    return n == 2 ? (a == cpu ? b : a) : -1;
}

static void pin(pthread_t tid, int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(tid, sizeof set, &set) != 0) perror("pin");
}

/* ---- prefetch ring: consumer -> helper, single producer / single consumer -- */

typedef struct {
    const char* p;
    uint32_t bytes;
} pf_req_t;

typedef struct {
    _Alignas(64) _Atomic uint32_t tail;     // consumer writes
    _Atomic bool stop;
    _Alignas(64) _Atomic uint32_t head;     // helper writes (how far it has consumed)
    _Alignas(64) pf_req_t* req;             // [cap]
    uint32_t cap;
    uint32_t count;                         // consumer private: requests written
    uint64_t dropped;                       // consumer private: ring was full
    pthread_t tid;
} pf_ring_t;

// _mm_prefetch needs a constant hint, so switch once per range.
static inline void prefetch_range(const char* p, size_t bytes) {
    size_t st = g_stride;
    if (g_load) {   // demand loads: independent, and an ascending pattern the L2 streamer can pick up
        long acc = 0;
        for (size_t o = 0; o < bytes; o += st) acc += *(const volatile long*)(p + o);
        g_sink = acc;
        return;
    }
    switch (g_hint) {
    case _MM_HINT_T0:  for (size_t o = 0; o < bytes; o += st) _mm_prefetch(p + o, _MM_HINT_T0);  break;
    case _MM_HINT_T2:  for (size_t o = 0; o < bytes; o += st) _mm_prefetch(p + o, _MM_HINT_T2);  break;
    case _MM_HINT_NTA: for (size_t o = 0; o < bytes; o += st) _mm_prefetch(p + o, _MM_HINT_NTA); break;
    default:           for (size_t o = 0; o < bytes; o += st) _mm_prefetch(p + o, _MM_HINT_T1);  break;
    }
}

// Pull every line of each requested range toward the consumer.  T1 fills L2
// and leaves the small shared L1 alone; NTA is worth trying from a separate
// core, where the goal is to land lines in L3 rather than that core's L2.
static void* helper_main(void* arg) {
    pf_ring_t* r = arg;
    uint32_t head = 0;
    for (;;) {
        uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
        if (head == tail) {
            if (atomic_load_explicit(&r->stop, memory_order_acquire)) break;
            __builtin_ia32_pause();
            continue;
        }
        while (head != tail) {
            pf_req_t q = r->req[head % r->cap];
            prefetch_range(q.p, q.bytes);
            head++;
        }
        atomic_store_explicit(&r->head, head, memory_order_relaxed);
    }
    return NULL;
}

static void pf_start(pf_ring_t* r, int cpu) {
    memset(r, 0, sizeof *r);
    r->cap = 8u * (uint32_t)g_window;
    r->req = aligned_alloc(64, (size_t)r->cap * sizeof(pf_req_t));
    pthread_create(&r->tid, NULL, helper_main, r);
    pin(r->tid, cpu);
}

static void pf_stop(pf_ring_t* r) {
    atomic_store_explicit(&r->stop, true, memory_order_release);
    pthread_join(r->tid, NULL);
    free(r->req);
}

static inline void pf_issue(pf_ring_t* r, const void* p, size_t bytes) {
    if (r->count - atomic_load_explicit(&r->head, memory_order_relaxed) >= r->cap) { r->dropped++; return; }
    pf_req_t* q = &r->req[r->count % r->cap];
    q->p = p;
    q->bytes = (uint32_t)bytes;
    r->count++;
    atomic_store_explicit(&r->tail, r->count, memory_order_release);
}

/* ---- commands ------------------------------------------------------------ */

enum { OP_SEARCH, OP_INDEX, OP_INSERT, OP_DELETE };

typedef struct {
    int kind;
    void* key;          // search / insert / delete
    long index;         // index
    int extent;         // resolved at queue time
    long local;         // index: offset inside the extent
    size_t pos;         // search mode: lower_bound position inside the extent
    uint64_t gen;       // generation the resolution is valid for
} cmd_t;

static pf_ring_t g_ring;

// Resolve where the operation lands and get its extent on its way into cache.
static void resolve(nextarray_t* na, cmd_t* c) {
    const char* p;
    size_t bytes;
    if (c->kind == OP_INDEX) {
        c->extent = nextarray_locate_index(na, c->index, &c->local);
        c->gen = na->count_gen;
        if (c->extent < 0) return;
        p = (const char*)nextarray_extent_data(na, c->extent, &bytes) + (size_t)c->local * (size_t)na->node_size;
        bytes = (size_t)na->node_size;
    } else {
        c->extent = nextarray_locate(na, c->key);
        c->gen = na->structure_gen;
        p = nextarray_extent_data(na, c->extent, &bytes);
    }
    if (g_prefetch_bytes && bytes > g_prefetch_bytes) bytes = g_prefetch_bytes;
    if (g_prefetch == PF_HELPER) {
        pf_issue(&g_ring, p, bytes);
    } else if (g_prefetch == PF_SELF) {
        prefetch_range(p, bytes);
    }
}

// Execute, trusting the resolved extent unless the structure moved since.
static int execute(nextarray_t* na, cmd_t* c) {
    bool fresh = c->extent >= 0 && c->gen == (c->kind == OP_INDEX ? na->count_gen : na->structure_gen);
    switch (c->kind) {
    case OP_SEARCH: return (fresh ? nextarray_search_in(na, c->extent, c->key) : nextarray_search(na, c->key)) ? 0 : -1;
    case OP_INDEX:  return (fresh ? nextarray_at_in(na, c->extent, c->local) : nextarray_at(na, c->index)) ? 0 : -1;
    case OP_INSERT: return (fresh ? nextarray_insert_in(na, c->extent, c->key) : nextarray_insert(na, c->key)) == 1 ? 0 : -1;
    case OP_DELETE: return fresh ? nextarray_delete_in(na, c->extent, c->key) : nextarray_delete(na, c->key);
    }
    return -1;
}

typedef void (*fill_fn)(cmd_t* c, size_t op, void* ctx);

static void fill_search(cmd_t* c, size_t op, void* ctx) { c->kind = OP_SEARCH; c->key = &((Node*)ctx)[op]; }
static void fill_index(cmd_t* c, size_t op, void* ctx)  { c->kind = OP_INDEX;  c->index = (long)((size_t*)ctx)[op]; }
static void fill_insert(cmd_t* c, size_t op, void* ctx) { c->kind = OP_INSERT; c->key = &((Node*)ctx)[op]; }
static void fill_delete(cmd_t* c, size_t op, void* ctx) { c->kind = OP_DELETE; c->key = &((Node*)ctx)[op]; }

/* ---- search mode: the helper does the index walk and the binary search ---- */
/*
 * The consumer fills commands into a shared ring; the helper resolves them in
 * groups of g_group, one index level or one probe for the whole group before
 * the next, so g_group misses are in flight instead of one.  It then touches
 * just the lines the operation will use (the tail the memmove shifts) and
 * publishes.  The consumer checks the generation counter and the position's
 * neighbours, which are now warm, and executes without searching.
 */
typedef struct {
    _Alignas(64) _Atomic uint32_t tail;     // consumer: commands filled
    _Atomic bool stop;
    _Alignas(64) _Atomic uint32_t done;     // helper: commands resolved
    _Alignas(64) cmd_t* cmds;               // [cap] the ring itself
    uint32_t cap;
    uint64_t fallbacks;                     // consumer: resolutions that were stale or wrong
    pthread_t tid;
} sr_ring_t;

static sr_ring_t g_sr[MAX_HELPERS];
static nextarray_t* g_na;
int g_update_prefetch = 1;          // also prefetch the lookup-tree path and max entry an insert/delete will write
static inline void prefetch_update_path(nextarray_t* na, int e);

// The node size as a constant in the u64 build (which already assumes 8-byte
// nodes: nextarray_cmp and nextarray_lower_bound read them as uint64_t), so
// the resolve's address arithmetic is shifts, not imul and div.
#ifdef NEXTARRAY_U64_KEYS
#define NODE_SIZE(na) ((void)(na), sizeof(uint64_t))
#else
#define NODE_SIZE(na) ((size_t)(na)->node_size)
#endif

// lower_bound over lines that are already in cache or prefetched: the
// directory's samples and maxes (a span of 8, one line) and a resolved
// extent.  nextarray_lower_bound's two prefetches per probe only cost issue
// slots here.
static inline size_t lb(const char* base, size_t n, size_t ns, const void* key, int (*cmp)(const void*, const void*)) {
#ifdef NEXTARRAY_U64_KEYS
    (void)ns; (void)cmp;
    if (n == 0) return 0;
    const uint64_t* a = (const uint64_t*)base;
    const uint64_t* p = a;
    uint64_t k = *(const uint64_t*)key;
    while (n > 1) {
        size_t half = n / 2;
        p = p[half] < k ? p + half : p;
        n -= half;
    }
    return (size_t)(p - a) + (*p < k);
#else
    return nextarray_lower_bound(base, n, ns, key, cmp);
#endif
}

static inline void touch_range(const char* p, size_t bytes) {
    long acc = 0;
    for (size_t o = 0; o < bytes; o += 64) acc += *(const volatile long*)(p + o);
    g_sink = acc;
}

// Which extent each key lands in, one level at a time for the whole group,
// with the next level's line prefetched for every command before any reads
// it: the top level (binary search, small enough to stay cached), then in the
// directory extent its every-64th sample, one line of every-8th samples and
// one line of maxs.
static void group_locate(nextarray_t* na, cmd_t** cs, int n) {
    const size_t ns = NODE_SIZE(na);
    const nextarray_dir_t* d[MAX_GROUP];
    int t[MAX_GROUP], n1[MAX_GROUP];
    size_t g[MAX_GROUP];
    bool active[MAX_GROUP];
    for (int i = 0; i < n; i++) {
        size_t tt = (size_t)nextarray_locate_top(na, cs[i]->key);   // sampled, stays in L1/L2
        t[i] = (int)tt;
        d[i] = na->dirs[tt];
        _mm_prefetch((const char*)&d[i]->n, _MM_HINT_T0);
        _mm_prefetch(nextarray_dir_smax2(d[i], ns), _MM_HINT_T0);
        _mm_prefetch(nextarray_dir_smax2(d[i], ns) + 32 * ns, _MM_HINT_T0);
    }
    for (int i = 0; i < n; i++) {
        n1[i] = (d[i]->n + 7) / 8;
        int n2 = (n1[i] + 7) / 8;
        g[i] = lb(nextarray_dir_smax2(d[i], ns), (size_t)n2, ns, cs[i]->key, na->compare);
        active[i] = g[i] < (size_t)n2;
        if (!active[i]) cs[i]->extent = (t[i] << NA_DIR_BITS) | (d[i]->n - 1);
        else _mm_prefetch(nextarray_dir_smax1(d[i], ns) + (g[i] << 3) * ns, _MM_HINT_T0);
    }
    for (int i = 0; i < n; i++) {
        if (!active[i]) continue;
        size_t lo = g[i] << 3, span = (size_t)n1[i] - lo < 8 ? (size_t)n1[i] - lo : 8;
        g[i] = lo + lb(nextarray_dir_smax1(d[i], ns) + lo * ns, span, ns, cs[i]->key, na->compare);
        _mm_prefetch(nextarray_dir_max(d[i]) + (g[i] << 3) * ns, _MM_HINT_T0);
    }
    for (int i = 0; i < n; i++) {
        if (!active[i]) continue;
        size_t lo = g[i] << 3, span = (size_t)d[i]->n - lo < 8 ? (size_t)d[i]->n - lo : 8;
        size_t s = lo + lb(nextarray_dir_max(d[i]) + lo * ns, span, ns, cs[i]->key, na->compare);
        if (s >= (size_t)d[i]->n) s = (size_t)d[i]->n - 1;
        cs[i]->extent = (t[i] << NA_DIR_BITS) | (int)s;
        // group_search reads the extent's block pointer and count first thing
        _mm_prefetch((const char*)&d[i]->block[s], _MM_HINT_T0);
        _mm_prefetch((const char*)&d[i]->cnt[s], _MM_HINT_T0);
    }
}

// Positional lookups as a group: one count-tree level for all of them per
// round, the next level's line prefetched for the whole group first; the top
// tree's levels, then the directory extent's three.
static void group_locate_index(nextarray_t* na, cmd_t** cs, int n) {
    long rem[MAX_GROUP];
    int node[MAX_GROUP];
    long total = (long)na->total_count;
    for (int i = 0; i < n; i++) {
        long idx = cs[i]->index < 0 ? cs[i]->index + total : cs[i]->index;
        rem[i] = idx; node[i] = (idx < 0 || idx >= total) ? -1 : 0;
    }
    for (int level = 0; level < na->tree_height; level++) {
        for (int i = 0; i < n; i++) if (node[i] >= 0)
            _mm_prefetch((const char*)&na->lookup_tree[na->level_offset[level] + node[i] * LOOKUP_NODE_SIZE], _MM_HINT_T0);
        for (int i = 0; i < n; i++) {
            if (node[i] < 0) continue;
            const long* nd = &na->lookup_tree[na->level_offset[level] + node[i] * LOOKUP_NODE_SIZE];
            int pos = 0;
            while (pos < LOOKUP_NODE_SIZE && nd[pos] <= rem[i]) pos++;
            if (pos == LOOKUP_NODE_SIZE) { node[i] = -1; continue; }
            if (pos > 0) rem[i] -= nd[pos - 1];
            node[i] = node[i] * LOOKUP_NODE_SIZE + pos;
        }
    }
    // node[i] is now the directory extent; walk its 16-ary tree
    int t[MAX_GROUP];
    for (int i = 0; i < n; i++) {
        t[i] = node[i] < na->ndirs ? node[i] : -1;
        node[i] = 0;
    }
    static const int base[3] = { 0, NA_DLT_L1, NA_DLT_L2 };
    for (int level = 0; level < 3; level++) {
        for (int i = 0; i < n; i++) if (t[i] >= 0)
            _mm_prefetch((const char*)&na->dirs[t[i]]->dlt[(base[level] + node[i]) * 16], _MM_HINT_T0);
        for (int i = 0; i < n; i++) {
            if (t[i] < 0) continue;
            const int* nd = &na->dirs[t[i]]->dlt[(base[level] + node[i]) * 16];
            int pos = 0;
            while (pos < 16 && nd[pos] <= rem[i]) pos++;
            if (pos == 16) { t[i] = -1; continue; }
            if (pos > 0) rem[i] -= nd[pos - 1];
            node[i] = node[i] * 16 + pos;
        }
    }
    for (int i = 0; i < n; i++) {
        cs[i]->extent = (t[i] >= 0 && node[i] < na->dirs[t[i]]->n) ? (t[i] << NA_DIR_BITS) | node[i] : -1;
        cs[i]->local = rem[i];
        if (cs[i]->extent >= 0) _mm_prefetch(nextarray_block(na, cs[i]->extent) + (size_t)cs[i]->local * (size_t)na->node_size, _MM_HINT_T0);
    }
}

// Binary search inside each extent, one probe per command per round.
// The same branchless steps as nextarray_lower_bound (a conditional move per
// probe, no coin-flip branch), one probe per command per round.
static void group_search(nextarray_t* na, cmd_t** cs, int n) {
    const size_t ns = NODE_SIZE(na);
    const char* base[MAX_GROUP];
    const char* p[MAX_GROUP];
    size_t m[MAX_GROUP];
    bool any = false;
    for (int i = 0; i < n; i++) {
        base[i] = p[i] = nextarray_block(na, cs[i]->extent);
        m[i] = (size_t)nextarray_count(na, cs[i]->extent);
        if (m[i] > 1) { _mm_prefetch(p[i] + m[i] / 2 * ns, _MM_HINT_T0); any = true; }
    }
    // each command's next probe is prefetched right after its step, so every
    // load has a whole round of the group between prefetch and use
    while (any) {
        any = false;
        for (int i = 0; i < n; i++) {
            if (m[i] <= 1) continue;
            size_t half = m[i] / 2;
            p[i] = nextarray_cmp(na, p[i] + half * ns, cs[i]->key) < 0 ? p[i] + half * ns : p[i];
            m[i] -= half;
            if (m[i] > 1) { _mm_prefetch(p[i] + m[i] / 2 * ns, _MM_HINT_T0); any = true; }
        }
    }
    for (int i = 0; i < n; i++)
        cs[i]->pos = m[i] ? (size_t)(p[i] - base[i]) / ns + (nextarray_cmp(na, p[i], cs[i]->key) < 0) : 0;
}

// Warm exactly what execution will use: the neighbours of pos, and for
// insert/delete the tail from pos to the end that the memmove shifts.
static void group_touch(nextarray_t* na, cmd_t** cs, int n) {
    size_t ns = (size_t)na->node_size;
    if (g_update_prefetch)
        for (int i = 0; i < n; i++) if (cs[i]->kind != OP_SEARCH) prefetch_update_path(na, cs[i]->extent);
    for (int i = 0; i < n; i++) {
        cmd_t* c = cs[i];
        const char* base = nextarray_block(na, c->extent);
        size_t cnt = (size_t)nextarray_count(na, c->extent);
        size_t from = c->pos ? c->pos - 1 : 0;
        size_t to = (c->kind == OP_SEARCH) ? (c->pos + 1 < cnt ? c->pos + 1 : cnt) : cnt;
        if (to > from) touch_range(base + from * ns, (to - from) * ns);
    }
}

static void* search_helper_main(void* arg) {
    sr_ring_t* r = arg;
    nextarray_t* na = g_na;
    uint32_t head = 0;
    cmd_t* keys[MAX_GROUP];
    for (;;) {
        uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
        if (head == tail) {
            if (atomic_load_explicit(&r->stop, memory_order_acquire)) break;
            __builtin_ia32_pause();
            continue;
        }
        uint32_t avail = tail - head;
        int n = avail < (uint32_t)g_group ? (int)avail : g_group;
        uint64_t sgen = na->structure_gen, cgen = na->count_gen;   // snapshot before touching anything
        int nk = 0;
        for (int i = 0; i < n; i++) {
            cmd_t* c = &r->cmds[(head + (uint32_t)i) % r->cap];
            if (c->kind == OP_INDEX) {
                c->extent = nextarray_locate_index(na, c->index, &c->local);
                c->gen = cgen;
                if (c->extent >= 0) touch_range((const char*)nextarray_block(na, c->extent) + (size_t)c->local * (size_t)na->node_size, 8);
            } else {
                c->gen = sgen;
                keys[nk++] = c;
            }
        }
        if (nk) {
            group_locate(na, keys, nk);
            if (!g_warm_index_only) { group_search(na, keys, nk); group_touch(na, keys, nk); }
        }
        if (g_evict_kib) {
            // Push what we just read out of this core's L2 so it lands in the shared L3
            // (the L3 is a victim cache: lines only get there when an L2 evicts them).
            static __thread char* junk;
            if (!junk) { junk = aligned_alloc(4096, g_evict_kib << 10); memset(junk, 1, g_evict_kib << 10); }
            long acc = 0;
            for (size_t o = 0; o < (g_evict_kib << 10); o += 64) acc += *(volatile long*)(junk + o);
            g_sink = acc;
        }
        head += (uint32_t)n;
        atomic_store_explicit(&r->done, head, memory_order_release);
    }
    return NULL;
}

static void sr_start(sr_ring_t* r, int cpu) {
    memset(r, 0, sizeof *r);
    r->cap = 2u * (uint32_t)(g_window / g_helpers + g_group);
    r->cmds = aligned_alloc(64, (size_t)r->cap * sizeof(cmd_t));
    memset(r->cmds, 0, (size_t)r->cap * sizeof(cmd_t));
    pthread_create(&r->tid, NULL, search_helper_main, r);
    pin(r->tid, cpu);
}

static void sr_stop(sr_ring_t* r) {
    atomic_store_explicit(&r->stop, true, memory_order_release);
    pthread_join(r->tid, NULL);
    free(r->cmds);
}

// Execute a helper-resolved command; fall back to the full operation if the
// structure moved since or the position does not check out.
static int execute_resolved(nextarray_t* na, cmd_t* c) {
    size_t ns = (size_t)na->node_size;
    if (c->kind == OP_INDEX) {
        if (c->extent >= 0 && c->gen == na->count_gen) return nextarray_at_in(na, c->extent, c->local) ? 0 : -1;
        g_sr[0].fallbacks++;
        return nextarray_at(na, c->index) ? 0 : -1;
    }
    bool fresh = c->extent >= 0 && c->gen == na->structure_gen;
    if (fresh) {
        const char* base = nextarray_block(na, c->extent);
        size_t cnt = (size_t)nextarray_count(na, c->extent), pos = c->pos;
        int at = pos < cnt ? nextarray_cmp(na, base + pos * ns, c->key) : 1;            // node at pos vs key
        bool below_ok = pos == 0 || (pos <= cnt && nextarray_cmp(na, base + (pos - 1) * ns, c->key) < 0);
        // An earlier operation in the group shifted this extent: the extent is
        // still the right one (structure_gen), only pos moved, so search the
        // extent again, which is in cache, instead of redoing the whole
        // operation from the top (bptree2's leaf_pos).
        if (!below_ok || at < 0) {
            pos = lb(base, cnt, ns, c->key, na->compare);
            at = pos < cnt ? nextarray_cmp(na, base + pos * ns, c->key) : 1;
        }
        switch (c->kind) {
        case OP_SEARCH: if (at == 0) return 0; break;
        case OP_DELETE: if (at == 0) return nextarray_delete_at(na, c->extent, pos); break;
        case OP_INSERT:
            if (at > 0) {
                int r = nextarray_insert_at(na, c->extent, pos, c->key);
                if (r == 1) return 0;
            }
            break;
        }
    }
    g_sr[0].fallbacks++;
    switch (c->kind) {
    case OP_SEARCH: return nextarray_search(na, c->key) ? 0 : -1;
    case OP_INSERT: return nextarray_insert(na, c->key) == 1 ? 0 : -1;
    case OP_DELETE: return nextarray_delete(na, c->key);
    }
    return -1;
}

static uint64_t g_warm_missed;

// Lines execution will touch: the neighbours of pos and, for insert/delete,
// the tail the memmove shifts.  Queued to the sibling (pf ring, demand loads).
static void warm_issue(nextarray_t* na, const cmd_t* c) {
    if (c->extent < 0) return;
    size_t ns = (size_t)na->node_size;
    const char* base = nextarray_block(na, c->extent);
    if (c->kind == OP_INDEX) { pf_issue(&g_ring, base + (size_t)c->local * ns, ns); return; }
    size_t cnt = (size_t)nextarray_count(na, c->extent);
    size_t from = c->pos ? c->pos - 1 : 0;
    size_t to = c->kind == OP_SEARCH ? (c->pos + 1 < cnt ? c->pos + 1 : cnt) : cnt;
    if (to > from) pf_issue(&g_ring, base + from * ns, (to - from) * ns);
}

// The lines an insert/delete writes after the memmove: the extent's leaf and
// level-1 node of its directory extent's count tree, the top tree's leaf
// (higher levels stay cached), and the extent's max and its every-8th
// sample.  All known once the extent is.
static inline void prefetch_update_path(nextarray_t* na, int e) {
    int t = e >> NA_DIR_BITS, s = nextarray_entry_of(e);
    const nextarray_dir_t* d = na->dirs[t];
    size_t ns = (size_t)na->node_size;
    _mm_prefetch((const char*)&d->dlt[(NA_DLT_L2 + s / 16) * 16], _MM_HINT_T0);
    _mm_prefetch((const char*)&d->dlt[(NA_DLT_L1 + s / 256) * 16], _MM_HINT_T0);
    _mm_prefetch((const char*)&na->lookup_tree[na->level_offset[na->tree_height - 1] + (t / LOOKUP_NODE_SIZE) * LOOKUP_NODE_SIZE], _MM_HINT_T0);
    _mm_prefetch(nextarray_dir_max(d) + (size_t)s * ns, _MM_HINT_T0);
    _mm_prefetch(nextarray_dir_smax1(d, ns) + (size_t)(s / 8) * ns, _MM_HINT_T0);
}

static void self_warm(nextarray_t* na, const cmd_t* c) {
    if (c->extent < 0) return;
    size_t ns = (size_t)na->node_size;
    const char* base = nextarray_block(na, c->extent);
    if (c->kind == OP_INDEX) { _mm_prefetch(base + (size_t)c->local * ns, _MM_HINT_T0); return; }
    if (c->kind != OP_SEARCH && g_update_prefetch) prefetch_update_path(na, c->extent);
    size_t cnt = (size_t)nextarray_count(na, c->extent);
    size_t from = c->pos ? c->pos - 1 : 0;
    size_t to = c->kind == OP_SEARCH ? (c->pos + 1 < cnt ? c->pos + 1 : cnt) : cnt;
    for (size_t o = from * ns; o < to * ns; o += 64) _mm_prefetch(base + o, _MM_HINT_T0);
}

// Search-mode phase: command i is dealt to helper i % g_helpers; each helper's
// ring holds window/g_helpers of them ahead of execution.  The consumer
// executes in order, waiting on the owning helper's done counter.
static void run_phase_search(nextarray_t* na, const char* name, size_t op_count, fill_fn fill, void* ctx) {
    int H = g_helpers;
    uint32_t base[MAX_HELPERS], filled[MAX_HELPERS];
    for (int h = 0; h < H; h++) { base[h] = atomic_load_explicit(&g_sr[h].tail, memory_order_relaxed); filled[h] = 0; }
    size_t ahead = op_count < (size_t)g_window ? op_count : (size_t)g_window;
    size_t queued = 0;
    for (; queued < ahead; queued++) {
        int h = (int)(queued % (size_t)H);
        fill(&g_sr[h].cmds[(base[h] + filled[h]) % g_sr[h].cap], queued, ctx);
        filled[h]++;
    }
    for (int h = 0; h < H; h++) atomic_store_explicit(&g_sr[h].tail, base[h] + filled[h], memory_order_release);
    for (size_t i = 0; i < op_count; i++) {
        int h = (int)(i % (size_t)H);
        uint32_t seq = base[h] + (uint32_t)(i / (size_t)H);
        while ((int32_t)(atomic_load_explicit(&g_sr[h].done, memory_order_acquire) - seq) <= 0) __builtin_ia32_pause();
        if (execute_resolved(na, &g_sr[h].cmds[seq % g_sr[h].cap]) != 0) {
            fprintf(stderr, "%s: operation %zu failed\n", name, i);
            exit(1);
        }
        if (g_selfwarm && i + (size_t)g_selfwarm < op_count) {
            // consumer-side: prefetch the resolved tail of op i+g_selfwarm into our own L1/L2
            size_t j = i + (size_t)g_selfwarm;
            int hj = (int)(j % (size_t)H);
            uint32_t sj = base[hj] + (uint32_t)(j / (size_t)H);
            if ((int32_t)(atomic_load_explicit(&g_sr[hj].done, memory_order_acquire) - sj) > 0)
                self_warm(na, &g_sr[hj].cmds[sj % g_sr[hj].cap]);
            else
                g_warm_missed++;
        }
        if (g_warm && i + (size_t)g_warm < op_count) {
            // op i+g_warm: if its helper is done with it, have the sibling warm its tail now
            size_t j = i + (size_t)g_warm;
            int hj = (int)(j % (size_t)H);
            uint32_t sj = base[hj] + (uint32_t)(j / (size_t)H);
            if ((int32_t)(atomic_load_explicit(&g_sr[hj].done, memory_order_acquire) - sj) > 0)
                warm_issue(na, &g_sr[hj].cmds[sj % g_sr[hj].cap]);
            else
                g_warm_missed++;
        }
        if (queued < op_count) {
            int q = (int)(queued % (size_t)H);
            fill(&g_sr[q].cmds[(base[q] + filled[q]) % g_sr[q].cap], queued, ctx);
            filled[q]++;
            queued++;
            atomic_store_explicit(&g_sr[q].tail, base[q] + filled[q], memory_order_release);
        }
    }
}

/* ---- pipeline mode: the same level-at-a-time trick, no helper threads ----- */
/*
 * The consumer resolves a group of g_group operations itself with the misses
 * of each index level and each probe round in flight together, then executes
 * the group in order while prefetching the memmove ranges g_tail_ahead
 * operations ahead.  One thread, no rings.
 */
static void run_phase_pipeline(nextarray_t* na, const char* name, size_t op_count, fill_fn fill, void* ctx) {
    static cmd_t cmds[MAX_GROUP];   // static: nothing is allocated in the timed path
    cmd_t* keys[MAX_GROUP];
    uint64_t t_resolve = 0, t_locate = 0, t_prefetch = 0, t_execute = 0, t0 = __rdtsc();
    for (size_t start = 0; start < op_count; start += (size_t)g_group) {
        int n = (int)(op_count - start < (size_t)g_group ? op_count - start : (size_t)g_group);
        uint64_t sgen = na->structure_gen, cgen = na->count_gen;
        uint64_t ta = verbose ? __rdtsc() : 0;
        int nk = 0, ni = 0;
        cmd_t* idxs[MAX_GROUP];
        for (int i = 0; i < n; i++) {
            cmd_t* c = &cmds[i];
            fill(c, start + (size_t)i, ctx);
            if (c->kind == OP_INDEX) { c->gen = cgen; idxs[ni++] = c; }
            else { c->gen = sgen; keys[nk++] = c; }
        }
        if (ni) group_locate_index(na, idxs, ni);
        if (nk) {
            uint64_t tl = verbose ? __rdtsc() : 0;
            group_locate(na, keys, nk);
            if (verbose) t_locate += __rdtsc() - tl;
            group_search(na, keys, nk);
        }
        if (verbose) t_resolve += __rdtsc() - ta;
        // rolling prefetch of the memmove ranges, then execute in order
        for (int i = 0; i < n && i < g_tail_ahead; i++) self_warm(na, &cmds[i]);
        for (int i = 0; i < n; i++) {
            uint64_t tc = verbose ? __rdtsc() : 0;
            if (i + g_tail_ahead < n) self_warm(na, &cmds[i + g_tail_ahead]);
            uint64_t td = verbose ? __rdtsc() : 0;
            if (execute_resolved(na, &cmds[i]) != 0) {
                fprintf(stderr, "%s: operation %zu failed\n", name, start + (size_t)i);
                exit(1);
            }
            if (verbose) { uint64_t te = __rdtsc(); t_prefetch += td - tc; t_execute += te - td; }
        }
    }
    if (verbose) {
        double tot = (double)(__rdtsc() - t0);
        printf("  %s pipeline split: resolve %.0f%% (locate %.0f%%)  prefetch %.0f%%  execute %.0f%%\n", name,
               100.0 * t_resolve / tot, 100.0 * t_locate / tot, 100.0 * t_prefetch / tot, 100.0 * t_execute / tot);
    }
}

/* ---- warm mode: helpers read ahead to warm the L3, main thread does all the work ---- */
/*
 * The helpers get the keys `window` operations ahead and run the same
 * locate/search/touch reads, but nothing they compute is used.  The consumer
 * runs the single-thread pipeline over the same operations; if the helpers'
 * reads have made it into the shared L3, its misses become L3 hits.
 */
static void run_phase_warm(nextarray_t* na, const char* name, size_t op_count, fill_fn fill, void* ctx) {
    int H = g_helpers;
    uint32_t base[MAX_HELPERS], filled[MAX_HELPERS];
    for (int h = 0; h < H; h++) { base[h] = atomic_load_explicit(&g_sr[h].tail, memory_order_relaxed); filled[h] = 0; }
    size_t queued = 0;
    // feed the helpers `window` ahead, then keep them that far ahead as groups complete
    #define FEED_TO(limit) do { \
        while (queued < (limit) && queued < op_count) { \
            int h = (int)(queued % (size_t)H); \
            fill(&g_sr[h].cmds[(base[h] + filled[h]) % g_sr[h].cap], queued, ctx); \
            filled[h]++; queued++; \
        } \
        for (int h = 0; h < H; h++) atomic_store_explicit(&g_sr[h].tail, base[h] + filled[h], memory_order_release); \
    } while (0)
    FEED_TO((size_t)g_window);

    static cmd_t cmds[MAX_GROUP];   // static: nothing is allocated in the timed path
    cmd_t* keys[MAX_GROUP];
    for (size_t start = 0; start < op_count; start += (size_t)g_group) {
        FEED_TO(start + (size_t)g_window);
        int n = (int)(op_count - start < (size_t)g_group ? op_count - start : (size_t)g_group);
        uint64_t sgen = na->structure_gen, cgen = na->count_gen;
        int nk = 0, ni = 0;
        cmd_t* idxs[MAX_GROUP];
        for (int i = 0; i < n; i++) {
            cmd_t* c = &cmds[i];
            fill(c, start + (size_t)i, ctx);
            if (c->kind == OP_INDEX) { c->gen = cgen; idxs[ni++] = c; }
            else { c->gen = sgen; keys[nk++] = c; }
        }
        if (ni) group_locate_index(na, idxs, ni);
        if (nk) { group_locate(na, keys, nk); group_search(na, keys, nk); }
        for (int i = 0; i < n && i < g_tail_ahead; i++) self_warm(na, &cmds[i]);
        for (int i = 0; i < n; i++) {
            if (i + g_tail_ahead < n) self_warm(na, &cmds[i + g_tail_ahead]);
            if (execute_resolved(na, &cmds[i]) != 0) { fprintf(stderr, "%s: operation %zu failed\n", name, start + (size_t)i); exit(1); }
        }
    }
    #undef FEED_TO
}

// One pass of op_count operations, timed into ph.  Waiting for idle helper
// rings and allocating the ring happen outside the timed window.
static void run_pass(nextarray_t* na, const char* name, size_t op_count, fill_fn fill, void* ctx, Phase* ph) {
    if (g_prefetch == PF_WARM || g_prefetch == PF_SEARCH) {
        // the rings are shared with the helpers; they have to be idle between passes
        for (int h = 0; h < g_helpers; h++)
            while (atomic_load_explicit(&g_sr[h].done, memory_order_acquire) != atomic_load_explicit(&g_sr[h].tail, memory_order_relaxed)) __builtin_ia32_pause();
    }
    if (g_prefetch == PF_WARM) {
        phase_start(ph);
        run_phase_warm(na, name, op_count, fill, ctx);
        phase_stop(ph);
        return;
    }
    if (g_prefetch == PF_PIPELINE) {
        phase_start(ph);
        run_phase_pipeline(na, name, op_count, fill, ctx);
        phase_stop(ph);
        return;
    }
    if (g_prefetch == PF_SEARCH) {
        phase_start(ph);
        run_phase_search(na, name, op_count, fill, ctx);
        phase_stop(ph);
        return;
    }
    // Run op_count operations keeping `window` of them resolved ahead.
    cmd_t* ring = calloc((size_t)g_window, sizeof *ring);
    size_t ahead = op_count < (size_t)g_window ? op_count : (size_t)g_window;
    phase_start(ph);
    for (size_t i = 0; i < ahead; i++) { fill(&ring[i], i, ctx); resolve(na, &ring[i]); }
    for (size_t i = 0; i < op_count; i++) {
        cmd_t* c = &ring[i % (size_t)g_window];
        if (execute(na, c) != 0) {
            fprintf(stderr, "%s: operation %zu failed\n", name, i);
            exit(1);
        }
        size_t next = i + (size_t)g_window;
        if (next < op_count) { fill(c, next, ctx); resolve(na, c); }
    }
    phase_stop(ph);
    free(ring);
}

// Put the structure back after a pass of inserts or deletes (untimed), so the
// next pass starts from the same contents.  Rebalancing returns the extents
// the inserts split off to the pool.
static void undo_insert(nextarray_t* na, size_t n, void* ctx) {
    for (size_t i = 0; i < n; i++) nextarray_delete(na, &((Node*)ctx)[i]);
    nextarray_balance(na);
}
static void undo_delete(nextarray_t* na, size_t n, void* ctx) {
    for (size_t i = 0; i < n; i++) nextarray_insert(na, &((Node*)ctx)[i]);
    nextarray_balance(na);
}

// Time a phase: passes of op_count operations until Phase has enough (see
// benchmark_helpers.h), with undo between passes.
static void run_phase(nextarray_t* na, const char* name, size_t op_count, fill_fn fill, void* ctx,
                      void (*undo)(nextarray_t*, size_t, void*), long bytes_model, Event* ev) {
    printf("--- %s benchmarks ---\n", name);
    uint64_t fb = g_sr[0].fallbacks;
    long passes = 0;
    Phase ph;
    phase_begin(&ph, (long)op_count);
    for (;;) {
        run_pass(na, name, op_count, fill, ctx, &ph);
        passes++;
        if (!phase_again(&ph)) break;
        if (undo) undo(na, op_count, ctx);
    }
    phase_record(&ph, ev, bytes_model);
    if (g_prefetch == PF_PIPELINE || g_prefetch == PF_SEARCH)
        printf("  %s: %lu of %zu resolved operations fell back to a full operation\n", name,
               (unsigned long)(g_sr[0].fallbacks - fb), (size_t)passes * op_count);
}

/* ---- main ---------------------------------------------------------------- */

static void print_usage(const char* prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Batched nextarray benchmark with a prefetch helper on the hyperthread sibling\n\n");
    printf("  -i, --input FILE         Path file with pregenerated nodes [REQUIRED]\n");
    printf("  -l, --list  FILE         Path file with sorted nodes\n");
    printf("  -c, --count              How many nodes to use\n");
    printf("  -e, --extent-size SIZE   Number of nodes per extent (default: %d)\n", DEFAULT_EXTENT_SIZE);
    printf("  -o, --overprovisioning   Overprovisioning factor (default: %.1f)\n", DEFAULT_OVERPROVISIONING);
    printf("  -w, --window N           Operations resolved/prefetched ahead (default: %d)\n", DEFAULT_WINDOW);
    printf("  -p, --prefetch MODE      none | self | helper | search | pipeline (default: helper)\n");
    printf("                           pipeline: the consumer resolves groups level-at-a-time itself, no helper threads\n");
    printf("  -D, --tail-ahead N       pipeline mode: prefetch memmove ranges N ops ahead within the group (default: 4)\n");
    printf("                           warm: helpers (-n, -H) read ahead to warm the L3; the main thread runs the pipeline\n");
    printf("  -E, --evict KIB          warm mode: helper streams KIB of junk after each group to push its L2 into L3\n");
    printf("  -I, --warm-index-only    warm mode: helpers walk only the max index, never the extents\n");
    printf("  -U, --no-update-prefetch do not prefetch the tree path / max entry an insert or delete writes\n");
    printf("                           search: the helper also does the index walk and binary search\n");
    printf("  -g, --group N            search mode: operations resolved together (default: 8, max %d)\n", MAX_GROUP);
    printf("  -n, --helpers N          search mode: helper threads on consecutive cpus from -H (default: 1)\n");
    printf("  -W, --warm N             search mode: sibling warmer pulls each op's tail into L2 N ops ahead (default: off)\n");
    printf("  -S, --self-warm N        search mode: consumer prefetches the tail of the op N ahead itself (default: off)\n");
    printf("  -H, --helper-cpu N       Pin the helper to cpu N (default: cpu 0's HT sibling)\n");
    printf("  -b, --prefetch-bytes N   Prefetch at most N bytes per extent (default: whole extent)\n");
    printf("  -t, --hint t0|t1|t2|nta  Prefetch hint (default: t1)\n");
    printf("  -s, --stride N           Bytes between touched lines (default: 64)\n");
    printf("  -L, --load               Helper uses demand loads instead of prefetch instructions\n");
    printf("  -d, --debug LEVEL        Validate against the sorted list files\n");
    printf("  -h, --help\n");
}

static int parse_arguments(int argc, char* argv[]) {
    static struct option opts[] = {
        {"input", required_argument, 0, 'i'}, {"list", required_argument, 0, 'l'},
        {"count", required_argument, 0, 'c'}, {"extent-size", required_argument, 0, 'e'},
        {"overprovisioning", required_argument, 0, 'o'}, {"window", required_argument, 0, 'w'},
        {"prefetch", required_argument, 0, 'p'}, {"helper-cpu", required_argument, 0, 'H'},
        {"prefetch-bytes", required_argument, 0, 'b'}, {"hint", required_argument, 0, 't'}, {"stride", required_argument, 0, 's'}, {"load", no_argument, 0, 'L'}, {"group", required_argument, 0, 'g'}, {"helpers", required_argument, 0, 'n'}, {"warm", required_argument, 0, 'W'}, {"self-warm", required_argument, 0, 'S'}, {"tail-ahead", required_argument, 0, 'D'}, {"no-update-prefetch", no_argument, 0, 'U'}, {"evict", required_argument, 0, 'E'}, {"warm-index-only", no_argument, 0, 'I'}, {"debug", required_argument, 0, 'd'},
        {"verbose", required_argument, 0, 'v'}, {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:l:c:e:o:w:p:H:b:t:s:Lg:n:W:S:D:UE:Id:v:h", opts, NULL)) != -1) {
        switch (c) {
        case 'i': g_inputfile = optarg; break;
        case 'l': g_listfile = optarg; break;
        case 'c': if (parse_size_bin(optarg, &g_count) != 0) return -1; break;
        case 'e': if (parse_size_bin(optarg, &g_extent_size) != 0) return -1; break;
        case 'o': g_overprovisioning = atof(optarg); break;
        case 'w': g_window = atoi(optarg); if (g_window < 1) return -1; break;
        case 'p':
            if (strcmp(optarg, "none") == 0) g_prefetch = PF_NONE;
            else if (strcmp(optarg, "self") == 0) g_prefetch = PF_SELF;
            else if (strcmp(optarg, "helper") == 0) g_prefetch = PF_HELPER;
            else if (strcmp(optarg, "search") == 0) g_prefetch = PF_SEARCH;
            else if (strcmp(optarg, "pipeline") == 0) g_prefetch = PF_PIPELINE;
            else if (strcmp(optarg, "warm") == 0) g_prefetch = PF_WARM;
            else return -1;
            break;
        case 'H': g_helper_cpu = atoi(optarg); break;
        case 'b': { uint64_t v; if (parse_size_bin(optarg, &v) != 0) return -1; g_prefetch_bytes = v; break; }
        case 's': g_stride = (size_t)atoi(optarg); if (g_stride < 8) return -1; break;
        case 'L': g_load = 1; break;
        case 'g': g_group = atoi(optarg); if (g_group < 1 || g_group > MAX_GROUP) return -1; break;
        case 'n': g_helpers = atoi(optarg); if (g_helpers < 1 || g_helpers > MAX_HELPERS) return -1; break;
        case 'W': g_warm = atoi(optarg); if (g_warm < 0) return -1; break;
        case 'S': g_selfwarm = atoi(optarg); if (g_selfwarm < 0) return -1; break;
        case 'D': g_tail_ahead = atoi(optarg); if (g_tail_ahead < 0) return -1; break;
        case 'U': g_update_prefetch = 0; break;
        case 'E': g_evict_kib = (size_t)atoi(optarg); break;
        case 'I': g_warm_index_only = 1; break;
        case 't':
            if (strcmp(optarg, "t0") == 0) g_hint = _MM_HINT_T0;
            else if (strcmp(optarg, "t1") == 0) g_hint = _MM_HINT_T1;
            else if (strcmp(optarg, "t2") == 0) g_hint = _MM_HINT_T2;
            else if (strcmp(optarg, "nta") == 0) g_hint = _MM_HINT_NTA;
            else return -1;
            break;
        case 'd': debug = atoi(optarg); break;
        case 'v': verbose = atoi(optarg); break;
        case 'h': print_usage(argv[0]); exit(0);
        default: return -1;
        }
    }
    if (!g_inputfile || (g_count == 0 && !g_listfile) || (g_count > 0 && g_listfile)) return -1;
    return 0;
}

static int dump_list(void* ctx, const char* filename) { return nextarray_dump_nodes_to_file(ctx, filename); }

int main(int argc, char* argv[]) {
    if (parse_arguments(argc, argv) != 0) { print_usage(argv[0]); return 1; }
    static const char* mode_names[] = { "none", "self", "helper", "search", "pipeline", "warm" };

    Node* insert_nodes = NULL;
    Node* delete_nodes = NULL;
    size_t* indexes = NULL;
    size_t valid_count = g_count, op_count = 0;
    if (valid_count == 0) {
        if (get_file_size(g_listfile, &valid_count) != 0) return 1;
        valid_count /= sizeof(Node);
    }

    nextarray_t* na = nextarray_alloc((size_t)(valid_count * g_overprovisioning), (int)g_extent_size, sizeof(Node), compare_nodes);
    if (!na) return 1;
    long start = get_time_us();
    if (load_nodes(g_inputfile, g_listfile, (Node**)&na->buffer, &insert_nodes, &delete_nodes, &indexes, &valid_count, &op_count) != 0) return 1;
    report_elapsed_time("Loaded nodes", start);
    start = get_time_us();
    nextarray_bulk_init(na, valid_count, g_nodes_sorted);
    if (nextarray_balance(na) != 0) return 1;
    report_elapsed_time("  bulk_init + balance", start);
    if (debug > 0 && validate_nodes_by_file(g_inputfile, "original", valid_count, na, dump_list) != 0) return 1;

    // Consumer on cpu 0.  Helper on the sibling, so its prefetches land in the L2 they share.
    const int main_cpu = 0;
    if (g_helper_cpu < 0) g_helper_cpu = ht_sibling(main_cpu);
    pin(pthread_self(), main_cpu);
    g_na = na;
    if (g_prefetch == PF_HELPER) pf_start(&g_ring, g_helper_cpu);
    if (g_prefetch == PF_SEARCH && g_warm) {
        g_load = 1;                                  // the warmer uses demand loads
        pf_start(&g_ring, ht_sibling(main_cpu));
        printf("  warmer on cpu %d (hyperthread sibling), %d ops ahead\n", ht_sibling(main_cpu), g_warm);
    }
    if (g_prefetch == PF_SEARCH || g_prefetch == PF_WARM) {
        // helpers on g_helper_cpu, g_helper_cpu+1, ... skipping the consumer and its sibling
        int cpu = g_helper_cpu;
        for (int h = 0; h < g_helpers; h++, cpu++) {
            while (cpu == main_cpu || cpu == ht_sibling(main_cpu)) cpu++;
            sr_start(&g_sr[h], cpu);
            if (h) printf("  helper %d cpu %d\n", h, cpu);
        }
    }
    printf("nextarray2: %zu nodes, %d-node extents, window %d, prefetch %s, consumer cpu %d\n",
           valid_count, na->extent_size, g_window, mode_names[g_prefetch], main_cpu);
    if (g_prefetch == PF_HELPER || g_prefetch == PF_SEARCH || g_prefetch == PF_WARM)
        printf("  helper cpu %d (%s of cpu %d)\n", g_helper_cpu,
               g_helper_cpu == ht_sibling(main_cpu) ? "hyperthread sibling" : "separate core", main_cpu);

    Stats stats;
    memset(&stats, 0, sizeof stats);
    snprintf(stats.notes, sizeof stats.notes, "extent_size=%lu,window=%d,prefetch=%s,helper_cpu=%d,bytes=%zu,hint=%d,stride=%zu,load=%d,group=%d,helpers=%d,warm=%d,selfwarm=%d,tail_ahead=%d,update_prefetch=%d,evict_kib=%zu",
             (unsigned long)g_extent_size, g_window, mode_names[g_prefetch], g_prefetch == PF_HELPER ? g_helper_cpu : -1, g_prefetch_bytes, g_hint, g_stride, g_load, g_group, g_helpers, g_warm, g_selfwarm, g_tail_ahead, g_update_prefetch, g_evict_kib);
    stats.nodes = valid_count;
    long find_extent = (long)(log2((double)(valid_count / g_extent_size)) * sizeof(Node) * 2);
    long search_extent = (long)(log2((double)g_extent_size / g_overprovisioning) * sizeof(Node));
    long avg_extent_cpy = (long)((g_extent_size / g_overprovisioning) / 2 * sizeof(Node));

    run_phase(na, "search", op_count, fill_search, delete_nodes, NULL, find_extent + search_extent, &stats.search);
    run_phase(na, "index", op_count, fill_index, indexes, NULL, find_extent + search_extent, &stats.index);
    run_phase(na, "insert", op_count, fill_insert, insert_nodes, undo_insert, find_extent + search_extent + avg_extent_cpy, &stats.insert);
    if (debug > 0 && validate_nodes_by_file(g_inputfile, "insert", valid_count, na, dump_list) != 0) return 1;
    run_phase(na, "delete", op_count, fill_delete, delete_nodes, undo_delete, find_extent + search_extent + avg_extent_cpy, &stats.delete);
    if (debug > 0 && validate_nodes_by_file(g_inputfile, "delete", valid_count, na, dump_list) != 0) return 1;
    // one narrow key range in increasing order (see gen_nodes.h)
    if (g_clustered_count)
        run_phase(na, "clustered", g_clustered_count, fill_insert, g_clustered_nodes, undo_insert,
                  find_extent + search_extent + avg_extent_cpy, &stats.clustered);

    if (g_prefetch == PF_HELPER) {
        pf_stop(&g_ring);
        if (g_ring.dropped) printf("  helper ring full %lu times (prefetches skipped)\n", (unsigned long)g_ring.dropped);
    }
    if (g_prefetch == PF_WARM) for (int h = 0; h < g_helpers; h++) sr_stop(&g_sr[h]);
    if (g_prefetch == PF_SEARCH) {
        for (int h = 0; h < g_helpers; h++) sr_stop(&g_sr[h]);
        if (g_warm) pf_stop(&g_ring);
        if (g_warm || g_selfwarm)
            printf("  warmer: %lu ops not yet resolved when their warm was due, %lu ring-full drops\n",
                   (unsigned long)g_warm_missed, (unsigned long)g_ring.dropped);
    }
    printf("\n");
    print_results("NextArray2:", &stats);
    printf("\n");
    stats_write_json(&stats, "nextarray2");

    free(insert_nodes);
    free(delete_nodes);
    free(indexes);
    nextarray_free(na);
    return 0;
}
