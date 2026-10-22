#include "diskarray.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <immintrin.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>


// Forward declarations
static inline int diskarray_flush_buffers(diskarray_t* na, bool force);
static inline void diskarray_worker_process_completions(diskarray_t* na, worker_thread_data_t* worker);
static void* diskarray_worker_thread_main(void* arg);

// Index of the first element >= key in a sorted array, or count if none.
// Generic nodes go through na->compare.  Built with -DDISKARRAY_U64_KEYS the
// nodes are 8-byte unsigned keys in numeric order (what the benchmarks use):
// the compare is inlined and the search is branchless (a conditional move per
// probe instead of a coin-flip branch), as in nextarray2.
#ifdef DISKARRAY_U64_KEYS
static inline int diskarray_cmp(const diskarray_t* na, const void* a, const void* b) {
    (void)na;
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}
static inline size_t lower_bound(const char* base, size_t count, size_t node_size,
                                 const void* key, int (*cmp)(const void*, const void*)) {
    (void)node_size; (void)cmp;
    if (count == 0) return 0;
    const uint64_t* a = (const uint64_t*)base;
    const uint64_t* p = a;
    uint64_t k = *(const uint64_t*)key;
    size_t n = count;
    while (n > 1) {
        size_t half = n / 2;
        p = p[half] < k ? p + half : p;
        n -= half;
    }
    return (size_t)(p - a) + (*p < k);
}
#else
static inline int diskarray_cmp(const diskarray_t* na, const void* a, const void* b) { return na->compare(a, b); }
static inline size_t lower_bound(const char* base, size_t count, size_t node_size,
                                 const void* key, int (*cmp)(const void*, const void*)) {
    size_t lo = 0, n = count;
    while (n > 0) {
        size_t half = n / 2;
        if (cmp(base + (lo + half) * node_size, key) < 0) { lo += half + 1; n -= half + 1; }
        else n = half;
    }
    return lo;
}
#endif

///// DIRECTORY ////////////////////
/*
 * The order of the extents: directory extents of DA_DIR_CAP ids with slack,
 * each with a 16-ary running-count tree and max samples every 8 and 64
 * entries; a top level with one entry per directory extent (count, max with
 * its samples, 8-ary running-count tree).  Same layout as nextarray.  A
 * handle is (directory extent << DA_DIR_BITS) | entry; pos_of[id] is the
 * handle of extent id.
 */
#define GROUP 8
#define DLT_L1 1
#define DLT_L2 17
#define HANDLE(t, s) (((t) << DA_DIR_BITS) | (s))
#define HANDLE_DIR(h) ((h) >> DA_DIR_BITS)
#define HANDLE_ENTRY(h) ((h) & (DA_DIR_CAP - 1))

static inline char* dmax(const diskarray_t* na, const diskarray_dir_t* d, int s) {
    return d->max + (size_t)s * (size_t)na->node_size;
}

static void dlt_rebuild(const diskarray_t* na, diskarray_dir_t* d) {
    for (int j = 0; j < 256; j++) {
        int* leaf = &d->dlt[(DLT_L2 + j) * 16];
        int run = 0;
        for (int i = 0; i < 16; i++) {
            int s = j * 16 + i;
            if (s < d->n) run += na->count[d->id[s]];
            leaf[i] = run;
        }
    }
    for (int j = 0; j < 16; j++) {
        int* node = &d->dlt[(DLT_L1 + j) * 16];
        int run = 0;
        for (int i = 0; i < 16; i++) { run += d->dlt[(DLT_L2 + j * 16 + i) * 16 + 15]; node[i] = run; }
    }
    int run = 0;
    for (int i = 0; i < 16; i++) { run += d->dlt[(DLT_L1 + i) * 16 + 15]; d->dlt[i] = run; }
}

__attribute__((target("avx512f")))
static inline void dlt_update(diskarray_dir_t* d, int s, int delta) {
    const __m512i v = _mm512_set1_epi32(delta);
    int* nodes[3] = { &d->dlt[0], &d->dlt[(DLT_L1 + s / 256) * 16], &d->dlt[(DLT_L2 + s / 16) * 16] };
    int pos[3] = { s / 256, (s / 16) % 16, s % 16 };
    for (int l = 0; l < 3; l++) {
        __m512i x = _mm512_load_si512(nodes[l]);
        _mm512_store_si512(nodes[l], _mm512_mask_add_epi32(x, (__mmask16)(0xFFFFu << pos[l]), x, v));
    }
}

// Entry holding position rem inside directory extent d; *local is the offset in it.
__attribute__((target("avx512f")))
static inline int dlt_find(const diskarray_dir_t* d, long rem, long* local) {
    static const int base[3] = { 0, DLT_L1, DLT_L2 };
    int node = 0;
    for (int l = 0; l < 3; l++) {
        const int* nd = &d->dlt[(base[l] + node) * 16];
        __mmask16 m = _mm512_cmpgt_epi32_mask(_mm512_load_si512(nd), _mm512_set1_epi32((int)rem));
        if (!m) return -1;
        int p = __builtin_ctz(m);
        if (p) rem -= nd[p - 1];
        node = node * 16 + p;
    }
    *local = rem;
    return node;
}

static void dsamples_rebuild(const diskarray_t* na, diskarray_dir_t* d) {
    size_t ns = (size_t)na->node_size;
    int n1 = (d->n + GROUP - 1) / GROUP, n2 = (n1 + GROUP - 1) / GROUP;
    for (int g = 0; g < n1; g++) {
        int last = g * GROUP + GROUP - 1;
        if (last > d->n - 1) last = d->n - 1;
        memcpy(d->smax1 + (size_t)g * ns, dmax(na, d, last), ns);
    }
    for (int g = 0; g < n2; g++) {
        int last = g * GROUP + GROUP - 1;
        if (last > n1 - 1) last = n1 - 1;
        memcpy(d->smax2 + (size_t)g * ns, d->smax1 + (size_t)last * ns, ns);
    }
}

// max[s] := v and the samples of it.
static inline void dset_max(const diskarray_t* na, diskarray_dir_t* d, int s, const void* v) {
    size_t ns = (size_t)na->node_size;
    memcpy(dmax(na, d, s), v, ns);
    int n1 = (d->n + GROUP - 1) / GROUP;
    if (s % GROUP != GROUP - 1 && s != d->n - 1) return;
    int g = s / GROUP;
    memcpy(d->smax1 + (size_t)g * ns, v, ns);
    if (g % GROUP != GROUP - 1 && g != n1 - 1) return;
    memcpy(d->smax2 + (size_t)(g / GROUP) * ns, v, ns);
}

// First entry whose max >= key, or the last entry.
static inline int dfind(const diskarray_t* na, const diskarray_dir_t* d, const void* key) {
    size_t ns = (size_t)na->node_size;
    int n1 = (d->n + GROUP - 1) / GROUP, n2 = (n1 + GROUP - 1) / GROUP;
    size_t g = lower_bound(d->smax2, (size_t)n2, ns, key, na->compare);
    if (g >= (size_t)n2) return d->n - 1;
    size_t lo = g * GROUP, span = (size_t)n1 - lo < GROUP ? (size_t)n1 - lo : GROUP;
    g = lo + lower_bound(d->smax1 + lo * ns, span, ns, key, na->compare);
    lo = g * GROUP;
    // the caller reads id[s] next: same group, so start that line now and
    // let its miss overlap the max line's
    _mm_prefetch((const char*)&d->id[lo], _MM_HINT_T0);
    span = (size_t)d->n - lo < GROUP ? (size_t)d->n - lo : GROUP;
    size_t s = lo + lower_bound(d->max + lo * ns, span, ns, key, na->compare);
    return s < (size_t)d->n ? (int)s : d->n - 1;
}

// Directory extent t changed shape: its indexes and its ids' handles.
static void drebuild(diskarray_t* na, int t) {
    diskarray_dir_t* d = na->dirs[t];
    for (int s = 0; s < d->n; s++) na->pos_of[d->id[s]] = HANDLE(t, s);
    dlt_rebuild(na, d);
    if (d->n) dsamples_rebuild(na, d);
}

static diskarray_dir_t* dir_alloc(diskarray_t* na) {
    size_t ns = (size_t)na->node_size;
    size_t keys = (size_t)(DA_DIR_CAP + DA_DIR_CAP / GROUP + DA_DIR_CAP / GROUP / GROUP) * ns;
    size_t bytes = (sizeof(diskarray_dir_t) + keys + 63) & ~(size_t)63;
    diskarray_dir_t* d = aligned_alloc(64, bytes);
    if (!d) return NULL;
    memset(d, 0, bytes);
    d->max = (char*)(d + 1);
    d->smax1 = d->max + (size_t)DA_DIR_CAP * ns;
    d->smax2 = d->smax1 + (size_t)(DA_DIR_CAP / GROUP) * ns;
    return d;
}

static inline long* tlt_node(const diskarray_t* na, int level, int idx) {
    return &na->lookup_tree[na->level_offset[level] + idx * LOOKUP_NODE_SIZE];
}

static void tsamples_rebuild(diskarray_t* na) {
    size_t ns = (size_t)na->node_size;
    int n1 = (na->ndirs + GROUP - 1) / GROUP, n2 = (n1 + GROUP - 1) / GROUP;
    for (int g = 0; g < n1; g++) {
        int last = g * GROUP + GROUP - 1;
        if (last > na->ndirs - 1) last = na->ndirs - 1;
        memcpy(na->tsmax1 + (size_t)g * ns, na->tmax + (size_t)last * ns, ns);
    }
    for (int g = 0; g < n2; g++) {
        int last = g * GROUP + GROUP - 1;
        if (last > n1 - 1) last = n1 - 1;
        memcpy(na->tsmax2 + (size_t)g * ns, na->tsmax1 + (size_t)last * ns, ns);
    }
}

static inline void tset_max(diskarray_t* na, int t, const void* v) {
    size_t ns = (size_t)na->node_size;
    memcpy(na->tmax + (size_t)t * ns, v, ns);
    int n1 = (na->ndirs + GROUP - 1) / GROUP;
    if (t % GROUP != GROUP - 1 && t != na->ndirs - 1) return;
    int g = t / GROUP;
    memcpy(na->tsmax1 + (size_t)g * ns, v, ns);
    if (g % GROUP != GROUP - 1 && g != n1 - 1) return;
    memcpy(na->tsmax2 + (size_t)(g / GROUP) * ns, v, ns);
}

static inline int tfind(const diskarray_t* na, const void* key) {
    size_t ns = (size_t)na->node_size;
    int n1 = (na->ndirs + GROUP - 1) / GROUP, n2 = (n1 + GROUP - 1) / GROUP;
    size_t g = lower_bound(na->tsmax2, (size_t)n2, ns, key, na->compare);
    if (g >= (size_t)n2) return na->ndirs - 1;
    size_t lo = g * GROUP, span = (size_t)n1 - lo < GROUP ? (size_t)n1 - lo : GROUP;
    g = lo + lower_bound(na->tsmax1 + lo * ns, span, ns, key, na->compare);
    lo = g * GROUP;
    span = (size_t)na->ndirs - lo < GROUP ? (size_t)na->ndirs - lo : GROUP;
    size_t t = lo + lower_bound(na->tmax + lo * ns, span, ns, key, na->compare);
    return t < (size_t)na->ndirs ? (int)t : na->ndirs - 1;
}

// The top count tree and samples, from tcnt (after the top level changed shape).
static void tlt_rebuild(diskarray_t* na) {
    int level = na->tree_height - 1;
    int nodes = (na->ndirs + LOOKUP_NODE_SIZE - 1) / LOOKUP_NODE_SIZE;
    memset(na->lookup_tree, 0, (size_t)na->total_tree_nodes * LOOKUP_NODE_SIZE * sizeof(long));
    for (int j = 0; j < nodes; j++) {
        long* leaf = tlt_node(na, level, j);
        long run = 0;
        for (int i = 0; i < LOOKUP_NODE_SIZE; i++) {
            int t = j * LOOKUP_NODE_SIZE + i;
            if (t < na->ndirs) run += na->tcnt[t];
            leaf[i] = run;
        }
    }
    for (level--; level >= 0; level--) {
        int child_nodes = nodes;
        nodes = (child_nodes + LOOKUP_NODE_SIZE - 1) / LOOKUP_NODE_SIZE;
        for (int j = 0; j < nodes; j++) {
            long* node = tlt_node(na, level, j);
            long run = 0;
            for (int i = 0; i < LOOKUP_NODE_SIZE; i++) {
                int c = j * LOOKUP_NODE_SIZE + i;
                if (c < child_nodes) run += tlt_node(na, level + 1, c)[LOOKUP_NODE_SIZE - 1];
                node[i] = run;
            }
        }
    }
    if (na->ndirs > 0) tsamples_rebuild(na);
}

__attribute__((target("avx512f")))
static inline void tlt_update(diskarray_t* na, int t, long delta) {
    na->tcnt[t] += delta;
    const __m512i v = _mm512_set1_epi64(delta);
    int idx = t;
    for (int level = na->tree_height - 1; level >= 0; level--) {
        int pos = idx % LOOKUP_NODE_SIZE;
        idx /= LOOKUP_NODE_SIZE;
        long* node = tlt_node(na, level, idx);
        __m512i x = _mm512_load_si512(node);
        _mm512_store_si512(node, _mm512_mask_add_epi64(x, (__mmask8)(0xFFu << pos), x, v));
    }
}

