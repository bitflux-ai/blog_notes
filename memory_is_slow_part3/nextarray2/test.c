#include "nextarray.h"
#include "benchmark_helpers.h"
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
int g_nofail = 0;
int g_total_errors = 0;

void handle_error(const char* message) {
    printf("ERROR: %s\n", message);
    g_total_errors++;
    if (!g_nofail) exit(1);
}

void print_usage(const char* program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Test NextArray data structure functionality\n\n");
    printf("Options:\n");
    printf("  -i, --input FILE         Path file with pregenerated nodes [REQUIRED]\n");
    printf("  -l, --list  FILE         Path file with sorted nodes for debug\n");
    printf("  -e, --extent-size SIZE   Number of nodes per extent (default: %d)\n", DEFAULT_EXTENT_SIZE);
    printf("  -o, --overprovisioning   Overprovisioning factor (default: %f)\n", DEFAULT_OVERPROVISIONING);
    printf("  -c, --count              How many nodes to use\n");
    printf("  -d, --debug LEVEL        Enable debug mode\n");
    printf("  -v, --verbose LEVEL      Enable verbose mode\n");
    printf("      --nofail             Continue testing after failures (default: fail on first error)\n");
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
        {"nofail",     no_argument,       0, 1000},
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
            case 1000: // --nofail
                g_nofail = 1;
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

// Test helper to dump nodes to file using _at()
int test_dump_using_at(void* context, const char* filename) {
    if (!context) return -1;
    nextarray_t* na = (nextarray_t*)context;
    
    printf("  Testing _at() access and dumping to %s...\n", filename);
    long starttime = get_time_us();
    
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        printf("  ERROR: Failed to open %s for writing\n", filename);
        return -1;
    }
    
    for (long i = 0; i < (long)na->total_count; i++) {
        Node* node = (Node*)nextarray_at(na, i);
        if (!node) {
            printf("  ERROR: nextarray_at(%ld) returned NULL\n", i);
            fclose(fp);
            return -1;
        }
        fwrite(node, sizeof(Node), 1, fp);
    }
    fclose(fp);
    
    report_elapsed_time("  _at() dump", starttime);
    return 0;
}

// Test helper to dump nodes to file using _next() iterator
int test_dump_using_next(void* context, const char* filename) {
    if (!context) return -1;
    nextarray_t* na = (nextarray_t*)context;
    
    printf("  Testing _next() iterator and dumping to %s...\n", filename);
    long starttime = get_time_us();
    
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        printf("  ERROR: Failed to open %s for writing\n", filename);
        return -1;
    }
    
    nextarray_iter_t iter = {0};
    Node* node;
    long count = 0;
    
    while ((node = (Node*)nextarray_next(na, &iter)) != NULL) {
        fwrite(node, sizeof(Node), 1, fp);
        count++;
    }
    fclose(fp);
    
    if (count != (long)na->total_count) {
        printf("  ERROR: Iterator returned %ld nodes, expected %ld\n", count, (long)na->total_count);
        return -1;
    }
    
    report_elapsed_time("  _next() dump", starttime);
    return 0;
}

// Comprehensive test function for nextarray functionality
void test_nextarray_functionality(nextarray_t* na, Node* test_nodes, size_t count, const char* test_name) {
    long starttime;
    char str1[100];
    
    printf("Testing %s (count=%zu):\n", test_name, count);
    
    // Test search functionality
    printf("  _search(): ");
    starttime = get_time_us();
    for (size_t i = 0; i < count; i++) {
        Node* found = (Node*)nextarray_search(na, &test_nodes[i]);
        if (found == NULL) {
            snprintf(str1, sizeof(str1), "Search failed to find node with value %ld i=%zu", test_nodes[i].value, i);
            nextarray_print_stats(na);
            handle_error(str1);
        } else if (found->value != test_nodes[i].value) {
            snprintf(str1, sizeof(str1), "Search found value %ld expected %ld i=%zu", found->value, test_nodes[i].value, i);
            handle_error(str1);
        }
    }
    report_elapsed_time(" ", starttime);
    
    // Test _at() functionality
    printf("  _at(): ");
    starttime = get_time_us();
    if (validate_nodes_by_file(g_inputfile, "original", count, (void*)na, test_dump_using_at) == 0) {
        printf("PASS");
        report_elapsed_time(" ", starttime);
    } else {
        printf("FAIL (validation failed)\n");
        handle_error("_at() functionality failed");
    }
    
    // Test _next() functionality  
    printf("  _next(): ");
    starttime = get_time_us();
    if (validate_nodes_by_file(g_inputfile, "original", count, (void*)na, test_dump_using_next) == 0) {
        printf("PASS");
        report_elapsed_time(" ", starttime);
    } else {
        printf("FAIL (validation failed)\n");
        handle_error("_next() functionality failed");
    }
    
    // Test delete functionality
    printf("  _delete(): ");
    starttime = get_time_us();
    int delete_errors = 0;
    size_t original_count = na->total_count;
    
    for (size_t i = 0; i < count && delete_errors < 10; i++) {
        if (nextarray_delete(na, &test_nodes[i]) != 0) {
            delete_errors++;
        }
    }
    
    if (delete_errors == 0 && na->total_count == 0) {
        printf("PASS (deleted %zu nodes)", original_count);
        report_elapsed_time(" ", starttime);
    } else {
        printf("FAIL (%d delete errors, final count=%zu)\n", delete_errors, na->total_count);
        handle_error("_delete() functionality failed");
    }
    
    printf("\n");
}

