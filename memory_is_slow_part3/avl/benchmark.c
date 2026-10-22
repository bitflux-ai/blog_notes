#include <math.h>
#include <getopt.h>
#include <avl.h>
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
    printf("Benchmark AVL tree data structures\n\n");
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

int avl_dump_list(void *context, const char *filename) {
    if (!context) return -1;
    avl_tree_t *avl = (avl_tree_t *)context;

    const size_t chunk_elems = 4096;
    Node *buf = malloc(chunk_elems * sizeof(Node));
    if (!buf) return -1;

    FILE *fp = fopen(filename, "wb");
    if (!fp) { free(buf); return -1; }

    size_t count = 0;
    for (avl_node_t *p = avl->head; p; p = p->next) {
        Node *src = (Node *)p->item;
        buf[count++] = *src;
        if (count == chunk_elems) {
            size_t n = fwrite(buf, sizeof(Node), count, fp);
            if (n != count) { fclose(fp); free(buf); return -1; }
            count = 0;
        }
    }
    if (count) {
        size_t n = fwrite(buf, sizeof(Node), count, fp);
        if (n != count) { fclose(fp); free(buf); return -1; }
    }

    fclose(fp);
    free(buf);
    return 0;
}

// Populates a AVL tree of `size` nodes
avl_tree_t* create_avl_tree(size_t size, Node* nodes) {
    long starttime = get_time_us();
    avl_tree_t* avl_tree = avl_alloc_tree(compare_nodes, NULL);
    if (!avl_tree) {
        return NULL;
    }
    if (verbose > 0) {
        report_elapsed_time("avl_alloc_tree", starttime);
    }
    starttime = get_time_us();
    size_t i = 0;
    while (i < size) {
        if (avl_insert(avl_tree, &nodes[i]) == NULL) {
            i++;
        }
    }
    if (i != size) {
        printf("Failed to insert %zu nodes into AVL tree\n", size - i);
        return NULL;
    }
    if (verbose > 0) {
        report_elapsed_time("avl_insert", starttime);
    }
    return avl_tree;
}