__attribute__((target("avx512f")))
static inline int tlt_find(const diskarray_t* na, long index, long* rem_out) {
    long rem = index;
    int idx = 0;
    for (int level = 0; level < na->tree_height; level++) {
        const long* node = tlt_node(na, level, idx);
        __mmask8 m = _mm512_cmpgt_epi64_mask(_mm512_load_si512(node), _mm512_set1_epi64(rem));
        if (!m) return -1;
        int pos = __builtin_ctz(m);
        if (pos) rem -= node[pos - 1];
        idx = idx * LOOKUP_NODE_SIZE + pos;
    }
    if (idx >= na->ndirs) return -1;
    *rem_out = rem;
    return idx;
}

static int top_height(int cap) {
    int height = 1, nodes = (cap + LOOKUP_NODE_SIZE - 1) / LOOKUP_NODE_SIZE;
    while (nodes > 1) { nodes = (nodes + LOOKUP_NODE_SIZE - 1) / LOOKUP_NODE_SIZE; height++; }
    return height;
}

// Room for cap directory extents in the top level.
static int top_reserve(diskarray_t* na, int cap) {
    if (cap <= na->dirs_cap) return 0;
    size_t ns = (size_t)na->node_size;
    diskarray_dir_t** dirs = realloc(na->dirs, (size_t)cap * sizeof *dirs);
    long* tcnt = realloc(na->tcnt, (size_t)cap * sizeof *tcnt);
    char* tmax = realloc(na->tmax, (size_t)cap * ns);
    char* tsmax1 = realloc(na->tsmax1, (size_t)(cap / GROUP + 1) * ns);
    char* tsmax2 = realloc(na->tsmax2, (size_t)(cap / GROUP / GROUP + 1) * ns);
    if (!dirs || !tcnt || !tmax || !tsmax1 || !tsmax2) return -1;
    na->dirs = dirs; na->tcnt = tcnt; na->tmax = tmax; na->tsmax1 = tsmax1; na->tsmax2 = tsmax2;
    na->dirs_cap = cap;
    free(na->lookup_tree);
    na->tree_height = top_height(cap);
    na->total_tree_nodes = 0;
    for (int level = 0, nodes = 1; level < na->tree_height; level++, nodes *= LOOKUP_NODE_SIZE) {
        na->level_offset[level] = na->total_tree_nodes * LOOKUP_NODE_SIZE;
        na->total_tree_nodes += nodes;
    }
    na->lookup_tree = aligned_alloc(64, ((size_t)na->total_tree_nodes * LOOKUP_NODE_SIZE * sizeof(long) + 63) & ~(size_t)63);
    if (!na->lookup_tree) return -1;
    tlt_rebuild(na);
    return 0;
}

static inline void diskarray_refresh_ends(diskarray_t* na) {
    size_t ns = (size_t)na->node_size;
    if (na->ndirs == 0) return;
    const diskarray_dir_t* first = na->dirs[0];
    const diskarray_dir_t* last = na->dirs[na->ndirs - 1];
    na->head = (char*)na->mins + (size_t)first->id[0] * ns;
    na->tail = (char*)na->maxs + (size_t)last->id[last->n - 1] * ns;
}

// Build the directory from ids[0..m) in key order (counts and maxs already set).
static int diskarray_build_directory(diskarray_t* na, const int* ids, int m) {
    size_t ns = (size_t)na->node_size;
    for (int t = 0; t < na->ndirs; t++) free(na->dirs[t]);
    na->ndirs = 0;
    int ndirs = m ? (m + DA_DIR_FILL - 1) / DA_DIR_FILL : 1;
    if (top_reserve(na, ndirs * 2 > 16 ? ndirs * 2 : 16) != 0) return -1;
    int k = 0;
    for (int t = 0; t < ndirs; t++) {
        diskarray_dir_t* d = dir_alloc(na);
        if (!d) return -1;
        int take = m / ndirs + (t < m % ndirs ? 1 : 0);
        long total = 0;
        for (int s = 0; s < take; s++, k++) {
            d->id[s] = ids[k];
            memcpy(dmax(na, d, s), (char*)na->maxs + (size_t)ids[k] * ns, ns);
            total += na->count[ids[k]];
        }
        d->n = take;
        na->dirs[t] = d;
        na->ndirs = t + 1;
        drebuild(na, t);
        na->tcnt[t] = total;
        memcpy(na->tmax + (size_t)t * ns, dmax(na, d, take - 1), ns);
    }
    tlt_rebuild(na);
    diskarray_refresh_ends(na);
    na->structure_gen++;
    na->count_gen++;
    return 0;
}

// The ids in key order, into a scratch array (room for `extra` more) the
// caller munmaps; *m entries.
static int* diskarray_order(diskarray_t* na, int extra, int* m, size_t* bytes) {
    int total = 0;
    for (int t = 0; t < na->ndirs; t++) total += na->dirs[t]->n;
    *bytes = ((size_t)total + (size_t)extra + 16) * sizeof(int);
    int* ids = mmap(NULL, *bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ids == MAP_FAILED) return NULL;
    int k = 0;
    for (int t = 0; t < na->ndirs; t++)
        for (int s = 0; s < na->dirs[t]->n; s++) ids[k++] = na->dirs[t]->id[s];
    *m = k;
    return ids;
}

// maxs[id] := last node of the extent's buffer, and the directory's copy at
// position h (pos_of[id], or the position a command resolved).
static inline void diskarray_set_max_at(diskarray_t* na, int id, int h, const void* buffer) {
    size_t ns = (size_t)na->node_size;
    const char* value = (const char*)buffer + (size_t)(na->count[id] - 1) * ns;
    memcpy((char*)na->maxs + (size_t)id * ns, value, ns);
    if (h < 0) return;
    int t = HANDLE_DIR(h), s = HANDLE_ENTRY(h);
    diskarray_dir_t* d = na->dirs[t];
    dset_max(na, d, s, value);
    if (s == d->n - 1) tset_max(na, t, value);
}

static inline void diskarray_set_max(diskarray_t* na, int id, const void* buffer) {
    diskarray_set_max_at(na, id, na->pos_of[id], buffer);
}

// The extent (id) a node belongs in: first extent whose max >= node, or the
// last; *h (if set) gets its directory position.
static inline int diskarray_find_extent_at(diskarray_t* na, const void* node, int* h) {
    if (na->ndirs == 0) return -1;
    int t = tfind(na, node);
    const diskarray_dir_t* d = na->dirs[t];
    int s = dfind(na, d, node);
    if (h) *h = HANDLE(t, s);
    return d->id[s];
}

static inline int diskarray_find_extent(diskarray_t* na, void* node) {
    return diskarray_find_extent_at(na, node, NULL);
}

// The extent (id) holding global index global_index, the offset in it, and
// (if h) its directory position.
static inline int diskarray_find_extent_by_index(diskarray_t* na, long global_index, long* local_index, int* h) {
    if (global_index < 0 || global_index >= (long)na->total_count) return -1;
    long rem;
    int t = tlt_find(na, global_index, &rem);
    if (t < 0) return -1;
    const diskarray_dir_t* d = na->dirs[t];
    int s = dlt_find(d, rem, local_index);
    if (s < 0 || s >= d->n) return -1;
    if (h) *h = HANDLE(t, s);
    return d->id[s];
}

///// DEXTENT MANAGEMENT ////////////////////

// Allocate a free dextent from the pool
static inline int diskarray_allocate_dextent(diskarray_t* na, int worker_id) {
    worker_thread_data_t* worker = &na->workers[worker_id];
    if (worker->dextent_count == 0) {
        printf("ERROR: No free dextents available for worker %d\n", worker_id);
        return -1;
    }
    int dextent_idx = worker->dextent_pool[worker->dextent_head];
    worker->dextent_head = (worker->dextent_head + 1) % worker->num_dextents;
    worker->dextent_count--;
    na->dextent_count--;
    return dextent_idx;
}

// Return a dextent to the free pool
static inline void diskarray_free_dextent(diskarray_t* na, int dextent_idx) {
    worker_thread_data_t* worker = &na->workers[dextent_idx % na->num_worker_threads];
    if (worker->dextent_count >= worker->num_dextents) {
        printf("ERROR: Dextent pool overflow\n");
        return;
    }
    worker->dextent_pool[worker->dextent_tail] = dextent_idx;
    worker->dextent_tail = (worker->dextent_tail + 1) % worker->num_dextents;
    worker->dextent_count++;
    na->dextent_count++;
}

///// MAIN <-> WORKER RINGS ////////////////////

// Pull finished requests off a worker's completion ring: free the old dextent
// of a copy-on-write flush and release the in-flight slot.  Only the main
// thread touches dextent pools, so there is no race with the worker.
static inline void diskarray_io_drain(diskarray_t* na, worker_thread_data_t* worker) {
    uint32_t tail = atomic_load_explicit(&worker->cpl_tail, memory_order_acquire);
    while (worker->cpl_head != tail) {
        const diskarray_done_t* d = &worker->cpl[worker->cpl_head % (uint32_t)na->num_max_bgio];
        if (d->old_dextent_idx >= 0) diskarray_free_dextent(na, d->old_dextent_idx);
        worker->cpl_head++;
        worker->inflight--;
    }
}

static inline void diskarray_io_drain_all(diskarray_t* na) {
    for (int i = 0; i < na->num_worker_threads; i++) diskarray_io_drain(na, &na->workers[i]);
}

// Hand a request to a worker.  The caller has checked inflight < num_max_bgio.
// The request is visible to the worker only after diskarray_io_publish().
static inline void diskarray_io_issue(diskarray_t* na, worker_thread_data_t* worker,
                                      int buffer_idx, int extent_idx, int dextent_idx, int old_dextent_idx) {
    diskarray_io_t* r = &worker->req[worker->req_count % (uint32_t)na->num_max_bgio];
    r->buffer_idx = buffer_idx;
    r->extent_idx = extent_idx;
    r->dextent_idx = dextent_idx;
    r->old_dextent_idx = old_dextent_idx;
    worker->req_count++;
    worker->inflight++;
}

// Make every issued request visible to its worker (one release store per
// worker that has something new).
static inline void diskarray_io_publish(diskarray_t* na) {
    for (int i = 0; i < na->num_worker_threads; i++) {
        worker_thread_data_t* w = &na->workers[i];
        if (atomic_load_explicit(&w->req_tail, memory_order_relaxed) != w->req_count)
            atomic_store_explicit(&w->req_tail, w->req_count, memory_order_release);
    }
}

///// I/O BUFFER MANAGEMENT ////////////////////

// Move buffer to end of LRU pool (mark as most recently used)
static inline void diskarray_mark_buffer_used(diskarray_t* na, int buffer_idx) {
    int worker_id = buffer_idx / na->num_io_buffers_per_worker;
    worker_thread_data_t* worker = &na->workers[worker_id];
    // Find buffer in pool
    int pos = -1;
    for (int i = 0; i < worker->num_io_buffers; i++) {
        if (worker->buffer_pool[i] == buffer_idx) {
            pos = i;
            break;
        }
    }

    if (pos == -1) return; // Buffer not in pool (shouldn't happen)

    // Shift everything left to fill the gap using memmove
    memmove(&worker->buffer_pool[pos], &worker->buffer_pool[pos + 1], 
            (worker->num_io_buffers - pos - 1) * sizeof(int));

    // Put this buffer at the end (most recently used)
    worker->buffer_pool[worker->num_io_buffers - 1] = buffer_idx;
}

// Mark buffer as dirty
static inline void diskarray_mark_dirty(diskarray_t* na, int buffer_idx) {
    if (!na->dirty_bits[buffer_idx]) {
        na->dirty_bits[buffer_idx] = true;
        na->num_dirty_buffers++;
    }
}

// Find an available I/O buffer using LRU policy
static inline int diskarray_get_free_buffer(diskarray_t* na, int worker_id) {
    int first_unused = -1;
    int first_flush = -1;
    int first_prefetch = -1;
    int first_mapped = -1;
    int check = INT_MIN;
    worker_thread_data_t* worker = &na->workers[worker_id];

    // Try up to 3 times with different strategies
    for (int attempt = 0; attempt < 3; attempt++) {
        first_unused = -1;
        first_flush = -1;
        first_prefetch = -1;
        first_mapped = -1;

        // Loop through all buffers looking for candidates
        for (int i = 0; i < worker->num_io_buffers; i++) {
            int buffer_idx = worker->buffer_pool[i];
            int buffer_state = na->buffer_to_extent[buffer_idx];

            if (buffer_state == BUFFER_UNUSED) {
                first_unused = buffer_idx;
                break; // Found unused, use it immediately
            } else if (buffer_state == BUFFER_FLUSHED && first_flush == -1) {
                first_flush = buffer_idx;
                break;
            } else if (buffer_state == BUFFER_PREFETCHED && first_prefetch == -1) {
                first_prefetch = buffer_idx;
                break;
            }
            // buffer_state >= 0 means mapped right now (unmap always leaves a
            // negative state): someone holds a pointer into it, so it is never
            // a candidate.  map_extent waits and retries when nothing is free.
        }

        // If we found at least one candidate, break
        check = first_unused+first_flush+first_prefetch+first_mapped;
        if (first_unused != -1 || first_flush != -1 || first_prefetch != -1 || first_mapped != -1) {
            break;
        }

        // Strategy based on attempt number
        if (attempt == 0) {
            // Workers handle completion processing automatically
        } else if (attempt == 1) {
            return DISKARRAY_IO_ERROR_NO_BUFFER; // Don't flush if not allowed
        }
    }

    // No buffers available
    if (check < -3) {
        return DISKARRAY_IO_ERROR_NO_BUFFER;
    }

    // Return the best available buffer
    int buffer_idx = -1;
    if (first_unused != -1) {
        buffer_idx = first_unused;
    } else if (first_flush != -1) {
        buffer_idx = first_flush;
    } else if (first_prefetch != -1) {
        buffer_idx = first_prefetch;
    } else if (first_mapped != -1) {
        buffer_idx = first_mapped;
    } else {
        printf("ERROR: diskarray_get_free_buffer: Should never get here\n");
        exit(1);
    }

    // Clear mappings
    int extent_idx = na->buffer_to_extent[buffer_idx];
    if (extent_idx >= 0) {
        na->extent_to_buffer[extent_idx] = BUFFER_UNUSED;
    }
    extent_idx = na->buffer_to_extent_io[buffer_idx];
    if (extent_idx >= 0) {
        na->extent_to_buffer_io[extent_idx] = BUFFER_UNUSED;
    }
    na->buffer_to_extent[buffer_idx] = BUFFER_UNUSED;
    na->buffer_to_extent_io[buffer_idx] = BUFFER_UNUSED;
    diskarray_mark_buffer_used(na, buffer_idx);
    // printf("MAIN: get_free_buffer Worker %d buffer %d num_io_buffers_per_worker %d\n", worker_id, buffer_idx, na->num_io_buffers_per_worker);
    return buffer_idx;
}

