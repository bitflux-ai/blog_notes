#include <math.h>
#include <getopt.h>
#include "sortedarray.h"
#include "benchmark_helpers.h"
#include "gen_nodes.h"

// Global configuration
char* g_inputfile = NULL;
char* g_listfile = NULL;
uint64_t g_count = 0;
int debug = 0;
int verbose = 0;

void print_usage(const char* program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Benchmark Sorted Array data structures\n\n");
    printf("Options:\n");
    printf("  -i, --input FILE         Path file with pregenerated nodes [REQUIRED]\n");
    printf("  -l, --list  FILE         Path file with sorted nodes for debug\n");
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
        {"count",   required_argument, 0, 'c'},
        {"debug",      required_argument, 0, 'd'},
        {"verbose",    required_argument, 0, 'v'},
        {"help",       no_argument,       0, 'h'},
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

int sortedarray_dump_list(void *context, const char *filename) {
    if (!context) return -1;
    sortedarray_t *sortedarray = (sortedarray_t *)context;

    FILE *fp = fopen(filename, "wb");
    if (!fp) { 
        fprintf(stderr, "Error: failed to open file %s\n", filename);
        return -1;
    }

    size_t n = fwrite(sortedarray->buffer, sizeof(Node), sortedarray->count, fp);
    if (n != sortedarray->count) {
        fprintf(stderr, "Error: failed to write to file %s expected %zu got %zu\n", filename, sortedarray->count, n);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

int main(int argc, char* argv[]) {
    char str1[32];
    // Parse command line arguments
    if (parse_arguments(argc, argv) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    Node* insert_nodes = NULL;
    Node* delete_nodes = NULL;
    size_t* indexes = NULL;
    size_t valid_count = g_count;
    size_t op_count = 0;

    // Initialize SortedArray
    if (valid_count == 0) {
        if (get_file_size(g_listfile, &valid_count) != 0) {
            fprintf(stderr, "Error: failed to get file size of %s\n", g_listfile);
            return -1;
        }
    }
    sortedarray_t* sortedarray = sortedarray_alloc(valid_count*1.5, sizeof(Node), compare_nodes);
    if (!sortedarray) {
        printf("Failed to create SortedArray tree\n");
        return 1;
    }

    // Load nodes into SortedArray
    long starttime = get_time_us();
    if (load_nodes(g_inputfile, g_listfile, (Node **)&sortedarray->buffer, &insert_nodes, &delete_nodes, &indexes, &valid_count, &op_count) != 0) {
        printf("Failed to create nodes\n");
        return 1;
    }

    // Initialize SortedArray
    starttime = get_time_us();
    sortedarray_bulk_init(sortedarray, valid_count, g_nodes_sorted);
    report_elapsed_time("SortedArray sorted", starttime);
    sortedarray->count = valid_count;

    uint64_t overhead = sizeof(sortedarray_t)+sizeof(Node)*valid_count*0.2;
    printf("SortedArray overhead:  %s\n", snprintf_bytes(str1, sizeof(str1), overhead));
    uint64_t ram_usage = overhead + sizeof(Node)*(valid_count+op_count*2);
    printf("RAM usage:             %s\n", snprintf_bytes(str1, sizeof(str1), ram_usage));

    // Validate original nodes
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "original", valid_count, (void*)sortedarray, sortedarray_dump_list) != 0) {
            return 1;
        }
    }

    printf("--- Computing statistics ---\n");

    //// Process SortedArray Statistics ///////////////////////
    Stats stats;
    memset(&stats, 0, sizeof(Stats));
    Node *node = NULL;
    long bytes = 0;

    stats.nodes = (uint64_t)valid_count;

    long sum = 0;
    long count = 0;

    // Get Minimum for SortedArray
    bytes = sizeof(Node); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        node = (Node *)sortedarray->head;
        if (node == NULL) {
            printf("Error: SortedArray tree is empty\n");
            return 1;
        } else if (node->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.min, (double)node->value, op_count, bytes, starttime);

    // Get Maximum for SortedArray
    bytes = sizeof(Node); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        node = (Node *)sortedarray->tail;
        if (node == NULL) {
            printf("Error: SortedArray tree is empty\n");
            return 1;
        } else if (node->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.max, (double)node->value, op_count, bytes, starttime);

    // Calculate Median for SortedArray
    bytes = sizeof(Node); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        node = (Node *)sortedarray_at(sortedarray, valid_count/2);
        if (node == NULL) {
            printf("Error: SortedArray tree is empty\n");
            return 1;
        } else if (node->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.median, (double)node->value, op_count, bytes, starttime);

    // Calculate Mean for SortedArray
    bytes = sizeof(Node)*(valid_count); // O(n)
    starttime = get_time_us();
    sum = 0;
    count = 0;
    node = (Node *)sortedarray_at(sortedarray, 0);
    while (node) {
        sum += node->value;
        count++;
        node = (Node *)sortedarray_at(sortedarray, count);
    }
    record_event(&stats.mean, (double)sum / count, op_count, bytes, starttime);

    // Compute standard deviation for SortedArray also O(n)
    //  NOTE: not resetting timer because you need to get mean to do stddev
    double sum_sq_diff = 0;
    node = (Node *)sortedarray_at(sortedarray, 0);
    while (node) {
        double diff = node->value - stats.mean.value;
        sum_sq_diff += diff * diff;
        node = (Node *)sortedarray_at(sortedarray, count);
    }
    record_event(&stats.stddev, sqrt(sum_sq_diff / count), 1, bytes*2, starttime);

    printf("Calculated basic statics for SortedArray\n");

    //// Benchmarking Search ///////////////////////
    printf("--- Search benchmarks ---\n");

    // SortedArray search
    bytes = log2((double)valid_count)*sizeof(Node); // O(log n)
    Phase ph;
    phase_begin(&ph, op_count);
    do {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (sortedarray_search(sortedarray, &delete_nodes[i]) == NULL) {
                printf("Failed to search value %ld into SortedArray tree\n", delete_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
    } while (phase_again(&ph));
    phase_record(&ph, &stats.search, bytes);

    printf("Completed Search benchmarks\n");

    //// Benchmarking Index ///////////////////////
    printf("--- Index benchmarks ---\n");

    // SortedArray index
    bytes = log2((double)valid_count)*sizeof(Node); // O(log n)
    phase_begin(&ph, op_count);
    do {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (sortedarray_at(sortedarray, indexes[i]) == NULL) {
                printf("Failed to search value %ld into SortedArray tree\n", delete_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
    } while (phase_again(&ph));
    phase_record(&ph, &stats.index, bytes);

    printf("Completed Index benchmarks\n");

    //// Benchmarking Insert ///////////////////////
    printf("--- Insert benchmarks ---\n");

    // SortedArray insert
    bytes = log2((double)valid_count)*sizeof(Node)+valid_count*sizeof(Node)/2; // O(n)
    phase_begin(&ph, op_count);
    for (;;) {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (sortedarray_insert(sortedarray, &insert_nodes[i]) == NULL) {
                printf("Failed to insert value %ld into SortedArray tree\n", insert_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
        if (!phase_again(&ph)) break;
        for (uint64_t i = 0; i < op_count; i++) sortedarray_delete(sortedarray, &insert_nodes[i]);
    }
    phase_record(&ph, &stats.insert, bytes);

    printf("Completed Insert benchmarks\n");

    // Validate
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "insert", valid_count, (void*)sortedarray, sortedarray_dump_list) != 0) {
            return 1;
        }
    }

    //// Benchmarking delete ///////////////////////
    printf("--- Delete benchmarks ---\n");

    // SortedArray delete
    bytes = log2((double)valid_count)*sizeof(Node)+valid_count*sizeof(Node)/2; // O(n)
    phase_begin(&ph, op_count);
    for (;;) {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (sortedarray_delete(sortedarray, &delete_nodes[i]) == NULL) {
                printf("Failed to delete value %ld from SortedArray tree\n", delete_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
        if (!phase_again(&ph)) break;
        for (uint64_t i = 0; i < op_count; i++) sortedarray_insert(sortedarray, &delete_nodes[i]);
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
                if (sortedarray_insert(sortedarray, &g_clustered_nodes[i]) == NULL) {
                    printf("Failed to insert value %ld into SortedArray tree\n", g_clustered_nodes[i].value);
                    return 1;
                }
            }
            phase_stop(&ph);
            if (!phase_again(&ph)) break;
            for (uint64_t i = 0; i < op_count; i++) sortedarray_delete(sortedarray, &g_clustered_nodes[i]);
        }
        phase_record(&ph, &stats.clustered, bytes);
        printf("Completed Clustered insert benchmarks\n");
    }

    // Validate
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "delete", valid_count, (void*)sortedarray, sortedarray_dump_list) != 0) {
            return 1;
        }
    }

    // Print results
    printf("\n");
    print_results("SortedArray:", &stats);
    printf("\n");

    stats_write_json(&stats, "sortedarray");

    // Cleanup
    free(insert_nodes);
    free(delete_nodes);
    free(indexes);
    sortedarray_free(sortedarray);

    return 0;
}
