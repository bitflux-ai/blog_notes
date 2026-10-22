#include "diskarray.h"
#include "benchmark_helpers.h"
#include "targets.h"
#include <math.h>
#include <getopt.h>

// Default values
#define DEFAULT_EXTENT_SIZE 512
#define DEFAULT_OVERPROVISIONING 2.0
#define DEFAULT_NUM_IO_BUFFERS 100
#define DEFAULT_ALLOWED_DIRTY_PERCENT 50
#define DEFAULT_WORKERS 4

// Global configuration
char* g_inputfile = NULL;
char* g_listfile = NULL;
char* g_diskfile = "test_diskarray.dat";
uint64_t g_extent_size = DEFAULT_EXTENT_SIZE;
double g_overprovisioning = DEFAULT_OVERPROVISIONING;
uint64_t g_count = 0;
int g_num_io_buffers = DEFAULT_NUM_IO_BUFFERS;
int g_allowed_dirty_percent = DEFAULT_ALLOWED_DIRTY_PERCENT;
int g_workers = DEFAULT_WORKERS;
int debug = 0;
int verbose = 0;
int g_nofail = 0;
int g_total_errors = 0;

void handle_error(const char* message) {
    printf("ERROR: %s\n", message);
    g_total_errors++;
    if (!g_nofail) exit(1);
}


// Resolve -t targets.yml once; the striped allocator wants every drive's path.
static char* g_targets = NULL;
static const char** g_target_paths = NULL;
static int g_num_targets = 0;

static int resolve_targets(void) {
    if (!g_targets || g_target_paths) return 0;
    target_drive_t* drives = NULL;
    int n = targets_read(g_targets, &drives);
    if (n < 1) { fprintf(stderr, "no drives found in %s\n", g_targets); return -1; }
    if (targets_resolve(drives, n) != 0) return -1;
    g_target_paths = malloc((size_t)n * sizeof(char*));
    g_num_targets = 0;
    printf("Targets from %s:\n", g_targets);
    for (int i = 0; i < n; i++) {
        g_target_paths[g_num_targets++] = strdup(drives[i].path);
        printf("  [%d] %s  %s\n", g_num_targets - 1, drives[i].path, drives[i].nqn);
    }
    free(drives);
    return g_num_targets ? 0 : -1;
}

// Open the diskarray on -t targets (striped) or -f diskfile, then pin the
// consumer to cpu 0 and each worker to its own core.
static diskarray_t* open_diskarray(size_t max_nodes, int extent_size, int node_size, int num_io_buffers,
                                   int allowed_dirty_percent, int workers, int (*compare)(const void*, const void*)) {
    if (resolve_targets() != 0) return NULL;
    diskarray_t* na = g_num_targets
        ? diskarray_alloc_striped(g_target_paths, g_num_targets, max_nodes, extent_size, node_size,
                                  num_io_buffers, allowed_dirty_percent, workers, compare)
        : diskarray_alloc(g_diskfile, max_nodes, extent_size, node_size, num_io_buffers,
                          allowed_dirty_percent, workers, compare);
    if (!na) return NULL;
    pthread_t* tids = malloc((size_t)na->num_worker_threads * sizeof(pthread_t));
    for (int i = 0; i < na->num_worker_threads; i++) tids[i] = diskarray_worker_thread(na, i);
    pin_main_and_workers(tids, na->num_worker_threads);
    free(tids);
    return na;
}