// Submit to all workers and wait for all IO to complete
int diskarray_submit_and_wait_all(diskarray_t* na) {
    if (!na || !na->workers) return -1;
    diskarray_io_publish(na);
    for (long spins = 0; spins < 100000000L; spins++) {
        diskarray_io_drain_all(na);
        bool idle = true;
        for (int i = 0; i < na->num_worker_threads && idle; i++)
            if (na->workers[i].inflight != 0) idle = false;
        if (idle) return 0;
        __builtin_ia32_pause();
    }
    return -1;
}

// Flush all dirty buffers to disk
int diskarray_flush_dirty_buffers(diskarray_t* na) {
    printf("diskarray_flush_dirty_buffers: Before");
    diskarray_debug_buffers(na);
    for (int i = 0; i < na->num_io_buffers; i++) {
        // If the dirty bit is not set, skip
        if (!na->dirty_bits[i]) {
            continue;
        }

        // Get the current extent mapping
        int extent_idx = na->buffer_to_extent[i];
        if (extent_idx < 0) {
            continue; // Buffer not mapped to an extent
        }

        // Mark buffer as flushable and update mappings
        na->buffer_to_extent[i] = BUFFER_FLUSHABLE;
        na->extent_to_buffer[extent_idx] = BUFFER_UNUSED;
        na->extent_to_buffer_io[extent_idx] = i;
        na->buffer_to_extent_io[i] = extent_idx;
        na->flushable++;

        // Workers handle completion processing automatically
    }
    diskarray_flush_buffers(na, true);
    // Workers handle all completion processing
    printf("diskarray_flush_dirty_buffers: After");
    diskarray_debug_buffers(na);
    return 0;
}


///// PREFETCH FUNCTIONS ////////////////////

///// BUFFER I/O QUEUE FUNCTIONS ////////////////////

// Prefetch an extent from disk into a buffer (routes to worker thread)
static inline int diskarray_prefetch_extent(diskarray_t* na, int extent_idx, bool submit) {
    // Already in a buffer, or already on its way?  Then whoever asked is
    // about to use it: move it to the most-recently-used end, so the buffers
    // evicted for later prefetches are ones whose commands already ran (the
    // pool's order was assignment order only, and a hot buffer a queued
    // command was counting on got read again while that command executed).
    int buffer_idx = na->extent_to_buffer[extent_idx];
    if (buffer_idx < 0) buffer_idx = na->extent_to_buffer_io[extent_idx];
    if (buffer_idx >= 0) {
        diskarray_mark_buffer_used(na, buffer_idx);
        return buffer_idx;
    }

    // The dextent decides the worker (and so the drive).  A new extent gets a
    // dextent from the pool of the worker extent_idx % workers.
    int dextent_idx = na->extent_to_dextent[extent_idx];
    int worker_id;
    if (dextent_idx < 0) {
        worker_id = extent_idx % na->num_worker_threads;
        dextent_idx = diskarray_allocate_dextent(na, worker_id);
        if (dextent_idx < 0) {
            printf("diskarray_prefetch_extent: no dextent for extent %d\n", extent_idx);
            return DISKARRAY_IO_ERROR_NO_BUFFER;
        }
        na->extent_to_dextent[extent_idx] = dextent_idx;
    } else {
        worker_id = dextent_idx % na->num_worker_threads;
    }
    worker_thread_data_t* worker = &na->workers[worker_id];

    diskarray_io_drain(na, worker);
    if (worker->inflight >= na->num_max_bgio) return DISKARRAY_IO_ERROR_QUEUE_FULL;

    buffer_idx = diskarray_get_free_buffer(na, worker_id);
    if (buffer_idx < 0) return DISKARRAY_IO_ERROR_NO_BUFFER;

    na->buffer_to_extent[buffer_idx] = BUFFER_PREFETCHING;
    na->buffer_to_extent_io[buffer_idx] = extent_idx;
    na->extent_to_buffer_io[extent_idx] = buffer_idx;

    diskarray_io_issue(na, worker, buffer_idx, extent_idx, dextent_idx, -1);
    if (submit) diskarray_io_publish(na);
    return DISKARRAY_IO_SUCCESS;
}

// Flush buffers that are in BUFFER_FLUSHABLE state
static inline int diskarray_flush_buffers(diskarray_t* na, bool force) {
    int flushed = 0;
    for (int worker_id = 0; worker_id < na->num_worker_threads; worker_id++) {
        worker_thread_data_t* worker = &na->workers[worker_id];
        diskarray_io_drain(na, worker);
        for (int i = 0; i < worker->num_io_buffers; i++) {
            int buffer_idx = worker->buffer_start_idx + i;
            if (na->buffer_to_extent[buffer_idx] != BUFFER_FLUSHABLE) continue;
            int extent_idx = na->buffer_to_extent_io[buffer_idx];
            if (extent_idx < 0) {
                printf("ERROR: Flushable buffer %d has no extent mapping\n", buffer_idx);
                exit(1);
            }
            if (worker->inflight >= na->num_max_bgio) break;   // try again later

            // Copy-on-write: write to a fresh dextent; the old one is freed
            // when the completion is drained.
            int old_dextent_idx = na->extent_to_dextent[extent_idx];
            int new_dextent_idx = diskarray_allocate_dextent(na, worker_id);
            if (new_dextent_idx < 0) {
                printf("ERROR: No free dextents available for flush - worker %d\n", worker_id);
                exit(1);
            }
            na->buffer_to_extent[buffer_idx] = BUFFER_FLUSHING;
            na->extent_to_dextent[extent_idx] = new_dextent_idx;
            diskarray_io_issue(na, worker, buffer_idx, extent_idx, new_dextent_idx, old_dextent_idx);
            flushed++;
        }
    }
    if (force) diskarray_io_publish(na);
    return flushed;
}

// REMOVED: diskarray_process_bgio - All I/O completion handled by worker threads

///// WORKER THREAD FUNCTIONS ////////////////////

// Worker-specific completion processing
static inline void diskarray_worker_process_completions(diskarray_t* na, worker_thread_data_t* worker) {
    struct io_uring_cqe* cqe;
    unsigned head, n = 0;
    io_uring_for_each_cqe(&worker->ring, head, cqe) {
        // ops[] is indexed by the worker-local buffer index: one I/O per buffer.
        const diskarray_io_t r = worker->ops[(uint32_t)io_uring_cqe_get_data64(cqe)];
        int state = na->buffer_to_extent[r.buffer_idx];
        if (state != BUFFER_PREFETCHING && state != BUFFER_FLUSHING) {
            printf("ERROR: Worker %d completion for buffer %d extent %d in invalid state %d\n",
                   worker->worker_id, r.buffer_idx, r.extent_idx, state);
            exit(1);
        }
        if (cqe->res != (__s32)na->extent_disk_size) {
            printf("ERROR: Worker %d %s of buffer %d extent %d dextent %d returned %d (%s)\n",
                   worker->worker_id, r.old_dextent_idx >= 0 ? "write" : "read", r.buffer_idx,
                   r.extent_idx, r.dextent_idx, cqe->res, cqe->res < 0 ? strerror(-cqe->res) : "short");
            exit(1);
        }
        // Publish the data: these plain stores are ordered before the release
        // store of the state, which is what the main thread spins on.
        na->extent_to_buffer_io[r.extent_idx] = r.buffer_idx;
        na->dirty_bits[r.buffer_idx] = false;
        atomic_store_explicit(&na->buffer_to_extent[r.buffer_idx],
                              state == BUFFER_PREFETCHING ? BUFFER_PREFETCHED : BUFFER_FLUSHED,
                              memory_order_release);
        diskarray_done_t* d = &worker->cpl[(worker->cpl_count + n) % (uint32_t)na->num_max_bgio];
        d->buffer_idx = r.buffer_idx;
        d->old_dextent_idx = r.old_dextent_idx;
        n++;
        worker->io_completions++;
    }
    if (n) {
        io_uring_cq_advance(&worker->ring, n);
        worker->outstanding -= (int)n;
        worker->cpl_count += n;
        atomic_store_explicit(&worker->cpl_tail, worker->cpl_count, memory_order_release);
    }
}

// Queue I/O operations from worker's pending queue to io_uring
// Turn newly published requests into SQEs and submit them.  Returns how many.
static inline int diskarray_worker_queue_io(diskarray_t* na, worker_thread_data_t* worker) {
    uint32_t tail = atomic_load_explicit(&worker->req_tail, memory_order_acquire);
    int n = 0;
    while (worker->req_head != tail) {
        // Copy the request out of the shared ring into this buffer's slot of ops[];
        // the ring entry may be reused as soon as req_head moves on.
        const diskarray_io_t* r = &worker->req[worker->req_head % (uint32_t)na->num_max_bgio];
        int local = r->buffer_idx - worker->buffer_start_idx;
        worker->ops[local] = *r;
        int drive = r->dextent_idx % na->num_drives;
        off_t offset = (off_t)(r->dextent_idx / na->num_drives) * (off_t)na->extent_disk_size;
        struct io_uring_sqe* sqe = io_uring_get_sqe(&worker->ring);   // ring holds num_max_bgio entries
        if (r->old_dextent_idx >= 0)
            io_uring_prep_write_fixed(sqe, drive, na->io_buffers[r->buffer_idx], na->extent_disk_size, offset, local);
        else
            io_uring_prep_read_fixed(sqe, drive, na->io_buffers[r->buffer_idx], na->extent_disk_size, offset, local);
        sqe->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data64(sqe, (uint64_t)local);
        worker->req_head++;
        n++;
    }
    if (n) {
        int ret = io_uring_submit(&worker->ring);
        if (ret < 0) {
            printf("ERROR: Worker %d io_uring_submit failed: %s\n", worker->worker_id, strerror(-ret));
            exit(1);
        }
        worker->outstanding += n;
    }
    return n;
}

// High-performance worker thread main loop
static int diskarray_worker_setup(diskarray_t* na, worker_thread_data_t* worker) {
    unsigned entries = na->num_max_bgio < 8 ? 8 : (unsigned)na->num_max_bgio;
    int ret = io_uring_queue_init(entries, &worker->ring, IORING_SETUP_SINGLE_ISSUER);
    if (ret < 0) {
        printf("ERROR: Worker %d io_uring_queue_init: %s\n", worker->worker_id, strerror(-ret));
        return -1;
    }
    io_uring_register_ring_fd(&worker->ring);
    ret = io_uring_register_files(&worker->ring, na->disk_fds, (unsigned)na->num_drives);
    if (ret < 0) {
        printf("ERROR: Worker %d io_uring_register_files: %s\n", worker->worker_id, strerror(-ret));
        io_uring_queue_exit(&worker->ring);
        return -1;
    }
    struct iovec* iov = calloc((size_t)worker->num_io_buffers, sizeof *iov);
    if (!iov) { io_uring_queue_exit(&worker->ring); return -1; }
    for (int j = 0; j < worker->num_io_buffers; j++) {
        iov[j].iov_base = na->io_buffers[worker->buffer_start_idx + j];
        iov[j].iov_len = na->extent_disk_size;
    }
    ret = io_uring_register_buffers(&worker->ring, iov, (unsigned)worker->num_io_buffers);
    free(iov);
    if (ret < 0) {
        printf("ERROR: Worker %d io_uring_register_buffers: %s\n", worker->worker_id, strerror(-ret));
        io_uring_queue_exit(&worker->ring);
        return -1;
    }
    return 0;
}

// Worker thread: spin on the request ring, submit, reap, repeat.
static void* diskarray_worker_thread_main(void* arg) {
    worker_thread_data_t* worker = arg;
    diskarray_t* na = worker->na_ptr;
    if (diskarray_worker_setup(na, worker) != 0) {
        worker->errors++;
        atomic_store(&worker->status, WORKER_FAILED);
        return NULL;
    }
    atomic_store(&worker->status, WORKER_READY);
    for (;;) {
        int busy = diskarray_worker_queue_io(na, worker);
        diskarray_worker_process_completions(na, worker);
        if (busy) continue;
        if (worker->outstanding == 0 && atomic_load_explicit(&worker->shutdown, memory_order_acquire))
            break;
        __builtin_ia32_pause();
    }
    io_uring_queue_exit(&worker->ring);
    return NULL;
}

///// EXTENT RESOLUTION FUNCTIONS ////////////////////

