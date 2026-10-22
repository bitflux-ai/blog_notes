#include "art.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Node type tags (must match leaf/internal discrimination). */
#define ART_NODE4   1
#define ART_NODE16  2
#define ART_NODE48  3
#define ART_NODE256 4
#define ART_LEAF    5

typedef struct {
    art_node_t n;
    uint8_t keys[4];
    art_node_t *children[4];
} art_node4_t;

typedef struct {
    art_node_t n;
    uint8_t keys[16];
    art_node_t *children[16];
} art_node16_t;

typedef struct {
    art_node_t n;
    uint8_t keys[256]; /* byte -> child index+1, 0 = empty */
    art_node_t *children[48];
} art_node48_t;

typedef struct {
    art_node_t n;
    art_node_t *children[256];
} art_node256_t;

/* ---------- key helpers ---------- */

static void key_bytes(uint64_t v, uint8_t out[ART_KEY_BYTES]) {
    for (int i = 0; i < ART_KEY_BYTES; i++) {
        out[i] = (uint8_t)((v >> (56 - 8 * i)) & 0xffu);
    }
}

static int is_leaf(const void *n) {
    return n && *(const uint8_t *)n == ART_LEAF;
}

/* Subtree key count. Leaves must not be read as art_node_t (smaller object). */
static size_t node_count(const void *n) {
    if (!n) {
        return 0;
    }
    if (*(const uint8_t *)n == ART_LEAF) {
        return 1;
    }
    return ((const art_node_t *)n)->count;
}



static void account(art_t *t, size_t bytes, int sign) {
    if (sign > 0) {
        t->overhead += bytes;
        t->nodes_allocated++;
    } else {
        if (t->overhead >= bytes) {
            t->overhead -= bytes;
        }
        if (t->nodes_allocated > 0) {
            t->nodes_allocated--;
        }
    }
}

/* ---------- allocation ---------- */

static art_leaf_t *alloc_leaf(art_t *t, uint64_t value) {
    art_leaf_t *leaf = calloc(1, sizeof(*leaf));
    if (!leaf) {
        return NULL;
    }
    leaf->type = ART_LEAF;
    leaf->value = value;
    account(t, sizeof(*leaf), +1);
    return leaf;
}

static art_node4_t *alloc_node4(art_t *t) {
    art_node4_t *n = calloc(1, sizeof(*n));
    if (!n) {
        return NULL;
    }
    n->n.type = ART_NODE4;
    account(t, sizeof(*n), +1);
    return n;
}

static art_node16_t *alloc_node16(art_t *t) {
    art_node16_t *n = calloc(1, sizeof(*n));
    if (!n) {
        return NULL;
    }
    n->n.type = ART_NODE16;
    account(t, sizeof(*n), +1);
    return n;
}

static art_node48_t *alloc_node48(art_t *t) {
    art_node48_t *n = calloc(1, sizeof(*n));
    if (!n) {
        return NULL;
    }
    n->n.type = ART_NODE48;
    account(t, sizeof(*n), +1);
    return n;
}

static art_node256_t *alloc_node256(art_t *t) {
    art_node256_t *n = calloc(1, sizeof(*n));
    if (!n) {
        return NULL;
    }
    n->n.type = ART_NODE256;
    account(t, sizeof(*n), +1);
    return n;
}

static size_t sizeof_node(const art_node_t *n) {
    switch (n->type) {
        case ART_NODE4: return sizeof(art_node4_t);
        case ART_NODE16: return sizeof(art_node16_t);
        case ART_NODE48: return sizeof(art_node48_t);
        case ART_NODE256: return sizeof(art_node256_t);
        default: return 0;
    }
}

static void free_leaf(art_t *t, art_leaf_t *leaf) {
    account(t, sizeof(*leaf), -1);
    free(leaf);
}

static void free_inner(art_t *t, art_node_t *n) {
    account(t, sizeof_node(n), -1);
    free(n);
}

static void free_tree(art_t *t, void *n) {
    if (!n) {
        return;
    }
    if (is_leaf(n)) {
        free_leaf(t, (art_leaf_t *)n);
        return;
    }
    art_node_t *node = (art_node_t *)n;
    switch (node->type) {
        case ART_NODE4: {
            art_node4_t *p = (art_node4_t *)node;
            for (int i = 0; i < p->n.num_children; i++) {
                free_tree(t, p->children[i]);
            }
            break;
        }
        case ART_NODE16: {
            art_node16_t *p = (art_node16_t *)node;
            for (int i = 0; i < p->n.num_children; i++) {
                free_tree(t, p->children[i]);
            }
            break;
        }
        case ART_NODE48: {
            /* children[] can have holes (delete just clears a slot) */
            art_node48_t *p = (art_node48_t *)node;
            for (int b = 0; b < 256; b++) {
                if (p->keys[b]) {
                    free_tree(t, p->children[p->keys[b] - 1]);
                }
            }
            break;
        }
        case ART_NODE256: {
            art_node256_t *p = (art_node256_t *)node;
            for (int i = 0; i < 256; i++) {
                if (p->children[i]) {
                    free_tree(t, p->children[i]);
                }
            }
            break;
        }
        default:
            break;
    }
    free_inner(t, node);
}

