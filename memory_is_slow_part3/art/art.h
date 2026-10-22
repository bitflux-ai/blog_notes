#ifndef ART_H
#define ART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Adaptive Radix Tree (ART) specialized for 8-byte keys (suite Node = uint64).
 *
 * Keys are stored big-endian so numeric order matches lexicographic byte order.
 * Classic adaptive nodes: Node4 / Node16 / Node48 / Node256 + path compression.
 *
 * Extra vs textbook ART (needed for this suite):
 * - per-node subtree `count` for O(height) order-statistic `at`
 * - in-order iterator for full scans
 * - cached min/max leaf pointers for O(1) head/tail
 *
 * This is the natural integer-key competitor to AVL / B+ / nextarray.
 */

#define ART_KEY_BYTES 8
#define ART_MAX_PREFIX 8

typedef struct art_node art_node_t;
typedef struct art_leaf art_leaf_t;

struct art_leaf {
    uint8_t type; /* ART_LEAF */
    /* suite Node payload (uint64 value); address is stable until delete */
    uint64_t value;
};

/* Internal header shared by Node4/16/48/256 (always first member).
 * num_children must hold 0..256 (Node256 can be full) — not uint8_t. */
struct art_node {
    uint8_t type;
    uint16_t num_children;
    uint8_t partial_len;
    uint8_t partial[ART_MAX_PREFIX];
    size_t count; /* keys in this subtree */
};

typedef struct {
    art_node_t *root;   /* may be leaf or internal; NULL if empty */
    void *head;         /* &min_leaf->value (suite Node*), or NULL */
    void *tail;         /* &max_leaf->value (suite Node*), or NULL */
    size_t count;
    uint64_t overhead;  /* rough structural bytes */
    size_t nodes_allocated;
    int (*compare)(const void *a, const void *b); /* unused for ordering; kept for API parity */
} art_t;

/* Explicit stack iterator (no leaf linked list). Zero before first use. */
typedef struct {
    void *stack[16];
    int child_i[16]; /* position of the current child: slot (Node4/16) or key byte (Node48/256) */
    int sp;
    art_leaf_t *cur;
    int started;
} art_iter_t;

void *art_search(art_t *t, void *search_key);
void *art_at(art_t *t, long index);
int art_insert(art_t *t, void *node);   /* 1 success, 0 duplicate/fail */
int art_delete(art_t *t, void *search_key); /* 0 success, nonzero miss/fail */
void *art_next(art_t *t, art_iter_t *iter);

art_t *art_alloc(int (*compare)(const void *a, const void *b));
void art_free(art_t *t);

/* Insert all keys (optionally qsort first). */
int art_bulk_init(art_t *t, void *nodes, size_t count, bool sorted);

int art_dump_nodes_to_file(art_t *t, const char *file);
void art_print_stats(art_t *t);

#endif /* ART_H */
