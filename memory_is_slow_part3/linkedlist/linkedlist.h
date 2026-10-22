#include <stdlib.h>

typedef struct linklist_node_t {
    void* item;
    struct linklist_node_t* next;
    struct linklist_node_t* prev;
} linklist_node_t;

typedef struct linklist_tree_t {
    linklist_node_t* head;
    linklist_node_t* tail;
    int (*compare)(const void *a, const void *b);
    long count;
} linklist_tree_t;


linklist_node_t* linklist_search(linklist_tree_t* list, void* item) {
    linklist_node_t* current = list->head;
    int cmp;
    while (current != NULL) {
        cmp = list->compare(current->item, item);
        if (cmp == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}


linklist_node_t* linklist_at(linklist_tree_t* list, int index) {
    linklist_node_t* current = list->head;
    for (int i = 0; i < index; i++) {
        current = current->next;
    }
    return current;
}


linklist_node_t* linklist_create(void* item) {
    linklist_node_t* new_node = malloc(sizeof(linklist_node_t));
    if (new_node == NULL) {
        return NULL;
    }
    new_node->item = item;
    new_node->next = NULL;
    new_node->prev = NULL;
    return new_node;
}

linklist_node_t* linklist_insert(linklist_tree_t* list, void* item) {
    linklist_node_t* current = list->head;
    linklist_node_t* prev = NULL;
    int cmp;
    while (current != NULL) {
        cmp = list->compare(current->item, item);
        if (cmp == 0) {
            return NULL;
        } else if (cmp > 0) {
            break;
        }
        prev = current;
        current = current->next;
    }

    linklist_node_t* new_node = linklist_create(item);
    if (new_node == NULL) {
        return NULL;
    }

    new_node->prev = prev;
    new_node->next = current;

    if (prev != NULL) {
        prev->next = new_node;
    } else {
        list->head = new_node;
    }
    if (current != NULL) {
        current->prev = new_node;
    } else {
        list->tail = new_node;
    }

    return new_node;
}


void* linklist_delete_node(linklist_tree_t* list, linklist_node_t* node) {
    if (node == NULL) {
        return NULL;
    }
    if (node->prev == NULL) {
        list->head = node->next;
    } else {
        node->prev->next = node->next;
    }
    if (node->next == NULL) {
        list->tail = node->prev;
    } else {
        node->next->prev = node->prev;
    }
    list->count--;
    void* item = node->item;
    free(node);
    return item;
}

void* linklist_delete(linklist_tree_t* list, void* item) {
    linklist_node_t* node = linklist_search(list, item);
    if (node != NULL) {
        return linklist_delete_node(list, node);
    }
    return NULL;
}

linklist_tree_t* linklist_alloc_tree(int (*compare)(const void *a, const void *b)) {
    linklist_tree_t* tree = malloc(sizeof(linklist_tree_t));
    if (tree == NULL) {
        return NULL;
    }
    tree->head = NULL;
    tree->tail = NULL;
    tree->compare = compare;
    tree->count = 0;
    return tree;
}


void linklist_free_tree(linklist_tree_t* list) {
    linklist_node_t* current = list->head;
    while (current != NULL) {
        linklist_node_t* next = current->next;
        free(current);
        current = next;
    }
    list->head = NULL;
    list->tail = NULL;
    list->count = 0;
    free(list);
}