/* ---------- child index helpers (sorted by key byte) ---------- */

static int find_child_idx(const art_node_t *node, uint8_t byte) {
    switch (node->type) {
        case ART_NODE4: {
            const art_node4_t *p = (const art_node4_t *)node;
            for (int i = 0; i < p->n.num_children; i++) {
                if (p->keys[i] == byte) {
                    return i;
                }
            }
            return -1;
        }
        case ART_NODE16: {
            const art_node16_t *p = (const art_node16_t *)node;
            for (int i = 0; i < p->n.num_children; i++) {
                if (p->keys[i] == byte) {
                    return i;
                }
            }
            return -1;
        }
        case ART_NODE48: {
            const art_node48_t *p = (const art_node48_t *)node;
            uint8_t idx = p->keys[byte];
            return idx ? (int)idx - 1 : -1;
        }
        case ART_NODE256: {
            const art_node256_t *p = (const art_node256_t *)node;
            return p->children[byte] ? (int)byte : -1;
        }
        default:
            return -1;
    }
}

static art_node_t *get_child(const art_node_t *node, uint8_t byte) {
    int idx = find_child_idx(node, byte);
    if (idx < 0) {
        return NULL;
    }
    switch (node->type) {
        case ART_NODE4:
            return ((art_node4_t *)node)->children[idx];
        case ART_NODE16:
            return ((art_node16_t *)node)->children[idx];
        case ART_NODE48:
            return ((art_node48_t *)node)->children[idx];
        case ART_NODE256:
            return ((art_node256_t *)node)->children[byte];
        default:
            return NULL;
    }
}

/* Child pointer at ordered slot i (0 .. num_children-1), and its key byte. */
static art_node_t *child_at_slot(const art_node_t *node, int slot, uint8_t *out_byte) {
    switch (node->type) {
        case ART_NODE4: {
            const art_node4_t *p = (const art_node4_t *)node;
            if (slot < 0 || slot >= p->n.num_children) {
                return NULL;
            }
            if (out_byte) {
                *out_byte = p->keys[slot];
            }
            return p->children[slot];
        }
        case ART_NODE16: {
            const art_node16_t *p = (const art_node16_t *)node;
            if (slot < 0 || slot >= p->n.num_children) {
                return NULL;
            }
            if (out_byte) {
                *out_byte = p->keys[slot];
            }
            return p->children[slot];
        }
        case ART_NODE48: {
            /* children[] is packed by insertion order — walk bytes for key order */
            const art_node48_t *p = (const art_node48_t *)node;
            int seen = 0;
            for (int b = 0; b < 256; b++) {
                if (p->keys[b]) {
                    if (seen == slot) {
                        if (out_byte) {
                            *out_byte = (uint8_t)b;
                        }
                        return p->children[p->keys[b] - 1];
                    }
                    seen++;
                }
            }
            return NULL;
        }
        case ART_NODE256: {
            const art_node256_t *p = (const art_node256_t *)node;
            int seen = 0;
            for (int b = 0; b < 256; b++) {
                if (p->children[b]) {
                    if (seen == slot) {
                        if (out_byte) {
                            *out_byte = (uint8_t)b;
                        }
                        return p->children[b];
                    }
                    seen++;
                }
            }
            return NULL;
        }
        default:
            return NULL;
    }
}

static int num_children(const art_node_t *node) {
    if (node->type == ART_NODE256) {
        return node->num_children;
    }
    return node->num_children;
}

/* ---------- prefix ---------- */

static int check_prefix(const art_node_t *node, const uint8_t *key, int depth) {
    int max = ART_KEY_BYTES - depth;
    if (max < 0) {
        max = 0;
    }
    int len = node->partial_len;
    if (len > max) {
        len = max;
    }
    int matched = 0;
    for (; matched < len; matched++) {
        if (node->partial[matched] != key[depth + matched]) {
            break;
        }
    }
    return matched;
}

/* When leaf is under a compressed path, compare remaining bytes via full leaf key. */
static int leaf_matches(const art_leaf_t *leaf, const uint8_t *key) {
    uint8_t kb[ART_KEY_BYTES];
    key_bytes(leaf->value, kb);
    return memcmp(kb, key, ART_KEY_BYTES) == 0;
}

