#include "bptree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- helpers ---------- */

static inline char *key_at(void *keys, size_t i, int node_size) {
    return (char *)keys + i * (size_t)node_size;
}

static inline void key_copy(void *dst, const void *src, int node_size) {
    memcpy(dst, src, (size_t)node_size);
}

/* Copy/move `n` consecutive keys (not bytes). */
static inline void keys_copy(void *dst, const void *src, size_t n, int node_size) {
    memcpy(dst, src, n * (size_t)node_size);
}

static inline void key_move(void *dst, const void *src, size_t n, int node_size) {
    memmove(dst, src, n * (size_t)node_size);
}

/* Binary search in a sorted key array: first index with keys[i] >= key.
 * Sets *found if an equal key exists. */
static int lower_bound(void *keys, int n, const void *key, int node_size,
                       int (*cmp)(const void *, const void *), bool *found) {
    int lo = 0, hi = n;
    *found = false;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int c = cmp(key_at(keys, mid, node_size), key);
        if (c < 0) {
            lo = mid + 1;
        } else {
            if (c == 0) {
                *found = true;
            }
            hi = mid;
        }
    }
    if (lo < n && cmp(key_at(keys, lo, node_size), key) == 0) {
        *found = true;
    }
    return lo;
}

/* Child index for search: first i with key < separator[i], else n. */
static int find_child(bptree_node_t *node, const void *key, int node_size,
                      int (*cmp)(const void *, const void *)) {
    /* separators: go right while key >= separator[i] */
    int lo = 0, hi = node->n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (cmp(key, key_at(node->u.internal.keys, mid, node_size)) >= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static size_t subtree_count(bptree_node_t *node) {
    if (!node) {
        return 0;
    }
    if (node->is_leaf) {
        return (size_t)node->n;
    }
    size_t sum = 0;
    for (int i = 0; i <= node->n; i++) {
        sum += node->u.internal.counts[i];
    }
    return sum;
}

static void refresh_head_tail(bptree_t *t) {
    if (!t->leftmost || t->leftmost->n == 0) {
        t->head = NULL;
        t->tail = NULL;
        return;
    }
    t->head = key_at(t->leftmost->u.leaf.keys, 0, t->node_size);
    t->tail = key_at(t->rightmost->u.leaf.keys, t->rightmost->n - 1, t->node_size);
}

static void account_node(bptree_t *t, bptree_node_t *node, int sign) {
    size_t bytes = sizeof(bptree_node_t);
    if (node->is_leaf) {
        bytes += (size_t)t->leaf_capacity * (size_t)t->node_size;
    } else {
        bytes += (size_t)(t->branch_factor - 1) * (size_t)t->node_size;
        bytes += (size_t)t->branch_factor * sizeof(bptree_node_t *);
        bytes += (size_t)t->branch_factor * sizeof(size_t);
    }
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

/* Each node is one cache-line-aligned block: the header, then its arrays.
 * The u.leaf/u.internal pointers point into the block, so a descent touches
 * one contiguous object per level instead of chasing to separate arrays. */
static size_t round64(size_t n) {
    return (n + 63) & ~(size_t)63;
}

static void *alloc_block(size_t bytes) {
    void *p = aligned_alloc(64, round64(bytes));
    if (p) {
        memset(p, 0, round64(bytes));
    }
    return p;
}

static bptree_node_t *alloc_leaf(bptree_t *t) {
    size_t hdr = round64(sizeof(bptree_node_t));
    bptree_node_t *n = alloc_block(hdr + (size_t)t->leaf_capacity * (size_t)t->node_size);
    if (!n) {
        return NULL;
    }
    n->is_leaf = true;
    n->n = 0;
    n->u.leaf.keys = (char *)n + hdr;
    account_node(t, n, +1);
    return n;
}

static bptree_node_t *alloc_internal(bptree_t *t) {
    size_t hdr = sizeof(bptree_node_t);
    size_t keys = (size_t)(t->branch_factor - 1) * (size_t)t->node_size;
    size_t children = (size_t)t->branch_factor * sizeof(bptree_node_t *);
    size_t counts = (size_t)t->branch_factor * sizeof(size_t);
    /* keys first (binary search), then children, then counts (at) */
    size_t keys_off = hdr;
    size_t children_off = (keys_off + keys + 7) & ~(size_t)7;
    size_t counts_off = children_off + children;
    bptree_node_t *n = alloc_block(counts_off + counts);
    if (!n) {
        return NULL;
    }
    n->is_leaf = false;
    n->n = 0;
    n->u.internal.keys = (char *)n + keys_off;
    n->u.internal.children = (bptree_node_t **)((char *)n + children_off);
    n->u.internal.counts = (size_t *)((char *)n + counts_off);
    account_node(t, n, +1);
    return n;
}

static void free_node_recursive(bptree_t *t, bptree_node_t *node) {
    if (!node) {
        return;
    }
    if (!node->is_leaf) {
        for (int i = 0; i <= node->n; i++) {
            free_node_recursive(t, node->u.internal.children[i]);
        }
    }
    account_node(t, node, -1);
    free(node);
}

/* First key of a subtree (left spine). */
static void *min_key_of(bptree_node_t *node, int node_size) {
    while (node && !node->is_leaf) {
        node = node->u.internal.children[0];
    }
    if (!node || node->n == 0) {
        return NULL;
    }
    return key_at(node->u.leaf.keys, 0, node_size);
}

/* ---------- path helpers for count updates ---------- */

static void set_child(bptree_node_t *parent, int i, bptree_node_t *child) {
    parent->u.internal.children[i] = child;
    if (child) {
        child->parent = parent;
    }
    parent->u.internal.counts[i] = subtree_count(child);
}

static void recompute_count_at(bptree_node_t *parent, int i) {
    parent->u.internal.counts[i] = subtree_count(parent->u.internal.children[i]);
}

/* Walk from node up to root, fixing this child's count slot. */
static void fix_counts_to_root(bptree_node_t *node) {
    while (node && node->parent) {
        bptree_node_t *p = node->parent;
        int i;
        for (i = 0; i <= p->n; i++) {
            if (p->u.internal.children[i] == node) {
                recompute_count_at(p, i);
                break;
            }
        }
        node = p;
    }
}

/* ---------- search / at / iterate ---------- */

void *bptree_search(bptree_t *t, void *search_key) {
    if (!t || !t->root || t->count == 0) {
        return NULL;
    }
    bptree_node_t *node = t->root;
    while (!node->is_leaf) {
        int ci = find_child(node, search_key, t->node_size, t->compare);
        node = node->u.internal.children[ci];
    }
    bool found = false;
    int pos = lower_bound(node->u.leaf.keys, node->n, search_key, t->node_size, t->compare, &found);
    if (!found) {
        return NULL;
    }
    return key_at(node->u.leaf.keys, pos, t->node_size);
}

void *bptree_at(bptree_t *t, long index) {
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

    bptree_node_t *node = t->root;
    while (!node->is_leaf) {
        int c;
        for (c = 0; c <= node->n; c++) {
            size_t sc = node->u.internal.counts[c];
            if (i < sc) {
                break;
            }
            i -= sc;
        }
        if (c > node->n) {   /* checked before node moves to the child */
            return NULL;
        }
        node = node->u.internal.children[c];
    }
    if (i >= (size_t)node->n) {
        return NULL;
    }
    return key_at(node->u.leaf.keys, (int)i, t->node_size);
}

void *bptree_next(bptree_t *t, bptree_iter_t *iter) {
    if (!t || !iter) {
        return NULL;
    }
    if (!iter->leaf) {
        if (!t->leftmost || t->leftmost->n == 0) {
            return NULL;
        }
        iter->leaf = t->leftmost;
        iter->idx = 0;
    } else {
        iter->idx++;
        if (iter->idx >= iter->leaf->n) {
            iter->leaf = iter->leaf->u.leaf.next;
            iter->idx = 0;
            if (!iter->leaf || iter->leaf->n == 0) {
                return NULL;
            }
        }
    }
    return key_at(iter->leaf->u.leaf.keys, iter->idx, t->node_size);
}

/* ---------- insert ---------- */

/* Insert key into non-full leaf at position pos. */
static void leaf_insert_at(bptree_t *t, bptree_node_t *leaf, int pos, const void *key) {
    if (pos < leaf->n) {
        key_move(key_at(leaf->u.leaf.keys, pos + 1, t->node_size),
                 key_at(leaf->u.leaf.keys, pos, t->node_size),
                 (size_t)(leaf->n - pos), t->node_size);
    }
    key_copy(key_at(leaf->u.leaf.keys, pos, t->node_size), key, t->node_size);
    leaf->n++;
}

/* Split full leaf; returns new right sibling. Mid key (first of right) written to *up_key. */
static bptree_node_t *split_leaf(bptree_t *t, bptree_node_t *leaf, void *up_key) {
    bptree_node_t *right = alloc_leaf(t);
    if (!right) {
        return NULL;
    }
    int mid = leaf->n / 2; /* left keeps [0,mid), right gets [mid,n) */
    int rcount = leaf->n - mid;
    keys_copy(right->u.leaf.keys, key_at(leaf->u.leaf.keys, (size_t)mid, t->node_size),
              (size_t)rcount, t->node_size);
    right->n = rcount;
    leaf->n = mid;

    right->u.leaf.next = leaf->u.leaf.next;
    right->u.leaf.prev = leaf;
    if (leaf->u.leaf.next) {
        leaf->u.leaf.next->u.leaf.prev = right;
    }
    leaf->u.leaf.next = right;
    if (t->rightmost == leaf) {
        t->rightmost = right;
    }

    key_copy(up_key, key_at(right->u.leaf.keys, 0, t->node_size), t->node_size);
    right->parent = leaf->parent;
    return right;
}

/* Insert separator + right child into non-full internal at child slot after left_idx. */
static void internal_insert_after(bptree_t *t, bptree_node_t *parent, int left_idx,
                                  const void *sep, bptree_node_t *right) {
    /* new child index = left_idx + 1; new separator index = left_idx */
    int sep_idx = left_idx;
    int child_idx = left_idx + 1;
    /* shift separators [sep_idx .. n) right */
    if (sep_idx < parent->n) {
        key_move(key_at(parent->u.internal.keys, sep_idx + 1, t->node_size),
                 key_at(parent->u.internal.keys, sep_idx, t->node_size),
                 (size_t)(parent->n - sep_idx), t->node_size);
    }
    /* shift children/counts [child_idx .. n] right (inclusive of last child) */
    for (int i = parent->n; i >= child_idx; i--) {
        parent->u.internal.children[i + 1] = parent->u.internal.children[i];
        parent->u.internal.counts[i + 1] = parent->u.internal.counts[i];
    }
    key_copy(key_at(parent->u.internal.keys, sep_idx, t->node_size), sep, t->node_size);
    set_child(parent, child_idx, right);
    parent->n++;
}

static int insert_recursive(bptree_t *t, bptree_node_t *node, const void *key,
                            void *up_key, bptree_node_t **up_right);

static int insert_into_leaf(bptree_t *t, bptree_node_t *leaf, const void *key,
                            void *up_key, bptree_node_t **up_right) {
    bool found = false;
    int pos = lower_bound(leaf->u.leaf.keys, leaf->n, key, t->node_size, t->compare, &found);
    if (found) {
        return 0; /* duplicate */
    }
    if (leaf->n < t->leaf_capacity) {
        leaf_insert_at(t, leaf, pos, key);
        *up_right = NULL;
        return 1;
    }
    /* split then insert into correct half */
    bptree_node_t *right = split_leaf(t, leaf, up_key);
    if (!right) {
        return 0;
    }
    /* if key goes to right half */
    if (t->compare(key, up_key) >= 0) {
        bool f2 = false;
        int p2 = lower_bound(right->u.leaf.keys, right->n, key, t->node_size, t->compare, &f2);
        if (f2) {
            return 0;
        }
        leaf_insert_at(t, right, p2, key);
    } else {
        leaf_insert_at(t, leaf, pos, key);
        /* up_key still first of right */
        key_copy(up_key, key_at(right->u.leaf.keys, 0, t->node_size), t->node_size);
    }
    *up_right = right;
    return 1;
}

static int insert_recursive(bptree_t *t, bptree_node_t *node, const void *key,
                            void *up_key, bptree_node_t **up_right) {
    if (node->is_leaf) {
        return insert_into_leaf(t, node, key, up_key, up_right);
    }

    /* Each level keeps its own count for the child it went into: +1 when
     * the child just grew, a recount of the child (and its new right
     * sibling) when it split.  The caller does the same one level up, so no
     * walk back to the root is needed. */
    int ci = find_child(node, key, t->node_size, t->compare);
    _Alignas(16) unsigned char child_up[BPTREE_MAX_NODE_SIZE];
    bptree_node_t *child_right = NULL;
    int ok = insert_recursive(t, node->u.internal.children[ci], key, child_up, &child_right);
    if (!ok) {
        return 0;
    }
    if (!child_right) {
        node->u.internal.counts[ci] += 1;
        *up_right = NULL;
        return 1;
    }
    recompute_count_at(node, ci);

    /* need to insert separator child_up + child_right after ci */
    if (node->n + 1 < t->branch_factor) {
        internal_insert_after(t, node, ci, child_up, child_right);
        *up_right = NULL;
        return 1;
    }

    /* node full: temporarily insert then split, or split first.
     * Use temporary oversize by inserting into a shadow via split-then-place.
     * Simpler: allocate temp arrays sized branch_factor. */
    int max_children = t->branch_factor + 1;
    bptree_node_t **tmp_ch = calloc((size_t)max_children, sizeof(bptree_node_t *));
    size_t *tmp_ct = calloc((size_t)max_children, sizeof(size_t));
    void *tmp_keys = calloc((size_t)t->branch_factor, (size_t)t->node_size);
    if (!tmp_ch || !tmp_ct || !tmp_keys) {
        free(tmp_ch);
        free(tmp_ct);
        free(tmp_keys);
        return 0;
    }
    for (int i = 0; i <= node->n; i++) {
        tmp_ch[i] = node->u.internal.children[i];
        tmp_ct[i] = node->u.internal.counts[i];
    }
    if (node->n > 0) {
        keys_copy(tmp_keys, node->u.internal.keys, (size_t)node->n, t->node_size);
    }
    int tn = node->n;
    /* insert into temp at after ci */
    for (int i = tn; i > ci; i--) {
        key_copy(key_at(tmp_keys, (size_t)i, t->node_size),
                 key_at(tmp_keys, (size_t)i - 1, t->node_size), t->node_size);
    }
    for (int i = tn + 1; i > ci + 1; i--) {
        tmp_ch[i] = tmp_ch[i - 1];
        tmp_ct[i] = tmp_ct[i - 1];
    }
    key_copy(key_at(tmp_keys, (size_t)ci, t->node_size), child_up, t->node_size);
    tmp_ch[ci + 1] = child_right;
    tmp_ct[ci + 1] = subtree_count(child_right);
    tn++;

    /* split temp: promote mid separator */
    int mid = tn / 2;
    key_copy(up_key, key_at(tmp_keys, (size_t)mid, t->node_size), t->node_size);

    bptree_node_t *right = alloc_internal(t);
    if (!right) {
        free(tmp_ch);
        free(tmp_ct);
        free(tmp_keys);
        return 0;
    }

    /* rebuild left (node) */
    node->n = mid;
    for (int i = 0; i <= mid; i++) {
        set_child(node, i, tmp_ch[i]);
    }
    if (mid > 0) {
        keys_copy(node->u.internal.keys, tmp_keys, (size_t)mid, t->node_size);
    }
    for (int i = mid + 1; i < t->branch_factor; i++) {
        node->u.internal.children[i] = NULL;
        node->u.internal.counts[i] = 0;
    }

    /* rebuild right */
    int rseps = tn - mid - 1;
    right->n = rseps;
    for (int i = 0; i <= rseps; i++) {
        set_child(right, i, tmp_ch[mid + 1 + i]);
    }
    if (rseps > 0) {
        keys_copy(right->u.internal.keys,
                  key_at(tmp_keys, (size_t)mid + 1, t->node_size),
                  (size_t)rseps, t->node_size);
    }

    free(tmp_ch);
    free(tmp_ct);
    free(tmp_keys);
    *up_right = right;
    return 1;
}

int bptree_insert(bptree_t *t, void *node) {
    if (!t || !node) {
        return 0;
    }
    if (!t->root) {
        bptree_node_t *leaf = alloc_leaf(t);
        if (!leaf) {
            return 0;
        }
        key_copy(key_at(leaf->u.leaf.keys, 0, t->node_size), node, t->node_size);
        leaf->n = 1;
        t->root = leaf;
        t->leftmost = leaf;
        t->rightmost = leaf;
        t->count = 1;
        refresh_head_tail(t);
        return 1;
    }

    _Alignas(16) unsigned char up_key[BPTREE_MAX_NODE_SIZE];
    bptree_node_t *up_right = NULL;
    int ok = insert_recursive(t, t->root, node, up_key, &up_right);
    if (!ok) {
        return 0;
    }
    if (up_right) {
        bptree_node_t *new_root = alloc_internal(t);
        if (!new_root) {
            return 0;
        }
        set_child(new_root, 0, t->root);
        set_child(new_root, 1, up_right);
        key_copy(key_at(new_root->u.internal.keys, 0, t->node_size), up_key, t->node_size);
        new_root->n = 1;
        t->root = new_root;
    }
    t->count++;
    refresh_head_tail(t);
    return 1;
}

/* ---------- delete ---------- */

static int leaf_min(bptree_t *t) {
    return (t->leaf_capacity + 1) / 2;
}

static int internal_min_children(bptree_t *t) {
    return (t->branch_factor + 1) / 2;
}

static void leaf_remove_at(bptree_t *t, bptree_node_t *leaf, int pos) {
    if (pos < leaf->n - 1) {
        key_move(key_at(leaf->u.leaf.keys, pos, t->node_size),
                 key_at(leaf->u.leaf.keys, pos + 1, t->node_size),
                 (size_t)(leaf->n - pos - 1), t->node_size);
    }
    leaf->n--;
}

static int child_index(bptree_node_t *parent, bptree_node_t *child) {
    for (int i = 0; i <= parent->n; i++) {
        if (parent->u.internal.children[i] == child) {
            return i;
        }
    }
    return -1;
}

/* Update separator that points at the first key of `child` (separator index child_idx-1). */
static void update_separator_for_child(bptree_t *t, bptree_node_t *parent, int child_idx) {
    if (child_idx <= 0 || !parent) {
        return;
    }
    void *mk = min_key_of(parent->u.internal.children[child_idx], t->node_size);
    if (mk) {
        key_copy(key_at(parent->u.internal.keys, child_idx - 1, t->node_size), mk, t->node_size);
    }
}

static void remove_child_slot(bptree_t *t, bptree_node_t *parent, int child_idx) {
    /* remove separator before this child if child_idx > 0, else separator 0 */
    int sep_idx = (child_idx > 0) ? child_idx - 1 : 0;
    if (parent->n > 0) {
        if (sep_idx < parent->n - 1) {
            key_move(key_at(parent->u.internal.keys, sep_idx, t->node_size),
                     key_at(parent->u.internal.keys, sep_idx + 1, t->node_size),
                     (size_t)(parent->n - sep_idx - 1), t->node_size);
        }
    }
    for (int i = child_idx; i < parent->n; i++) {
        parent->u.internal.children[i] = parent->u.internal.children[i + 1];
        parent->u.internal.counts[i] = parent->u.internal.counts[i + 1];
        if (parent->u.internal.children[i]) {
            parent->u.internal.children[i]->parent = parent;
        }
    }
    parent->u.internal.children[parent->n] = NULL;
    parent->u.internal.counts[parent->n] = 0;
    parent->n--;
    (void)t;
}

static void free_leaf_node(bptree_t *t, bptree_node_t *leaf) {
    if (leaf->u.leaf.prev) {
        leaf->u.leaf.prev->u.leaf.next = leaf->u.leaf.next;
    }
    if (leaf->u.leaf.next) {
        leaf->u.leaf.next->u.leaf.prev = leaf->u.leaf.prev;
    }
    if (t->leftmost == leaf) {
        t->leftmost = leaf->u.leaf.next;
    }
    if (t->rightmost == leaf) {
        t->rightmost = leaf->u.leaf.prev;
    }
    account_node(t, leaf, -1);
    free(leaf);
}

static void free_internal_node_only(bptree_t *t, bptree_node_t *node) {
    account_node(t, node, -1);
    free(node);
}

/* Borrow / merge after delete underfills a node. child is the underfull node. */
static void rebalance_after_delete(bptree_t *t, bptree_node_t *child);

static void rebalance_leaf(bptree_t *t, bptree_node_t *leaf) {
    bptree_node_t *parent = leaf->parent;
    if (!parent) {
        /* root leaf may be underfull */
        return;
    }
    int idx = child_index(parent, leaf);
    int min_keys = leaf_min(t);

    /* try left sibling */
    if (idx > 0) {
        bptree_node_t *left = parent->u.internal.children[idx - 1];
        if (left->is_leaf && left->n > min_keys) {
            /* borrow last of left */
            leaf_insert_at(t, leaf, 0, key_at(left->u.leaf.keys, left->n - 1, t->node_size));
            left->n--;
            update_separator_for_child(t, parent, idx);
            recompute_count_at(parent, idx - 1);
            recompute_count_at(parent, idx);
            fix_counts_to_root(parent);
            return;
        }
    }
    /* try right sibling */
    if (idx < parent->n) {
        bptree_node_t *right = parent->u.internal.children[idx + 1];
        if (right->is_leaf && right->n > min_keys) {
            leaf_insert_at(t, leaf, leaf->n, key_at(right->u.leaf.keys, 0, t->node_size));
            leaf_remove_at(t, right, 0);
            update_separator_for_child(t, parent, idx + 1);
            recompute_count_at(parent, idx);
            recompute_count_at(parent, idx + 1);
            fix_counts_to_root(parent);
            return;
        }
    }

    /* merge with sibling */
    if (idx > 0) {
        bptree_node_t *left = parent->u.internal.children[idx - 1];
        /* append leaf into left */
        keys_copy(key_at(left->u.leaf.keys, (size_t)left->n, t->node_size),
                  leaf->u.leaf.keys, (size_t)leaf->n, t->node_size);
        left->n += leaf->n;
        free_leaf_node(t, leaf);
        remove_child_slot(t, parent, idx);
        recompute_count_at(parent, idx - 1);
        fix_counts_to_root(parent);
        if (parent->n + 1 < internal_min_children(t) && parent->parent) {
            rebalance_after_delete(t, parent);
        } else if (parent == t->root && parent->n == 0) {
            /* root with single child */
            t->root = parent->u.internal.children[0];
            if (t->root) {
                t->root->parent = NULL;
            }
            free_internal_node_only(t, parent);
        }
        return;
    }
    if (idx < parent->n) {
        bptree_node_t *right = parent->u.internal.children[idx + 1];
        keys_copy(key_at(leaf->u.leaf.keys, (size_t)leaf->n, t->node_size),
                  right->u.leaf.keys, (size_t)right->n, t->node_size);
        leaf->n += right->n;
        free_leaf_node(t, right);
        remove_child_slot(t, parent, idx + 1);
        recompute_count_at(parent, idx);
        fix_counts_to_root(parent);
        if (parent->n + 1 < internal_min_children(t) && parent->parent) {
            rebalance_after_delete(t, parent);
        } else if (parent == t->root && parent->n == 0) {
            t->root = parent->u.internal.children[0];
            if (t->root) {
                t->root->parent = NULL;
            }
            free_internal_node_only(t, parent);
        }
    }
}

static void rebalance_internal(bptree_t *t, bptree_node_t *node) {
    bptree_node_t *parent = node->parent;
    if (!parent) {
        return;
    }
    int idx = child_index(parent, node);
    int min_ch = internal_min_children(t);

    if (idx > 0) {
        bptree_node_t *left = parent->u.internal.children[idx - 1];
        if (!left->is_leaf && left->n + 1 > min_ch) {
            /* rotate from left: move parent separator down, left's last sep up */
            /* make room at front of node */
            for (int i = node->n; i >= 0; i--) {
                node->u.internal.children[i + 1] = node->u.internal.children[i];
                node->u.internal.counts[i + 1] = node->u.internal.counts[i];
            }
            if (node->n > 0) {
                key_move(key_at(node->u.internal.keys, 1, t->node_size),
                         node->u.internal.keys, (size_t)node->n, t->node_size);
            }
            key_copy(key_at(node->u.internal.keys, 0, t->node_size),
                     key_at(parent->u.internal.keys, idx - 1, t->node_size), t->node_size);
            set_child(node, 0, left->u.internal.children[left->n]);
            key_copy(key_at(parent->u.internal.keys, idx - 1, t->node_size),
                     key_at(left->u.internal.keys, left->n - 1, t->node_size), t->node_size);
            left->u.internal.children[left->n] = NULL;
            left->u.internal.counts[left->n] = 0;
            left->n--;
            node->n++;
            recompute_count_at(parent, idx - 1);
            recompute_count_at(parent, idx);
            fix_counts_to_root(parent);
            return;
        }
    }
    if (idx < parent->n) {
        bptree_node_t *right = parent->u.internal.children[idx + 1];
        if (!right->is_leaf && right->n + 1 > min_ch) {
            key_copy(key_at(node->u.internal.keys, node->n, t->node_size),
                     key_at(parent->u.internal.keys, idx, t->node_size), t->node_size);
            set_child(node, node->n + 1, right->u.internal.children[0]);
            key_copy(key_at(parent->u.internal.keys, idx, t->node_size),
                     key_at(right->u.internal.keys, 0, t->node_size), t->node_size);
            for (int i = 0; i < right->n; i++) {
                right->u.internal.children[i] = right->u.internal.children[i + 1];
                right->u.internal.counts[i] = right->u.internal.counts[i + 1];
                if (right->u.internal.children[i]) {
                    right->u.internal.children[i]->parent = right;
                }
            }
            if (right->n > 1) {
                key_move(right->u.internal.keys,
                         key_at(right->u.internal.keys, 1, t->node_size),
                         (size_t)(right->n - 1), t->node_size);
            }
            right->u.internal.children[right->n] = NULL;
            right->u.internal.counts[right->n] = 0;
            right->n--;
            node->n++;
            recompute_count_at(parent, idx);
            recompute_count_at(parent, idx + 1);
            fix_counts_to_root(parent);
            return;
        }
    }

    /* merge */
    if (idx > 0) {
        bptree_node_t *left = parent->u.internal.children[idx - 1];
        /* append parent separator then node's keys/children into left */
        key_copy(key_at(left->u.internal.keys, left->n, t->node_size),
                 key_at(parent->u.internal.keys, idx - 1, t->node_size), t->node_size);
        for (int i = 0; i <= node->n; i++) {
            set_child(left, left->n + 1 + i, node->u.internal.children[i]);
        }
        if (node->n > 0) {
            keys_copy(key_at(left->u.internal.keys, (size_t)left->n + 1, t->node_size),
                      node->u.internal.keys, (size_t)node->n, t->node_size);
        }
        left->n = left->n + 1 + node->n;
        free_internal_node_only(t, node);
        remove_child_slot(t, parent, idx);
        recompute_count_at(parent, idx - 1);
        fix_counts_to_root(parent);
        if (parent == t->root && parent->n == 0) {
            t->root = parent->u.internal.children[0];
            if (t->root) {
                t->root->parent = NULL;
            }
            free_internal_node_only(t, parent);
        } else if (parent->n + 1 < min_ch && parent->parent) {
            rebalance_after_delete(t, parent);
        }
        return;
    }
    if (idx < parent->n) {
        bptree_node_t *right = parent->u.internal.children[idx + 1];
        key_copy(key_at(node->u.internal.keys, node->n, t->node_size),
                 key_at(parent->u.internal.keys, idx, t->node_size), t->node_size);
        for (int i = 0; i <= right->n; i++) {
            set_child(node, node->n + 1 + i, right->u.internal.children[i]);
        }
        if (right->n > 0) {
            keys_copy(key_at(node->u.internal.keys, (size_t)node->n + 1, t->node_size),
                      right->u.internal.keys, (size_t)right->n, t->node_size);
        }
        node->n = node->n + 1 + right->n;
        free_internal_node_only(t, right);
        remove_child_slot(t, parent, idx + 1);
        recompute_count_at(parent, idx);
        fix_counts_to_root(parent);
        if (parent == t->root && parent->n == 0) {
            t->root = parent->u.internal.children[0];
            if (t->root) {
                t->root->parent = NULL;
            }
            free_internal_node_only(t, parent);
        } else if (parent->n + 1 < min_ch && parent->parent) {
            rebalance_after_delete(t, parent);
        }
    }
}

static void rebalance_after_delete(bptree_t *t, bptree_node_t *child) {
    if (!child) {
        return;
    }
    if (child->is_leaf) {
        if (child->n < leaf_min(t) && child->parent) {
            rebalance_leaf(t, child);
        }
    } else {
        if (child->n + 1 < internal_min_children(t) && child->parent) {
            rebalance_internal(t, child);
        }
    }
}

int bptree_delete(bptree_t *t, void *search_key) {
    if (!t || !t->root || t->count == 0 || !search_key) {
        return -1;
    }

    /* Remember the path so the counts along it can be decremented directly
     * (instead of a walk back up that searches each parent for its child). */
    bptree_node_t *path[64];
    int path_ci[64];
    int depth = 0;
    bptree_node_t *node = t->root;
    while (!node->is_leaf) {
        int ci = find_child(node, search_key, t->node_size, t->compare);
        if (depth < 64) {
            path[depth] = node;
            path_ci[depth] = ci;
        }
        depth++;
        node = node->u.internal.children[ci];
    }
    bool found = false;
    int pos = lower_bound(node->u.leaf.keys, node->n, search_key, t->node_size, t->compare, &found);
    if (!found) {
        return -1;
    }

    bool was_first = (pos == 0);
    leaf_remove_at(t, node, pos);
    t->count--;
    if (depth <= 64) {
        for (int d = 0; d < depth; d++) {
            path[d]->u.internal.counts[path_ci[d]] -= 1;
        }
    } else {
        fix_counts_to_root(node);
    }

    if (t->count == 0) {
        free_leaf_node(t, node);
        t->root = NULL;
        t->leftmost = NULL;
        t->rightmost = NULL;
        t->head = NULL;
        t->tail = NULL;
        return 0;
    }

    /* if we removed the first key of this leaf, separators above may need update */
    if (was_first && node->parent && node->n > 0) {
        int idx = child_index(node->parent, node);
        update_separator_for_child(t, node->parent, idx);
        /* walk further up if this leaf is still the leftmost of ancestors */
        bptree_node_t *p = node->parent;
        bptree_node_t *cur = node;
        while (p) {
            int ci = child_index(p, cur);
            if (ci > 0) {
                update_separator_for_child(t, p, ci);
                break;
            }
            cur = p;
            p = p->parent;
        }
    }

    if (node->n < leaf_min(t) && node->parent) {
        rebalance_leaf(t, node);
    } else if (!node->parent && node->n == 0) {
        /* empty root leaf already handled via count==0 */
    }

    /* collapse root with single child */
    while (t->root && !t->root->is_leaf && t->root->n == 0) {
        bptree_node_t *old = t->root;
        t->root = old->u.internal.children[0];
        if (t->root) {
            t->root->parent = NULL;
        }
        free_internal_node_only(t, old);
    }

    refresh_head_tail(t);
    return 0;
}

/* ---------- lifecycle / bulk ---------- */

bptree_t *bptree_alloc(int leaf_capacity, int branch_factor, int node_size,
                       int (*compare)(const void *a, const void *b)) {
    if (leaf_capacity < 2 || branch_factor < 3 || node_size <= 0 ||
        node_size > BPTREE_MAX_NODE_SIZE || !compare) {
        return NULL;
    }
    bptree_t *t = calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->leaf_capacity = leaf_capacity;
    t->branch_factor = branch_factor;
    t->node_size = node_size;
    t->compare = compare;
    t->bulk_fill = 1.0;
    t->overhead = sizeof(*t);
    return t;
}

void bptree_free(bptree_t *t) {
    if (!t) {
        return;
    }
    free_node_recursive(t, t->root);
    free(t);
}

/* How many nodes `total` items go into, each taking about fill * capacity
 * but never fewer than min_per_node (the B+ tree minimum, which delete
 * relies on) unless everything fits in one node.  bulk_take spreads the
 * items evenly: node i gets total / nodes, plus one for the first
 * total % nodes of them, so with nodes <= total / min_per_node each gets
 * between min_per_node and 2 * min_per_node - 1 <= capacity. */
static size_t bulk_nodes(size_t total, int capacity, double fill, size_t min_per_node) {
    size_t per = (size_t)((double)capacity * fill);
    if (per < min_per_node) {
        per = min_per_node;
    }
    if (per > (size_t)capacity) {
        per = (size_t)capacity;
    }
    size_t nodes = (total + per - 1) / per;
    if (nodes > 1 && total / nodes < min_per_node) {
        nodes = total / min_per_node;
    }
    return nodes ? nodes : 1;
}

static size_t bulk_take(size_t total, size_t nodes, size_t i) {
    return total / nodes + (i < total % nodes ? 1 : 0);
}

/* Build one internal level from an array of child nodes; returns new nodes, count in *out_n. */
static bptree_node_t **build_level(bptree_t *t, bptree_node_t **children, size_t nchildren,
                                   size_t *out_n) {
    size_t nparents = bulk_nodes(nchildren, t->branch_factor, t->bulk_fill,
                                  (size_t)internal_min_children(t));
    bptree_node_t **parents = calloc(nparents, sizeof(bptree_node_t *));
    if (!parents) {
        return NULL;
    }
    size_t ci = 0;
    for (size_t p = 0; p < nparents; p++) {
        bptree_node_t *node = alloc_internal(t);
        if (!node) {
            free(parents);
            return NULL;
        }
        size_t take = bulk_take(nchildren, nparents, p);
        for (size_t i = 0; i < take; i++) {
            set_child(node, (int)i, children[ci + i]);
            if (i > 0) {
                void *mk = min_key_of(children[ci + i], t->node_size);
                if (mk) {
                    key_copy(key_at(node->u.internal.keys, (int)i - 1, t->node_size),
                             mk, t->node_size);
                }
            }
        }
        node->n = (int)take - 1;
        parents[p] = node;
        ci += take;
    }
    *out_n = nparents;
    return parents;
}

int bptree_bulk_init(bptree_t *t, void *nodes, size_t count, bool sorted) {
    if (!t || (!nodes && count > 0)) {
        return -1;
    }
    /* clear existing */
    free_node_recursive(t, t->root);
    t->root = NULL;
    t->leftmost = NULL;
    t->rightmost = NULL;
    t->head = NULL;
    t->tail = NULL;
    t->count = 0;

    if (count == 0) {
        return 0;
    }

    if (!sorted) {
        qsort(nodes, count, (size_t)t->node_size, t->compare);
    }

    /* pack leaves to bulk_fill, evenly */
    size_t nleaves = bulk_nodes(count, t->leaf_capacity, t->bulk_fill, (size_t)leaf_min(t));
    bptree_node_t **leaves = calloc(nleaves, sizeof(bptree_node_t *));
    if (!leaves) {
        return -1;
    }

    size_t offset = 0;
    for (size_t i = 0; i < nleaves; i++) {
        bptree_node_t *leaf = alloc_leaf(t);
        if (!leaf) {
            free(leaves);
            return -1;
        }
        size_t take = bulk_take(count, nleaves, i);
        keys_copy(leaf->u.leaf.keys,
                  key_at(nodes, offset, t->node_size),
                  take, t->node_size);
        leaf->n = (int)take;
        leaves[i] = leaf;
        if (i > 0) {
            leaves[i - 1]->u.leaf.next = leaf;
            leaf->u.leaf.prev = leaves[i - 1];
        }
        offset += take;
    }

    t->leftmost = leaves[0];
    t->rightmost = leaves[nleaves - 1];
    t->count = count;

    if (nleaves == 1) {
        t->root = leaves[0];
        free(leaves);
        refresh_head_tail(t);
        return 0;
    }

    bptree_node_t **level = leaves;
    size_t level_n = nleaves;
    while (level_n > 1) {
        size_t next_n = 0;
        bptree_node_t **next = build_level(t, level, level_n, &next_n);
        if (level != leaves) {
            free(level);
        }
        if (!next) {
            free(leaves);
            return -1;
        }
        level = next;
        level_n = next_n;
    }
    t->root = level[0];
    if (level != leaves) {
        free(level);
    }
    free(leaves);
    refresh_head_tail(t);
    return 0;
}

int bptree_dump_nodes_to_file(bptree_t *t, const char *file) {
    if (!t || !file) {
        return -1;
    }
    FILE *fp = fopen(file, "wb");
    if (!fp) {
        return -1;
    }
    const size_t chunk = 4096;
    void *buf = malloc(chunk * (size_t)t->node_size);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    size_t fill = 0;
    bptree_iter_t iter;
    memset(&iter, 0, sizeof(iter));
    void *node;
    while ((node = bptree_next(t, &iter)) != NULL) {
        key_copy(key_at(buf, (int)fill, t->node_size), node, t->node_size);
        fill++;
        if (fill == chunk) {
            if (fwrite(buf, (size_t)t->node_size, fill, fp) != fill) {
                free(buf);
                fclose(fp);
                return -1;
            }
            fill = 0;
        }
    }
    if (fill) {
        if (fwrite(buf, (size_t)t->node_size, fill, fp) != fill) {
            free(buf);
            fclose(fp);
            return -1;
        }
    }
    free(buf);
    fclose(fp);
    return 0;
}

void bptree_print_stats(bptree_t *t) {
    if (!t) {
        return;
    }
    printf("bptree: count=%zu leaf_cap=%d branch=%d nodes=%zu overhead=%llu root=%p\n",
           t->count, t->leaf_capacity, t->branch_factor, t->nodes_allocated,
           (unsigned long long)t->overhead, (void *)t->root);
}
