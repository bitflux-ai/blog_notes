#ifndef NEXTARRAY_H
#define NEXTARRAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * NextArray: a sorted array cut into fixed-capacity extents.  Inserts and
 * deletes shift nodes inside one extent only.  The directory that keeps the
 * extents in key order is cut the same way (directory extents with slack),
 * so a split or drain shifts one directory extent, not the whole array.
 * See ARCH.md.
 */

#define LOOKUP_NODE_SIZE 8      // entries per lookup-tree node: 8 longs = one cache line
#define MAX_INDEX_LEVELS 8

/*
 * Recursive directory: the per-extent directory
 * (block, count, max) is itself cut into directory extents of
 * NA_DIR_CAP entries with slack, so an extent split shifts entries inside
 * one directory extent instead of the whole array.  A small top level has
 * one entry per directory extent.
 */
#define NA_DIR_CAP   4096       // entries per directory extent
#define NA_DIR_FILL  3072       // entries per directory extent after bulk_init (75%)
#define NA_DLT_NODES 273        // 16-ary count tree over 4096 entries: 1 + 16 + 256 nodes

typedef struct {
    _Alignas(64) int dlt[NA_DLT_NODES * 16];   // running counts, 16 int32 = one line per node
    void* block[NA_DIR_CAP];    // extent data
    int cnt[NA_DIR_CAP];        // nodes in use (0 past n)
    int n;                      // entries in use
    char* max;                  // [NA_DIR_CAP] copy of each extent's last node
    char* smax1;                // [NA_DIR_CAP/8] every 8th max (last of each group)
    char* smax2;                // [NA_DIR_CAP/64]
} nextarray_dir_t;

typedef struct {
    // Nodes
    void* buffer;               // every extent block, back to back (huge pages)
    size_t buffer_size;         // capacity of buffer in nodes
    int node_size;
    int extent_size;            // capacity of one extent in nodes
    int (*compare)(const void* a, const void* b);
    int num_extents;            // blocks in buffer
    int num_data_extents;       // blocks bulk_init fills (90%)
    void** free_blocks;         // stack of unused blocks
    int num_free;
    void* head;
    void* tail;
    size_t total_count;

    // Top level: one entry per directory extent, in key order.
    nextarray_dir_t** dirs;
    long* tcnt;                 // nodes under each directory extent
    char* tmax;                 // max node of each directory extent
    char* tsmax1;               // every 8th tmax (last of each group)
    char* tsmax2;               // every 64th
    int ndirs, dirs_cap;

    // 8-ary running-count tree over tcnt (same shape as the original's).
    long* lookup_tree;
    int tree_height;
    int total_tree_nodes;
    int level_offset[16];

    uint64_t overhead;

    // Change counters for callers that resolve an extent ahead of time:
    // structure_gen moves when extents move (split, drain, directory split),
    // count_gen on every count change (positions move).
    uint64_t structure_gen;
    uint64_t count_gen;
} nextarray_t;

typedef struct {
    int dir;
    int extent_idx;
    int idx;
} nextarray_iter_t;

///// KEY COMPARE ////////////////////
// Generic nodes go through na->compare.  Built with -DNEXTARRAY_U64_KEYS the
// nodes are 8-byte unsigned keys in numeric order (what the benchmarks use):
// the compare is inlined and lower_bound is branchless (a conditional move
// per probe instead of a coin-flip branch), as art's radix descent and
// libart's memcmp already are.
#ifdef NEXTARRAY_U64_KEYS
static inline int nextarray_cmp(const nextarray_t* na, const void* a, const void* b) {
    (void)na;
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}
// First index with base[i] >= key, or count.
static inline size_t nextarray_lower_bound(const char* base, size_t count, size_t node_size, const void* key,
                                           int (*cmp)(const void*, const void*)) {
    (void)node_size; (void)cmp;
    if (count == 0) return 0;
    const uint64_t* a = (const uint64_t*)base;
    const uint64_t* p = a;
    uint64_t k = *(const uint64_t*)key;
    size_t n = count;
    while (n > 1) {
        size_t half = n / 2;
        // both places the next probe can land: branchless gives up the
        // speculative load a predicted branch would have started, so start
        // both of them instead
        __builtin_prefetch(p + half / 2);
        __builtin_prefetch(p + half + half / 2);
        p = p[half] < k ? p + half : p;
        n -= half;
    }
    return (size_t)(p - a) + (*p < k);
}
#else
static inline int nextarray_cmp(const nextarray_t* na, const void* a, const void* b) { return na->compare(a, b); }
static inline size_t nextarray_lower_bound(const char* base, size_t count, size_t node_size, const void* key,
                                           int (*cmp)(const void*, const void*)) {
    size_t lo = 0, n = count;
    while (n > 0) {
        size_t half = n / 2;
        if (cmp(base + (lo + half) * node_size, key) < 0) { lo += half + 1; n -= half + 1; }
        else n = half;
    }
    return lo;
}
#endif