void print_usage(const char* program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Test DiskArray data structure functionality\n\n");
    printf("Options:\n");
    printf("  -i, --input FILE         Path file with pregenerated nodes [REQUIRED]\n");
    printf("  -l, --list  FILE         Path file with sorted nodes for debug\n");
    printf("  -f, --diskfile FILE      Disk file or block device for data storage (default: %s)\n", g_diskfile);
    printf("  -t, --targets FILE       targets.yml: stripe over every NVMe-oF drive in it (overrides -f)\n");
    printf("  -e, --extent-size SIZE   Number of nodes per extent (default: %d)\n", DEFAULT_EXTENT_SIZE);
    printf("  -o, --overprovisioning   Overprovisioning factor (default: %f)\n", DEFAULT_OVERPROVISIONING);
    printf("  -c, --count              How many nodes to use\n");
    printf("      --io-buffers NUM     Number of I/O buffers (default: %d)\n", DEFAULT_NUM_IO_BUFFERS);
    printf("  -w, --workers NUM        Number of worker threads (default: %d)\n", DEFAULT_WORKERS);
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
        {"diskfile", required_argument, 0, 'f'},
        {"targets",  required_argument, 0, 't'},
        {"extent-size", required_argument, 0, 'e'},
        {"overprovisioning", required_argument, 0, 'o'},
        {"count",   required_argument, 0, 'c'},
        {"io-buffers", required_argument, 0, 1001},
        {"workers", required_argument, 0, 'w'},
        {"debug",      required_argument, 0, 'd'},
        {"verbose",    required_argument, 0, 'v'},
        {"nofail",     no_argument,       0, 1000},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "i:l:f:e:o:c:w:d:v:ht:", long_options, NULL)) != -1) {
        switch (c) {
            case 'i':
                g_inputfile = optarg;
                break;
            case 'l':
                g_listfile = optarg;
                break;
            case 'f':
                g_diskfile = optarg;
                break;
            case 't':
                g_targets = optarg;
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
            case 'w':
                g_workers = atoi(optarg);
                if (g_workers <= 0) {
                    fprintf(stderr, "Error: number of workers must be positive\n");
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
            case 1001: // --io-buffers
                g_num_io_buffers = atoi(optarg);
                if (g_num_io_buffers <= 0) {
                    fprintf(stderr, "Error: io-buffers must be positive\n");
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

// Test helper to dump nodes to file using _at()
int test_dump_using_at(void* context, const char* filename) {
    if (!context) return -1;
    diskarray_t* na = (diskarray_t*)context;
    
    printf("  Testing _at() access and dumping to %s...\n", filename);
    long starttime = get_time_us();
    
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        printf("  ERROR: Failed to open %s for writing\n", filename);
        return -1;
    }
    
    for (long i = 0; i < (long)na->total_count; i++) {
        Node node;
        if (diskarray_at(na, i, &node) != 0) {
            printf("  ERROR: diskarray_at(%ld) failed\n", i);
            fclose(fp);
            return -1;
        }
        fwrite(&node, sizeof(Node), 1, fp);
    }
    fclose(fp);
    
    report_elapsed_time("  _at() dump", starttime);
    return 0;
}

// Test helper to dump nodes to file using _next() iterator
int test_dump_using_next(void* context, const char* filename) {
    if (!context) return -1;
    diskarray_t* na = (diskarray_t*)context;
    
    printf("  Testing _next() iterator and dumping to %s...\n", filename);
    long starttime = get_time_us();
    
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        printf("  ERROR: Failed to open %s for writing\n", filename);
        return -1;
    }
    
    // Use prefetching iterator for better performance
    diskarray_iter_t* iter = diskarray_iter_create(na, 2);
    if (!iter) {
        printf("  ERROR: Failed to create iterator\n");
        fclose(fp);
        return -1;
    }
    
    Node node;
    long count = 0;
    
    while (diskarray_next(na, iter, &node) == 0) {
        fwrite(&node, sizeof(Node), 1, fp);
        count++;
    }
    
    diskarray_iter_free(na, iter);
    fclose(fp);
    
    if (count != (long)na->total_count) {
        printf("  ERROR: Iterator returned %ld nodes, expected %ld\n", count, (long)na->total_count);
        return -1;
    }
    
    report_elapsed_time("  _next() dump", starttime);
    return 0;
}

// Comprehensive test function for diskarray functionality
void test_diskarray_functionality(diskarray_t* na, Node* test_nodes, size_t count, const char* test_name) {
    long starttime;
    char str1[100];

    printf("Testing %s (count=%zu):\n", test_name, count);

    // Test search functionality
    printf("  _search(): ");
    starttime = get_time_us();
    int search_errors = 0;
    for (size_t i = 0; i < count; i++) {
        Node found;
        int result = diskarray_search(na, &test_nodes[i], &found);
        if (result != 0) {
            snprintf(str1, sizeof(str1), "Search failed to find node with value %ld i=%zu", test_nodes[i].value, i);
            diskarray_debug_stats(na);
            handle_error(str1);
            search_errors++;
        } else if (found.value != test_nodes[i].value) {
            snprintf(str1, sizeof(str1), "Search found value %ld expected %ld i=%zu", found.value, test_nodes[i].value, i);
            handle_error(str1);
            search_errors++;
        }
    }
    if (search_errors == 0) {
        printf("PASS");
    } else {
        printf("FAIL (%d search errors)", search_errors);
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
    size_t original_count = na->total_count;
    int delete_errors = 0;

    for (size_t i = 0; i < count && delete_errors < 10; i++) {
        int result = diskarray_delete(na, &test_nodes[i]);
        if (result != 0) {
            snprintf(str1, sizeof(str1), "Delete failed for node %zu (value=%lu) with error %d\n", 
                     i, test_nodes[i].value, result);
            handle_error(str1);
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
int diskarray_dump_list(void *context, const char *filename) {
    if (!context) return -1;
    diskarray_t *diskarray = (diskarray_t *)context;
    return diskarray_dump_nodes_to_file(diskarray, filename);
}

int main(int argc, char* argv[]) {
    char str1[64], str2[64], str3[64];
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

    printf("=== DiskArray Functionality Test ===\n\n");

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

    printf("=== Test 1: bulk_init() without balance() ===\n");
    diskarray_t* na = open_diskarray(valid_count * g_overprovisioning, g_extent_size, sizeof(Node), 
                                     g_num_io_buffers, g_allowed_dirty_percent, g_workers, compare_nodes);
    snprintf_count(str1, sizeof(str1), na->num_extents);
    snprintf_bytes(str2, sizeof(str2), na->extent_size * sizeof(Node));
    snprintf_bytes(str3, sizeof(str3), na->num_io_buffers * na->extent_disk_size);
    printf("DiskArray:\n  extents %s of %s total I/O buffer size %s (buffers: %d)\n", 
           str1, str2, str3, na->num_io_buffers);
    snprintf_count(str1, sizeof(str1), na->total_tree_nodes);
    snprintf_bytes(str2, sizeof(str2), LOOKUP_NODE_SIZE*sizeof(*na->lookup_tree));
    printf("  lookup height: %d lt_nodes: %s size %s\n", na->tree_height, str1, str2);

    if (na) {
        diskarray_bulk_init(na, node_buffer, valid_count, true);
        test_diskarray_functionality(na, node_buffer, valid_count, "bulk_init() no balance");
        diskarray_free(na);
    } else {
        handle_error("Failed to create diskarray");
    }

    // Test 2: bulk_init() with balance()  
    printf("=== Test 2: bulk_init() with balance() ===\n");
    na = open_diskarray(valid_count * g_overprovisioning, g_extent_size, sizeof(Node), 
                        g_num_io_buffers, g_allowed_dirty_percent, g_workers, compare_nodes);
    if (na) {
        diskarray_bulk_init(na, node_buffer, valid_count, true);
        if (diskarray_balance(na) != 0) {
            handle_error("balance() failed");
        } else {
            test_diskarray_functionality(na, node_buffer, valid_count, "bulk_init() with balance");
        }
        diskarray_free(na);
    } else {
        handle_error("Failed to create diskarray");
    }

    // Test 3: _insert() without balance()
    printf("=== Test 3: _insert() without balance() ===\n");
    na = open_diskarray(valid_count * g_overprovisioning, g_extent_size, sizeof(Node), 
                        g_num_io_buffers, g_allowed_dirty_percent, g_workers, compare_nodes);
    if (na) {
        int insert_errors = 0;
        printf("Inserting %zu nodes individually...\n", valid_count);
        starttime = get_time_us();
        for (size_t i = 0; i < valid_count && insert_errors < 10; i++) {
            int result = diskarray_insert(na, &node_buffer[i]);
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
            test_diskarray_functionality(na, node_buffer, valid_count, "_insert() no balance");
        } else {
            printf("Too many insert errors, skipping functionality tests\n\n");
            handle_error("Too many insert errors");
        }
        diskarray_free(na);
    } else {
        handle_error("Failed to create diskarray");
    }
    
    // Test 4: _insert() with balance()
    printf("=== Test 4: _insert() with balance() ===\n");
    na = open_diskarray(valid_count * g_overprovisioning, g_extent_size, sizeof(Node), 
                        g_num_io_buffers, g_allowed_dirty_percent, g_workers, compare_nodes);
    if (na) {
        int insert_errors = 0;
        printf("Inserting %zu nodes individually...\n", valid_count);
        starttime = get_time_us();
        for (size_t i = 0; i < valid_count && insert_errors < 10; i++) {
            int result = diskarray_insert(na, &node_buffer[i]);
            if (result < 0) {
                insert_errors++;
                if (insert_errors <= 3) {
                    printf("  Insert error %d at node %zu (value=%ld)\n", insert_errors, i, node_buffer[i].value);
                }
            }
        }
        printf("Individual insertions completed (errors=%d)", insert_errors);
        report_elapsed_time(" ", starttime);
        
        if (insert_errors < 10 && diskarray_balance(na) == 0) {
            test_diskarray_functionality(na, node_buffer, valid_count, "_insert() with balance");
        } else {
            if (insert_errors >= 10) {
                printf("Too many insert errors, skipping functionality tests\n\n");
                handle_error("Too many insert errors");
            } else {
                printf("Balance failed, skipping functionality tests\n\n");
                handle_error("Balance failed");
            }
        }
        diskarray_free(na);
    } else {
        handle_error("Failed to create DiskArray");
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
