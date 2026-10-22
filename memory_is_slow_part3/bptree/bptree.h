#ifndef BPTREE_H
#define BPTREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * In-RAM order-statistic B+ tree for the benchmark suite.
 *
 * Design notes (fair peer to nextarray):
 * - Leaves hold up to `leaf_capacity` contiguous Node keys (like an extent).
 * - Internal nodes hold up to `branch_factor` children with separator keys and
 *   per-child subtree counts so search/at/insert/delete stay O(log n) with
 *   multiway fanout and dense sequential leaf data.
 * - Leaves are doubly linked for O(n) sequential scan.
 *
 * Default leaf_capacity 512 matches nextarray's default extent_size so leaf
 * payload and per-mutate memmove cost are comparable.
 */

/* Largest key/Node size; insert keeps separators in stack buffers this big. */
#define BPTREE_MAX_NODE_SIZE 64

typedef struct bptree_node bptree_node_t;

struct bptree_node {
    bool is_leaf;
    int n; /* leaf: #keys; internal: #separator keys (= #children - 1) */
    bptree_node_t *parent;
    union {
        struct {
            void *keys;           /* n contiguous nodes, node_size each */
            bptree_node_t *next;
            bptree_node_t *prev;
        } leaf;
        struct {
            void *keys;           /* n separators (copies of first key of right child) */
            bptree_node_t **children; /* n + 1 children */
            size_t *counts;       /* subtree key counts per child, length n + 1 */
        } internal;
    } u;
};

typedef struct {
    bptree_node_t *root;
    bptree_node_t *leftmost;  /* first leaf */
    bptree_node_t *rightmost; /* last leaf */
    void *head;               /* pointer to min Node (in leftmost leaf) */
    void *tail;               /* pointer to max Node (in rightmost leaf) */

    size_t count;             /* total live keys */
    int leaf_capacity;        /* max keys per leaf */
    int branch_factor;        /* max children per internal node */
    int node_size;            /* sizeof one key/Node, <= BPTREE_MAX_NODE_SIZE */
    double bulk_fill;         /* bulk_init fills leaves/internals to this fraction (default 1) */
    int (*compare)(const void *a, const void *b);

    uint64_t overhead;        /* rough structural bytes (excludes key payloads) */
    size_t nodes_allocated;   /* tree nodes currently live */
} bptree_t;

typedef struct {
    bptree_node_t *leaf;
    int idx;
} bptree_iter_t;

/* Search for exact key; returns pointer into a leaf, or NULL. */
void *bptree_search(bptree_t *t, void *search_key);

/* Rank access: index in [0, count). Negative indexes count from end like nextarray. */
void *bptree_at(bptree_t *t, long index);

/* Insert a copy of *node. Returns 1 on success, 0 on duplicate/failure. */
int bptree_insert(bptree_t *t, void *node);

/* Delete by key. Returns 0 on success, non-zero if missing/failure. */
int bptree_delete(bptree_t *t, void *search_key);

/* In-order iterator; pass zeroed iter. Returns NULL when exhausted. */
void *bptree_next(bptree_t *t, bptree_iter_t *iter);

/* Create empty tree. leaf_capacity and branch_factor must be >= 2. */
bptree_t *bptree_alloc(int leaf_capacity, int branch_factor, int node_size,
                       int (*compare)(const void *a, const void *b));

void bptree_free(bptree_t *t);

/*
 * Bulk load from a contiguous array of `count` nodes already owned by the
 * caller (copied into leaves). If sorted==false, qsort is applied first on a
 * temporary path via the caller's buffer (sorted in place).  Leaves and
 * internal nodes are filled to t->bulk_fill of capacity, spread evenly, so
 * the first inserts don't all split and the first deletes don't all merge.
 */
int bptree_bulk_init(bptree_t *t, void *nodes, size_t count, bool sorted);

/* Dump all keys in order to a binary file of Node-sized records. */
int bptree_dump_nodes_to_file(bptree_t *t, const char *file);

/* Optional debug. */
void bptree_print_stats(bptree_t *t);

#endif /* BPTREE_H */
