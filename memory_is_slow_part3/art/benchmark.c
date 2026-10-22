#include "art.h"
#include "benchmark_helpers.h"
#include "gen_nodes.h"
#include <math.h>
#include <getopt.h>

char *g_inputfile = NULL;
char *g_listfile = NULL;
uint64_t g_count = 0;
int g_skip_rank = 0; /* skip median/index: art_at walks every child per level */
int debug = 0;
int verbose = 0;

void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Benchmark Adaptive Radix Tree (ART) for uint64 Node keys\n\n");
    printf("Options:\n");
    printf("  -i, --input FILE         Path file with pregenerated nodes [REQUIRED]\n");
    printf("  -l, --list  FILE         Path file with sorted nodes for debug\n");
    printf("  -c, --count              How many nodes to use\n");
    printf("  -R, --skip-rank          Skip median/index (art_at is slow)\n");
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
        {"count", required_argument, 0, 'c'},
        {"skip-rank", no_argument, 0, 'R'},
        {"debug", required_argument, 0, 'd'},
        {"verbose", required_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "i:l:c:Rd:v:h", long_options, NULL)) != -1) {
        switch (c) {
            case 'i':
                g_inputfile = optarg;
                break;
            case 'l':
                g_listfile = optarg;
                break;
            case 'c':
                if (parse_size_bin(optarg, &g_count) != 0) {
                    fprintf(stderr, "Error: invalid count value\n");
                    return -1;
                }
                break;
            case 'R':
                g_skip_rank = 1;
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
    return 0;
}

int art_dump_list(void *context, const char *filename) {
    if (!context) {
        return -1;
    }
    return art_dump_nodes_to_file((art_t *)context, filename);
}

int main(int argc, char *argv[]) {
    char str1[32];

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
     * (see nextarray/sortedarray). Preallocate so bulk_init sees sorted keys. */
    if (g_listfile != NULL || gen_is_file(g_inputfile)) {
        nodes = calloc(valid_count, sizeof(Node));
        if (!nodes) {
            printf("Failed to allocate node buffer\n");
            return 1;
        }
    }

    art_t *tree = art_alloc(compare_nodes);
    if (!tree) {
        printf("Failed to create ART\n");
        return 1;
    }
    printf("ART: Adaptive Radix Tree (uint64 big-endian keys, Node4/16/48/256)\n");

    long starttime = get_time_us();
    if (load_nodes(g_inputfile, g_listfile, &nodes, &insert_nodes, &delete_nodes, &indexes,
                   &valid_count, &op_count) != 0) {
        printf("Failed to create nodes\n");
        return 1;
    }
    report_elapsed_time("Loaded nodes", starttime);

    starttime = get_time_us();
    if (art_bulk_init(tree, nodes, valid_count, g_nodes_sorted) != 0) {
        printf("Failed to bulk_init ART\n");
        return 1;
    }
    report_elapsed_time("  art_bulk_init", starttime);

    uint64_t overhead = tree->overhead;
    printf("ART overhead:          %s\n", snprintf_bytes(str1, sizeof(str1), overhead));
    uint64_t ram_usage = overhead + sizeof(Node) * (valid_count + op_count * 2);
    printf("RAM usage (approx):    %s\n", snprintf_bytes(str1, sizeof(str1), ram_usage));
    if (verbose > 0) {
        art_print_stats(tree);
    }

    if (debug > 0) {
        if (validate_nodes_by_file(g_inputfile, "original", valid_count, (void *)tree,
                                   art_dump_list) != 0) {
            return 1;
        }
    }

    printf("--- Computing statistics ---\n");

    Stats stats;
    memset(&stats, 0, sizeof(Stats));
    Node *node = NULL;
    long bytes = 0;

    snprintf(stats.notes, sizeof(stats.notes), "structure=art,key_bytes=8,rank=%s", g_skip_rank ? "skipped" : "subtree_counts");
    stats.nodes = (uint64_t)valid_count;

    /* Rough model: ~1-8 node hops, adaptive fanout */
    long path = (long)(8 * (sizeof(void *) + 16));
    long sum = 0;
    long count = 0;

    bytes = sizeof(Node);
    starttime = get_time_us();
    for (size_t i = 0; i < op_count; i++) {
        node = (Node *)tree->head;
        if (node == NULL) {
            printf("Failed to get minimum from ART\n");
            return 1;
        } else if (node->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.min, (double)node->value, (long)op_count, bytes, starttime);

    bytes = sizeof(Node);
    starttime = get_time_us();
    for (size_t i = 0; i < op_count; i++) {
        node = (Node *)tree->tail;
        if (node == NULL) {
            printf("Failed to get maximum from ART\n");
            return 1;
        } else if (node->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.max, (double)node->value, (long)op_count, bytes, starttime);

    if (!g_skip_rank) {
        bytes = path + sizeof(Node);
        starttime = get_time_us();
        for (size_t i = 0; i < op_count; i++) {
            node = (Node *)art_at(tree, (long)(valid_count / 2));
            if (node == NULL) {
                printf("Failed to get median from ART\n");
                return 1;
            }
        }
        record_event(&stats.median, (double)node->value, (long)op_count, bytes, starttime);
    } else {
        printf("Skipping median/index (--skip-rank; art_at walks every child per level)\n");
    }

    bytes = (long)(valid_count * sizeof(Node));
    starttime = get_time_us();
    sum = 0;
    count = 0;
    art_iter_t iter;
    memset(&iter, 0, sizeof(iter));
    node = (Node *)art_next(tree, &iter);
    while (node) {
        sum += (long)node->value;
        count++;
        node = (Node *)art_next(tree, &iter);
    }
    if ((size_t)count != valid_count) {
        printf("ART scan count mismatch: got %ld expected %zu\n", count, valid_count);
        return 1;
    }
    record_event(&stats.mean, (double)sum / (double)count, 1, bytes, starttime);

    double sum_sq_diff = 0;
    memset(&iter, 0, sizeof(iter));
    node = (Node *)art_next(tree, &iter);
    while (node) {
        double diff = (double)node->value - stats.mean.value;
        sum_sq_diff += diff * diff;
        node = (Node *)art_next(tree, &iter);
    }
    record_event(&stats.stddev, sqrt(sum_sq_diff / (double)count), 1, bytes * 2, starttime);

    printf("Calculated basic statics for ART\n");

    printf("--- Search benchmarks ---\n");
    bytes = path;
    Phase ph;
    phase_begin(&ph, (long)op_count);
    do {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (art_search(tree, &delete_nodes[i]) == NULL) {
                printf("Failed to search value %ld into ART\n", (long)delete_nodes[i].value);
                art_print_stats(tree);
                return 1;
            }
        }
        phase_stop(&ph);
    } while (phase_again(&ph));
    phase_record(&ph, &stats.search, bytes);
    printf("Completed Search benchmarks\n");

    if (!g_skip_rank) {
        printf("--- Index benchmarks ---\n");
        bytes = path;
        phase_begin(&ph, (long)op_count);
        do {
            phase_start(&ph);
            for (uint64_t i = 0; i < op_count; i++) {
                if (art_at(tree, (long)indexes[i]) == NULL) {
                    printf("Failed to index rank %zu into ART\n", indexes[i]);
                    return 1;
                }
            }
            phase_stop(&ph);
        } while (phase_again(&ph));
        phase_record(&ph, &stats.index, bytes);
        printf("Completed Index benchmarks\n");
    }

    printf("--- Insert benchmarks ---\n");
    bytes = path;
    phase_begin(&ph, (long)op_count);
    for (;;) {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (art_insert(tree, &insert_nodes[i]) == 0) {
                printf("Failed to insert value %ld into ART\n", (long)insert_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
        if (!phase_again(&ph)) break;
        for (uint64_t i = 0; i < op_count; i++) art_delete(tree, &insert_nodes[i]);
    }
    phase_record(&ph, &stats.insert, bytes);
    printf("Completed Insert benchmarks\n");

    if (debug > 0) {
        if (validate_nodes_by_file(g_inputfile, "insert", valid_count, (void *)tree,
                                   art_dump_list) != 0) {
            return 1;
        }
    }

    printf("--- Delete benchmarks ---\n");
    bytes = path;
    phase_begin(&ph, (long)op_count);
    for (;;) {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (art_delete(tree, &delete_nodes[i]) != 0) {
                printf("Failed to delete value %ld from ART\n", (long)delete_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
        if (!phase_again(&ph)) break;
        for (uint64_t i = 0; i < op_count; i++) art_insert(tree, &delete_nodes[i]);
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
                if (art_insert(tree, &g_clustered_nodes[i]) == 0) {
                    printf("Failed to insert value %ld into ART\n", (long)g_clustered_nodes[i].value);
                    return 1;
                }
            }
            phase_stop(&ph);
            if (!phase_again(&ph)) break;
            for (uint64_t i = 0; i < op_count; i++) art_delete(tree, &g_clustered_nodes[i]);
        }
        phase_record(&ph, &stats.clustered, bytes);
        printf("Completed Clustered insert benchmarks\n");
    }

    if (debug > 0) {
        if (validate_nodes_by_file(g_inputfile, "delete", valid_count, (void *)tree,
                                   art_dump_list) != 0) {
            return 1;
        }
    }

    printf("\n");
    print_results("ART:", &stats);
    printf("\n");

    stats_write_json(&stats, "art");

    free(insert_nodes);
    free(delete_nodes);
    free(indexes);
    free(nodes);
    art_free(tree);
    return 0;
}
