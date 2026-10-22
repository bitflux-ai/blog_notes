#include <math.h>
#include <getopt.h>
#include "linkedlist.h"
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
    printf("Benchmark Double Linked List data structures\n\n");
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

int linkedlist_dump_list(void *context, const char *filename) {
    if (!context) return -1;
    linklist_tree_t *ll_tree = (linklist_tree_t *)context;

    const size_t chunk_elems = 4096;
    Node *buf = malloc(chunk_elems * sizeof(Node));
    if (!buf) return -1;

    FILE *fp = fopen(filename, "wb");
    if (!fp) { free(buf); return -1; }

    size_t count = 0;
    for (linklist_node_t *p = ll_tree->head; p; p = p->next) {
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

// Populates a LinkedList tree of `size` nodes
linklist_tree_t* create_ll_tree(int size, Node* nodes) {
    long starttime = get_time_us();
    linklist_tree_t* ll_tree = linklist_alloc_tree(compare_nodes);
    if (!ll_tree) {
        return NULL;
    }
    if (verbose > 0) {
        report_elapsed_time("linklist_alloc_tree", starttime);
    }
    starttime = get_time_us();
    int i = 0;
    while (i < size) {
        if (linklist_insert(ll_tree, &nodes[i]) == NULL) {
            i++;
        }
    }
    if (i != size) {
        printf("Failed to insert %d nodes into LinkedList tree\n", size - i);
        return NULL;
    }
    if (verbose > 0) {
        report_elapsed_time("linklist_insert", starttime);
    }
    return ll_tree;
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

    // Create Linked List
    starttime = get_time_us();
    linklist_tree_t* ll_tree = create_ll_tree(valid_count, nodes);
    if (!ll_tree) {
        printf("Failed to create LinkedList tree\n");
        return 1;
    }
    report_elapsed_time("LinkedList tree created", starttime);
    uint64_t overhead = sizeof(linklist_tree_t)+sizeof(linklist_node_t)*valid_count;
    printf("LinkedList overhead:   %s\n", snprintf_bytes(str1, sizeof(str1), overhead));
    uint64_t ram_usage = overhead + sizeof(Node)*(valid_count+op_count*2);
    printf("RAM usage:             %s\n", snprintf_bytes(str1, sizeof(str1), ram_usage));

    // Validate original nodes
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "original", valid_count, (void*)ll_tree, linkedlist_dump_list) != 0) {
            return 1;
        }
    }

    printf("--- Computing statistics ---\n");

    //// Process LinkedList Statistics ///////////////////////
    Stats stats;
    memset(&stats, 0, sizeof(Stats));
    Node *node = NULL;
    linklist_node_t *ll_node = ll_tree->head;
    long bytes = 0;

    stats.nodes = (uint64_t)valid_count;

    long sum = 0;
    long count = 0;

    // Get Minimum for LinkedList
    bytes = sizeof(Node) + sizeof(linklist_node_t); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        ll_node = ll_tree->head;
        node = (Node *)ll_node->item;
        if (node == NULL) {
            printf("Failed to get minimum value from LinkedList\n");
            return 1;
        } else if (node->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.min, (double)node->value, op_count, bytes, starttime);

    // Get Maximum for LinkedList
    bytes = sizeof(Node) + sizeof(linklist_node_t); // O(1)
    starttime = get_time_us();
    ll_node = ll_tree->tail;
    for (size_t i=0; i<op_count; i++) {
        ll_node = ll_tree->tail;
        node = (Node *)ll_node->item;
        if (node == NULL) {
            printf("Failed to get maximum value from LinkedList\n");
            return 1;
        } else if (node->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.max, (double)node->value, op_count, bytes, starttime);

    // Calculate Median for LinkedList
    bytes = ((double)(valid_count/2))*(sizeof(linklist_node_t)+sizeof(Node)); // O(n)
    count = 0;
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        node = linklist_at(ll_tree, valid_count/2)->item;
        if (node == NULL) {
            printf("Failed to get median value from LinkedList\n");
            return 1;
        } else if (node->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.median, (double)node->value, 1, bytes, starttime);


    // Calculate Mean for LinkedList
    bytes = ((double)valid_count)*(sizeof(linklist_node_t)+sizeof(Node)); // O(n)
    starttime = get_time_us();
    ll_node = ll_tree->head;
    while (ll_node) {
        Node *node = (Node *)ll_node->item;
        sum += node->value;
        count++;
        ll_node = ll_node->next;
    }
    record_event(&stats.mean, (double)sum / count, 1, bytes, starttime);

    // Compute standard deviation for LinkedList also O(n)
    //  NOTE: not resetting timer because you need to get mean to do stddev
    double sum_sq_diff = 0;
    ll_node = ll_tree->head;
    while (ll_node) {
        Node *node = (Node *)ll_node->item;
        double diff = node->value - stats.mean.value;
        sum_sq_diff += diff * diff;
        ll_node = ll_node->next;
    }
    record_event(&stats.stddev, sqrt(sum_sq_diff / count), 1, bytes*2, starttime);

    printf("Calculated basic statics for LinkedList\n");

    //// Benchmarking Search ///////////////////////
    printf("--- Search benchmarks ---\n");

    // LinkedList search
    bytes = ((double)valid_count/2)*(sizeof(linklist_node_t)+sizeof(Node)); // O(n)
    Phase ph;
    phase_begin(&ph, op_count);
    do {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (linklist_search(ll_tree, &delete_nodes[i]) == NULL) {
                printf("Failed to search value %ld into LinkedList tree\n", delete_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
    } while (phase_again(&ph));
    phase_record(&ph, &stats.search, bytes);

    printf("Completed Search benchmarks\n");

    //// Benchmarking Index ///////////////////////
    printf("--- Index benchmarks ---\n");

    // LinkedList index
    bytes = ((double)valid_count/2)*(sizeof(linklist_node_t)+sizeof(Node)); // O(n)
    phase_begin(&ph, op_count);
    do {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (linklist_at(ll_tree, indexes[i]) == NULL) {
                printf("Failed to search value %ld into LinkedList tree\n", indexes[i]);
                return 1;
            }
        }
        phase_stop(&ph);
    } while (phase_again(&ph));
    phase_record(&ph, &stats.index, bytes);

    printf("Completed Index benchmarks\n");

    //// Benchmarking Insert ///////////////////////
    printf("--- Insert benchmarks ---\n");

    // LinkedList insert
    bytes = ((double)valid_count/2)*(sizeof(linklist_node_t)+sizeof(Node));
    phase_begin(&ph, op_count);
    for (;;) {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (linklist_insert(ll_tree, &insert_nodes[i]) == NULL) {
                printf("Failed to insert value %ld into LinkedList tree\n", insert_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
        if (!phase_again(&ph)) break;
        for (uint64_t i = 0; i < op_count; i++) linklist_delete(ll_tree, &insert_nodes[i]);
    }
    phase_record(&ph, &stats.insert, bytes);

    printf("Completed Insert benchmarks\n");

    // Validate
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "insert", valid_count, (void*)ll_tree, linkedlist_dump_list) != 0) {
            return 1;
        }
    }

    //// Benchmarking delete ///////////////////////
    printf("--- Delete benchmarks ---\n");

    // LinkedList delete
    bytes = ((double)valid_count/2)*(sizeof(linklist_node_t)+sizeof(Node));
    phase_begin(&ph, op_count);
    for (;;) {
        phase_start(&ph);
        for (uint64_t i = 0; i < op_count; i++) {
            if (linklist_delete(ll_tree, &delete_nodes[i]) == NULL) {
                printf("Failed to delete value %ld from LinkedList tree\n", delete_nodes[i].value);
                return 1;
            }
        }
        phase_stop(&ph);
        if (!phase_again(&ph)) break;
        for (uint64_t i = 0; i < op_count; i++) linklist_insert(ll_tree, &delete_nodes[i]);
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
                if (linklist_insert(ll_tree, &g_clustered_nodes[i]) == NULL) {
                    printf("Failed to insert value %ld into LinkedList tree\n", g_clustered_nodes[i].value);
                    return 1;
                }
            }
            phase_stop(&ph);
            if (!phase_again(&ph)) break;
            for (uint64_t i = 0; i < op_count; i++) linklist_delete(ll_tree, &g_clustered_nodes[i]);
        }
        phase_record(&ph, &stats.clustered, bytes);
        printf("Completed Clustered insert benchmarks\n");
    }

    // Validate
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "delete", valid_count, (void*)ll_tree, linkedlist_dump_list) != 0) {
            return 1;
        }
    }

    // Print results
    printf("\n");
    print_results("LinkedList:", &stats);
    printf("\n");

    stats_write_json(&stats, "linkedlist");

    // Cleanup
    free(insert_nodes);
    free(delete_nodes);
    free(indexes);
    free(nodes);
    linklist_free_tree(ll_tree);

    return 0;
}
