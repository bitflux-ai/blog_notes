/*
 * NextArray with a recursive directory.
 *
 * The data is the same as nextarray.c: sorted nodes in fixed-capacity
 * extents, one big huge-page buffer of extent blocks.  What changes is the
 * directory that keeps the extents in key order.  nextarray.c keeps one flat
 * slot array (block pointer, count, max per extent), so a split renumbers
 * every later slot and rebuilds both indexes: O(extents).  Here the directory
 * is itself cut into directory extents of NA_DIR_CAP entries with slack:
 *
 *   top level      one entry per directory extent: pointer, node count, max
 *                  binary search on max; 8-ary running-count tree for at()
 *   directory ext  NA_DIR_CAP entries: block, count, max
 *                  max samples every 8 and 64 entries; 16-ary count tree
 *   extent         the nodes, as before
 *
 * An extent split shifts entries inside one directory extent and rebuilds
 * that directory extent's small indexes; a full directory extent splits in
 * two and shifts the (small) top level.  Both are bounded, not O(extents).
 */
#include "nextarray.h"

#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define AVX512 __attribute__((target("avx512f")))
#define HUGE_PAGE (2u << 20)
#define GROUP 8

/* ---- small helpers ------------------------------------------------------- */

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

static void* alloc_array(uint64_t bytes) {
    bytes = (bytes + 511) / 512 * 512;
    if (bytes < HUGE_PAGE) return aligned_alloc(512, bytes);
    uint64_t rounded = (bytes + HUGE_PAGE - 1) / HUGE_PAGE * HUGE_PAGE;
    void* p = aligned_alloc(HUGE_PAGE, rounded);
    if (p) madvise(p, rounded, MADV_HUGEPAGE);
    return p;
}

static inline char* node_at(const nextarray_t* na, const void* block, size_t i) {
    return (char*)block + i * (size_t)na->node_size;
}

static inline char* dmax(const nextarray_t* na, const nextarray_dir_t* d, int s) {
    return d->max + (size_t)s * (size_t)na->node_size;
}

/* ---- directory extent: count tree ---------------------------------------- */
/*
 * 16-ary tree of running int32 counts over cnt[0..NA_DIR_CAP): root (1 node),
 * 16 nodes, 256 leaves; entry i of a node is the count of its children 0..i.
 */
#define DLT_L1 1
#define DLT_L2 17

static void dlt_rebuild(nextarray_dir_t* d) {
    for (int j = 0; j < 256; j++) {
        int* leaf = &d->dlt[(DLT_L2 + j) * 16];
        int run = 0;
        for (int i = 0; i < 16; i++) { run += d->cnt[j * 16 + i]; leaf[i] = run; }
    }
    for (int j = 0; j < 16; j++) {
        int* node = &d->dlt[(DLT_L1 + j) * 16];
        int run = 0;
        for (int i = 0; i < 16; i++) { run += d->dlt[(DLT_L2 + j * 16 + i) * 16 + 15]; node[i] = run; }
    }
    int run = 0;
    for (int i = 0; i < 16; i++) { run += d->dlt[(DLT_L1 + i) * 16 + 15]; d->dlt[i] = run; }
}

AVX512
static inline void dlt_update(nextarray_dir_t* d, int s, int delta) {
    const __m512i v = _mm512_set1_epi32(delta);
    int* nodes[3] = { &d->dlt[0], &d->dlt[(DLT_L1 + s / 256) * 16], &d->dlt[(DLT_L2 + s / 16) * 16] };
    int pos[3] = { s / 256, (s / 16) % 16, s % 16 };
    for (int l = 0; l < 3; l++) {
        __m512i x = _mm512_load_si512(nodes[l]);
        _mm512_store_si512(nodes[l], _mm512_mask_add_epi32(x, (__mmask16)(0xFFFFu << pos[l]), x, v));
    }
}

