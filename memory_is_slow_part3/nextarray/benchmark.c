#include "nextarray.h"
#include "benchmark_helpers.h"
#include "gen_nodes.h"
#include <math.h>
#include <getopt.h>

// Default values
#define DEFAULT_EXTENT_SIZE 512
#define DEFAULT_OVERPROVISIONING 2.0

// Global configuration
char* g_inputfile = NULL;
char* g_listfile = NULL;
uint64_t g_extent_size = DEFAULT_EXTENT_SIZE;
double g_overprovisioning = DEFAULT_OVERPROVISIONING;
uint64_t g_count = 0;
int debug = 0;
int verbose = 0;

void print_usage(const char* program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Benchmark NextArray data structures\n\n");
    printf("Options:\n");
    printf("  -i, --input FILE         Path file with pregenerated nodes [REQUIRED]\n");
    printf("  -l, --list  FILE         Path file with sorted nodes for debug\n");
    printf("  -e, --extent-size SIZE   Number of nodes per extent (default: %d)\n", DEFAULT_EXTENT_SIZE);
    printf("  -o, --overprovisioning   Overprovisioning factor (default: %f)\n", DEFAULT_OVERPROVISIONING);
    printf("  -c, --count              How many nodes to use\n");
    printf("  -d, --debug LEVEL        Enable debug mode\n");
    printf("  -v, --verbose LEVEL      Enable verbose mode\n");
    printf("  -h, --help               Show this help message\n");
    printf("\nOne of --list or --count must be specified\n");
}