/* ---------- min / max leaf ---------- */

/* The first child at key-order position *pos or after it, or NULL; *pos
 * becomes that child's position: its slot in Node4/16 (kept in key order),
 * its key byte in Node48/256.  Walking children with *pos + 1 visits each
 * node once, where child_at_slot(s) rescans a Node48/256 byte map from 0. */
static void *child_from(const art_node_t *node, int *pos) {
    switch (node->type) {
        case ART_NODE4:
        case ART_NODE16:
            return child_at_slot(node, *pos, NULL);
        case ART_NODE48: {
            const art_node48_t *p = (const art_node48_t *)node;
            for (int b = *pos; b < 256; b++) {
                if (p->keys[b]) {
                    *pos = b;
                    return p->children[p->keys[b] - 1];
                }
            }
            return NULL;
        }
        case ART_NODE256: {
            const art_node256_t *p = (const art_node256_t *)node;
            for (int b = *pos; b < 256; b++) {
                if (p->children[b]) {
                    *pos = b;
                    return p->children[b];
                }
            }
            return NULL;
        }
        default:
            return NULL;
    }
}

static art_leaf_t *minimum(void *n) {
    while (n && !is_leaf(n)) {
        art_node_t *node = (art_node_t *)n;
        n = child_at_slot(node, 0, NULL);
    }
    return (art_leaf_t *)n;
}

static art_leaf_t *maximum(void *n) {
    while (n && !is_leaf(n)) {
        art_node_t *node = (art_node_t *)n;
        int nc = num_children(node);
        n = child_at_slot(node, nc - 1, NULL);
    }
    return (art_leaf_t *)n;
}

static void refresh_ends(art_t *t) {
    if (!t->root || t->count == 0) {
        t->head = NULL;
        t->tail = NULL;
        return;
    }
    art_leaf_t *mn = minimum(t->root);
    art_leaf_t *mx = maximum(t->root);
    t->head = mn ? (void *)&mn->value : NULL;
    t->tail = mx ? (void *)&mx->value : NULL;
}

/* ---------- grow / add child ---------- */

static art_node_t *add_child4(art_t *t, art_node4_t *n, uint8_t byte, void *child,
                              size_t child_keys);
static art_node_t *add_child16(art_t *t, art_node16_t *n, uint8_t byte, void *child,
                               size_t child_keys);
static art_node_t *add_child48(art_t *t, art_node48_t *n, uint8_t byte, void *child,
                               size_t child_keys);
static art_node_t *add_child256(art_node256_t *n, uint8_t byte, void *child, size_t child_keys);

static art_node_t *add_child(art_t *t, art_node_t *n, uint8_t byte, void *child,
                             size_t child_keys) {
    switch (n->type) {
        case ART_NODE4:
            return add_child4(t, (art_node4_t *)n, byte, child, child_keys);
        case ART_NODE16:
            return add_child16(t, (art_node16_t *)n, byte, child, child_keys);
        case ART_NODE48:
            return add_child48(t, (art_node48_t *)n, byte, child, child_keys);
        case ART_NODE256:
            return add_child256((art_node256_t *)n, byte, child, child_keys);
        default:
            return n;
    }
}

static art_node_t *add_child4(art_t *t, art_node4_t *n, uint8_t byte, void *child,
                              size_t child_keys) {
    if (n->n.num_children < 4) {
        int idx;
        for (idx = 0; idx < n->n.num_children; idx++) {
            if (byte < n->keys[idx]) {
                break;
            }
        }
        for (int i = n->n.num_children; i > idx; i--) {
            n->keys[i] = n->keys[i - 1];
            n->children[i] = n->children[i - 1];
        }
        n->keys[idx] = byte;
        n->children[idx] = child;
        n->n.num_children++;
        n->n.count += child_keys;
        return &n->n;
    }
    /* grow to node16 — preserve subtree size, then add_child increments for new child */
    art_node16_t *n16 = alloc_node16(t);
    if (!n16) {
        return &n->n;
    }
    n16->n.partial_len = n->n.partial_len;
    memcpy(n16->n.partial, n->n.partial, ART_MAX_PREFIX);
    n16->n.num_children = n->n.num_children;
    n16->n.count = n->n.count;
    memcpy(n16->keys, n->keys, 4);
    memcpy(n16->children, n->children, 4 * sizeof(void *));
    free_inner(t, &n->n);
    return add_child16(t, n16, byte, child, child_keys);
}

