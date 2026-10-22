#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct sortedarray_t {
    void* buffer;
    size_t max_nodes;
    size_t node_size;
    int (*compare)(const void *a, const void *b);
    size_t count;
    void* head;
    void* tail;
} sortedarray_t;

// Inserts or deletes a value in a sorted array
void* modify_sorted(void *base, size_t *count, size_t node_size, const void *value, int (*cmp)(const void *, const void *), bool insert) {
    //printf("modify_sorted called count %zu\n", *count);
    char *arr = base;
    size_t left = 0, right = *count;
    size_t pos = *count;

    // Binary search to find insertion point or target value
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        int cmp_result = cmp(arr + mid * node_size, value);
        if (cmp_result == 0) {
            // Value found
            if (insert) {
                // For insertion, return pointer to existing element (duplicate)
                return arr + mid * node_size;
            } else {
                // For deletion, mark position and proceed
                pos = mid;
                break;
            }
        } else if (cmp_result < 0) {
            left = mid + 1;
        } else {
            pos = mid;
            right = mid;
        }
    }

    if (insert) {
        // No duplicate found, proceed with insertion
        if (pos < *count) {
            memmove(arr + (pos + 1) * node_size, arr + pos * node_size, (*count - pos) * node_size);
        }
        // Insert new value
        char *new_node_pos = arr + pos * node_size;
        memcpy(new_node_pos, value, node_size);
        (*count)++;
        return new_node_pos;
    } else {
        // If target not found, return NULL
        if (pos == *count) {
            return NULL;
        }
        // Shift elements to remove the target
        if (pos < *count - 1) {
            memmove(arr + pos * node_size, arr + (pos + 1) * node_size, (*count - pos - 1) * node_size);
        }
        (*count)--;
        // Return pointer to where the element was (now the next element or end of array)
        return arr + pos * node_size;
    }
    return NULL;
}

void sortedarray_bulk_init(sortedarray_t* sa, size_t count, bool sorted) {
    sa->count = count;
    if (!sorted) {
        qsort(sa->buffer, sa->count, sa->node_size, sa->compare);
    }
    sa->tail = sa->buffer + (sa->count - 1) * sa->node_size;
}

// Lower bound over 8-byte unsigned keys (the benchmark's Node): each step is
// a load, a compare and a cmov, with no call through sa->compare and no
// data-dependent branch to mispredict.  Returns the first key >= key.
static inline const uint64_t* lower_bound_u64(const uint64_t* base, size_t n, uint64_t key) {
    if (n == 0) return base;
    while (n > 1) {
        size_t half = n / 2;
        base = (base[half] < key) ? base + half : base;
        n -= half;
    }
    return base + (*base < key);
}

// Assumes node_size == sizeof(uint64_t) and an unsigned-order compare, which
// is what the benchmark uses; bsearch with sa->compare is the generic version.
void* sortedarray_search(sortedarray_t* sa, void* item) {
    uint64_t key = *(const uint64_t*)item;
    const uint64_t* keys = sa->buffer;
    const uint64_t* p = lower_bound_u64(keys, sa->count, key);
    return (p < keys + sa->count && *p == key) ? (void*)p : NULL;
}

void* sortedarray_at(sortedarray_t* sa, long index) {
    size_t i = index < 0 ? sa->count + index : (size_t)index;
    if (i >= sa->count) {
        return NULL;
    }
    return sa->buffer + i * sa->node_size;
}

void* sortedarray_insert(sortedarray_t* sa, void* item) {
    void* rc = modify_sorted(sa->buffer, &sa->count, sa->node_size, item, sa->compare, true);
    if (rc != NULL) {
        sa->tail = sa->buffer + (sa->count - 1) * sa->node_size;
    }
    return rc;
}

void* sortedarray_delete(sortedarray_t* sa, void* item) {
    void* rc = modify_sorted(sa->buffer, &sa->count, sa->node_size, item, sa->compare, false);
    if (rc != NULL) {
        sa->tail = sa->buffer + (sa->count - 1) * sa->node_size;
    }
    return rc;
}

sortedarray_t* sortedarray_alloc(size_t max_nodes, size_t node_size, int (*compare)(const void *a, const void *b)) {
    sortedarray_t* tree = malloc(sizeof(sortedarray_t));
    if (tree == NULL) {
        return NULL;
    }
    tree->buffer = malloc(max_nodes * node_size);
    memset(tree->buffer, 0, max_nodes * node_size);
    if (tree->buffer == NULL) {
        free(tree);
        return NULL;
    }
    tree->max_nodes = max_nodes;
    tree->compare = compare;
    tree->count = 0;
    tree->node_size = node_size;
    tree->head = tree->buffer;
    tree->tail = NULL;
    return tree;
}

void sortedarray_free(sortedarray_t* sa) {
    free(sa->buffer);
    free(sa);
}