// Entry holding position `rem` inside the directory extent; *local is the
// offset inside that extent.
AVX512
static inline int dlt_find(const nextarray_dir_t* d, long rem, long* local) {
    const __m512i r = _mm512_set1_epi32((int)rem);
    int node = 0;   // root
    __mmask16 m = _mm512_cmpgt_epi32_mask(_mm512_load_si512(&d->dlt[0]), r);
    if (!m) return -1;
    int p = __builtin_ctz(m);
    if (p) rem -= d->dlt[p - 1];
    node = p;
    const int* n1 = &d->dlt[(DLT_L1 + node) * 16];
    m = _mm512_cmpgt_epi32_mask(_mm512_load_si512(n1), _mm512_set1_epi32((int)rem));
    if (!m) return -1;
    p = __builtin_ctz(m);
    if (p) rem -= n1[p - 1];
    node = node * 16 + p;
    const int* n2 = &d->dlt[(DLT_L2 + node) * 16];
    m = _mm512_cmpgt_epi32_mask(_mm512_load_si512(n2), _mm512_set1_epi32((int)rem));
    if (!m) return -1;
    p = __builtin_ctz(m);
    if (p) rem -= n2[p - 1];
    (void)r;
    *local = rem;
    return node * 16 + p;
}

/* ---- directory extent: max samples --------------------------------------- */