static art_node_t *add_child16(art_t *t, art_node16_t *n, uint8_t byte, void *child,
                               size_t child_keys) {
    if (n->n.num_children < 16) {
        int idx;
        for (idx = 0; idx < n->n.num_children; idx++) {
            if (byte < n->keys[idx]) {
                break;
            }
        }
        for (int i = n->n.num_children; i > idx; i--) {
            n->keys[i] = n->keys[i - 1];
            n->children[i] = n->children[i - 1];
        }
        n->keys[idx] = byte;
        n->children[idx] = child;
        n->n.num_children++;
        n->n.count += child_keys;
        return &n->n;
    }
    art_node48_t *n48 = alloc_node48(t);
    if (!n48) {
        return &n->n;
    }
    n48->n.partial_len = n->n.partial_len;
    memcpy(n48->n.partial, n->n.partial, ART_MAX_PREFIX);
    memcpy(n48->children, n->children, 16 * sizeof(void *));
    for (int i = 0; i < n->n.num_children; i++) {
        n48->keys[n->keys[i]] = (uint8_t)(i + 1);
    }
    n48->n.num_children = n->n.num_children;
    n48->n.count = n->n.count;
    free_inner(t, &n->n);
    return add_child48(t, n48, byte, child, child_keys);
}

static art_node_t *add_child48(art_t *t, art_node48_t *n, uint8_t byte, void *child,
                               size_t child_keys) {
    if (n->n.num_children < 48) {
        int pos = 0;
        while (pos < 48 && n->children[pos]) {
            pos++;
        }
        n->children[pos] = child;
        n->keys[byte] = (uint8_t)(pos + 1);
        n->n.num_children++;
        n->n.count += child_keys;
        return &n->n;
    }
    art_node256_t *n256 = alloc_node256(t);
    if (!n256) {
        return &n->n;
    }
    n256->n.partial_len = n->n.partial_len;
    memcpy(n256->n.partial, n->n.partial, ART_MAX_PREFIX);
    for (int b = 0; b < 256; b++) {
        if (n->keys[b]) {
            n256->children[b] = n->children[n->keys[b] - 1];
        }
    }
    n256->n.num_children = n->n.num_children;
    n256->n.count = n->n.count;
    free_inner(t, &n->n);
    return add_child256(n256, byte, child, child_keys);
}

static art_node_t *add_child256(art_node256_t *n, uint8_t byte, void *child, size_t child_keys) {
    n->children[byte] = child;
    n->n.num_children++;
    n->n.count += child_keys;
    return &n->n;
}

/* ---------- insert ---------- */

static void *insert_recursive(art_t *t, void *nptr, const uint8_t *key, uint64_t value,
                              int depth, int *out_added, void **ref);

static art_node_t *make_node_from_two_leaves(art_t *t, art_leaf_t *l1, art_leaf_t *l2,
                                            int depth) {
    uint8_t k1[ART_KEY_BYTES], k2[ART_KEY_BYTES];
    key_bytes(l1->value, k1);
    key_bytes(l2->value, k2);

    art_node4_t *n = alloc_node4(t);
    if (!n) {
        return NULL;
    }

    /* longest common prefix from depth */
    int i = depth;
    int pref = 0;
    while (i < ART_KEY_BYTES && k1[i] == k2[i]) {
        if (pref < ART_MAX_PREFIX) {
            n->n.partial[pref] = k1[i];
        }
        pref++;
        i++;
    }
    n->n.partial_len = (uint8_t)((pref > ART_MAX_PREFIX) ? ART_MAX_PREFIX : pref);
    /* if pref > MAX, still skip those bytes in depth via partial_len stored full?
     * Classic ART stores only MAX_PREFIX bytes but partial_len can be longer and
     * remaining recovered from a leaf. We clamp stored bytes and keep true length. */
    n->n.partial_len = (uint8_t)pref; /* may be > MAX_PREFIX in theory; for uint64 max 8 */
    if (n->n.partial_len > ART_MAX_PREFIX) {
        n->n.partial_len = ART_MAX_PREFIX;
        /* For 8-byte keys, pref <= 8-depth <= 8, so OK if ART_MAX_PREFIX==8 */
    }

    depth += pref;
    if (depth >= ART_KEY_BYTES) {
        /* should not happen: two different keys */
        free_inner(t, &n->n);
        return NULL;
    }

    add_child4(t, n, k1[depth], l1, 1);
    add_child4(t, n, k2[depth], l2, 1);
    /* counts: each add_child adds 1 → final count 2 */
    return &n->n;
}

