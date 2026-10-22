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
} nextarray_t;

typedef struct {
    int dir;
    int extent_idx;
    int idx;
} nextarray_iter_t;

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

///// DEBUG HELPERS ////////////////////

bool nextarray_print_stats(nextarray_t* na);
void nextarray_debug_extent(nextarray_t* na, int extent_index);
int nextarray_dump_nodes_to_file(nextarray_t* na, const char* file);

#endif