static void dsamples_rebuild(const nextarray_t* na, nextarray_dir_t* d) {
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

// Extent s's max is now its last node: refresh max[s] and the samples of it.
static inline void dset_max(const nextarray_t* na, nextarray_dir_t* d, int s) {
    size_t ns = (size_t)na->node_size;
    const char* v = node_at(na, d->block[s], (size_t)d->cnt[s] - 1);
    memcpy(dmax(na, d, s), v, ns);
    int n1 = (d->n + GROUP - 1) / GROUP;
    if (s % GROUP != GROUP - 1 && s != d->n - 1) return;
    int g = s / GROUP;
    memcpy(d->smax1 + (size_t)g * ns, v, ns);
    if (g % GROUP != GROUP - 1 && g != n1 - 1) return;
    memcpy(d->smax2 + (size_t)(g / GROUP) * ns, v, ns);
}

// First entry whose max >= key, or the last entry.
static inline int dfind(const nextarray_t* na, const nextarray_dir_t* d, const void* key) {
    size_t ns = (size_t)na->node_size;
    int n1 = (d->n + GROUP - 1) / GROUP, n2 = (n1 + GROUP - 1) / GROUP;
    size_t g = lower_bound(d->smax2, (size_t)n2, ns, key, na->compare);
    if (g >= (size_t)n2) return d->n - 1;
    size_t lo = g * GROUP, span = (size_t)n1 - lo < GROUP ? (size_t)n1 - lo : GROUP;
    g = lo + lower_bound(d->smax1 + lo * ns, span, ns, key, na->compare);
    lo = g * GROUP;
    span = (size_t)d->n - lo < GROUP ? (size_t)d->n - lo : GROUP;
    size_t s = lo + lower_bound(d->max + lo * ns, span, ns, key, na->compare);
    return s < (size_t)d->n ? (int)s : d->n - 1;
}

static void drebuild(const nextarray_t* na, nextarray_dir_t* d) {
    dlt_rebuild(d);
    if (d->n) dsamples_rebuild(na, d);
}

static nextarray_dir_t* dir_alloc(nextarray_t* na) {
    size_t ns = (size_t)na->node_size;
    size_t keys = (size_t)(NA_DIR_CAP + NA_DIR_CAP / GROUP + NA_DIR_CAP / GROUP / GROUP) * ns;
    size_t bytes = (sizeof(nextarray_dir_t) + keys + 63) & ~(size_t)63;
    nextarray_dir_t* d = aligned_alloc(64, bytes);
    if (!d) return NULL;
    memset(d, 0, bytes);
    d->max = (char*)(d + 1);
    d->smax1 = d->max + (size_t)NA_DIR_CAP * ns;
    d->smax2 = d->smax1 + (size_t)(NA_DIR_CAP / GROUP) * ns;
    na->overhead += bytes;
    return d;
}

/* ---- top level ------------------------------------------------------------ */

static int top_height(int cap) {
    int height = 1, nodes = (cap + LOOKUP_NODE_SIZE - 1) / LOOKUP_NODE_SIZE;
    while (nodes > 1) { nodes = (nodes + LOOKUP_NODE_SIZE - 1) / LOOKUP_NODE_SIZE; height++; }
    return height;
}

static inline long* tlt_node(const nextarray_t* na, int level, int idx) {
    return &na->lookup_tree[na->level_offset[level] + idx * LOOKUP_NODE_SIZE];
}

/* ---- top level: max samples ----------------------------------------------- */
/* tmax sampled like a directory extent's maxs: every 8th and every 64th, so
 * finding the directory extent is a short search in L1 plus two lines. */

static void tsamples_rebuild(nextarray_t* na) {
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

// Directory extent t's max is now v: tmax[t] and the samples of it.
static inline void tset_max(nextarray_t* na, int t, const void* v) {
    size_t ns = (size_t)na->node_size;
    memcpy(na->tmax + (size_t)t * ns, v, ns);
    int n1 = (na->ndirs + GROUP - 1) / GROUP;
    if (t % GROUP != GROUP - 1 && t != na->ndirs - 1) return;
    int g = t / GROUP;
    memcpy(na->tsmax1 + (size_t)g * ns, v, ns);
    if (g % GROUP != GROUP - 1 && g != n1 - 1) return;
    memcpy(na->tsmax2 + (size_t)(g / GROUP) * ns, v, ns);
}

// First directory extent whose max >= key, or the last one.
static inline int tfind(const nextarray_t* na, const void* key) {
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

// Rebuild the top count tree and max samples (after the top level changed shape).
static void tlt_rebuild(nextarray_t* na) {
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

AVX512
static inline void tlt_update(nextarray_t* na, int t, long delta) {
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

AVX512
static inline int tlt_find(const nextarray_t* na, long index, long* rem_out) {
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

// (Re)size the top arrays and its count tree for `cap` directory extents.
static int top_reserve(nextarray_t* na, int cap) {
    if (cap <= na->dirs_cap) return 0;
    size_t ns = (size_t)na->node_size;
    nextarray_dir_t** dirs = realloc(na->dirs, (size_t)cap * sizeof *dirs);
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
    na->lookup_tree = aligned_alloc(64, (size_t)na->total_tree_nodes * LOOKUP_NODE_SIZE * sizeof(long));
    if (!na->lookup_tree) return -1;
    tlt_rebuild(na);
    return 0;
}

// Directory extent and entry whose extent should hold key.
static inline int locate(const nextarray_t* na, const void* key, int* s) {
    int t = tfind(na, key);
    *s = dfind(na, na->dirs[t], key);
    return t;
}

static inline void refresh_ends(nextarray_t* na) {
    if (na->total_count == 0 || na->ndirs == 0) { na->head = na->tail = NULL; return; }
    nextarray_dir_t* first = na->dirs[0];
    nextarray_dir_t* last = na->dirs[na->ndirs - 1];
    na->head = first->block[0];
    na->tail = node_at(na, last->block[last->n - 1], (size_t)last->cnt[last->n - 1] - 1);
}

// Directory extent t is full: move its upper half into a new one after it.
static int split_dir(nextarray_t* na, int t) {
    size_t ns = (size_t)na->node_size;
    if (top_reserve(na, na->ndirs + 1 > na->dirs_cap ? na->dirs_cap * 2 : na->dirs_cap) != 0) return -1;
    nextarray_dir_t* d = na->dirs[t];
    nextarray_dir_t* nd = dir_alloc(na);
    if (!nd) return -1;
    int h = d->n / 2, moved = d->n - h;
    memcpy(nd->block, &d->block[h], (size_t)moved * sizeof(void*));
    memcpy(nd->cnt, &d->cnt[h], (size_t)moved * sizeof(int));
    memcpy(nd->max, dmax(na, d, h), (size_t)moved * ns);
    nd->n = moved;
    memset(&d->cnt[h], 0, (size_t)moved * sizeof(int));
    d->n = h;
    drebuild(na, d);
    drebuild(na, nd);
    long ncount = 0;
    for (int i = 0; i < moved; i++) ncount += nd->cnt[i];

    int tail = na->ndirs - t - 1;
    memmove(&na->dirs[t + 2], &na->dirs[t + 1], (size_t)tail * sizeof(void*));
    memmove(&na->tcnt[t + 2], &na->tcnt[t + 1], (size_t)tail * sizeof(long));
    memmove(na->tmax + (size_t)(t + 2) * ns, na->tmax + (size_t)(t + 1) * ns, (size_t)tail * ns);
    na->dirs[t + 1] = nd;
    na->tcnt[t + 1] = ncount;
    na->tcnt[t] -= ncount;
    memcpy(na->tmax + (size_t)(t + 1) * ns, na->tmax + (size_t)t * ns, ns);
    tset_max(na, t, dmax(na, d, h - 1));
    na->ndirs++;
    tlt_rebuild(na);
    return 0;
}

// Extent s of directory extent t is full: its upper half goes into a free
// block in a new entry right after it.  Only this directory extent shifts.
static int split_extent(nextarray_t* na, int t, int s) {
    size_t ns = (size_t)na->node_size;
    if (na->num_free == 0) {
        printf("nextarray_insert: no free extent left\n");
        return -1;
    }
    nextarray_dir_t* d = na->dirs[t];
    void* nb = na->free_blocks[--na->num_free];
    int tail = d->n - s - 1;
    memmove(&d->block[s + 2], &d->block[s + 1], (size_t)tail * sizeof(void*));
    memmove(&d->cnt[s + 2], &d->cnt[s + 1], (size_t)tail * sizeof(int));
    memmove(dmax(na, d, s + 2), dmax(na, d, s + 1), (size_t)tail * ns);
    d->n++;
    int half = na->extent_size / 2, upper = d->cnt[s] - half;
    memcpy(nb, node_at(na, d->block[s], (size_t)half), (size_t)upper * ns);
    d->block[s + 1] = nb;
    d->cnt[s + 1] = upper;
    d->cnt[s] = half;
    memcpy(dmax(na, d, s + 1), dmax(na, d, s), ns);
    memcpy(dmax(na, d, s), node_at(na, d->block[s], (size_t)half - 1), ns);
    drebuild(na, d);
    return 0;
}

/* ---- API ------------------------------------------------------------------- */

void* nextarray_search(nextarray_t* na, void* search_key) {
    if (na->total_count == 0) return NULL;
    int s, t = locate(na, search_key, &s);
    const nextarray_dir_t* d = na->dirs[t];
    size_t ns = (size_t)na->node_size, cnt = (size_t)d->cnt[s];
    char* base = d->block[s];
    size_t pos = lower_bound(base, cnt, ns, search_key, na->compare);
    if (pos < cnt && na->compare(base + pos * ns, search_key) == 0) return base + pos * ns;
    return NULL;
}

void* nextarray_at(nextarray_t* na, long index) {
    long total = (long)na->total_count;
    if (total == 0) {
        printf("NextArray is empty\n");
        exit(1);
    }
    if (index < 0) index += total;
    if (index == 0) return na->head;
    if (index == total - 1) return na->tail;
    if (index < 0 || index >= total) {
        printf("Index %ld out of bounds for NextArray of size %ld\n", index, total);
        return NULL;
    }
    long rem, local;
    int t = tlt_find(na, index, &rem);
    if (t < 0) return NULL;
    const nextarray_dir_t* d = na->dirs[t];
    int s = dlt_find(d, rem, &local);
    if (s < 0 || s >= d->n) return NULL;
    return node_at(na, d->block[s], (size_t)local);
}

int nextarray_insert(nextarray_t* na, void* node) {
    size_t ns = (size_t)na->node_size;
    if (na->ndirs == 0) {
        // First node: one directory extent with one extent.
        if (na->num_free == 0 || top_reserve(na, 16) != 0) return -1;
        nextarray_dir_t* d = dir_alloc(na);
        if (!d) return -1;
        d->block[0] = na->free_blocks[--na->num_free];
        d->n = 1;
        memcpy(d->block[0], node, ns);
        d->cnt[0] = 1;
        memcpy(d->max, node, ns);
        drebuild(na, d);
        na->dirs[0] = d;
        na->tcnt[0] = 1;
        memcpy(na->tmax, node, ns);
        na->ndirs = 1;
        na->total_count = 1;
        tlt_rebuild(na);
        refresh_ends(na);
        return 1;
    }
    int s, t = locate(na, node, &s);
    nextarray_dir_t* d = na->dirs[t];

    if (d->cnt[s] == na->extent_size) {
        if (d->n == NA_DIR_CAP) {
            if (split_dir(na, t) != 0) return -1;
            t = locate(na, node, &s);
            d = na->dirs[t];
        }
        if (split_extent(na, t, s) != 0) return -1;
        if (na->compare(node, dmax(na, d, s)) > 0) s++;
    }

    char* base = d->block[s];
    size_t cnt = (size_t)d->cnt[s];
    size_t pos = lower_bound(base, cnt, ns, node, na->compare);
    if (pos < cnt && na->compare(base + pos * ns, node) == 0) return 0;   // already present
    memmove(base + (pos + 1) * ns, base + pos * ns, (cnt - pos) * ns);
    memcpy(base + pos * ns, node, ns);
    d->cnt[s]++;
    dlt_update(d, s, +1);
    tlt_update(na, t, +1);
    na->total_count++;
    if (pos == cnt) {
        dset_max(na, d, s);
        if (s == d->n - 1) tset_max(na, t, node);
    }
    refresh_ends(na);
    return 1;
}

int nextarray_delete(nextarray_t* na, void* search_key) {
    if (na->total_count == 0) return -1;
    size_t ns = (size_t)na->node_size;
    int s, t = locate(na, search_key, &s);
    nextarray_dir_t* d = na->dirs[t];
    char* base = d->block[s];
    size_t cnt = (size_t)d->cnt[s];
    size_t pos = lower_bound(base, cnt, ns, search_key, na->compare);
    if (pos >= cnt || na->compare(base + pos * ns, search_key) != 0) return -1;

    memmove(base + pos * ns, base + (pos + 1) * ns, (cnt - pos - 1) * ns);
    d->cnt[s]--;
    dlt_update(d, s, -1);
    tlt_update(na, t, -1);
    na->total_count--;

    if (d->cnt[s] == 0) {
        // Drained: the block goes back on the free stack and the entry leaves
        // its directory extent (shifting only that directory extent).
        na->free_blocks[na->num_free++] = d->block[s];
        int tail = d->n - s - 1;
        memmove(&d->block[s], &d->block[s + 1], (size_t)tail * sizeof(void*));
        memmove(&d->cnt[s], &d->cnt[s + 1], (size_t)tail * sizeof(int));
        memmove(dmax(na, d, s), dmax(na, d, s + 1), (size_t)tail * ns);
        d->n--;
        d->cnt[d->n] = 0;
        if (d->n == 0) {
            free(d);
            int ttail = na->ndirs - t - 1;
            memmove(&na->dirs[t], &na->dirs[t + 1], (size_t)ttail * sizeof(void*));
            memmove(&na->tcnt[t], &na->tcnt[t + 1], (size_t)ttail * sizeof(long));
            memmove(na->tmax + (size_t)t * ns, na->tmax + (size_t)(t + 1) * ns, (size_t)ttail * ns);
            na->ndirs--;
            tlt_rebuild(na);
        } else {
            drebuild(na, d);
            if (s == d->n) tset_max(na, t, dmax(na, d, d->n - 1));
        }
    } else if (pos == cnt - 1) {
        dset_max(na, d, s);
        if (s == d->n - 1) tset_max(na, t, dmax(na, d, s));
    }
    refresh_ends(na);
    return 0;
}

void* nextarray_next(nextarray_t* na, nextarray_iter_t* iter) {
    while (iter->dir < na->ndirs) {
        const nextarray_dir_t* d = na->dirs[iter->dir];
        if (iter->extent_idx < d->n) {
            if (iter->idx < d->cnt[iter->extent_idx])
                return node_at(na, d->block[iter->extent_idx], (size_t)iter->idx++);
            iter->extent_idx++;
            iter->idx = 0;
            continue;
        }
        iter->dir++;
        iter->extent_idx = 0;
        iter->idx = 0;
    }
    return NULL;
}

/* ---- setup ----------------------------------------------------------------- */

nextarray_t* nextarray_alloc(size_t buffer_size, int extent_size, int node_size,
                             int (*compare)(const void* a, const void* b)) {
    nextarray_t* na = calloc(1, sizeof *na);
    if (!na) return NULL;
    na->node_size = node_size;
    na->compare = compare;

    uint64_t extent_bytes = ((uint64_t)extent_size * node_size + 511) / 512 * 512;
    na->extent_size = (int)(extent_bytes / node_size);
    na->num_extents = (int)(buffer_size / (size_t)na->extent_size);
    if (na->num_extents < 6) {
        printf("NextArray needs at least 6 extents; increase the buffer size\n");
        exit(1);
    }
    na->num_data_extents = na->num_extents / 10 >= 4 ? na->num_extents / 10 * 9 : na->num_extents - 4;

    uint64_t buffer_bytes = (uint64_t)na->num_extents * extent_bytes;
    na->buffer_size = buffer_bytes / node_size;
    na->buffer = alloc_array(buffer_bytes);
    na->free_blocks = alloc_array((uint64_t)na->num_extents * sizeof(void*));
    if (!na->buffer || !na->free_blocks) {
        printf("Failed to allocate the NextArray buffer\n");
        nextarray_free(na);
        return NULL;
    }
    memset(na->buffer, 0, buffer_bytes);
    // Free stack: lowest block on top.
    for (int i = 0; i < na->num_extents; i++)
        na->free_blocks[i] = (char*)na->buffer + (uint64_t)(na->num_extents - 1 - i) * extent_bytes;
    na->num_free = na->num_extents;
    na->overhead = sizeof *na + (uint64_t)na->num_extents * sizeof(void*);
    if (top_reserve(na, 16) != 0) {
        nextarray_free(na);
        return NULL;
    }
    return na;
}

void nextarray_free(nextarray_t* na) {
    if (!na) return;
    for (int t = 0; t < na->ndirs; t++) free(na->dirs[t]);
    free(na->dirs);
    free(na->tcnt);
    free(na->tmax);
    free(na->tsmax1);
    free(na->tsmax2);
    free(na->lookup_tree);
    free(na->free_blocks);
    free(na->buffer);
    free(na);
}

// The first `count` nodes of buffer are the data set.  Deal them out over
// the first num_data_extents blocks exactly as nextarray.c does, then build
// directory extents NA_DIR_FILL entries full over those blocks.
void nextarray_bulk_init(nextarray_t* na, size_t count, bool sorted) {
    if (count == 0) return;
    size_t ns = (size_t)na->node_size;
    uint64_t extent_bytes = (uint64_t)na->extent_size * ns;
    if (!sorted) {
        printf("Sorting buffer...\n");
        qsort(na->buffer, count, ns, na->compare);
    }
    int used = count < (size_t)na->num_data_extents ? (int)count : na->num_data_extents;
    size_t per = count / (size_t)used, remainder = count % (size_t)used;
    size_t src = count;
    #define EXTENT_COUNT(e) ((int)(per + ((size_t)(e) < remainder ? 1 : 0)))
    for (int e = used - 1; e >= 0; e--) {
        size_t n = (size_t)EXTENT_COUNT(e);
        src -= n;
        memmove((char*)na->buffer + (uint64_t)e * extent_bytes, (char*)na->buffer + src * ns, n * ns);
    }
    // blocks [0, used) are taken; the free stack keeps the rest
    na->num_free = 0;
    for (int i = na->num_extents - 1; i >= used; i--)
        na->free_blocks[na->num_free++] = (char*)na->buffer + (uint64_t)i * extent_bytes;

    int ndirs = (used + NA_DIR_FILL - 1) / NA_DIR_FILL;
    if (top_reserve(na, ndirs * 2 > 16 ? ndirs * 2 : 16) != 0) {
        printf("nextarray_bulk_init: out of memory\n");
        exit(1);
    }
    int e = 0;
    for (int t = 0; t < ndirs; t++) {
        nextarray_dir_t* d = dir_alloc(na);
        if (!d) {
            printf("nextarray_bulk_init: out of memory\n");
            exit(1);
        }
        int take = used / ndirs + (t < used % ndirs ? 1 : 0);
        long total = 0;
        for (int i = 0; i < take; i++, e++) {
            d->block[i] = (char*)na->buffer + (uint64_t)e * extent_bytes;
            d->cnt[i] = EXTENT_COUNT(e);
            memcpy(dmax(na, d, i), node_at(na, d->block[i], (size_t)d->cnt[i] - 1), ns);
            total += d->cnt[i];
        }
        d->n = take;
        drebuild(na, d);
        na->dirs[t] = d;
        na->tcnt[t] = total;
        tset_max(na, t, dmax(na, d, take - 1));
    }
    #undef EXTENT_COUNT
    na->ndirs = ndirs;
    na->total_count = count;
    tlt_rebuild(na);
    refresh_ends(na);
}

// Splits and drains are local here, so nothing needs evening out.
int nextarray_balance(nextarray_t* na) {
    (void)na;
    return 0;
}

/* ---- debug helpers --------------------------------------------------------- */

int nextarray_dump_nodes_to_file(nextarray_t* na, const char* file) {
    FILE* fp = fopen(file, "wb");
    if (!fp) {
        printf("Failed to open file %s\n", file);
        return -1;
    }
    int rc = 0;
    for (int t = 0; t < na->ndirs && rc == 0; t++) {
        const nextarray_dir_t* d = na->dirs[t];
        for (int s = 0; s < d->n && rc == 0; s++)
            if (fwrite(d->block[s], (size_t)na->node_size, (size_t)d->cnt[s], fp) != (size_t)d->cnt[s]) {
                printf("Failed to write to file %s\n", file);
                rc = -1;
            }
    }
    fclose(fp);
    return rc;
}

void nextarray_debug_extent(nextarray_t* na, int e) {
    for (int t = 0; t < na->ndirs; t++) {
        const nextarray_dir_t* d = na->dirs[t];
        if (e < d->n) {
            printf("DEBUG Extent [%d] (dir %d entry %d): count=%d capacity=%d\n", e, t, e, d->cnt[e], na->extent_size);
            for (int i = 0; i < d->cnt[e]; i++)
                printf("  DEBUG data[%d] = %ld\n", i, *(long*)node_at(na, d->block[e], (size_t)i));
            return;
        }
        e -= d->n;
    }
    printf("Invalid extent index\n");
}

bool nextarray_print_stats(nextarray_t* na) {
    bool ok = true;
    long prev_max = 0, total = 0;
    int extents = 0;
    printf("NextArray Stats (recursive directory):\n");
    printf("  Blocks: %d, free %d; directory extents: %d\n", na->num_extents, na->num_free, na->ndirs);
    for (int t = 0; t < na->ndirs; t++) {
        const nextarray_dir_t* d = na->dirs[t];
        long dtotal = 0;
        for (int s = 0; s < d->n; s++) {
            long min = *(long*)node_at(na, d->block[s], 0);
            long max = *(long*)node_at(na, d->block[s], (size_t)d->cnt[s] - 1);
            if (extents > 0 && prev_max >= min) {
                printf("  overlap at dir %d entry %d\n", t, s);
                ok = false;
            }
            if (d->cnt[s] <= 0) { printf("  empty entry at dir %d entry %d\n", t, s); ok = false; }
            prev_max = max;
            dtotal += d->cnt[s];
            extents++;
        }
        if (dtotal != na->tcnt[t]) { printf("  dir %d count %ld != tcnt %ld\n", t, dtotal, na->tcnt[t]); ok = false; }
        total += dtotal;
    }
    printf("  Extents in use: %d, total count %zu\n", extents, na->total_count);
    if ((size_t)total != na->total_count) { printf("  total mismatch %ld\n", total); ok = false; }
    return ok;
}