static void *insert_recursive(art_t *t, void *nptr, const uint8_t *key, uint64_t value,
                              int depth, int *out_added, void **ref) {
    *out_added = 0;
    if (!nptr) {
        art_leaf_t *leaf = alloc_leaf(t, value);
        if (!leaf) {
            return NULL;
        }
        *out_added = 1;
        if (ref) {
            *ref = leaf;
        }
        return leaf;
    }

    if (is_leaf(nptr)) {
        art_leaf_t *leaf = (art_leaf_t *)nptr;
        if (leaf_matches(leaf, key)) {
            return leaf; /* duplicate */
        }
        art_leaf_t *new_leaf = alloc_leaf(t, value);
        if (!new_leaf) {
            return leaf;
        }
        art_node_t *split = make_node_from_two_leaves(t, leaf, new_leaf, depth);
        if (!split) {
            free_leaf(t, new_leaf);
            return leaf;
        }
        *out_added = 1;
        if (ref) {
            *ref = split;
        }
        return split;
    }

    art_node_t *node = (art_node_t *)nptr;

    /* prefix check */
    if (node->partial_len) {
        int matched = check_prefix(node, key, depth);
        int plen = node->partial_len;
        if (matched < plen) {
            /* split prefix */
            art_node4_t *n4 = alloc_node4(t);
            if (!n4) {
                return node;
            }
            n4->n.partial_len = (uint8_t)matched;
            memcpy(n4->n.partial, node->partial, matched);

            uint8_t old_byte = node->partial[matched];
            /* shrink old node prefix */
            int old_plen = node->partial_len - matched - 1;
            memmove(node->partial, node->partial + matched + 1,
                    old_plen > 0 ? (size_t)old_plen : 0);
            node->partial_len = (uint8_t)(old_plen > 0 ? old_plen : 0);

            add_child4(t, n4, old_byte, node, node->count);

            art_leaf_t *leaf = alloc_leaf(t, value);
            if (!leaf) {
                return &n4->n;
            }
            add_child4(t, n4, key[depth + matched], leaf, 1);
            *out_added = 1;
            if (ref) {
                *ref = &n4->n;
            }
            return &n4->n;
        }
        depth += node->partial_len;
    }

    uint8_t b = key[depth];
    art_node_t *child = get_child(node, b);
    if (child) {
        int added = 0;
        void *new_child = insert_recursive(t, child, key, value, depth + 1, &added, NULL);
        /* update child pointer if node grew/replaced */
        if (new_child != child) {
            int idx = find_child_idx(node, b);
            switch (node->type) {
                case ART_NODE4:
                    ((art_node4_t *)node)->children[idx] = new_child;
                    break;
                case ART_NODE16:
                    ((art_node16_t *)node)->children[idx] = new_child;
                    break;
                case ART_NODE48:
                    ((art_node48_t *)node)->children[idx] = new_child;
                    break;
                case ART_NODE256:
                    ((art_node256_t *)node)->children[b] = new_child;
                    break;
                default:
                    break;
            }
        }
        if (added) {
            /* One new key under this child — bump path size only. */
            node->count += 1;
            *out_added = 1;
        }
        if (ref) {
            *ref = node;
        }
        return node;
    }

    /* no child: add leaf (add_child does count += 1) */
    art_leaf_t *leaf = alloc_leaf(t, value);
    if (!leaf) {
        return node;
    }
    art_node_t *maybe_new = add_child(t, node, b, leaf, 1);
    *out_added = 1;
    if (ref) {
        *ref = maybe_new;
    }
    return maybe_new;
}

int art_insert(art_t *t, void *node) {
    if (!t || !node) {
        return 0;
    }
    uint64_t value = ((const uint64_t *)node)[0]; /* Node.value */
    uint8_t key[ART_KEY_BYTES];
    key_bytes(value, key);

    if (!t->root) {
        art_leaf_t *leaf = alloc_leaf(t, value);
        if (!leaf) {
            return 0;
        }
        t->root = (art_node_t *)leaf;
        t->head = &leaf->value;
        t->tail = &leaf->value;
        t->count = 1;
        return 1;
    }

    int added = 0;
    void *new_root = insert_recursive(t, t->root, key, value, 0, &added, NULL);
    t->root = (art_node_t *)new_root;
    if (!added) {
        return 0;
    }
    t->count++;
    /* A new min or max is the leaf just inserted; find it with a plain
     * descent (maximum() scans Node48/256 byte maps, and a sorted bulk load
     * makes every insert a new max). */
    if (!t->head || value < *(uint64_t *)t->head) {
        t->head = art_search(t, node);
    }
    if (!t->tail || value > *(uint64_t *)t->tail) {
        t->tail = art_search(t, node);
    }
    return 1;
}

/* ---------- search / at ---------- */