int parse_arguments(int argc, char* argv[]) {
    int c;
    static struct option long_options[] = {
        {"input",   required_argument, 0, 'i'},
        {"list",    required_argument, 0, 'l'},
        {"extent-size", required_argument, 0, 'e'},
        {"overprovisioning", required_argument, 0, 'o'},
        {"count",   required_argument, 0, 'c'},
        {"debug",      required_argument, 0, 'd'},
        {"verbose",    required_argument, 0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "i:l:e:o:c:d:v:h", long_options, NULL)) != -1) {
        switch (c) {
            case 'i':
                g_inputfile = optarg;
                break;
            case 'l':
                g_listfile = optarg;
                break;
            case 'e':
                if (parse_size_bin(optarg, &g_extent_size) != 0) {
                    fprintf(stderr, "Error: invalid extent size value\n");
                    return -1;
                }
                break;
            case 'o':
                g_overprovisioning = atof(optarg);
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

    if (g_count > 0 &&  g_listfile != NULL) {
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

int nextarray_dump_list(void *context, const char *filename) {
    if (!context) return -1;
    nextarray_t *nextarray = (nextarray_t *)context;
    return nextarray_dump_nodes_to_file(nextarray, filename);
}

int main(int argc, char* argv[]) {
    char str1[32];
    char str2[32];
    char str3[32];
    // Parse command line arguments
    if (parse_arguments(argc, argv) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    Node* insert_nodes = NULL;
    Node* delete_nodes = NULL;
    size_t* indexes = NULL;
    size_t valid_count = g_count;
    size_t max_nodes;
    size_t op_count = 0;

    // Initialize NextArray
    if (valid_count == 0) {
        if (get_file_size(g_listfile, &valid_count) != 0) {
            fprintf(stderr, "Error: failed to get file size of %s\n", g_listfile);
            return -1;
        }
        valid_count /= sizeof(Node);
    }

    max_nodes = (size_t)(valid_count*g_overprovisioning);

    nextarray_t* nextarray = nextarray_alloc(max_nodes, g_extent_size, sizeof(Node), compare_nodes);
    if (!nextarray) {
        printf("Failed to create NextArray tree\n");
        return 1;
    }
    snprintf_count(str1, sizeof(str1), nextarray->num_extents);
    snprintf_bytes(str2, sizeof(str2), nextarray->extent_size);
    snprintf_bytes(str3, sizeof(str3), nextarray->buffer_size*nextarray->node_size);
    printf("NextArray:\n  extents %s of %s total buffer size %s\n", str1, str2, str3);
    snprintf_count(str1, sizeof(str1), nextarray->total_tree_nodes);
    snprintf_bytes(str2, sizeof(str2), LOOKUP_NODE_SIZE*sizeof(*nextarray->lookup_tree));
    printf("  lookup height: %d lt_nodes: %s size %s\n", nextarray->tree_height, str1, str2);

    // Load nodes into NextArray
    long starttime = get_time_us();
    if (load_nodes(g_inputfile, g_listfile, (Node **)&nextarray->buffer, &insert_nodes, &delete_nodes, &indexes, &valid_count, &op_count) != 0) {
        printf("Failed to create nodes\n");
        return 1;
    }
    report_elapsed_time("Loaded nodes", starttime);

    // Initialize NextArray
    starttime = get_time_us();
    nextarray_bulk_init(nextarray, valid_count, g_nodes_sorted);
    report_elapsed_time("  nextarray_bulk_init", starttime);

    starttime = get_time_us();
    if (nextarray_balance(nextarray) != 0) {
        printf("Failed to balance\n");
        return 1;
    }
    report_elapsed_time("  nextarray_balance", starttime);
    uint64_t overhead = nextarray->overhead;
    printf("NextArray overhead:    %s\n", snprintf_bytes(str1, sizeof(str1), overhead));
    uint64_t ram_usage = overhead + sizeof(Node)*(max_nodes+op_count*2);
    printf("RAM usage:             %s\n", snprintf_bytes(str1, sizeof(str1), ram_usage));

    // Validate original nodes
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "original", valid_count, (void*)nextarray, nextarray_dump_list) != 0) {
            return 1;
        }
    }

    printf("--- Computing statistics ---\n");

    //// Process NextArray Statistics ///////////////////////
    Stats stats;
    memset(&stats, 0, sizeof(Stats));
    Node* node = NULL;
    long bytes = 0;

    snprintf(stats.notes, sizeof(stats.notes), "extent_size=%ld", g_extent_size);
    stats.nodes = (uint64_t)valid_count;

    long find_extent = log2((double)(valid_count/g_extent_size))*sizeof(Node)*2; // O(log n) but of the extent range
    long search_extent = log2((double)g_extent_size/g_overprovisioning)*sizeof(Node); // O(log n) but of the extent array itself
    long avg_extent_cpy = (g_extent_size/g_overprovisioning)/2*sizeof(Node); // move 1/2 the valid nodes in extent on average

    long sum = 0;
    long count = 0;

    // Get Minimum for NextArray
    bytes = sizeof(void*)*3+sizeof(Node); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        node = (Node*)nextarray->head;
        if (node == NULL) {
            printf("Failed to get minimum value from NextArray\n");
            return 1;
        } else if (node->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.min, (double)node->value, op_count, bytes, starttime);

    // Get Maximum for NextArray
    bytes = sizeof(void*)*3+sizeof(Node); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        node = (Node*)nextarray->tail;
        if (node == NULL) {
            printf("Failed to get maximum value from NextArray\n");
            return 1;
        } else if (node->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.max, (double)node->value, op_count, bytes, starttime);

    // Get Median for NextArray
    bytes = find_extent + sizeof(Node); // O(log n/e)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        node = (Node*)nextarray_at(nextarray, valid_count/2);
        if (node == NULL) {
            printf("Failed to get median value from NextArray\n");
            return 1;
        }
    }
    record_event(&stats.median, (double)node->value, op_count, bytes, starttime);

    // Calculate Mean for NextArray
    bytes = valid_count*sizeof(Node); // O(n)
    starttime = get_time_us();
    sum = 0;
    count = 0;
    nextarray_iter_t iter;
    memset(&iter, 0, sizeof(nextarray_iter_t));
    node = (Node*)nextarray_next(nextarray, &iter);
    while (node) {
        sum += node->value;
        count++;
        node = (Node*)nextarray_next(nextarray, &iter);
    }
    record_event(&stats.mean, (double)sum / count, 1, bytes, starttime);

    // Calculate standard deviation for NextArray also O(n)
    //  NOTE: not resetting timer because you need to get mean to do stddev
    double sum_sq_diff = 0;
    memset(&iter, 0, sizeof(nextarray_iter_t));
    node = (Node*)nextarray_next(nextarray, &iter);
    while (node) {
        double diff = node->value - stats.mean.value;
        sum_sq_diff += diff * diff;
        node = (Node*)nextarray_next(nextarray, &iter);
    }
    record_event(&stats.stddev, sqrt(sum_sq_diff / count), 1, bytes*2, starttime);

    printf("Calculated basic statics for NextArray\n");

    //// Benchmarking Search ///////////////////////
    printf("--- Search benchmarks ---\n");

    // NextArray search
    bytes = find_extent + search_extent;
    Phase ph;
    phase_begin(&ph, op_count);
    do {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (nextarray_search(nextarray, &delete_nodes[i]) == NULL) {
                printf("Failed to search value %ld into NextArray tree\n", delete_nodes[i].value);
                nextarray_print_stats(nextarray);
                return 1;
            }
        }
        phase_stop(&ph);
    } while (phase_again(&ph));
    phase_record(&ph, &stats.search, bytes);

    printf("Completed Search benchmarks\n");

    //// Benchmarking Index ///////////////////////
    printf("--- Index benchmarks ---\n");

    // NextArray index
    bytes = find_extent + search_extent;
    phase_begin(&ph, op_count);
    do {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (nextarray_at(nextarray, indexes[i]) == NULL) {
                printf("Failed to search value %ld into NextArray tree\n", delete_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
    } while (phase_again(&ph));
    phase_record(&ph, &stats.index, bytes);

    printf("Completed Index benchmarks\n");


    //// Benchmarking Insert ///////////////////////
    printf("--- Insert benchmarks ---\n");

    // NextArray insert
    bytes = find_extent + search_extent + avg_extent_cpy;
    phase_begin(&ph, op_count);
    for (;;) {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (nextarray_insert(nextarray, &insert_nodes[i]) == 0) {
                printf("Failed to insert value %ld into NextArray tree\n", insert_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
        if (!phase_again(&ph)) break;
        for (uint64_t i = 0; i < op_count; i++) nextarray_delete(nextarray, &insert_nodes[i]);
        nextarray_balance(nextarray);
    }
    phase_record(&ph, &stats.insert, bytes);

    printf("Completed Insert benchmarks\n");

    // Validate
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "insert", valid_count, (void*)nextarray, nextarray_dump_list) != 0) {
            return 1;
        }
    }

    //// Benchmarking delete ///////////////////////
    printf("--- Delete benchmarks ---\n");

    // NextArray delete
    bytes = find_extent + search_extent + avg_extent_cpy;
    phase_begin(&ph, op_count);
    for (;;) {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (nextarray_delete(nextarray, &delete_nodes[i]) != 0) {
                printf("Failed to delete value %ld from NextArray tree\n", delete_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
        if (!phase_again(&ph)) break;
        for (uint64_t i = 0; i < op_count; i++) nextarray_insert(nextarray, &delete_nodes[i]);
        nextarray_balance(nextarray);
    }
    phase_record(&ph, &stats.delete, bytes);

    printf("Completed Delete benchmarks\n");

    //// Benchmarking clustered insert ///////////////////////
    // One narrow key range in increasing order (see gen_nodes.h): the
    // inserts all land in a few places, so this is where splits happen.
    if (g_clustered_count) {
        printf("--- Clustered insert benchmarks ---\n");
        phase_begin(&ph, op_count);
        for (;;) {
            phase_start(&ph);
            for (uint64_t i = 0; i < op_count; i++) {
                if (nextarray_insert(nextarray, &g_clustered_nodes[i]) == 0) {
                    printf("Failed to insert value %ld into NextArray tree\n", g_clustered_nodes[i].value);
                    return 1;
                }
            }
            phase_stop(&ph);
            if (!phase_again(&ph)) break;
            for (uint64_t i = 0; i < op_count; i++) nextarray_delete(nextarray, &g_clustered_nodes[i]);
            nextarray_balance(nextarray);
        }
        phase_record(&ph, &stats.clustered, bytes);
        printf("Completed Clustered insert benchmarks\n");
    }

    // Validate
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "delete", valid_count, (void*)nextarray, nextarray_dump_list) != 0) {
            return 1;
        }
    }

    // Print results
    printf("\n");
    print_results("NextArray:", &stats);
    printf("\n");

    stats_write_json(&stats, "nextarray");

    // Cleanup
    free(insert_nodes);
    free(delete_nodes);
    free(indexes);
    nextarray_free(nextarray);

    return 0;
}
