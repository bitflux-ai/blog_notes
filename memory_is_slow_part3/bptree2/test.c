#include "bptree.h"
#include "benchmark_helpers.h"
#include "gen_nodes.h"
#include <getopt.h>
#include <math.h>

#define DEFAULT_LEAF_SIZE 512
#define DEFAULT_BRANCH_FACTOR 16

char *g_inputfile = NULL;
char *g_listfile = NULL;
uint64_t g_leaf_size = DEFAULT_LEAF_SIZE;
uint64_t g_branch_factor = DEFAULT_BRANCH_FACTOR;
uint64_t g_count = 0;
int debug = 0;
int verbose = 0;
int g_nofail = 0;
int g_total_errors = 0;

void handle_error(const char *message) {
    printf("ERROR: %s\n", message);
    g_total_errors++;
    if (!g_nofail) {
        exit(1);
    }
}

void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Test B+ tree data structure functionality\n\n");
    printf("Options:\n");
    printf("  -i, --input FILE         Path file with pregenerated nodes [REQUIRED]\n");
    printf("  -l, --list  FILE         Path file with sorted nodes for debug\n");
    printf("  -e, --leaf-size SIZE     Max keys per leaf (default: %d)\n", DEFAULT_LEAF_SIZE);
    printf("  -b, --branch-factor N    Max children per internal node (default: %d)\n",
           DEFAULT_BRANCH_FACTOR);
    printf("  -c, --count              How many nodes to use\n");
    printf("  -d, --debug LEVEL        Enable debug mode\n");
    printf("  -v, --verbose LEVEL      Enable verbose mode\n");
    printf("      --nofail             Continue testing after failures\n");
    printf("  -h, --help               Show this help message\n");
    printf("\nOne of --list or --count must be specified\n");
}

int parse_arguments(int argc, char *argv[]) {
    int c;
    static struct option long_options[] = {
        {"input", required_argument, 0, 'i'},
        {"list", required_argument, 0, 'l'},
        {"leaf-size", required_argument, 0, 'e'},
        {"extent-size", required_argument, 0, 'e'},
        {"branch-factor", required_argument, 0, 'b'},
        {"count", required_argument, 0, 'c'},
        {"debug", required_argument, 0, 'd'},
        {"verbose", required_argument, 0, 'v'},
        {"nofail", no_argument, 0, 1000},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "i:l:e:b:c:d:v:h", long_options, NULL)) != -1) {
        switch (c) {
            case 'i':
                g_inputfile = optarg;
                break;
            case 'l':
                g_listfile = optarg;
                break;
            case 'e':
                if (parse_size_bin(optarg, &g_leaf_size) != 0) {
                    fprintf(stderr, "Error: invalid leaf size value\n");
                    return -1;
                }
                break;
            case 'b':
                if (parse_size_bin(optarg, &g_branch_factor) != 0) {
                    fprintf(stderr, "Error: invalid branch factor value\n");
                    return -1;
                }
                break;
            case 'c':
                if (parse_size_bin(optarg, &g_count) != 0) {
                    fprintf(stderr, "Error: invalid count value\n");
                    return -1;
                }
                break;
            case 'd':
                debug = atoi(optarg);
                if (debug < 0) {
                    return -1;
                }
                break;
            case 'v':
                verbose = atoi(optarg);
                if (verbose < 0) {
                    return -1;
                }
                break;
            case 1000:
                g_nofail = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            case '?':
            default:
                return -1;
        }
    }

    if (g_count > 0 && g_listfile != NULL) {
        fprintf(stderr, "Error: cannot specify both --count and --list\n");
        return -1;
    }
    if (g_count == 0 && g_listfile == NULL) {
        fprintf(stderr, "Error: must specify either --count or --list\n");
        return -1;
    }
    if (g_inputfile == NULL) {
        fprintf(stderr, "Error: must specify --inputfile\n");
        return -1;
    }
    return 0;
}