void *art_search(art_t *t, void *search_key) {
    if (!t || !t->root || !search_key) {
        return NULL;
    }
    uint64_t value = ((const uint64_t *)search_key)[0];
    uint8_t key[ART_KEY_BYTES];
    key_bytes(value, key);

    void *n = t->root;
    int depth = 0;
    while (n) {
        if (is_leaf(n)) {
            art_leaf_t *leaf = (art_leaf_t *)n;
            return leaf_matches(leaf, key) ? (void *)&leaf->value : NULL;
        }
        art_node_t *node = (art_node_t *)n;
        if (node->partial_len) {
            int matched = check_prefix(node, key, depth);
            if (matched != node->partial_len) {
                return NULL;
            }
            depth += node->partial_len;
        }
        if (depth >= ART_KEY_BYTES) {
            return NULL;
        }
        n = get_child(node, key[depth]);
        depth++;
    }
    return NULL;
}

void *art_at(art_t *t, long index) {
    if (!t || !t->root || t->count == 0) {
        return NULL;
    }
    size_t i;
    if (index < 0) {
        if ((size_t)(-index) > t->count) {
            return NULL;
        }
        i = t->count - (size_t)(-index);
    } else {
        i = (size_t)index;
    }
    if (i >= t->count) {
        return NULL;
    }

    void *n = t->root;
    while (n) {
        if (is_leaf(n)) {
            return i == 0 ? (void *)&((art_leaf_t *)n)->value : NULL;
        }
        art_node_t *node = (art_node_t *)n;
        void *ch;
        int pos = 0;
        while ((ch = child_from(node, &pos)) != NULL) {
            size_t c = node_count(ch);
            if (i < c) {
                break;
            }
            i -= c;
            pos++;
        }
        if (!ch) {
            return NULL;
        }
        n = ch;
    }
    return NULL;
}

/* ---------- iterator ---------- */

void *art_next(art_t *t, art_iter_t *iter) {
    if (!t || !iter || !t->root) {
        return NULL;
    }

    if (!iter->started) {
        iter->started = 1;
        iter->sp = 0;
        iter->cur = NULL;
        /* push path to minimum */
        void *n = t->root;
        while (n && !is_leaf(n)) {
            art_node_t *node = (art_node_t *)n;
            if (iter->sp >= 16) {
                return NULL;
            }
            int pos = 0;
            iter->stack[iter->sp] = n;
            n = child_from(node, &pos);
            iter->child_i[iter->sp] = pos;
            iter->sp++;
        }
        iter->cur = (art_leaf_t *)n;
        return iter->cur ? (void *)&iter->cur->value : NULL;
    }

    if (!iter->cur) {
        return NULL;
    }

    /* advance: pop until a node has a next child */
    while (iter->sp > 0) {
        int top = iter->sp - 1;
        art_node_t *node = (art_node_t *)iter->stack[top];
        int pos = iter->child_i[top] + 1;
        void *n = child_from(node, &pos);
        if (n) {
            iter->child_i[top] = pos;
            while (n && !is_leaf(n)) {
                if (iter->sp >= 16) {
                    return NULL;
                }
                int p = 0;
                iter->stack[iter->sp] = n;
                void *first = child_from((art_node_t *)n, &p);
                iter->child_i[iter->sp] = p;
                iter->sp++;
                n = first;
            }
            iter->cur = (art_leaf_t *)n;
            return iter->cur ? (void *)&iter->cur->value : NULL;
        }
        iter->sp--;
    }
    iter->cur = NULL;
    return NULL;
}

/* ---------- delete ---------- */

static art_node_t *remove_child(art_t *t, art_node_t *n, uint8_t byte);

static art_node_t *remove_child4(art_t *t, art_node4_t *n, uint8_t byte) {
    int idx = find_child_idx(&n->n, byte);
    if (idx < 0) {
        return &n->n;
    }
    /* Caller only uses this after deleting a leaf under this edge → delta 1. */
    for (int i = idx; i < n->n.num_children - 1; i++) {
        n->keys[i] = n->keys[i + 1];
        n->children[i] = n->children[i + 1];
    }
    n->n.num_children--;
    n->children[n->n.num_children] = NULL;
    if (n->n.count > 0) {
        n->n.count -= 1;
    }

    if (n->n.num_children == 1) {
        /* collapse: if only child is leaf, return leaf; if inner, merge prefix */
        void *child = n->children[0];
        if (is_leaf(child)) {
            free_inner(t, &n->n);
            return child;
        }
        art_node_t *ch = (art_node_t *)child;
        /* prepend this node's prefix + keys[0] to child prefix */
        int total = n->n.partial_len + 1 + ch->partial_len;
        uint8_t prefix[32];
        int p = 0;
        for (int i = 0; i < n->n.partial_len && p < ART_MAX_PREFIX; i++) {
            prefix[p++] = n->n.partial[i];
        }
        if (p < ART_MAX_PREFIX) {
            prefix[p++] = n->keys[0];
        }
        for (int i = 0; i < ch->partial_len && p < ART_MAX_PREFIX; i++) {
            prefix[p++] = ch->partial[i];
        }
        ch->partial_len = (uint8_t)(total > 255 ? 255 : total);
        memcpy(ch->partial, prefix, p < ART_MAX_PREFIX ? (size_t)p : ART_MAX_PREFIX);
        /* If total > MAX_PREFIX, true length is total but we only store MAX.
         * For 8-byte keys this is fine if we never need bytes past stored when
         * check_prefix uses min(partial_len, stored). We set partial_len to min(total, MAX). */
        if (ch->partial_len > ART_MAX_PREFIX) {
            ch->partial_len = ART_MAX_PREFIX;
        }
        free_inner(t, &n->n);
        return ch;
    }
    return &n->n;
}