int main(int argc, char* argv[]) {
    char str1[32];
    // Parse command line arguments
    if (parse_arguments(argc, argv) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    Node* nodes = NULL;
    Node* insert_nodes = NULL;
    Node* delete_nodes = NULL;
    size_t* indexes = NULL;
    size_t valid_count = g_count;
    size_t op_count = 0;

    // Load nodes
    long starttime = get_time_us();
    if (load_nodes(g_inputfile, g_listfile, &nodes, &insert_nodes, &delete_nodes, &indexes, &valid_count, &op_count) != 0) {
        printf("Failed to create nodes\n");
        return 1;
    }

    // Create AVL tree
    starttime = get_time_us();
    avl_tree_t* avl_tree = create_avl_tree(valid_count, nodes);
    if (!avl_tree) {
        printf("Failed to create AVL tree\n");
        return 1;
    }
    report_elapsed_time("AVL tree created", starttime);
    uint64_t overhead = sizeof(avl_tree_t)+sizeof(avl_node_t)*valid_count;
    printf("AVL overhead:      %s\n", snprintf_bytes(str1, sizeof(str1), overhead));
    uint64_t ram_usage = overhead + sizeof(Node)*(valid_count+op_count*2);
    printf("RAM usage:         %s\n", snprintf_bytes(str1, sizeof(str1), ram_usage));

    // Validate original nodes
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "original", valid_count, (void*)avl_tree, avl_dump_list) != 0) {
            return 1;
        }
    }

    printf("--- Computing statistics ---\n");

    //// Process AVL Tree Statistics ///////////////////////
    Stats stats;
    memset(&stats, 0, sizeof(Stats));
    Node *node = NULL;
    avl_node_t *avl_node = NULL;
    long bytes = 0;

    stats.nodes = (uint64_t)valid_count;


    long sum = 0;
    long count = 0;

    // Warmup run
    bytes = sizeof(Node) + sizeof(avl_node_t); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        avl_node = avl_tree->head;
        if (avl_node == NULL) {
            printf("Error: AVL tree is empty\n");
            return 1;
        }
        node = (Node *)avl_node->item;
        if (node == NULL) {
            printf("Error: AVL node is NULL\n");
            return 1;
        }
    }
    record_event(&stats.min, (double)node->value, op_count, bytes, starttime);


    // Get Minimum for AVL Tree
    bytes = sizeof(Node) + sizeof(avl_node_t); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        avl_node = avl_tree->head;
        if (avl_node == NULL) {
            printf("Error: AVL tree is empty\n");
            return 1;
        }
        node = (Node *)avl_node->item;
        if (node == NULL) {
            printf("Error: AVL node is NULL\n");
            return 1;
        }
    }
    record_event(&stats.min, (double)node->value, op_count, bytes, starttime);

    // Get Maximum for AVL Tree
    bytes = sizeof(Node) + sizeof(avl_node_t); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        avl_node = avl_tree->tail;
        if (avl_node == NULL) {
            printf("Error: AVL tree is empty\n");
            return 1;
        }
        node = (Node *)avl_node->item;
        if (node == NULL) {
            printf("Error: AVL node is NULL\n");
            return 1;
        }
    }
    record_event(&stats.max, (double)node->value, op_count, bytes, starttime);

    // Calculate Median for AVL Tree
    bytes = log2((double)(valid_count/2))*(sizeof(avl_node_t)+sizeof(Node)); // O(log n)
    count = 0;
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        node = avl_at(avl_tree, valid_count/2)->item;
        if (node == NULL) {
            printf("Error: AVL node is NULL\n");
            return 1;
        }
    }
    record_event(&stats.median, (double)node->value, op_count, bytes, starttime);

    // Calculate Mean for AVL Tree
    bytes = log2((double)valid_count)*(sizeof(avl_node_t)+sizeof(Node))+sizeof(Node)*valid_count; // O(n)
    starttime = get_time_us();
    avl_node = avl_tree->head;
    while (avl_node) {
        Node *node = (Node *)avl_node->item;
        sum += node->value;
        count++;
        avl_node = avl_node->next;
    }
    record_event(&stats.mean, (double)sum / count, 1, bytes, starttime);

    // Compute standard deviation for AVL Tree also O(n log n)
    //  NOTE: not resetting timer because you need to get mean to do stddev
    double sum_sq_diff = 0;
    avl_node = avl_tree->head;
    while (avl_node) {
        Node *node = (Node *)avl_node->item;
        double diff = node->value - stats.mean.value;
        sum_sq_diff += diff * diff;
        avl_node = avl_node->next;
    }
    record_event(&stats.stddev, sqrt(sum_sq_diff / count), 1, bytes*2, starttime);

    printf("Calculated basic statics for AVL Tree\n");

    //// Benchmarking Search ///////////////////////
    printf("--- Search benchmarks ---\n");

    // AVL search
    bytes = log2((double)valid_count)*(sizeof(avl_node_t)+sizeof(Node)); // O(log n)
    Phase ph;
    phase_begin(&ph, op_count);
    do {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (avl_search(avl_tree, &delete_nodes[i]) == NULL) {
                printf("Failed to search value %ld into AVL tree\n", delete_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
    } while (phase_again(&ph));
    phase_record(&ph, &stats.search, bytes);

    printf("Completed Search benchmarks\n");

    //// Benchmarking Index ///////////////////////
    printf("--- Index benchmarks ---\n");

    // AVL index
    bytes = log2((double)valid_count)*(sizeof(avl_node_t)+sizeof(Node)); // O(log n)
    phase_begin(&ph, op_count);
    do {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (avl_at(avl_tree, indexes[i]) == NULL) {
                printf("Failed to search value %ld into AVL tree\n", indexes[i]);
                return 1;
            }
        }
        phase_stop(&ph);
    } while (phase_again(&ph));
    phase_record(&ph, &stats.index, bytes);

    printf("Completed Index benchmarks\n");

    //// Benchmarking Insert ///////////////////////
    printf("--- Insert benchmarks ---\n");

    // AVL insert
    bytes = log2((double)valid_count)*(sizeof(avl_node_t)+sizeof(Node)); // O(log n)
    phase_begin(&ph, op_count);
    for (;;) {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (avl_insert(avl_tree, &insert_nodes[i]) == NULL) {
                printf("Failed to insert value %ld into AVL tree\n", insert_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
        if (!phase_again(&ph)) break;
        for (uint64_t i = 0; i < op_count; i++) avl_delete(avl_tree, &insert_nodes[i]);
    }
    phase_record(&ph, &stats.insert, bytes);

    printf("Completed Insert benchmarks\n");

    // Validate
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "insert", valid_count, (void*)avl_tree, avl_dump_list) != 0) {
            return 1;
        }
    }

    //// Benchmarking delete ///////////////////////
    printf("--- Delete benchmarks ---\n");

    // AVL delete
    bytes = log2((double)valid_count)*(sizeof(avl_node_t)+sizeof(Node)); // O(log n)
    phase_begin(&ph, op_count);
    for (;;) {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (avl_delete(avl_tree, &delete_nodes[i]) == NULL) {
                printf("Failed to delete value %ld from AVL tree\n", delete_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
        if (!phase_again(&ph)) break;
        for (uint64_t i = 0; i < op_count; i++) avl_insert(avl_tree, &delete_nodes[i]);
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
                if (avl_insert(avl_tree, &g_clustered_nodes[i]) == NULL) {
                    printf("Failed to insert value %ld into AVL tree\n", g_clustered_nodes[i].value);
                    return 1;
                }
            }
            phase_stop(&ph);
            if (!phase_again(&ph)) break;
            for (uint64_t i = 0; i < op_count; i++) avl_delete(avl_tree, &g_clustered_nodes[i]);
        }
        phase_record(&ph, &stats.clustered, bytes);
        printf("Completed Clustered insert benchmarks\n");
    }

    // Validate
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "delete", valid_count, (void*)avl_tree, avl_dump_list) != 0) {
            return 1;
        }
    }

    // Print results
    printf("\n");
    print_results("AVL Tree:", &stats);
    printf("\n");

    stats_write_json(&stats, "avl");

    // Cleanup
    free(insert_nodes);
    free(delete_nodes);
    free(indexes);
    free(nodes);
    avl_free_tree(avl_tree);

    return 0;
}