// Ensure extent is loaded in memory and return buffer pointer
static inline int diskarray_map_extent(diskarray_t* na, void** io_buffer, int extent_idx) {
    int buffer_state = -1;

    // Either get the buffer idx or start prefetching
    int buffer_idx = diskarray_prefetch_extent(na, extent_idx, true);

    // Extent does not have a buffer assigned
    if (buffer_idx < 0) {
        // We just prefetched the extent, workers will handle submission
        if (buffer_idx == DISKARRAY_IO_SUCCESS) {
            na->stat_sync_loads++;
            // Workers handle submission automatically, just get buffer index
            buffer_idx = na->extent_to_buffer_io[extent_idx];
        } else if (buffer_idx == DISKARRAY_IO_ERROR_NO_BUFFER || buffer_idx == DISKARRAY_IO_ERROR_QUEUE_FULL) {
            // Every buffer is busy: push the dirty ones out and wait for I/O to
            // free something up.  Time-based, since a slow disk needs longer than
            // any spin count (the drives finish in microseconds, a file may not).
            struct timespec t0, t;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            diskarray_flush_buffers(na, true);
            do {
                buffer_idx = diskarray_prefetch_extent(na, extent_idx, true);
                if (buffer_idx == DISKARRAY_IO_SUCCESS) break;
                __builtin_ia32_pause();
                clock_gettime(CLOCK_MONOTONIC, &t);
            } while ((t.tv_sec - t0.tv_sec) * 1000000000L + (t.tv_nsec - t0.tv_nsec) < 30000000000L);
            if (buffer_idx != DISKARRAY_IO_SUCCESS) {
                printf("diskarray_map_extent: waiting on io failed %d\n", buffer_idx);
                diskarray_debug_buffers(na);
                diskarray_debug_queue(na);
                exit(1);
            }
            buffer_idx = na->extent_to_buffer_io[extent_idx];
        } else {
            // Unexpected error
            printf("diskarray_map_extent: Unexpected error: %d\n", buffer_idx);
            exit(1);
        }
    }

    buffer_state = na->buffer_to_extent[buffer_idx];

    // Wait for any pending IO to complete
    if (buffer_state == BUFFER_PREFETCHING || buffer_state == BUFFER_FLUSHING) {
        // Let's flush the buffers while we're doing IO
        diskarray_flush_buffers(na, false);
        diskarray_io_publish(na);

        int loopcnt = 0;
        do {
            // trigger submit
            // Workers handle completion processing automatically
            buffer_state = atomic_load_explicit(&na->buffer_to_extent[buffer_idx], memory_order_acquire);
            //__builtin_ia32_pause();
            loopcnt++;
        } while (buffer_state != BUFFER_FLUSHED && buffer_state != BUFFER_PREFETCHED);
    }

    // If extent is in a buffer but not mapped lets do that now
    if (buffer_state != extent_idx) {
        if (buffer_state == BUFFER_FLUSHABLE) {
            na->flushable--;
        } else if (buffer_state != BUFFER_PREFETCHED && buffer_state != BUFFER_FLUSHED) {
            diskarray_debug_buffers(na);
            diskarray_debug_queue(na);
            printf("diskarray_map_extent: Invalid buffer_state %i, buffer_idx %i, extent_idx %i\n", buffer_state, buffer_idx, extent_idx);
            exit(1);
        }
        na->buffer_to_extent[buffer_idx] = extent_idx;
        na->extent_to_buffer[extent_idx] = buffer_idx;
    }
    na->extent_to_buffer_io[extent_idx] = BUFFER_MAPPED;
    na->buffer_to_extent_io[buffer_idx] = BUFFER_MAPPED;


    *io_buffer = na->io_buffers[buffer_idx];
    return DISKARRAY_IO_SUCCESS;
}

// hack to allow us to keep diskarray_map_extent() static inline, but still use it in _debug.c
int diskarray_map_extent_exportable(diskarray_t* na, void** io_buffer, int extent_idx) {
    return diskarray_map_extent(na, io_buffer, extent_idx);
}

static inline void diskarray_unmap_buffer(diskarray_t* na, void** buffer) {
    if (*buffer == NULL) {
        return;
    }
    // get buffer_idx from location
    int buffer_idx = (int)(((char*)*buffer - (char*)na->buffer) / na->buffer_stride);
    if (buffer_idx < 0 || buffer_idx >= na->num_io_buffers) {
        printf("diskarray_unmap_buffer: buffer_idx %i out of range\n", buffer_idx);
        exit(1);
    }

    // Get the current extent mapping
    int extent_idx = na->buffer_to_extent[buffer_idx];
    if (extent_idx < 0 || extent_idx >= na->num_extents) {
        printf("diskarray_unmap_buffer: extent_idx %i for buffer_idx %i out of range\n", extent_idx, buffer_idx);
        exit(1);
    }

    if (na->dirty_bits[buffer_idx] && na->count[extent_idx] == 0) {
        // An empty extent has nothing worth writing: drop the buffer and every
        // mapping to it, so the extent never points at an UNUSED buffer.
        na->dirty_bits[buffer_idx] = false;
        na->num_dirty_buffers--;
        na->buffer_to_extent[buffer_idx] = BUFFER_UNUSED;
        na->buffer_to_extent_io[buffer_idx] = BUFFER_UNUSED;
        na->extent_to_buffer[extent_idx] = BUFFER_UNUSED;
        na->extent_to_buffer_io[extent_idx] = BUFFER_UNUSED;
        *buffer = NULL;
        return;
    }
    if (na->dirty_bits[buffer_idx]) {
        na->buffer_to_extent[buffer_idx] = BUFFER_FLUSHABLE;
        na->flushable++;
    } else {
        na->buffer_to_extent[buffer_idx] = BUFFER_PREFETCHED;
    }




    // Mark buffer as flushable and update mappings
    if (extent_idx == BUFFER_UNUSED) {
        *buffer = NULL;
        return;
    }
    na->extent_to_buffer[extent_idx] = BUFFER_UNUSED;
    na->extent_to_buffer_io[extent_idx] = buffer_idx;
    na->buffer_to_extent_io[buffer_idx] = extent_idx;
    *buffer = NULL;
}

// hack to allow us to keep diskarray_unmap_buffer static inline, but still use it in _debug.c
void diskarray_unmap_buffer_exportable(diskarray_t* na, void** buffer) {
    diskarray_unmap_buffer(na, buffer);
}

// Inserts or deletes a value in a sorted array

// Update extents count, adds or subtracts from the count of an extent
// Extent id gained or lost delta nodes: its count, the total, and the two
// count trees on its path (its directory extent's and the top level's).
// h is its directory position (pos_of[id], or what a command resolved).
static inline void diskarray_extent_update_count_at(diskarray_t* na, int extent_idx, int h, int delta) {
    if (h < 0) return;
    int t = HANDLE_DIR(h), s = HANDLE_ENTRY(h);
    na->count[extent_idx] += delta;
    na->total_count = (size_t)((long)na->total_count + delta);
    na->count_gen++;
    dlt_update(na->dirs[t], s, delta);
    tlt_update(na, t, delta);
}

static inline void diskarray_extent_update_count(diskarray_t* na, int extent_idx, int delta) {
    diskarray_extent_update_count_at(na, extent_idx, na->pos_of[extent_idx], delta);
}

#define DISKARRAY_SET_MIN(na_, idx_, buffer_) \
  memcpy((char*)na_->mins + idx_ * na_->node_size, buffer_, na_->node_size)

#define DISKARRAY_SET_MAX(na_, idx_, buffer_) diskarray_set_max(na_, idx_, buffer_)

// Directory extent t is full: its upper half goes into a new one after it.
static int diskarray_split_dir(diskarray_t* na, int t) {
    size_t ns = (size_t)na->node_size;
    if (na->ndirs + 1 > na->dirs_cap && top_reserve(na, na->dirs_cap * 2) != 0) return -1;
    diskarray_dir_t* d = na->dirs[t];
    diskarray_dir_t* nd = dir_alloc(na);
    if (!nd) return -1;
    int h = d->n / 2, moved = d->n - h;
    memcpy(nd->id, &d->id[h], (size_t)moved * sizeof(int));
    memcpy(nd->max, dmax(na, d, h), (size_t)moved * ns);
    nd->n = moved;
    d->n = h;
    long ncount = 0;
    for (int i = 0; i < moved; i++) ncount += na->count[nd->id[i]];

    int tail = na->ndirs - t - 1;
    memmove(&na->dirs[t + 2], &na->dirs[t + 1], (size_t)tail * sizeof(void*));
    memmove(&na->tcnt[t + 2], &na->tcnt[t + 1], (size_t)tail * sizeof(long));
    memmove(na->tmax + (size_t)(t + 2) * ns, na->tmax + (size_t)(t + 1) * ns, (size_t)tail * ns);
    na->dirs[t + 1] = nd;
    na->tcnt[t + 1] = ncount;
    na->tcnt[t] -= ncount;
    memcpy(na->tmax + (size_t)(t + 1) * ns, na->tmax + (size_t)t * ns, ns);
    memcpy(na->tmax + (size_t)t * ns, dmax(na, d, h - 1), ns);
    na->ndirs++;
    // every directory extent after t moved: refresh their ids' handles
    for (int u = t; u < na->ndirs; u++) drebuild(na, u);
    tlt_rebuild(na);
    return 0;
}

// A new, empty extent right after extent id `after` in key order: an unused
// id from free_ids.  Only after's directory extent shifts.  Returns the id.
static inline int diskarray_insert_extent(diskarray_t* na, int after) {
    if (na->num_empty_extents == 0) {
        printf("can't find an empty extent after %i\n", after);
        diskarray_debug_stats(na);
        diskarray_debug_buffers(na);
        exit(1);
    }
    size_t ns = (size_t)na->node_size;
    int h = na->pos_of[after];
    int t = HANDLE_DIR(h), s = HANDLE_ENTRY(h);
    if (na->dirs[t]->n == DA_DIR_CAP) {
        if (diskarray_split_dir(na, t) != 0) {
            printf("diskarray_insert_extent: out of memory\n");
            exit(1);
        }
        h = na->pos_of[after];
        t = HANDLE_DIR(h);
        s = HANDLE_ENTRY(h);
    }
    diskarray_dir_t* d = na->dirs[t];
    int id = na->free_ids[--na->num_empty_extents];
    int tail = d->n - s - 1;
    memmove(&d->id[s + 2], &d->id[s + 1], (size_t)tail * sizeof(int));
    memmove(dmax(na, d, s + 2), dmax(na, d, s + 1), (size_t)tail * ns);
    d->id[s + 1] = id;
    memcpy(dmax(na, d, s + 1), dmax(na, d, s), ns);   // a valid separator until set_max
    d->n++;
    na->count[id] = 0;
    drebuild(na, t);
    na->structure_gen++;
    na->count_gen++;
    return id;
}

// After counts moved between extents of one directory extent directly
// (a split), rebuild that directory extent's count tree.
static inline void diskarray_recount(diskarray_t* na, int id) {
    int h = na->pos_of[id];
    if (h >= 0) dlt_rebuild(na, na->dirs[HANDLE_DIR(h)]);
}

// Only used by diskarray_balance to copy data between extents
static int diskarray_balance_copy_helper(diskarray_t* na, int start_index, int count, int righthanded, int a_idx, int b_idx) {
    // Validate indices
    if (a_idx < 0 || a_idx >= na->num_extents || b_idx < 0 || b_idx >= na->num_extents) {
        return -1;
    }

    void *dst_data;
    void *src_data;

    // Check if destination extent has enough capacity
    if ((na->extent_size - na->count[b_idx]) < count && na->count[b_idx] > 0) {
        printf("target extent %d capacity insufficient: available=%d, needed=%d\n", 
               b_idx, na->extent_size - na->count[b_idx], count);
        return -1;
    }

    // Map both extents into memory
    void* a_buffer = NULL;
    void* b_buffer = NULL;
    int result_a = diskarray_map_extent(na, &a_buffer, a_idx);
    int result_b = diskarray_map_extent(na, &b_buffer, b_idx);
    
    if (result_a != DISKARRAY_IO_SUCCESS || result_b != DISKARRAY_IO_SUCCESS) {
        printf("diskarray_balance_copy_helper: Failed to map extents %d or %d\n", a_idx, b_idx);
        return -1;
    }

    // If destination has data and we're inserting right-handed, make space
    if (na->count[b_idx] > 0 && righthanded) {
        dst_data = (char*)b_buffer + (na->node_size * count);
        src_data = b_buffer;
        memmove(dst_data, src_data, na->node_size * na->count[b_idx]);
    }

    // Copy data from source to destination
    if (righthanded) {
        dst_data = b_buffer;
        src_data = (char*)a_buffer + (start_index * na->node_size);
    } else {
        dst_data = (char*)b_buffer + (na->node_size * na->count[b_idx]);
        src_data = a_buffer;
    }
    memcpy(dst_data, src_data, na->node_size * count);

    // Update counts
    na->count[a_idx] -= count;
    na->count[b_idx] += count;

    // If source still has data and we removed from the beginning, shift remaining data
    if (na->count[a_idx] > 0 && !righthanded) {
        dst_data = a_buffer;
        src_data = (char*)a_buffer + (na->node_size * count);
        memmove(dst_data, src_data, na->node_size * na->count[a_idx]);
    }

    // Mark both buffers as dirty since we modified them
    int a_buffer_idx = na->extent_to_buffer[a_idx];
    int b_buffer_idx = na->extent_to_buffer[b_idx];
    if (a_buffer_idx >= 0) diskarray_mark_dirty(na, a_buffer_idx);
    if (b_buffer_idx >= 0) diskarray_mark_dirty(na, b_buffer_idx);

    // Update min/max for changed extents 
    DISKARRAY_SET_MIN(na, a_idx, a_buffer);
    DISKARRAY_SET_MAX(na, a_idx, a_buffer);
    DISKARRAY_SET_MIN(na, b_idx, b_buffer);
    DISKARRAY_SET_MAX(na, b_idx, b_buffer);

    diskarray_unmap_buffer(na, &a_buffer);
    diskarray_unmap_buffer(na, &b_buffer);

    return 0;
}