static art_node_t *remove_child16(art_t *t, art_node16_t *n, uint8_t byte) {
    int idx = find_child_idx(&n->n, byte);
    if (idx < 0) {
        return &n->n;
    }
    for (int i = idx; i < n->n.num_children - 1; i++) {
        n->keys[i] = n->keys[i + 1];
        n->children[i] = n->children[i + 1];
    }
    n->n.num_children--;
    n->children[n->n.num_children] = NULL;
    if (n->n.count > 0) {
        n->n.count -= 1;
    }

    if (n->n.num_children == 3) {
        /* shrink to node4 */
        art_node4_t *n4 = alloc_node4(t);
        if (!n4) {
            return &n->n;
        }
        n4->n.partial_len = n->n.partial_len;
        memcpy(n4->n.partial, n->n.partial, ART_MAX_PREFIX);
        n4->n.num_children = n->n.num_children;
        memcpy(n4->keys, n->keys, 3);
        memcpy(n4->children, n->children, 3 * sizeof(void *));
        n4->n.count = n->n.count;
        free_inner(t, &n->n);
        return &n4->n;
    }
    return &n->n;
}

static art_node_t *remove_child48(art_t *t, art_node48_t *n, uint8_t byte) {
    int idx = find_child_idx(&n->n, byte);
    if (idx < 0) {
        return &n->n;
    }
    /* Leave a hole: everything finds children through keys[], and
     * add_child48 reuses the first free slot. */
    n->children[idx] = NULL;
    n->keys[byte] = 0;
    n->n.num_children--;
    if (n->n.count > 0) {
        n->n.count -= 1;
    }

    if (n->n.num_children == 12) {
        art_node16_t *n16 = alloc_node16(t);
        if (!n16) {
            return &n->n;
        }
        n16->n.partial_len = n->n.partial_len;
        memcpy(n16->n.partial, n->n.partial, ART_MAX_PREFIX);
        int c = 0;
        for (int b = 0; b < 256 && c < 16; b++) {
            if (n->keys[b]) {
                n16->keys[c] = (uint8_t)b;
                n16->children[c] = n->children[n->keys[b] - 1];
                c++;
            }
        }
        n16->n.num_children = (uint16_t)c;
        n16->n.count = n->n.count;
        free_inner(t, &n->n);
        return &n16->n;
    }
    return &n->n;
}

static art_node_t *remove_child256(art_t *t, art_node256_t *n, uint8_t byte) {
    if (!n->children[byte]) {
        return &n->n;
    }
    n->children[byte] = NULL;
    n->n.num_children--;
    if (n->n.count > 0) {
        n->n.count -= 1;
    }

    if (n->n.num_children == 37) {
        art_node48_t *n48 = alloc_node48(t);
        if (!n48) {
            return &n->n;
        }
        n48->n.partial_len = n->n.partial_len;
        memcpy(n48->n.partial, n->n.partial, ART_MAX_PREFIX);
        int pos = 0;
        for (int b = 0; b < 256; b++) {
            if (n->children[b]) {
                n48->children[pos] = n->children[b];
                n48->keys[b] = (uint8_t)(pos + 1);
                pos++;
            }
        }
        n48->n.num_children = (uint16_t)pos;
        n48->n.count = n->n.count;
        free_inner(t, &n->n);
        return &n48->n;
    }
    return &n->n;
}

static art_node_t *remove_child(art_t *t, art_node_t *n, uint8_t byte) {
    switch (n->type) {
        case ART_NODE4:
            return remove_child4(t, (art_node4_t *)n, byte);
        case ART_NODE16:
            return remove_child16(t, (art_node16_t *)n, byte);
        case ART_NODE48:
            return remove_child48(t, (art_node48_t *)n, byte);
        case ART_NODE256:
            return remove_child256(t, (art_node256_t *)n, byte);
        default:
            return n;
    }
}