///// API ////////////////////

// Allocate an empty NextArray able to hold buffer_size nodes.  extent_size is
// rounded up so an extent is a multiple of 512 bytes.
nextarray_t* nextarray_alloc(size_t buffer_size, int extent_size, int node_size,
                             int (*compare)(const void* a, const void* b));
void nextarray_free(nextarray_t* na);

// Take the first `count` nodes already written into na->buffer (sorted or
// not) and spread them evenly over the data extents.
void nextarray_bulk_init(nextarray_t* na, size_t count, bool sorted);

// Even out the node count across extents.  Returns 0 on success.
int nextarray_balance(nextarray_t* na);

// Pointer to the node equal to search_key, or NULL.
void* nextarray_search(nextarray_t* na, void* search_key);

// Pointer to the node at sorted position index (negative counts from the end).
void* nextarray_at(nextarray_t* na, long index);

// 1 if inserted, 0 if an equal node was already present, -1 if out of extents.
int nextarray_insert(nextarray_t* na, void* node);

// 0 if deleted, -1 if not found.
int nextarray_delete(nextarray_t* na, void* search_key);

// Next node in sorted order, NULL at the end.
void* nextarray_next(nextarray_t* na, nextarray_iter_t* iter);

///// BATCHED / PREFETCH SUPPORT ////////////////////
// An extent handle is (directory extent << NA_DIR_BITS) | entry.  Resolve
// where an operation will land, so a caller can prefetch the extent and later
// execute without looking it up again.  The *_in variants trust the handle
// passed in; check structure_gen (count_gen for _at) first.
#define NA_DIR_BITS 12          // NA_DIR_CAP == 1 << NA_DIR_BITS
// Layout a batched resolver can compute addresses from without loading
// pointers: a directory extent's count tree has its root at node 0, the 16
// level-1 nodes from NA_DLT_L1 and the 256 leaves from NA_DLT_L2; its max
// arrays (max, every 8th, every 64th) follow the struct.
#define NA_DLT_L1 1
#define NA_DLT_L2 17
static inline char* nextarray_dir_max(const nextarray_dir_t* d) { return (char*)(d + 1); }
static inline char* nextarray_dir_smax1(const nextarray_dir_t* d, size_t ns) { return (char*)(d + 1) + NA_DIR_CAP * ns; }
static inline char* nextarray_dir_smax2(const nextarray_dir_t* d, size_t ns) { return (char*)(d + 1) + (NA_DIR_CAP + NA_DIR_CAP / 8) * ns; }
static inline nextarray_dir_t* nextarray_dir_of(const nextarray_t* na, int e) { return na->dirs[e >> NA_DIR_BITS]; }
static inline int nextarray_entry_of(int e) { return e & (NA_DIR_CAP - 1); }
static inline char* nextarray_block(const nextarray_t* na, int e) {
    return (char*)nextarray_dir_of(na, e)->block[nextarray_entry_of(e)];
}
static inline int nextarray_count(const nextarray_t* na, int e) { return nextarray_dir_of(na, e)->cnt[nextarray_entry_of(e)]; }

int   nextarray_locate(nextarray_t* na, const void* key);                  // extent handle
int   nextarray_locate_top(const nextarray_t* na, const void* key);      // directory extent
int   nextarray_locate_index(nextarray_t* na, long index, long* local);    // extent handle, or -1
void* nextarray_extent_data(const nextarray_t* na, int extent, size_t* bytes); // nodes in use
void* nextarray_search_in(nextarray_t* na, int extent, void* key);
void* nextarray_at_in(nextarray_t* na, int extent, long local);
int   nextarray_insert_in(nextarray_t* na, int extent, void* node);
int   nextarray_delete_in(nextarray_t* na, int extent, void* key);
// Position already known (caller searched and validated it):
//   insert_at: 1 inserted, -2 extent full (caller should fall back to insert_in)
//   delete_at: 0
int   nextarray_insert_at(nextarray_t* na, int extent, size_t pos, const void* node);
int   nextarray_delete_at(nextarray_t* na, int extent, size_t pos);

///// DEBUG HELPERS ////////////////////

bool nextarray_print_stats(nextarray_t* na);
void nextarray_debug_extent(nextarray_t* na, int extent_index);
int nextarray_dump_nodes_to_file(nextarray_t* na, const char* file);

#endif