// Dump function for validation 
int nextarray_dump_list(void *context, const char *filename) {
    if (!context) return -1;
    nextarray_t *nextarray = (nextarray_t *)context;
    return nextarray_dump_nodes_to_file(nextarray, filename);
}

int main(int argc, char* argv[]) {
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
    long starttime;

    printf("=== NextArray Functionality Test ===\n\n");

    // Determine actual count
    if (valid_count == 0) {
        if (get_file_size(g_listfile, &valid_count) != 0) {
            fprintf(stderr, "Error: failed to get file size of %s\n", g_listfile);
            return -1;
        }
    }

    // Load test data
    printf("Loading test data...\n");
    starttime = get_time_us();
    Node* node_buffer = malloc(valid_count * sizeof(Node));
    if (load_nodes(g_inputfile, g_listfile, &node_buffer, &insert_nodes, &delete_nodes, &indexes, &valid_count, &op_count) != 0) {
        handle_error("Failed to load nodes");
    }
    printf("Loaded %zu nodes for testing", valid_count);
    report_elapsed_time(" ", starttime);
    printf("\n");

    // Test 1: bulk_init() without balance()
    printf("=== Test 1: bulk_init() without balance() ===\n");
    nextarray_t* na = nextarray_alloc(valid_count * g_overprovisioning, g_extent_size, sizeof(Node), compare_nodes);
    if (na) {
        memcpy(na->buffer, node_buffer, valid_count * sizeof(Node));
        nextarray_bulk_init(na, valid_count, true);
        test_nextarray_functionality(na, node_buffer, valid_count, "bulk_init() no balance");
        nextarray_free(na);
    } else {
        handle_error("Failed to create NextArray");
    }

    // Test 2: bulk_init() with balance()  
    printf("=== Test 2: bulk_init() with balance() ===\n");
    na = nextarray_alloc(valid_count * g_overprovisioning, g_extent_size, sizeof(Node), compare_nodes);
    if (na) {
        memcpy(na->buffer, node_buffer, valid_count * sizeof(Node));
        nextarray_bulk_init(na, valid_count, true);
        if (nextarray_balance(na) != 0) {
            handle_error("balance() failed");
        } else {
            test_nextarray_functionality(na, node_buffer, valid_count, "bulk_init() with balance");
        }
        nextarray_free(na);
    } else {
        handle_error("Failed to create NextArray");
    }

    // Test 3: _insert() without balance()
    printf("=== Test 3: _insert() without balance() ===\n");
    na = nextarray_alloc(valid_count * g_overprovisioning, g_extent_size, sizeof(Node), compare_nodes);
    if (na) {
        int insert_errors = 0;
        printf("Inserting %zu nodes individually...\n", valid_count);
        starttime = get_time_us();
        for (size_t i = 0; i < valid_count && insert_errors < 10; i++) {
            int result = nextarray_insert(na, &node_buffer[i]);
            if (result < 0) {
                insert_errors++;
                if (insert_errors <= 3) {
                    printf("  Insert error %d at node %zu (value=%ld)\n", insert_errors, i, node_buffer[i].value);
                }
            }
        }
        printf("Individual insertions completed (errors=%d)", insert_errors);
        report_elapsed_time(" ", starttime);
        
        if (insert_errors < 10) {
            test_nextarray_functionality(na, node_buffer, valid_count, "_insert() no balance");
        } else {
            printf("Too many insert errors, skipping functionality tests\n\n");
            handle_error("Too many insert errors");
        }
        nextarray_free(na);
    } else {
        handle_error("Failed to create NextArray");
    }
    
    // Test 4: _insert() with balance()
    printf("=== Test 4: _insert() with balance() ===\n");
    na = nextarray_alloc(valid_count * g_overprovisioning, g_extent_size, sizeof(Node), compare_nodes);
    if (na) {
        int insert_errors = 0;
        printf("Inserting %zu nodes individually...\n", valid_count);
        starttime = get_time_us();
        for (size_t i = 0; i < valid_count && insert_errors < 10; i++) {
            int result = nextarray_insert(na, &node_buffer[i]);
            if (result < 0) {
                insert_errors++;
                if (insert_errors <= 3) {
                    printf("  Insert error %d at node %zu (value=%ld)\n", insert_errors, i, node_buffer[i].value);
                }
            }
        }
        printf("Individual insertions completed (errors=%d)", insert_errors);
        report_elapsed_time(" ", starttime);
        
        if (insert_errors < 10 && nextarray_balance(na) == 0) {
            test_nextarray_functionality(na, node_buffer, valid_count, "_insert() with balance");
        } else {
            if (insert_errors >= 10) {
                printf("Too many insert errors, skipping functionality tests\n\n");
                handle_error("Too many insert errors");
            } else {
                printf("Balance failed, skipping functionality tests\n\n");
                handle_error("Balance failed");
            }
        }
        nextarray_free(na);
    } else {
        handle_error("Failed to create NextArray");
    }

    // Cleanup
    free(node_buffer);
    free(insert_nodes);
    free(delete_nodes);
    free(indexes);

    printf("=== FINAL RESULTS ===\n");
    printf("Total errors: %d\n", g_total_errors);
    printf("Status: %s\n", g_total_errors == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    
    return g_total_errors > 0 ? 1 : 0;
}