int diskarray_dump_nodes_to_file(diskarray_t* na, const char* file) {
    FILE* fp = fopen(file, "wb");
    if (fp == NULL) {
        printf("Failed to open file %s\n", file);
        return -1;
    }

    printf("Dumping %ld nodes to file %s\n", na->total_count, file);

    // Clean up our IOs before we move on
    if (diskarray_submit_and_wait_all(na) != 0) {
        printf("diskarray_balance: Failed to wait for all IO to complete\n");
        exit(1);
    }

    // the extents in key order
    int non_empty_extents;
    size_t order_bytes;
    int* order = diskarray_order(na, 0, &non_empty_extents, &order_bytes);
    if (!order) { fclose(fp); return -1; }

    // we need at least 1 buffer to be active
    int bgio_count = (na->num_io_buffers - 1 > na->num_data_extents ? na->num_data_extents : na->num_io_buffers - 3);
    if (bgio_count > non_empty_extents) bgio_count = non_empty_extents;

    int prefetched = 0;
    // prefetch a bunch of reads
    for (int i = 0; i < bgio_count; i++) {
        diskarray_prefetch_extent(na, order[i], false);
        prefetched++;
    }
    diskarray_io_publish(na);
    size_t buffer_size = 8 * 1024 * 1024;
    void* buffer = malloc(buffer_size);
    size_t buffer_offset = 0;

    for (int i = 0; i < non_empty_extents; i++) {
        if (na->count[order[i]] == 0) {
            // shouldn't get here.
            printf("Skipping empty extent %d/%d\n", i, non_empty_extents);
            continue; // Skip empty extents
        }

        void* io_buffer;
        int result = diskarray_map_extent(na, &io_buffer, order[i]);
        if (result != DISKARRAY_IO_SUCCESS) {
            printf("Failed to load extent %d: error %d\n", i, result);
            fclose(fp);
            return -1;
        }

        // write the chunk and start the buffer over if we can't fit this extent
        if ((buffer_size - buffer_offset) < (size_t)(na->node_size * na->count[order[i]])) {
            size_t written = fwrite(buffer, 1, buffer_offset, fp);
            if (written != (size_t)buffer_offset) {
                printf("Failed to write to file %s - %zu bytes written of %zu\n", file, written, buffer_offset);
                fclose(fp);
                return -1;
            }
            buffer_offset = 0;
        }

        memcpy(buffer + buffer_offset, io_buffer, na->node_size * na->count[order[i]]);
        buffer_offset += na->node_size * na->count[order[i]];

        diskarray_unmap_buffer(na, &io_buffer);

        // refresh our prefetch queue
        if ((prefetched - i) <= bgio_count/4) {
            int to_prefetch = bgio_count - (prefetched - i);
            to_prefetch = to_prefetch + i >= non_empty_extents ? non_empty_extents - i - 1 : to_prefetch;
            for (int j = 0; j < to_prefetch; j++) {
                diskarray_prefetch_extent(na, order[i+j], false);
            }
            prefetched += to_prefetch;
            diskarray_io_publish(na);
        }
    }
    // write any remaining data to file
    size_t written = fwrite(buffer, 1, buffer_offset, fp);
    if (written != (size_t)buffer_offset) {
        printf("Failed to write to file %s - %zu bytes written of %zu\n", file, written, buffer_offset);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    munmap(order, order_bytes);
    return 0;
}

//// MAIN API ////////////////////////

// Search for a node in the diskarray
static int diskarray_search_in(diskarray_t* na, int extent_idx, void* search_key, void* result);
int diskarray_search(diskarray_t* na, void* search_key, void* result) {
    int extent_idx = diskarray_find_extent(na, search_key);
    if (extent_idx < 0) return -1;
    return diskarray_search_in(na, extent_idx, search_key, result);
}

// Search extent_idx (already resolved) for search_key.
static int diskarray_search_in(diskarray_t* na, int extent_idx, void* search_key, void* result) {

    // Ensure extent is loaded
    void* buffer = NULL;
    int errcode = diskarray_map_extent(na, &buffer, extent_idx);
    if (errcode != DISKARRAY_IO_SUCCESS) {
        printf("errcode: %d\n", errcode);
        return errcode; // I/O error
    }

    size_t ns = (size_t)na->node_size, cnt = (size_t)na->count[extent_idx];
    size_t pos = lower_bound(buffer, cnt, ns, search_key, na->compare);
    if (pos >= cnt || diskarray_cmp(na, (char*)buffer + pos * ns, search_key) != 0) {
        diskarray_unmap_buffer(na, &buffer);
        return -3; // Not found
    }
    memcpy(result, (char*)buffer + pos * ns, ns);
    diskarray_unmap_buffer(na, &buffer);
    return 0; // Success
}

// Get the node at a specific index
static int diskarray_at_in(diskarray_t* na, int extent_idx, long local_index, void* result);

// Resolve a global index to (extent, offset); -1 empty, -2 out of range, -3 not found.
static int diskarray_locate_index(diskarray_t* na, long index, int* extent_idx, long* local_index, int* h) {
    long total_count = (long)na->total_count;
    if (total_count == 0) return -1;
    if (index < 0) index += total_count;
    if (index < 0 || index >= total_count) return -2;
    *extent_idx = diskarray_find_extent_by_index(na, index, local_index, h);
    if (*extent_idx < 0 || na->count[*extent_idx] == 0) return -3;
    return 0;
}

int diskarray_at(diskarray_t* na, long index, void* result) {
    int extent_idx;
    long local_index;
    int rc = diskarray_locate_index(na, index, &extent_idx, &local_index, NULL);
    if (rc != 0) return rc;
    return diskarray_at_in(na, extent_idx, local_index, result);
}

// Copy out node local_index of extent_idx (already resolved).
static int diskarray_at_in(diskarray_t* na, int extent_idx, long local_index, void* result) {

    // Ensure extent is loaded
    void* buffer = NULL;
    int errcode = diskarray_map_extent(na, &buffer, extent_idx);
    if (errcode != DISKARRAY_IO_SUCCESS) {
        return errcode; // I/O error
    }

    void* node_ptr = (char*)buffer + (local_index * na->node_size);
    memcpy(result, node_ptr, na->node_size);
    diskarray_unmap_buffer(na, &buffer);
    return 0; // Success
}

// Insert a node into the diskarray
static int diskarray_insert_in(diskarray_t* na, int extent_idx, int h, void* node);
int diskarray_insert(diskarray_t* na, void* node) {
    int h, extent_idx = diskarray_find_extent_at(na, node, &h);
    if (extent_idx < 0) return -1;
    return diskarray_insert_in(na, extent_idx, h, node);
}

// Insert node into extent_idx (already resolved), splitting it if full.
// h: extent_idx's directory position, or -1 to look it up.
static int diskarray_insert_in(diskarray_t* na, int extent_idx, int h, void* node) {

    // Ensure extent is loaded
    void* buffer = NULL;
    int errcode = diskarray_map_extent(na, &buffer, extent_idx);
    if (errcode != DISKARRAY_IO_SUCCESS) {
        return errcode; // I/O error
    }

    int buffer_idx = na->extent_to_buffer[extent_idx];

    // Check if extent will overflow, split if it will
    if (na->count[extent_idx] >= na->extent_size) {
        int split_point = na->count[extent_idx] / 2;
        int new_extent_idx = diskarray_insert_extent(na, extent_idx);

        // Ensure new extent buffer is available
        void* new_buffer = NULL;
        errcode = diskarray_map_extent(na, &new_buffer, new_extent_idx);
        if (errcode != DISKARRAY_IO_SUCCESS) {
            return errcode; // I/O error
        }
        int new_buffer_idx = na->extent_to_buffer[new_extent_idx];

        // Move upper half to new extent
        memcpy(new_buffer, 
               (char*)buffer + (split_point * na->node_size), 
               (na->count[extent_idx] - split_point) * na->node_size);

        // Mark both buffers as dirty
        new_buffer_idx = na->extent_to_buffer[new_extent_idx];
        diskarray_mark_dirty(na, new_buffer_idx);

        // Update new extent
        na->count[new_extent_idx] = na->count[extent_idx] - split_point;
        DISKARRAY_SET_MIN(na, new_extent_idx, new_buffer);
        DISKARRAY_SET_MAX(na, new_extent_idx, new_buffer);

        // Update original extent
        na->count[extent_idx] = split_point;
        DISKARRAY_SET_MAX(na, extent_idx, buffer);

        // Insert into appropriate extent
        if (diskarray_cmp(na, node, (char*)buffer + ((na->count[extent_idx] - 1) * na->node_size)) > 0) {
            // inserting into new extent
            // finish up with original extent
            diskarray_mark_dirty(na, buffer_idx);
            diskarray_unmap_buffer(na, &buffer);

            // switch current extent to new extent
            extent_idx = new_extent_idx;
            buffer = new_buffer;
            buffer_idx = new_buffer_idx;
        } else {
            // inserting into original extent
            // finish up with new extent
            diskarray_mark_dirty(na, new_buffer_idx);
            diskarray_unmap_buffer(na, &new_buffer);
        }

        // Counts moved between the two (same directory extent): its count tree
        diskarray_recount(na, extent_idx);
        h = -1;   // positions moved
    }
    if (h < 0) h = na->pos_of[extent_idx];

    size_t ns = (size_t)na->node_size, cnt = (size_t)na->count[extent_idx];
    size_t pos = lower_bound(buffer, cnt, ns, node, na->compare);
    if (pos < cnt && diskarray_cmp(na, (char*)buffer + pos * ns, node) == 0) {
        diskarray_unmap_buffer(na, &buffer);
        return 0; // already present
    }
    memmove((char*)buffer + (pos + 1) * ns, (char*)buffer + pos * ns, (cnt - pos) * ns);
    memcpy((char*)buffer + pos * ns, node, ns);
    diskarray_extent_update_count_at(na, extent_idx, h, 1);
    if (pos == 0) DISKARRAY_SET_MIN(na, extent_idx, buffer);
    if (pos == cnt) diskarray_set_max_at(na, extent_idx, h, buffer);

    // Mark buffers as flushable
    diskarray_mark_dirty(na, buffer_idx);
    diskarray_unmap_buffer(na, &buffer);

    if (na->num_empty_extents == 0) {
        printf("Forcing a rebalance, we're out of empty extents");
        diskarray_balance(na);
    }
    return 1;
}

// Delete a node from the diskarray
static int diskarray_delete_in(diskarray_t* na, int extent_idx, int h, void* search_key);
int diskarray_delete(diskarray_t* na, void* search_key) {
    int h, extent_idx = diskarray_find_extent_at(na, search_key, &h);
    if (extent_idx < 0) return -41;
    return diskarray_delete_in(na, extent_idx, h, search_key);
}

// Delete search_key from extent_idx (already resolved).
// h: extent_idx's directory position, or -1 to look it up.
static int diskarray_delete_in(diskarray_t* na, int extent_idx, int h, void* search_key) {
    if (h < 0) h = na->pos_of[extent_idx];

    // Ensure extent is loaded
    void* buffer = NULL;
    int errcode = diskarray_map_extent(na, &buffer, extent_idx);
    if (errcode != DISKARRAY_IO_SUCCESS) {
        return errcode; // I/O error
    }

    size_t ns = (size_t)na->node_size, cnt = (size_t)na->count[extent_idx];
    size_t pos = lower_bound(buffer, cnt, ns, search_key, na->compare);
    if (pos >= cnt || diskarray_cmp(na, (char*)buffer + pos * ns, search_key) != 0) {
        diskarray_unmap_buffer(na, &buffer);
        return -42; // Node not found
    }
    memmove((char*)buffer + pos * ns, (char*)buffer + (pos + 1) * ns, (cnt - pos - 1) * ns);

    int buffer_idx = na->extent_to_buffer[extent_idx];
    diskarray_mark_dirty(na, buffer_idx);
    diskarray_extent_update_count_at(na, extent_idx, h, -1);

    if (na->count[extent_idx] == 0) {
        // Extent is now empty, but leave it in place for reuse
    } else if (pos == 0) {
        DISKARRAY_SET_MIN(na, extent_idx, buffer);
    } else if (pos == cnt - 1) {
        diskarray_set_max_at(na, extent_idx, h, buffer);
    }

    diskarray_unmap_buffer(na, &buffer);
    return 0; // Success
}

// Allows iterator style usage
// Create iterator with optional prefetching
// Advance a directory position (t, s) to the next entry; false past the end.
static inline bool diskarray_pos_next(const diskarray_t* na, int* t, int* s) {
    if (++*s < na->dirs[*t]->n) return true;
    while (++*t < na->ndirs) {
        *s = 0;
        if (na->dirs[*t]->n > 0) return true;
    }
    return false;
}

diskarray_iter_t* diskarray_iter_create(diskarray_t* na, int prefetch) {
    diskarray_iter_t* iter = malloc(sizeof(diskarray_iter_t));
    if (!iter) {
        return NULL;
    }

    // Cap prefetch to the max allowed by the diskarray
    int effective_prefetch = prefetch;
    if (effective_prefetch > na->num_max_bgio) {
        effective_prefetch = na->num_max_bgio;
    }
    if (effective_prefetch < 0) {
        effective_prefetch = 0;
    }

    // Initialize fields: the first extent in key order
    iter->dir = 0;
    iter->entry = 0;
    iter->look_dir = 0;
    iter->look_entry = 0;
    iter->ahead = 0;
    iter->extent_idx = na->dirs[0]->id[0];
    iter->idx = 0;
    iter->prefetch = effective_prefetch;
    iter->buffer = NULL;
    int errcode = diskarray_map_extent(na, &iter->buffer, iter->extent_idx);
    if (errcode != DISKARRAY_IO_SUCCESS) {
        printf("Error mapping extent %d: %d\n", iter->extent_idx, errcode);
        exit(1);
    }

    return iter;
}

// Free iterator
void diskarray_iter_free(diskarray_t* na, diskarray_iter_t* iter) {
    (void)na; // Suppress unused parameter warning
    if (!iter) return;
    if (iter->buffer != NULL) {
        diskarray_unmap_buffer(na, &iter->buffer);
    }
    free(iter);
}

int diskarray_next(diskarray_t* na, diskarray_iter_t* iter, void* result) {
    // Workers handle completion processing automatically

    // Prefetch ahead if needed: the lookahead cursor runs up to `prefetch`
    // extents past the current one
    if (iter->prefetch > 0 && iter->ahead < iter->prefetch / 2) {
        int prefetch_count = 0;
        int t = iter->look_dir, e = iter->look_entry;
        while (prefetch_count < iter->prefetch && diskarray_pos_next(na, &t, &e)) {
            int id = na->dirs[t]->id[e];
            if (na->count[id] == 0) {
                iter->look_dir = t; iter->look_entry = e; iter->ahead++;
                continue; // Skip empty extents
            }
            int result = diskarray_prefetch_extent(na, id, false);
            if (result == DISKARRAY_IO_SUCCESS) {
                prefetch_count++;
            } else if (result == DISKARRAY_IO_ERROR_QUEUE_FULL || result == DISKARRAY_IO_ERROR_NO_BUFFER) {
                break; // Can't prefetch more right now
            } else if (result < 0) {
                printf("WARNING: Prefetch failed with error %d\n", result);
                exit(1);
            }
            iter->look_dir = t; iter->look_entry = e; iter->ahead++;
        }
        diskarray_io_publish(na);
    }

    // Skip finished and empty extents
    if (iter->idx >= na->count[iter->extent_idx]) {
        diskarray_unmap_buffer(na, &iter->buffer);
        do {
            if (!diskarray_pos_next(na, &iter->dir, &iter->entry)) return -1;   // no more extents
            if (iter->ahead > 0) iter->ahead--;
            else { iter->look_dir = iter->dir; iter->look_entry = iter->entry; }
            iter->extent_idx = na->dirs[iter->dir]->id[iter->entry];
        } while (na->count[iter->extent_idx] == 0);
        iter->idx = 0;

        // Map the new extent
        int errcode = diskarray_map_extent(na, &iter->buffer, iter->extent_idx);
        if (errcode != DISKARRAY_IO_SUCCESS) {
            printf("Error mapping extent %d: %d\n", iter->extent_idx, errcode);
            exit(1);
        }
    }

    // Copy the current node to result
    void* node = (char*)iter->buffer + iter->idx * na->node_size;
    memcpy(result, node, na->node_size);
    iter->idx++;

    return 0; // Success
}

// Initialize worker threads for multithreaded I/O
// Worker w owns buffers [w*per, (w+1)*per) and dextents d with d % workers == w,
// so with workers a multiple of num_drives each worker's ring hits one drive.
static int diskarray_init_workers(diskarray_t* na, int num_workers) {
    if (num_workers <= 0) {
        printf("ERROR: Invalid number of worker threads %d\n", num_workers);
        return -1;
    }
    na->num_worker_threads = num_workers;
    na->num_io_buffers_per_worker = na->num_io_buffers / num_workers;
    na->num_max_bgio = na->num_io_buffers_per_worker;      // at most one I/O per buffer
    if (na->num_io_buffers_per_worker < 2) {
        printf("ERROR: %d I/O buffers is too few for %d workers\n", na->num_io_buffers, num_workers);
        return -1;
    }
    na->workers = aligned_alloc(64, (size_t)num_workers * sizeof(worker_thread_data_t));
    if (!na->workers) return -1;
    memset(na->workers, 0, (size_t)num_workers * sizeof(worker_thread_data_t));

    for (int i = 0; i < num_workers; i++) {
        worker_thread_data_t* w = &na->workers[i];
        w->worker_id = i;
        w->num_workers = num_workers;
        w->na_ptr = na;
        w->num_io_buffers = na->num_io_buffers_per_worker;
        w->buffer_start_idx = i * w->num_io_buffers;
        w->num_dextents = (na->num_dextents - i + num_workers - 1) / num_workers;
        w->dextent_pool = calloc((size_t)w->num_dextents, sizeof(int));
        w->buffer_pool = aligned_alloc(512, ((size_t)w->num_io_buffers * sizeof(int) + 511) / 512 * 512);
        w->req = aligned_alloc(64, ((size_t)na->num_max_bgio * sizeof(diskarray_io_t) + 63) / 64 * 64);
        w->cpl = aligned_alloc(64, ((size_t)na->num_max_bgio * sizeof(diskarray_done_t) + 63) / 64 * 64);
        w->ops = aligned_alloc(64, ((size_t)w->num_io_buffers * sizeof(diskarray_io_t) + 63) / 64 * 64);
        if (!w->dextent_pool || !w->buffer_pool || !w->req || !w->cpl || !w->ops) {
            printf("ERROR: Worker %d failed to allocate rings\n", i);
            return -1;
        }
        for (int j = 0; j < w->num_dextents; j++) w->dextent_pool[j] = i + j * num_workers;
        w->dextent_count = w->num_dextents;
        for (int j = 0; j < w->num_io_buffers; j++) w->buffer_pool[j] = w->buffer_start_idx + j;
    }
    for (int i = 0; i < num_workers; i++) {
        int ret = pthread_create(&na->workers[i].thread_id, NULL, diskarray_worker_thread_main, &na->workers[i]);
        if (ret != 0) {
            printf("ERROR: Failed to create worker thread %d: %s\n", i, strerror(ret));
            return -1;
        }
    }
    for (int i = 0; i < num_workers; i++) {
        int st;
        while ((st = atomic_load(&na->workers[i].status)) == WORKER_STARTING) usleep(100);
        if (st != WORKER_READY) return -1;
    }
    return 0;
}

// Cleanup worker threads
static void diskarray_cleanup_workers(diskarray_t* na) {
    if (!na->workers) return;
    for (int i = 0; i < na->num_worker_threads; i++)
        atomic_store_explicit(&na->workers[i].shutdown, true, memory_order_release);
    for (int i = 0; i < na->num_worker_threads; i++) {
        worker_thread_data_t* w = &na->workers[i];
        if (w->thread_id) pthread_join(w->thread_id, NULL);
        free(w->dextent_pool);
        free(w->buffer_pool);
        free(w->req);
        free(w->cpl);
        free(w->ops);
    }
    free(na->workers);
    na->workers = NULL;
    na->num_worker_threads = 0;
}

// Create an empty diskarray with disk backing
static uint64_t round_up(uint64_t v, uint64_t to) { return (v + to - 1) / to * to; }

// 512 B aligned; the I/O pool and anything else of 2 MiB or more go on huge pages.
static void* diskarray_alloc_array(uint64_t bytes) {
    bytes = round_up(bytes, 512);
    if (bytes < (2u << 20)) return aligned_alloc(512, bytes);
    uint64_t rounded = round_up(bytes, 2u << 20);
    void* p = aligned_alloc(2u << 20, rounded);
    if (p) madvise(p, rounded, MADV_HUGEPAGE);
    return p;
}

diskarray_t* diskarray_alloc_striped(const char* const* disk_paths, int num_paths, size_t max_nodes,
                                     int extent_size, int node_size, int num_io_buffers, int allowed_dirty_percent,
                                     int workers, int (*compare)(const void *a, const void *b)) {
    if (num_paths < 1 || workers < 1 || num_io_buffers < 2) {
        printf("diskarray_alloc: bad parameters\n");
        return NULL;
    }
#ifdef DISKARRAY_U64_KEYS
    if (node_size != 8) {
        printf("diskarray built with DISKARRAY_U64_KEYS takes 8-byte keys only (node_size %d)\n", node_size);
        return NULL;
    }
#endif
    diskarray_t* na = calloc(1, sizeof *na);
    if (!na) return NULL;
    na->node_size = node_size;
    na->num_io_buffers = num_io_buffers;
    na->allowed_dirty_percent = allowed_dirty_percent;
    na->watermark_flush = (num_io_buffers * allowed_dirty_percent) / 200;  // half of the dirty threshold
    na->compare = compare;

    // Extents are whole 4 KiB multiples on disk (O_DIRECT).
    na->extent_disk_size = round_up((uint64_t)extent_size * node_size, 4096);
    na->extent_size = (int)(na->extent_disk_size / node_size);
    na->num_extents = (int)((max_nodes + extent_size - 1) / extent_size);
    if (na->num_extents < 2) {
        printf("Error: diskarray needs at least 2 extents.\n");
        exit(1);
    }
    // bulk_init fills 90% of the extents; the rest are spare for splits.  A
    // caller expecting many splits can add spare extents and set
    // num_data_extents back down before bulk_init.
    na->num_data_extents = na->num_extents >= 10 ? (int)(na->num_extents * 0.90) : na->num_extents - 1;
    na->pending_capacity = 2 * (size_t)num_io_buffers;
    na->next_uid = 1;
    na->next_uid_io = 1;

    if (workers % num_paths) workers += num_paths - workers % num_paths;   // one drive per worker ring
    // Copy-on-write needs spare disk extents.  Extent e belongs to worker
    // e % workers, so with fewer extents than workers each pool still needs
    // room for its extent plus the copy in flight.
    na->num_dextents = na->num_extents * 4;
    if (na->num_dextents < workers * 4) na->num_dextents = workers * 4;
    na->dextent_count = na->num_dextents;

    // Drives.  A single path may be a plain file, which is created and sized;
    // several paths are block devices used as they are.
    na->disk_fds = malloc((size_t)num_paths * sizeof(int));
    if (!na->disk_fds) goto cleanup;
    for (int i = 0; i < num_paths; i++) na->disk_fds[i] = -1;
    for (int i = 0; i < num_paths; i++) {
        na->disk_fds[i] = open(disk_paths[i], O_RDWR | O_DIRECT | (num_paths == 1 ? O_CREAT : 0), 0644);
        if (na->disk_fds[i] < 0) {
            printf("Failed to open %s: %s\n", disk_paths[i], strerror(errno));
            goto cleanup;
        }
        na->num_drives = i + 1;
    }
    struct stat st;
    if (num_paths == 1 && fstat(na->disk_fds[0], &st) == 0 && S_ISREG(st.st_mode) &&
        ftruncate(na->disk_fds[0], (off_t)na->num_dextents * (off_t)na->extent_disk_size) < 0) {
        printf("Failed to set disk file size: %s\n", strerror(errno));
        goto cleanup;
    }

    // Tracking arrays
    uint64_t io_buffers_size = round_up((uint64_t)num_io_buffers * sizeof(void*), 4096);
    uint64_t dirty_bits_size = round_up((uint64_t)num_io_buffers * sizeof(bool), 512);
    uint64_t by_extent_size = round_up((uint64_t)na->num_extents * sizeof(int), 512);
    uint64_t by_buffer_size = round_up((uint64_t)num_io_buffers * sizeof(int), 512);
    uint64_t pending_cmds_size = round_up(na->pending_capacity * sizeof(diskarray_cmd_t*), 512);
    na->io_buffers = malloc(io_buffers_size);
    na->dirty_bits = aligned_alloc(512, dirty_bits_size);
    na->extent_to_buffer = diskarray_alloc_array(by_extent_size);
    na->extent_to_buffer_io = diskarray_alloc_array(by_extent_size);
    na->extent_to_dextent = diskarray_alloc_array(by_extent_size);
    na->buffer_to_extent = aligned_alloc(512, by_buffer_size);
    na->buffer_to_extent_io = aligned_alloc(512, by_buffer_size);
    na->pending_cmds = aligned_alloc(512, pending_cmds_size);
    if (!na->io_buffers || !na->dirty_bits || !na->extent_to_buffer || !na->extent_to_buffer_io ||
        !na->extent_to_dextent || !na->buffer_to_extent || !na->buffer_to_extent_io || !na->pending_cmds) {
        printf("Failed to allocate buffer tracking arrays\n");
        goto cleanup;
    }
    na->overhead = sizeof *na + io_buffers_size + dirty_bits_size + 3 * by_extent_size + 2 * by_buffer_size + pending_cmds_size;
    for (int i = 0; i < na->num_extents; i++) {
        na->extent_to_buffer[i] = BUFFER_UNUSED;
        na->extent_to_buffer_io[i] = BUFFER_UNUSED;
        na->extent_to_dextent[i] = BUFFER_UNUSED;
    }
    memset(na->dirty_bits, 0, dirty_bits_size);
    for (size_t i = 0; i < na->pending_capacity; i++) na->pending_cmds[i] = NULL;

    // The I/O pool: the only DMA target, so with DDIO this is what should fit in L3.
    na->buffer_stride = round_up(na->extent_disk_size, 4096);
    uint64_t pool_bytes = (uint64_t)num_io_buffers * na->buffer_stride;
    na->buffer = diskarray_alloc_array(pool_bytes);
    if (!na->buffer) {
        printf("Failed to allocate the I/O buffer pool (%lu bytes)\n", (unsigned long)pool_bytes);
        goto cleanup;
    }
    memset(na->buffer, 0, pool_bytes);
    for (int i = 0; i < num_io_buffers; i++) {
        na->io_buffers[i] = (char*)na->buffer + i * na->buffer_stride;
        na->buffer_to_extent[i] = BUFFER_UNUSED;
        na->buffer_to_extent_io[i] = BUFFER_UNUSED;
    }
    na->overhead += pool_bytes;

    // Per-extent metadata (by id) and the directory (order)
    uint64_t count_size = round_up((uint64_t)na->num_extents * sizeof(int), 512);
    uint64_t minmax_size = round_up((uint64_t)na->num_extents * node_size, 512);
    na->count = diskarray_alloc_array(count_size);
    na->mins = diskarray_alloc_array(minmax_size);
    na->maxs = diskarray_alloc_array(minmax_size);
    na->pos_of = diskarray_alloc_array(count_size);
    na->free_ids = diskarray_alloc_array(count_size);
    if (!na->count || !na->mins || !na->maxs || !na->pos_of || !na->free_ids) {
        printf("Failed to allocate memory for diskarray metadata\n");
        goto cleanup;
    }
    na->overhead += 3 * count_size + 2 * minmax_size;
    memset(na->count, 0, count_size);
    memset(na->mins, 0, minmax_size);
    memset(na->maxs, 0, minmax_size);
    // one empty extent (id 0) to start; the rest wait on free_ids, lowest on top
    for (int i = 0; i < na->num_extents; i++) na->pos_of[i] = -1;
    na->num_empty_extents = 0;
    for (int i = na->num_extents - 1; i >= 1; i--) na->free_ids[na->num_empty_extents++] = i;
    int first = 0;
    if (diskarray_build_directory(na, &first, 1) != 0) {
        printf("Failed to allocate the diskarray directory\n");
        goto cleanup;
    }

    if (diskarray_init_workers(na, workers) < 0) {
        printf("ERROR: Failed to initialize worker threads\n");
        goto cleanup;
    }
    return na;

cleanup:
    diskarray_free(na);
    return NULL;
}

diskarray_t* diskarray_alloc(const char* disk_file, size_t max_nodes, int extent_size,
                             int node_size, int num_io_buffers, int allowed_dirty_percent,
                             int workers, int (*compare)(const void *a, const void *b)) {
    const char* paths[1] = { disk_file };
    return diskarray_alloc_striped(paths, 1, max_nodes, extent_size, node_size, num_io_buffers,
                                   allowed_dirty_percent, workers, compare);
}

pthread_t diskarray_worker_thread(const diskarray_t* na, int worker) {
    return na->workers[worker].thread_id;
}

// Free a diskarray
// Safe on a partially built diskarray (used by the allocator's error path).
void diskarray_free(diskarray_t* na) {
    if (!na) return;
    diskarray_cleanup_workers(na);
    free(na->buffer);
    free(na->io_buffers);
    if (na->disk_fds) {
        for (int i = 0; i < na->num_drives; i++) close(na->disk_fds[i]);
        free(na->disk_fds);
    }
    free(na->dirty_bits);
    free(na->extent_to_buffer);
    free(na->buffer_to_extent);
    free(na->extent_to_buffer_io);
    free(na->buffer_to_extent_io);
    free(na->extent_to_dextent);
    free(na->pending_cmds);
    free(na->count);
    free(na->mins);
    free(na->maxs);
    free(na->pos_of);
    free(na->free_ids);
    for (int t = 0; t < na->ndirs; t++) free(na->dirs[t]);
    free(na->dirs);
    free(na->tcnt);
    free(na->tmax);
    free(na->tsmax1);
    free(na->tsmax2);
    free(na->lookup_tree);
    free(na);
}

// Initialize a diskarray with an array of nodes from a temporary buffer, sorted or not
// a sorted buffer as a bulk_init source
typedef struct { const char* at; size_t node_size; } buffer_source_t;

static const void* buffer_next(void* ctx, size_t count) {
    buffer_source_t* b = ctx;
    const void* p = b->at;
    b->at += count * b->node_size;
    return p;
}

void diskarray_bulk_init(diskarray_t* na, void* temp_buffer, size_t count, bool sorted) {
    if (count == 0) {
        return;
    }
    if (!sorted) {
        printf("Sorting buffer...\n");
        qsort(temp_buffer, count, na->node_size, na->compare);
    }
    buffer_source_t b = {temp_buffer, na->node_size};
    diskarray_bulk_init_stream(na, count, buffer_next, &b);
}

void diskarray_bulk_init_stream(diskarray_t* na, size_t count, const void* (*next)(void*, size_t), void* ctx) {
    if (count == 0) {
        return;
    }

    // At max we want to have 1/2 the io buffers prefetching and 1/2 flushing
    int bgio_count = (na->num_io_buffers - 2 > na->num_data_extents ? na->num_data_extents : na->num_io_buffers - 2)/2 - 1;
    void** buffers = malloc(bgio_count * sizeof(void*));
    for (int i = 0; i < bgio_count; i++) {
        buffers[i] = NULL;
    }
    int current_buffer = 0;
    int prefetched = 0;

    // prefetch a bunch of reads
    for (int i = 0; i < bgio_count; i++) {
        diskarray_prefetch_extent(na, i, false);
    }
    diskarray_io_publish(na);
    prefetched += bgio_count;


    printf("DEBUG: Bulk init with %zu nodes, %i extents, wm %i\n", count, na->num_data_extents, na->watermark_flush);
    // Calculate target count per extent - use floor to prefer slightly over-filled extents
    // size_t: count passes 2^31 at 16 GiB of nodes
    size_t tgt_count = count / na->num_data_extents;
    size_t remainder = count % na->num_data_extents;
    if (tgt_count + (remainder ? 1 : 0) > (size_t)na->extent_size) {
        printf("diskarray_bulk_init: %zu nodes do not fit in %d extents of %d (raise overprovisioning)\n",
               count, na->num_data_extents, na->extent_size);
        exit(1);
    }

    size_t remaining = count;

    // Work forwards through extents, starting from extent 0
    for (int extent_idx = 0; extent_idx < na->num_data_extents && remaining > 0; extent_idx++) {
        // First 'remainder' extents get tgt_count+1, rest get tgt_count
        int chunk_size = (int)((size_t)extent_idx < remainder ? tgt_count + 1 : tgt_count);
        chunk_size = (size_t)chunk_size > remaining ? (int)remaining : chunk_size;

        // when our buffer slots are full we dump as a group
        if (current_buffer >= bgio_count) {
            current_buffer = 0;
            // Unmap to flush io buffers as a chunk
            for (int i = 0; i < bgio_count; i++) {
                diskarray_unmap_buffer(na, &buffers[i]);
            }
            diskarray_flush_buffers(na, true);
            usleep(5); // let a write or two complete
        }

        // Refresh the prefetch queue when it gets low
        if ((prefetched - extent_idx) <= bgio_count/8) {
            int to_prefetch = bgio_count - (prefetched - extent_idx);
            to_prefetch = to_prefetch + extent_idx >= na->num_data_extents ? na->num_data_extents - extent_idx - 1 : to_prefetch;
            for (int j = 0; j < to_prefetch; j++) {
                diskarray_prefetch_extent(na, extent_idx + j, false);
            }
            prefetched += to_prefetch;
            diskarray_io_publish(na);
        }

        // Load extent into buffer slot
        int result = diskarray_map_extent(na, &buffers[current_buffer], extent_idx);
        if (result != DISKARRAY_IO_SUCCESS) {
            if (result == DISKARRAY_IO_ERROR_NO_BUFFER) {
                int loopcnt = 0;
                while (result != DISKARRAY_IO_SUCCESS && loopcnt < 10000) {
                    usleep(5);
                    result = diskarray_map_extent(na, &buffers[current_buffer], extent_idx);
                    loopcnt++;
                }
            }
            if (result != DISKARRAY_IO_SUCCESS) {
                printf("Failed to load extent %d during bulk init with error %d\n", extent_idx, result);
                printf("Buffer state at failure:\n");
                //diskarray_debug_buffers(na);
                exit(1);
            }
        }

        // Copy the next chunk_size sorted nodes to this extent
        memcpy(buffers[current_buffer], next(ctx, chunk_size), chunk_size * na->node_size);

        na->count[extent_idx] = chunk_size;
        DISKARRAY_SET_MIN(na, extent_idx, buffers[current_buffer]);
        DISKARRAY_SET_MAX(na, extent_idx, buffers[current_buffer]);

        // Mark buffer as dirty
        int buffer_idx = na->extent_to_buffer[extent_idx];
        diskarray_mark_dirty(na, buffer_idx);

        remaining -= chunk_size;
        current_buffer++;
    }
    na->total_count = count;

    // Unmap buffers
    for (int i = 0; i < bgio_count; i++) {
        diskarray_unmap_buffer(na, &buffers[i]);
    }

    // The directory: ids 0..num_data_extents-1 in key order; the rest are free
    {
        size_t bytes = (size_t)na->num_data_extents * sizeof(int);
        int* ids = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ids == MAP_FAILED) {
            printf("diskarray_bulk_init: out of memory\n");
            exit(1);
        }
        for (int i = 0; i < na->num_data_extents; i++) ids[i] = i;
        for (int i = 0; i < na->num_extents; i++) na->pos_of[i] = -1;
        na->num_empty_extents = 0;
        for (int i = na->num_extents - 1; i >= na->num_data_extents; i--) na->free_ids[na->num_empty_extents++] = i;
        if (diskarray_build_directory(na, ids, na->num_data_extents) != 0) {
            printf("diskarray_bulk_init: out of memory\n");
            exit(1);
        }
        munmap(ids, bytes);
    }

    // Clean up our IOs before we move on
    if (diskarray_submit_and_wait_all(na) != 0) {
        printf("diskarray_balance: Failed to wait for all IO to complete\n");
        exit(1);
    }
}

