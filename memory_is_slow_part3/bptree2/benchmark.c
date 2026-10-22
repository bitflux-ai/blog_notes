#include "bptree.h"
#include "benchmark_helpers.h"
#include "gen_nodes.h"
#include <math.h>
#include <getopt.h>

/* Defaults chosen for fair comparison with nextarray (extent_size=512).
 * branch_factor is independent: multiway fanout for the index path. */
#define DEFAULT_LEAF_SIZE 512
#define DEFAULT_BRANCH_FACTOR 16
/* B-tree practice: room for inserts without splitting and for deletes
 * without merging (a leaf must stay at least half full). */
#define DEFAULT_FILL 0.75

char *g_inputfile = NULL;
char *g_listfile = NULL;
uint64_t g_leaf_size = DEFAULT_LEAF_SIZE;
uint64_t g_branch_factor = DEFAULT_BRANCH_FACTOR;
uint64_t g_count = 0;
double g_fill = DEFAULT_FILL;
int debug = 0;
int verbose = 0;

void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Benchmark B+ tree data structure (order-statistic, in-RAM)\n\n");
    printf("Options:\n");
    printf("  -i, --input FILE         Path file with pregenerated nodes [REQUIRED]\n");
    printf("  -l, --list  FILE         Path file with sorted nodes for debug\n");
    printf("  -e, --leaf-size SIZE     Max keys per leaf (default: %d; peer of nextarray extent)\n",
           DEFAULT_LEAF_SIZE);
    printf("  -b, --branch-factor N    Max children per internal node (default: %d)\n",
           DEFAULT_BRANCH_FACTOR);
    printf("  -F, --fill FRACTION      Fill leaves/internal nodes to this at bulk load (default: %.2f)\n",
           DEFAULT_FILL);
    printf("  -c, --count              How many nodes to use\n");
    printf("  -d, --debug LEVEL        Enable debug mode\n");
    printf("  -v, --verbose LEVEL      Enable verbose mode\n");
    printf("  -h, --help               Show this help message\n");
    printf("\nOne of --list or --count must be specified\n");
}

int parse_arguments(int argc, char *argv[]) {
    int c;
    static struct option long_options[] = {
        {"input", required_argument, 0, 'i'},
        {"list", required_argument, 0, 'l'},
        {"leaf-size", required_argument, 0, 'e'},
        {"extent-size", required_argument, 0, 'e'}, /* alias for supersweep familiarity */
        {"branch-factor", required_argument, 0, 'b'},
        {"fill", required_argument, 0, 'F'},
        {"count", required_argument, 0, 'c'},
        {"debug", required_argument, 0, 'd'},
        {"verbose", required_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "i:l:e:b:F:c:d:v:h", long_options, NULL)) != -1) {
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
            case 'F':
                g_fill = atof(optarg);
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
                    fprintf(stderr, "Error: debug level must be non-negative\n");
                    return -1;
                }
                break;
            case 'v':
                verbose = atoi(optarg);
                if (verbose < 0) {
                    fprintf(stderr, "Error: verbose level must be non-negative\n");
                    return -1;
                }
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            case '?':
                return -1;
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
    if (g_leaf_size < 2) {
        fprintf(stderr, "Error: leaf-size must be >= 2\n");
        return -1;
    }
    if (g_branch_factor < 3) {
        fprintf(stderr, "Error: branch-factor must be >= 3\n");
        return -1;
    }
    if (!(g_fill > 0.0 && g_fill <= 1.0)) {
        fprintf(stderr, "Error: fill must be in (0, 1]\n");
        return -1;
    }

    return 0;
}

int bptree_dump_list(void *context, const char *filename) {
    if (!context) {
        return -1;
    }
    return bptree_dump_nodes_to_file((bptree_t *)context, filename);
}