int test_dump_using_at(void *context, const char *filename) {
    bptree_t *t = (bptree_t *)context;
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        return -1;
    }
    for (long i = 0; i < (long)t->count; i++) {
        Node *node = (Node *)bptree_at(t, i);
        if (!node) {
            fclose(fp);
            return -1;
        }
        if (fwrite(node, sizeof(Node), 1, fp) != 1) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

int test_dump_using_next(void *context, const char *filename) {
    return bptree_dump_nodes_to_file((bptree_t *)context, filename);
}

int main(int argc, char *argv[]) {
    if (parse_arguments(argc, argv) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    Node *nodes = NULL;
    Node *insert_nodes = NULL;
    Node *delete_nodes = NULL;
    size_t *indexes = NULL;
    size_t valid_count = g_count;
    size_t op_count = 0;

    if (valid_count == 0) {
        if (get_file_size(g_listfile, &valid_count) != 0) {
            return 1;
        }
        valid_count /= sizeof(Node);
    }

    bptree_t *tree = bptree_alloc((int)g_leaf_size, (int)g_branch_factor, sizeof(Node),
                                  compare_nodes);
    if (!tree) {
        handle_error("bptree_alloc failed");
        return 1;
    }

    /* load_nodes only returns the nodes sorted when *nodes is preallocated;
     * otherwise they come back in pool (random) order. */
    if (g_listfile != NULL || gen_is_file(g_inputfile)) {
        nodes = calloc(valid_count, sizeof(Node));
        if (!nodes) {
            handle_error("node buffer allocation failed");
            return 1;
        }
    }

    if (load_nodes(g_inputfile, g_listfile, &nodes, &insert_nodes, &delete_nodes, &indexes,
                   &valid_count, &op_count) != 0) {
        handle_error("load_nodes failed");
        return 1;
    }

    printf("=== bulk_init + ordered dump ===\n");
    if (bptree_bulk_init(tree, nodes, valid_count, g_nodes_sorted) != 0) {
        handle_error("bulk_init failed");
        return 1;
    }
    if (tree->count != valid_count) {
        handle_error("count mismatch after bulk_init");
    }
    if (verbose > 0) {
        bptree_print_stats(tree);
    }

    if (validate_nodes_by_file(g_inputfile, "original", valid_count, tree,
                               test_dump_using_next) != 0) {
        handle_error("iterator dump != original sorted list");
    } else {
        printf("OK: iterator dump matches original\n");
    }

    if (validate_nodes_by_file(g_inputfile, "original", valid_count, tree,
                               test_dump_using_at) != 0) {
        handle_error("_at dump != original sorted list");
    } else {
        printf("OK: _at dump matches original\n");
    }

    printf("=== search present keys ===\n");
    for (size_t i = 0; i < op_count; i++) {
        if (bptree_search(tree, &delete_nodes[i]) == NULL) {
            char buf[128];
            snprintf(buf, sizeof(buf), "search miss for delete_nodes[%zu]=%lu", i,
                     (unsigned long)delete_nodes[i].value);
            handle_error(buf);
        }
    }
    printf("OK: searched %zu keys\n", op_count);

    printf("=== index random ranks ===\n");
    for (size_t i = 0; i < op_count; i++) {
        if (bptree_at(tree, (long)indexes[i]) == NULL) {
            char buf[128];
            snprintf(buf, sizeof(buf), "at(%zu) failed", indexes[i]);
            handle_error(buf);
        }
    }
    printf("OK: indexed %zu ranks\n", op_count);

    printf("=== insert ===\n");
    for (size_t i = 0; i < op_count; i++) {
        if (bptree_insert(tree, &insert_nodes[i]) == 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "insert failed for %lu",
                     (unsigned long)insert_nodes[i].value);
            handle_error(buf);
        }
    }
    if (tree->count != valid_count + op_count) {
        handle_error("count mismatch after insert");
    }
    if (validate_nodes_by_file(g_inputfile, "insert", valid_count, tree,
                               test_dump_using_next) != 0) {
        handle_error("post-insert dump mismatch");
    } else {
        printf("OK: insert dump matches\n");
    }

    printf("=== delete ===\n");
    for (size_t i = 0; i < op_count; i++) {
        if (bptree_delete(tree, &delete_nodes[i]) != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "delete failed for %lu",
                     (unsigned long)delete_nodes[i].value);
            handle_error(buf);
        }
    }
    if (tree->count != valid_count) {
        handle_error("count mismatch after delete");
    }
    if (validate_nodes_by_file(g_inputfile, "delete", valid_count, tree,
                               test_dump_using_next) != 0) {
        handle_error("post-delete dump mismatch");
    } else {
        printf("OK: delete dump matches\n");
    }

    /* rebuild empty then insert-only path (stress splits) */
    printf("=== insert-only rebuild stress ===\n");
    bptree_free(tree);
    tree = bptree_alloc((int)g_leaf_size, (int)g_branch_factor, sizeof(Node), compare_nodes);
    if (!tree) {
        handle_error("realloc failed");
        return 1;
    }
    for (size_t i = 0; i < valid_count; i++) {
        if (bptree_insert(tree, &nodes[i]) == 0) {
            handle_error("insert-only rebuild failed");
            break;
        }
    }
    if (tree->count == valid_count) {
        if (validate_nodes_by_file(g_inputfile, "original", valid_count, tree,
                                   test_dump_using_next) != 0) {
            handle_error("insert-only rebuild dump mismatch");
        } else {
            printf("OK: insert-only rebuild matches original\n");
        }
    }

    free(nodes);
    free(insert_nodes);
    free(delete_nodes);
    free(indexes);
    bptree_free(tree);

    printf("\n=== Test complete: %d error(s) ===\n", g_total_errors);
    return g_total_errors ? 1 : 0;
}