// Redistributes data evenly across extents, walking them in key order.  An
// extent too full for its right neighbour to take the excess gets a fresh
// extent right after it; extents left empty go back on free_ids (their ids
// keep their buffer and dextent: a reused id starts at count 0, so whatever
// is there is never read).  The directory is rebuilt from the new order.
int diskarray_balance(diskarray_t* na) {
    if (na->total_count == 0) {
        return 0;
    }
    int m;
    size_t order_bytes;
    int* order = diskarray_order(na, na->num_empty_extents, &m, &order_bytes);
    if (!order) return -1;
    int rc = 0;
    int extents_balanced = 0;
    // Back to the bulk_init fill, but never above 90% of an extent, so a
    // balance forced by running out of spare extents compacts and frees some.
    int tgt_count = (na->total_count + na->num_data_extents - 1) / na->num_data_extents;
    if (tgt_count > na->extent_size * 9 / 10) tgt_count = na->extent_size * 9 / 10;
    if (((long)na->total_count + tgt_count - 1) / tgt_count > (long)na->num_extents) {
        printf("diskarray_balance: full (%zu nodes, %d extents of %d)\n", na->total_count, na->num_extents, na->extent_size);
        munmap(order, order_bytes);
        return -1;
    }

    for (int i = 0; i < m; i++) {
        int cur = order[i];
        int right = (i + 1 < m) ? order[i + 1] : -1;

        if (na->count[cur] == 0) {
            continue; // Skip empty extents
        }
        if ((na->count[cur] < tgt_count*1.2) && (na->count[cur] > tgt_count*0.8)) {
            continue; // extent is already balanced
        }
        printf("diskarray_balance: Extent %d is unbalanced: target=%d, actual=%d\n", cur, tgt_count, na->count[cur]);
        if (na->count[cur] > tgt_count) {
            // left extent is too full
            int count_to_move = na->count[cur] - tgt_count;
            if (right >= 0 && tgt_count - na->count[right] >= count_to_move) {
                // we can copy from left to right
                if (diskarray_balance_copy_helper(na, tgt_count, count_to_move, 1, cur, right) < 0) {
                    printf("diskarray_balance: Failed to copy data\n");
                    rc = -1;
                    goto out;
                }
            } else {
                // a fresh extent right after this one
                if (na->num_empty_extents == 0) {
                    printf("diskarray_balance: No empty extent found\n");
                    rc = -1;
                    goto out;
                }
                int fresh = na->free_ids[--na->num_empty_extents];
                na->count[fresh] = 0;
                memmove(&order[i + 2], &order[i + 1], (size_t)(m - i - 1) * sizeof(int));
                order[i + 1] = fresh;
                m++;
                if (diskarray_balance_copy_helper(na, tgt_count, count_to_move, 1, cur, fresh) < 0) {
                    printf("diskarray_balance: Failed to copy data 2\n");
                    rc = -1;
                    goto out;
                }
            }
        } else {
            // left extent is too empty, fill it from right extent
            int count_needed = tgt_count - na->count[cur];
            if (right >= 0) {
                int available = na->count[right] > count_needed ? count_needed : na->count[right];
                if (available == 0) {
                    continue;
                }
                if (diskarray_balance_copy_helper(na, 0, available, 0, right, cur) < 0) {
                    printf("diskarray_balance: Failed to copy data 3\n");
                    rc = -1;
                    goto out;
                }
                if (na->count[cur] < tgt_count*0.8) {
                    printf("do over %i\n", cur);
                     // we still have space, try to fill it more
                    i--;
                }
            }
        }
        extents_balanced++;
    }

    // Reclaim empty extents (keep one if everything is empty)
    {
        int k = 0;
        for (int i = 0; i < m; i++) {
            int id = order[i];
            if (na->count[id] == 0 && (k > 0 || i + 1 < m)) {
                na->pos_of[id] = -1;
                na->free_ids[na->num_empty_extents++] = id;
                extents_balanced++;
            } else {
                order[k++] = id;
            }
        }
        m = k;
    }

    if (extents_balanced == 0) {
        goto out;
    }

    // Update all extent lookups after balance operations
    for (int i = 0; i < m; i++) {
        int id = order[i];
        if (na->count[id] > 0) {
            void* buffer = NULL;
            int errcode = diskarray_map_extent(na, &buffer, id);
            if (errcode != DISKARRAY_IO_SUCCESS) {
                printf("diskarray_balance: Failed to map extent %i\n", id);
                rc = -1;
                goto out;
            }
            DISKARRAY_SET_MIN(na, id, buffer);
            DISKARRAY_SET_MAX(na, id, buffer);
            diskarray_unmap_buffer(na, &buffer);
        }
    }
    if (diskarray_build_directory(na, order, m) != 0) rc = -1;
out:
    munmap(order, order_bytes);
    return rc;
}