int main(int argc, char *argv[]) {
    char str1[32];
    char str2[32];

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
            fprintf(stderr, "Error: failed to get file size of %s\n", g_listfile);
            return 1;
        }
        valid_count /= sizeof(Node);
    }

    /* load_nodes only reloads the sorted list when *nodes is already non-NULL
     * (same pattern as nextarray). Without this, bulk_init is told data is sorted
     * but still holds an unsorted pool prefix — search then fails. */
    if (g_listfile != NULL || gen_is_file(g_inputfile)) {
        nodes = calloc(valid_count, sizeof(Node));
        if (!nodes) {
            printf("Failed to allocate node buffer\n");
            return 1;
        }
    }

    bptree_t *tree = bptree_alloc((int)g_leaf_size, (int)g_branch_factor, sizeof(Node),
                                  compare_nodes);
    if (!tree) {
        printf("Failed to create B+ tree\n");
        return 1;
    }
    tree->bulk_fill = g_fill;
    snprintf_count(str1, sizeof(str1), g_leaf_size);
    snprintf_count(str2, sizeof(str2), g_branch_factor);
    printf("B+ tree: leaf_size %s branch_factor %s fill %.2f\n", str1, str2, g_fill);

    long starttime = get_time_us();
    if (load_nodes(g_inputfile, g_listfile, &nodes, &insert_nodes, &delete_nodes, &indexes,
                   &valid_count, &op_count) != 0) {
        printf("Failed to create nodes\n");
        return 1;
    }
    report_elapsed_time("Loaded nodes", starttime);

    starttime = get_time_us();
    if (bptree_bulk_init(tree, nodes, valid_count, g_nodes_sorted) != 0) {
        printf("Failed to bulk_init B+ tree\n");
        return 1;
    }
    report_elapsed_time("  bptree_bulk_init", starttime);

    uint64_t overhead = tree->overhead;
    printf("B+ tree overhead:      %s\n", snprintf_bytes(str1, sizeof(str1), overhead));
    uint64_t ram_usage = overhead + sizeof(Node) * (valid_count + op_count * 2);
    /* leaf payloads are also in overhead accounting partially; include payload estimate */
    ram_usage += sizeof(Node) * valid_count;
    printf("RAM usage (approx):    %s\n", snprintf_bytes(str1, sizeof(str1), ram_usage));
    if (verbose > 0) {
        bptree_print_stats(tree);
    }

    if (debug > 0) {
        if (validate_nodes_by_file(g_inputfile, "original", valid_count, (void *)tree,
                                   bptree_dump_list) != 0) {
            return 1;
        }
    }

    printf("--- Computing statistics ---\n");

    Stats stats;
    memset(&stats, 0, sizeof(Stats));
    Node *node = NULL;
    long bytes = 0;

#ifdef BPTREE_U64_KEYS
    const char *keys = "u64";
#else
    const char *keys = "generic";
