#include "art.h"
#include "benchmark_helpers.h"
#include <getopt.h>

char *g_inputfile = NULL;
char *g_listfile = NULL;
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
    printf("Test Adaptive Radix Tree functionality\n\n");
    printf("Options:\n");
    printf("  -i, --input FILE         Path file with pregenerated nodes [REQUIRED]\n");
    printf("  -l, --list  FILE         Path file with sorted nodes for debug\n");
    printf("  -c, --count              How many nodes to use\n");
    printf("  -d, --debug LEVEL        Enable debug mode\n");
    printf("  -v, --verbose LEVEL      Enable verbose mode\n");
    printf("      --nofail             Continue after failures\n");
    printf("  -h, --help               Show this help message\n");
    printf("\nOne of --list or --count must be specified\n");
}

int parse_arguments(int argc, char *argv[]) {
    int c;
    static struct option long_options[] = {
        {"input", required_argument, 0, 'i'},
        {"list", required_argument, 0, 'l'},
        {"count", required_argument, 0, 'c'},
        {"debug", required_argument, 0, 'd'},
        {"verbose", required_argument, 0, 'v'},
        {"nofail", no_argument, 0, 1000},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "i:l:c:d:v:h", long_options, NULL)) != -1) {
        switch (c) {
            case 'i':
                g_inputfile = optarg;
                break;
            case 'l':
                g_listfile = optarg;
                break;
            case 'c':
                if (parse_size_bin(optarg, &g_count) != 0) {
                    return -1;
                }
                break;
            case 'd':
                debug = atoi(optarg);
                break;
            case 'v':
                verbose = atoi(optarg);
                break;
            case 1000:
                g_nofail = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                return -1;
        }
    }

    if (g_count > 0 && g_listfile != NULL) {
        return -1;
    }
    if (g_count == 0 && g_listfile == NULL) {
        return -1;
    }
    if (g_inputfile == NULL) {
        return -1;
    }
    return 0;
}

int test_dump_using_at(void *context, const char *filename) {
    art_t *t = (art_t *)context;
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        return -1;
    }
    for (long i = 0; i < (long)t->count; i++) {
        Node *node = (Node *)art_at(t, i);
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
    return art_dump_nodes_to_file((art_t *)context, filename);
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

    /* Preallocate so load_nodes reloads sorted list into nodes. */
    if (g_listfile != NULL) {
        nodes = calloc(valid_count, sizeof(Node));
        if (!nodes) {
            return 1;
        }
    }

    art_t *tree = art_alloc(compare_nodes);
    if (!tree) {
        handle_error("art_alloc failed");
        return 1;
    }

    if (load_nodes(g_inputfile, g_listfile, &nodes, &insert_nodes, &delete_nodes, &indexes,
                   &valid_count, &op_count) != 0) {
        handle_error("load_nodes failed");
        return 1;
    }

    printf("=== bulk_init + ordered dump ===\n");
    if (art_bulk_init(tree, nodes, valid_count, g_listfile != NULL) != 0) {
        handle_error("bulk_init failed");
        return 1;
    }
    if (tree->count != valid_count) {
        handle_error("count mismatch after bulk_init");
    }
    if (verbose > 0) {
        art_print_stats(tree);
    }

    if (validate_nodes_by_file(g_inputfile, "original", valid_count, tree,
                               test_dump_using_next) != 0) {
        handle_error("iterator dump != original");
    } else {
        printf("OK: iterator dump matches original\n");
    }

    if (validate_nodes_by_file(g_inputfile, "original", valid_count, tree,
                               test_dump_using_at) != 0) {
        handle_error("_at dump != original");
    } else {
        printf("OK: _at dump matches original\n");
    }

    printf("=== search / index ===\n");
    for (size_t i = 0; i < op_count; i++) {
        if (art_search(tree, &delete_nodes[i]) == NULL) {
            handle_error("search miss");
        }
    }
    for (size_t i = 0; i < op_count; i++) {
        if (art_at(tree, (long)indexes[i]) == NULL) {
            handle_error("at miss");
        }
    }
    printf("OK: search/index %zu ops\n", op_count);

    printf("=== insert ===\n");
    for (size_t i = 0; i < op_count; i++) {
        if (art_insert(tree, &insert_nodes[i]) == 0) {
            handle_error("insert failed");
        }
    }
    if (tree->count != valid_count + op_count) {
        handle_error("count after insert");
    }
    if (validate_nodes_by_file(g_inputfile, "insert", valid_count, tree,
                               test_dump_using_next) != 0) {
        handle_error("post-insert dump");
    } else {
        printf("OK: insert dump matches\n");
    }

    printf("=== delete ===\n");
    for (size_t i = 0; i < op_count; i++) {
        if (art_delete(tree, &delete_nodes[i]) != 0) {
            handle_error("delete failed");
        }
    }
    if (tree->count != valid_count) {
        handle_error("count after delete");
    }
    if (validate_nodes_by_file(g_inputfile, "delete", valid_count, tree,
                               test_dump_using_next) != 0) {
        handle_error("post-delete dump");
    } else {
        printf("OK: delete dump matches\n");
    }

    free(nodes);
    free(insert_nodes);
    free(delete_nodes);
    free(indexes);
    art_free(tree);

    printf("\n=== Test complete: %d error(s) ===\n", g_total_errors);
    return g_total_errors ? 1 : 0;
}