static void *delete_recursive(art_t *t, void *nptr, const uint8_t *key, int depth,
                              int *removed) {
    *removed = 0;
    if (!nptr) {
        return NULL;
    }

    if (is_leaf(nptr)) {
        art_leaf_t *leaf = (art_leaf_t *)nptr;
        if (leaf_matches(leaf, key)) {
            *removed = 1;
            free_leaf(t, leaf);
            return NULL;
        }
        return nptr;
    }

    art_node_t *node = (art_node_t *)nptr;
    if (node->partial_len) {
        int matched = check_prefix(node, key, depth);
        if (matched != node->partial_len) {
            return node;
        }
        depth += node->partial_len;
    }
    if (depth >= ART_KEY_BYTES) {
        return node;
    }

    uint8_t b = key[depth];
    art_node_t *child = get_child(node, b);
    if (!child) {
        return node;
    }

    int rem = 0;
    void *new_child = delete_recursive(t, child, key, depth + 1, &rem);
    if (!rem) {
        return node;
    }
    *removed = 1;

    if (!new_child) {
        /* Entire child edge gone (deleted leaf). remove_child does count -= 1. */
        art_node_t *repl = remove_child(t, node, b);
        return repl;
    }
    if (new_child != child) {
        int idx = find_child_idx(node, b);
        switch (node->type) {
            case ART_NODE4:
                ((art_node4_t *)node)->children[idx] = new_child;
                break;
            case ART_NODE16:
                ((art_node16_t *)node)->children[idx] = new_child;
                break;
            case ART_NODE48:
                ((art_node48_t *)node)->children[idx] = new_child;
                break;
            case ART_NODE256:
                ((art_node256_t *)node)->children[b] = new_child;
                break;
            default:
                break;
        }
    }
    /* Child still present but one key smaller. */
    if (node->count > 0) {
        node->count -= 1;
    }
    return node;
}

int art_delete(art_t *t, void *search_key) {
    if (!t || !t->root || !search_key || t->count == 0) {
        return -1;
    }
    uint64_t value = ((const uint64_t *)search_key)[0];
    uint8_t key[ART_KEY_BYTES];
    key_bytes(value, key);

    /* Only deleting the min or max moves head/tail; refreshing them walks
     * root-to-leaf twice, so don't do it on every delete.  Compare before
     * the delete frees the leaf. */
    bool end = (t->head && *(const uint64_t *)t->head == value) ||
               (t->tail && *(const uint64_t *)t->tail == value);
    int removed = 0;
    void *new_root = delete_recursive(t, t->root, key, 0, &removed);
    if (!removed) {
        return -1;
    }
    t->root = (art_node_t *)new_root;
    t->count--;
    if (end || t->count == 0) {
        refresh_ends(t);
    }
    return 0;
}

/* ---------- lifecycle ---------- */

art_t *art_alloc(int (*compare)(const void *a, const void *b)) {
    art_t *t = calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->compare = compare;
    t->overhead = sizeof(*t);
    return t;
}

void art_free(art_t *t) {
    if (!t) {
        return;
    }
    free_tree(t, t->root);
    free(t);
}

int art_bulk_init(art_t *t, void *nodes, size_t count, bool sorted) {
    if (!t) {
        return -1;
    }
    free_tree(t, t->root);
    t->root = NULL;
    t->head = NULL;
    t->tail = NULL;
    t->count = 0;

    if (count == 0) {
        return 0;
    }
    if (!nodes) {
        return -1;
    }

    /* Node is uint64_t value */
    uint64_t *arr = (uint64_t *)nodes;
    if (!sorted) {
        /* qsort via Node compare if available */
        if (t->compare) {
            qsort(nodes, count, sizeof(uint64_t), t->compare);
        } else {
            /* simple insertion not needed — require sorted or compare */
            return -1;
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (art_insert(t, &arr[i]) == 0) {
            /* duplicate or OOM — treat as hard failure for bulk */
            return -1;
        }
    }
    return 0;
}

int art_dump_nodes_to_file(art_t *t, const char *file) {
    if (!t || !file) {
        return -1;
    }
    FILE *fp = fopen(file, "wb");
    if (!fp) {
        return -1;
    }
    art_iter_t iter;
    memset(&iter, 0, sizeof(iter));
    void *p;
    while ((p = art_next(t, &iter)) != NULL) {
        if (fwrite(p, sizeof(uint64_t), 1, fp) != 1) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

void art_print_stats(art_t *t) {
    if (!t) {
        return;
    }
    printf("art: count=%zu nodes=%zu overhead=%llu root=%p head=%p tail=%p\n",
           t->count, t->nodes_allocated, (unsigned long long)t->overhead,
           (void *)t->root, (void *)t->head, (void *)t->tail);
}