///// ASYNC COMMAND FUNCTIONS ////////////////////

// Resolving a batch: commands go through the directory one level at a time,
// as a group, and every command's line for the next level is prefetched
// before any of them reads it, so the misses of the whole group are in
// flight together instead of one command's chain after another (the
// nextarray2 pipeline's group_locate).
#define RESOLVE_GROUP 32

// Key commands (search / insert / delete): top level, then in the directory
// extent its every-64th sample, a line of every-8th samples, a line of maxs.
static void diskarray_resolve_keys(diskarray_t* na, diskarray_cmd_t** cs, int n) {
    size_t ns = (size_t)na->node_size;
    const diskarray_dir_t* d[RESOLVE_GROUP];
    const void* key[RESOLVE_GROUP];
    int t[RESOLVE_GROUP], n1[RESOLVE_GROUP];
    size_t g[RESOLVE_GROUP];
    bool active[RESOLVE_GROUP];
    for (int i = 0; i < n; i++) {
        key[i] = cs[i]->command == DISKARRAY_INSERT ? cs[i]->node : cs[i]->search_key;
        t[i] = tfind(na, key[i]);   // sampled top level: stays cached
        d[i] = na->dirs[t[i]];
        _mm_prefetch((const char*)&d[i]->n, _MM_HINT_T0);
        _mm_prefetch(d[i]->smax2, _MM_HINT_T0);
        _mm_prefetch(d[i]->smax2 + 32 * ns, _MM_HINT_T0);
    }
    for (int i = 0; i < n; i++) {
        n1[i] = (d[i]->n + GROUP - 1) / GROUP;
        int n2 = (n1[i] + GROUP - 1) / GROUP;
        g[i] = lower_bound(d[i]->smax2, (size_t)n2, ns, key[i], na->compare);
        active[i] = g[i] < (size_t)n2;
        if (active[i]) _mm_prefetch(d[i]->smax1 + g[i] * GROUP * ns, _MM_HINT_T0);
        else g[i] = (size_t)d[i]->n - 1;   // past every sample: the last entry
    }
    for (int i = 0; i < n; i++) {
        if (!active[i]) continue;
        size_t lo = g[i] * GROUP, span = (size_t)n1[i] - lo < GROUP ? (size_t)n1[i] - lo : GROUP;
        g[i] = lo + lower_bound(d[i]->smax1 + lo * ns, span, ns, key[i], na->compare);
        _mm_prefetch(d[i]->max + g[i] * GROUP * ns, _MM_HINT_T0);
        _mm_prefetch((const char*)&d[i]->id[g[i] * GROUP], _MM_HINT_T0);
    }
    for (int i = 0; i < n; i++) {
        size_t s = g[i];
        if (active[i]) {
            size_t lo = g[i] * GROUP, span = (size_t)d[i]->n - lo < GROUP ? (size_t)d[i]->n - lo : GROUP;
            s = lo + lower_bound(d[i]->max + lo * ns, span, ns, key[i], na->compare);
            if (s >= (size_t)d[i]->n) s = (size_t)d[i]->n - 1;
        }
        cs[i]->pos = HANDLE(t[i], (int)s);
        cs[i]->extent_idx = d[i]->id[s];
    }
}