#endif
    snprintf(stats.notes, sizeof(stats.notes), "leaf_size=%lu,branch_factor=%lu,fill=%.2f,keys=%s",
             (unsigned long)g_leaf_size, (unsigned long)g_branch_factor, g_fill, keys);
    stats.nodes = (uint64_t)valid_count;

    double leaf_keys = (double)g_leaf_size * g_fill;   /* keys per leaf after bulk load */
    double n_leaves = (double)valid_count / leaf_keys;
    if (n_leaves < 1.0) {
        n_leaves = 1.0;
    }
    double fanout = (double)g_branch_factor * g_fill < 2.0 ? 2.0 : (double)g_branch_factor * g_fill;
    long find_path = (long)(log((double)n_leaves) / log(fanout) + 1.0) *
                     (sizeof(void *) * (size_t)g_branch_factor);
    long search_leaf = (long)(log2(leaf_keys) * sizeof(Node));
    long avg_leaf_cpy = (long)((leaf_keys / 2) * sizeof(Node));

    long sum = 0;
    long count = 0;

    /* min */
    bytes = sizeof(Node);
    starttime = get_time_us();
    for (size_t i = 0; i < op_count; i++) {
        node = (Node *)tree->head;
        if (node == NULL) {
            printf("Failed to get minimum value from B+ tree\n");
            return 1;
        } else if (node->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.min, (double)node->value, op_count, bytes, starttime);

    /* max */
    bytes = sizeof(Node);
    starttime = get_time_us();
    for (size_t i = 0; i < op_count; i++) {
        node = (Node *)tree->tail;
        if (node == NULL) {
            printf("Failed to get maximum value from B+ tree\n");
            return 1;
        } else if (node->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.max, (double)node->value, op_count, bytes, starttime);

    /* median / at */
    bytes = find_path + sizeof(Node);
    starttime = get_time_us();
    for (size_t i = 0; i < op_count; i++) {
        node = (Node *)bptree_at(tree, (long)(valid_count / 2));
        if (node == NULL) {
            printf("Failed to get median value from B+ tree\n");
            return 1;
        }
    }
    record_event(&stats.median, (double)node->value, op_count, bytes, starttime);

    /* mean: full ordered scan via leaf links */
    bytes = (long)(valid_count * sizeof(Node));
    starttime = get_time_us();
    sum = 0;
    count = 0;
    bptree_iter_t iter;
    memset(&iter, 0, sizeof(iter));
    node = (Node *)bptree_next(tree, &iter);
    while (node) {
        sum += (long)node->value;
        count++;
        node = (Node *)bptree_next(tree, &iter);
    }
    record_event(&stats.mean, (double)sum / (double)count, 1, bytes, starttime);

    double sum_sq_diff = 0;
    memset(&iter, 0, sizeof(iter));
    node = (Node *)bptree_next(tree, &iter);
    while (node) {
        double diff = (double)node->value - stats.mean.value;
        sum_sq_diff += diff * diff;
        node = (Node *)bptree_next(tree, &iter);
    }
    record_event(&stats.stddev, sqrt(sum_sq_diff / (double)count), 1, bytes * 2, starttime);

    printf("Calculated basic statics for B+ tree\n");

    printf("--- Search benchmarks ---\n");
    bytes = find_path + search_leaf;
    Phase ph;
    phase_begin(&ph, (long)op_count);
    do {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (bptree_search(tree, &delete_nodes[i]) == NULL) {
                printf("Failed to search value %ld into B+ tree\n", (long)delete_nodes[i].value);
                bptree_print_stats(tree);
                return 1;
            }
        }
        phase_stop(&ph);
    } while (phase_again(&ph));
    phase_record(&ph, &stats.search, bytes);
    printf("Completed Search benchmarks\n");

    printf("--- Index benchmarks ---\n");
    bytes = find_path + sizeof(Node);
    phase_begin(&ph, (long)op_count);
    do {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (bptree_at(tree, (long)indexes[i]) == NULL) {
                printf("Failed to index rank %zu into B+ tree\n", indexes[i]);
                return 1;
            }
        }
        phase_stop(&ph);
    } while (phase_again(&ph));
    phase_record(&ph, &stats.index, bytes);
    printf("Completed Index benchmarks\n");

    printf("--- Insert benchmarks ---\n");
    bytes = find_path + search_leaf + avg_leaf_cpy;
    phase_begin(&ph, (long)op_count);
    for (;;) {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (bptree_insert(tree, &insert_nodes[i]) == 0) {
                printf("Failed to insert value %ld into B+ tree\n", (long)insert_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
        if (!phase_again(&ph)) break;
        for (uint64_t i = 0; i < op_count; i++) bptree_delete(tree, &insert_nodes[i]);
    }
    phase_record(&ph, &stats.insert, bytes);
    printf("Completed Insert benchmarks\n");

    if (debug > 0) {
        if (validate_nodes_by_file(g_inputfile, "insert", valid_count, (void *)tree,
                                   bptree_dump_list) != 0) {
            return 1;
        }
    }

    printf("--- Delete benchmarks ---\n");
    bytes = find_path + search_leaf + avg_leaf_cpy;
    phase_begin(&ph, (long)op_count);
    for (;;) {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (bptree_delete(tree, &delete_nodes[i]) != 0) {
                printf("Failed to delete value %ld from B+ tree\n", (long)delete_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
        if (!phase_again(&ph)) break;
        for (uint64_t i = 0; i < op_count; i++) bptree_insert(tree, &delete_nodes[i]);
    }
    phase_record(&ph, &stats.delete, bytes);
    printf("Completed Delete benchmarks\n");

    //// Benchmarking clustered insert ///////////////////////
    // One narrow key range in increasing order (see gen_nodes.h): the
    // inserts all land in a few places, so this is where splits happen.
    if (g_clustered_count) {
        printf("--- Clustered insert benchmarks ---\n");
        phase_begin(&ph, (long)op_count);
        for (;;) {
            phase_start(&ph);
            for (uint64_t i = 0; i < op_count; i++) {
                if (bptree_insert(tree, &g_clustered_nodes[i]) == 0) {
                    printf("Failed to insert value %ld into B+ tree\n", (long)g_clustered_nodes[i].value);
                    return 1;
                }
            }
            phase_stop(&ph);
            if (!phase_again(&ph)) break;
            for (uint64_t i = 0; i < op_count; i++) bptree_delete(tree, &g_clustered_nodes[i]);
        }
        phase_record(&ph, &stats.clustered, bytes);
        printf("Completed Clustered insert benchmarks\n");
    }

    if (debug > 0) {
        if (validate_nodes_by_file(g_inputfile, "delete", valid_count, (void *)tree,
                                   bptree_dump_list) != 0) {
            return 1;
        }
    }

    printf("\n");
    print_results("B+ tree (bptree2, one at a time):", &stats);
    printf("\n");

    stats_write_json(&stats, "bptree2_single");

    free(insert_nodes);
    free(delete_nodes);
    free(indexes);
    free(nodes);
    bptree_free(tree);

    return 0;
}