// _at commands: the top count tree's levels, then the directory extent's three.
__attribute__((target("avx512f")))
static void diskarray_resolve_index(diskarray_t* na, diskarray_cmd_t** cs, int n) {
    long rem[RESOLVE_GROUP];
    int node[RESOLVE_GROUP], t[RESOLVE_GROUP];
    long total = (long)na->total_count;
    for (int i = 0; i < n; i++) {
        long idx = cs[i]->index < 0 ? cs[i]->index + total : cs[i]->index;
        rem[i] = idx;
        node[i] = (idx < 0 || idx >= total) ? -1 : 0;
    }
    for (int level = 0; level < na->tree_height; level++) {
        for (int i = 0; i < n; i++) if (node[i] >= 0) _mm_prefetch((const char*)tlt_node(na, level, node[i]), _MM_HINT_T0);
        for (int i = 0; i < n; i++) {
            if (node[i] < 0) continue;
            const long* nd = tlt_node(na, level, node[i]);
            __mmask8 m = _mm512_cmpgt_epi64_mask(_mm512_load_si512(nd), _mm512_set1_epi64(rem[i]));
            if (!m) { node[i] = -1; continue; }
            int p = __builtin_ctz(m);
            if (p) rem[i] -= nd[p - 1];
            node[i] = node[i] * LOOKUP_NODE_SIZE + p;
        }
    }
    for (int i = 0; i < n; i++) { t[i] = node[i] >= 0 && node[i] < na->ndirs ? node[i] : -1; node[i] = 0; }
    static const int base[3] = { 0, DLT_L1, DLT_L2 };
    for (int level = 0; level < 3; level++) {
        for (int i = 0; i < n; i++) if (t[i] >= 0) _mm_prefetch((const char*)&na->dirs[t[i]]->dlt[(base[level] + node[i]) * 16], _MM_HINT_T0);
        for (int i = 0; i < n; i++) {
            if (t[i] < 0) continue;
            const int* nd = &na->dirs[t[i]]->dlt[(base[level] + node[i]) * 16];
            __mmask16 m = _mm512_cmpgt_epi32_mask(_mm512_load_si512(nd), _mm512_set1_epi32((int)rem[i]));
            if (!m) { t[i] = -1; continue; }
            int p = __builtin_ctz(m);
            if (p) rem[i] -= nd[p - 1];
            node[i] = node[i] * 16 + p;
        }
        if (level == 2)
            for (int i = 0; i < n; i++) if (t[i] >= 0 && node[i] < na->dirs[t[i]]->n) _mm_prefetch((const char*)&na->dirs[t[i]]->id[node[i]], _MM_HINT_T0);
    }
    for (int i = 0; i < n; i++) {
        bool ok = t[i] >= 0 && node[i] < na->dirs[t[i]]->n;
        cs[i]->pos = ok ? HANDLE(t[i], node[i]) : -1;
        cs[i]->extent_idx = ok ? na->dirs[t[i]]->id[node[i]] : -1;
        cs[i]->local_index = rem[i];
    }
}

int diskarray_queue_cmds(diskarray_t* na, diskarray_cmd_t* cmds, size_t count) {
    // Write back dirty buffers once enough have piled up, so the prefetches
    // below find FLUSHED buffers instead of stalling in map_extent waiting for
    // one.  The writes ride along with this batch's reads.
    if (na->flushable >= na->watermark_flush) diskarray_flush_buffers(na, false);

    // Clean up completed commands first to reclaim space
    while (na->pending_head != na->pending_tail && na->pending_cmds[na->pending_head] == NULL) {
        na->pending_head = (na->pending_head + 1) % na->pending_capacity;
    }

    // Check available space in ring buffer
    size_t available = na->pending_capacity - ((na->pending_tail - na->pending_head + na->pending_capacity) % na->pending_capacity);
    if (count > available) {
        return -1; // Not enough space
    }

    // 1. Into the ring, remembering where so completion is O(1)
    for (size_t i = 0; i < count; i++) {
        diskarray_cmd_t* cmd = &cmds[i];
        cmd->uid = na->next_uid++;
        cmd->slot = na->pending_tail;
        na->pending_cmds[na->pending_tail] = cmd;
        na->pending_tail = (na->pending_tail + 1) % na->pending_capacity;
    }

    // 2. In groups of RESOLVE_GROUP: resolve each command's extent and
    // directory position, then start the group's I/O, so the reads start as
    // soon as their group is resolved rather than after the whole batch.
    // Positions move with every count change, so _at is pinned to count_gen;
    // the others only to structural changes (which is also when positions
    // move, so the position is exactly as valid as the extent).
    size_t queued = 0;
    for (size_t g0 = 0; g0 < count; g0 += RESOLVE_GROUP) {
        size_t g1 = g0 + RESOLVE_GROUP < count ? g0 + RESOLVE_GROUP : count;
        diskarray_cmd_t* keys[RESOLVE_GROUP];
        diskarray_cmd_t* idxs[RESOLVE_GROUP];
        int nk = 0, ni = 0;
        for (size_t i = g0; i < g1; i++) {
            diskarray_cmd_t* cmd = &cmds[i];
            cmd->local_index = 0;
            cmd->pos = -1;
            cmd->extent_idx = -1;
            cmd->gen = na->structure_gen;
            switch (cmd->command) {
                case DISKARRAY_SEARCH:
                case DISKARRAY_DELETE:
                case DISKARRAY_INSERT:
                    if (na->ndirs > 0) keys[nk++] = cmd;
                    break;
                case DISKARRAY_AT:
                    cmd->gen = na->count_gen;
                    idxs[ni++] = cmd;
                    break;
                default:
                    break;
            }
        }
        if (nk) diskarray_resolve_keys(na, keys, nk);
        if (ni) diskarray_resolve_index(na, idxs, ni);

        for (size_t i = g0; i < g1; i++) {
            diskarray_cmd_t* cmd = &cmds[i];
            int extent_idx = cmd->extent_idx;
            if (extent_idx < 0) {
                cmd->status = CMD_ERROR;
                continue;
            }

            // Execution comes a batch later: start the lines it will touch on
            // their way now (the extent's count, and for insert/delete the
            // count-tree path above it), like the extent's I/O below.
            _mm_prefetch((const char*)&na->count[extent_idx], _MM_HINT_T0);
            if (cmd->command == DISKARRAY_INSERT || cmd->command == DISKARRAY_DELETE) {
                const diskarray_dir_t* d = na->dirs[HANDLE_DIR(cmd->pos)];
                int e = HANDLE_ENTRY(cmd->pos);
                _mm_prefetch((const char*)&d->dlt[(DLT_L2 + e / 16) * 16], _MM_HINT_T0);
                _mm_prefetch((const char*)&d->dlt[(DLT_L1 + e / 256) * 16], _MM_HINT_T0);
            }

            // Try to prefetch extent
            int errcode = diskarray_prefetch_extent(na, extent_idx, false);
            na->stat_queued++;
            if (errcode >= 0) na->stat_queue_hits++;   // already in a buffer, or on its way
            if (errcode != DISKARRAY_IO_SUCCESS) {
                cmd->status = CMD_QUEUED; // Can't prefetch, will load synchronously later
                continue;
            }
            cmd->status = CMD_LOAD_PENDING;
            queued++;
        }
    }

    diskarray_io_publish(na);   // the prefetches and any flushes queued above
    return queued;
}

void diskarray_cmd_warm(diskarray_t* na, const diskarray_cmd_t* cmd) {
    int id = cmd->extent_idx;
    if (id < 0) return;
    int b = na->extent_to_buffer[id];
    if (b < 0) b = na->extent_to_buffer_io[id];
    if (b < 0 || b >= na->num_io_buffers) return;
    int state = atomic_load_explicit(&na->buffer_to_extent[b], memory_order_relaxed);
    if (state == BUFFER_PREFETCHING || state == BUFFER_FLUSHING || state == BUFFER_UNUSED) return;   // not there yet
    const char* p = na->io_buffers[b];
    size_t bytes = (size_t)na->count[id] * (size_t)na->node_size;
    for (size_t o = 0; o < bytes; o += 64) _mm_prefetch(p + o, _MM_HINT_T1);
}

int diskarray_cmd_wait(diskarray_t* na, diskarray_cmd_t* cmd) {
    // Workers handle all completion processing automatically
    
    // If still waiting for prefetch, the extent should be loaded synchronously later
    if (cmd->status == CMD_LOAD_PENDING) {
        cmd->status = CMD_QUEUED;
    }

    // Execute synchronous operation if ready
    if (cmd->status == CMD_QUEUED) {
        cmd->status = CMD_DONE;
        // Reuse the extent resolved at queue time unless the structure moved under it.
        bool cached = cmd->extent_idx >= 0 &&
                      cmd->gen == (cmd->command == DISKARRAY_AT ? na->count_gen : na->structure_gen);
        switch (cmd->command) {
            case DISKARRAY_SEARCH:
                cmd->retcode = cached ? diskarray_search_in(na, cmd->extent_idx, cmd->search_key, cmd->result)
                                      : diskarray_search(na, cmd->search_key, cmd->result);
                break;
            case DISKARRAY_AT:
                cmd->retcode = cached ? diskarray_at_in(na, cmd->extent_idx, cmd->local_index, cmd->result)
                                      : diskarray_at(na, cmd->index, cmd->result);
                break;
            case DISKARRAY_INSERT:
                cmd->retcode = cached ? diskarray_insert_in(na, cmd->extent_idx, cmd->pos, cmd->node)
                                      : diskarray_insert(na, cmd->node);
                break;
            case DISKARRAY_DELETE:
                cmd->retcode = cached ? diskarray_delete_in(na, cmd->extent_idx, cmd->pos, cmd->search_key)
                                      : diskarray_delete(na, cmd->search_key);
                break;
            default:
                cmd->status = CMD_ERROR;
                break;
        }
        
        // Remove cmd from the ring (its slot was recorded at queue time)
        if (cmd->slot < na->pending_capacity && na->pending_cmds[cmd->slot] == cmd)
            na->pending_cmds[cmd->slot] = NULL;

        // Advance head pointer past any NULL entries to reclaim space
        while (na->pending_head != na->pending_tail && na->pending_cmds[na->pending_head] == NULL) {
            na->pending_head = (na->pending_head + 1) % na->pending_capacity;
        }
    }
    
    return cmd->status;
}
