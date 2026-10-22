#include "diskarray.h"
#include "benchmark_helpers.h"
#include "targets.h"
#include <math.h>
#include <getopt.h>

// Default values
#define DEFAULT_EXTENT_SIZE 512
#define DEFAULT_OVERPROVISIONING 2.0
#define DEFAULT_NUM_IO_BUFFERS 1000
#define DEFAULT_ALLOWED_DIRTY_PERCENT 50
#define DEFAULT_WORKERS 4

// Global configuration
char* g_inputfile = NULL;
char* g_listfile = NULL;
char* g_diskfile = "diskarray.dat";
uint64_t g_extent_size = DEFAULT_EXTENT_SIZE;
double g_overprovisioning = DEFAULT_OVERPROVISIONING;
uint64_t g_count = 0;
int g_num_io_buffers = DEFAULT_NUM_IO_BUFFERS;
int g_allowed_dirty_percent = DEFAULT_ALLOWED_DIRTY_PERCENT;
int g_workers = DEFAULT_WORKERS;
int debug = 0;
int verbose = 0;


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
    printf("Benchmark diskarray data structures\n\n");
    printf("Options:\n");
    printf("  -i, --input FILE         Path file with pregenerated nodes [REQUIRED]\n");
    printf("  -l, --list  FILE         Path file with sorted nodes for debug\n");
    printf("  -f, --diskfile FILE      Disk file for data storage (default: %s)\n", g_diskfile);
    printf("  -t, --targets FILE       targets.yml: stripe over every NVMe-oF drive in it (overrides -f)\n");
    printf("  -e, --extent-size SIZE   Number of nodes per extent (default: %d)\n", DEFAULT_EXTENT_SIZE);
    printf("  -o, --overprovisioning   Overprovisioning factor (default: %f)\n", DEFAULT_OVERPROVISIONING);
    printf("  -c, --count              How many nodes to use\n");
    printf("  -b, --io-buffers NUM     Number of I/O buffers (default: %d)\n", DEFAULT_NUM_IO_BUFFERS);
    printf("  -p, --dirty-percent NUM  Max dirty buffer percentage (default: %d)\n", DEFAULT_ALLOWED_DIRTY_PERCENT);
    printf("  -w, --workers NUM        Number of worker threads (default: %d)\n", DEFAULT_WORKERS);
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
        {"diskfile", required_argument, 0, 'f'},
        {"targets",  required_argument, 0, 't'},
        {"extent-size", required_argument, 0, 'e'},
        {"overprovisioning", required_argument, 0, 'o'},
        {"count",   required_argument, 0, 'c'},
        {"io-buffers", required_argument, 0, 'b'},
        {"dirty-percent", required_argument, 0, 'p'},
        {"workers", required_argument, 0, 'w'},
        {"debug",      required_argument, 0, 'd'},
        {"verbose",    required_argument, 0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "i:l:f:e:o:c:b:p:w:d:v:ht:", long_options, NULL)) != -1) {
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
            case 'b':
                g_num_io_buffers = atoi(optarg);
                if (g_num_io_buffers <= 0) {
                    fprintf(stderr, "Error: number of I/O buffers must be positive\n");
                    return -1;
                }
                break;
            case 'p':
                g_allowed_dirty_percent = atoi(optarg);
                if (g_allowed_dirty_percent < 0 || g_allowed_dirty_percent > 100) {
                    fprintf(stderr, "Error: dirty percentage must be between 0 and 100\n");
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

int diskarray_dump_list(void *context, const char *filename) {
    if (!context) return -1;
    diskarray_t *diskarray = (diskarray_t *)context;
    return diskarray_dump_nodes_to_file(diskarray, filename);
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

    // Initialize DiskArray
    if (valid_count == 0) {
        if (get_file_size(g_listfile, &valid_count) != 0) {
            fprintf(stderr, "Error: failed to get file size of %s\n", g_listfile);
            return -1;
        }
        valid_count /= sizeof(Node);
    }

    max_nodes = (size_t)(valid_count*g_overprovisioning);

    diskarray_t* diskarray = open_diskarray(max_nodes, g_extent_size, sizeof(Node), 
                                            g_num_io_buffers, g_allowed_dirty_percent, g_workers, compare_nodes);
    if (!diskarray) {
        printf("Failed to create diskarray\n");
        return 1;
    }
    snprintf_count(str1, sizeof(str1), diskarray->num_extents);
    snprintf_bytes(str2, sizeof(str2), diskarray->extent_size * sizeof(Node));
    snprintf_bytes(str3, sizeof(str3), diskarray->num_io_buffers * diskarray->extent_disk_size);
    printf("DiskArray:\n  extents %s of %s total I/O buffer size %s (buffers: %d)\n", 
           str1, str2, str3, diskarray->num_io_buffers);
    snprintf_count(str1, sizeof(str1), diskarray->total_tree_nodes);
    snprintf_bytes(str2, sizeof(str2), LOOKUP_NODE_SIZE*sizeof(*diskarray->lookup_tree));
    printf("  lookup height: %d lt_nodes: %s size %s\n", diskarray->tree_height, str1, str2);

    // Load nodes into temporary buffer
    Node* temp_buffer = malloc(valid_count * sizeof(Node));
    long starttime = get_time_us();
    if (load_nodes(g_inputfile, g_listfile, &temp_buffer, &insert_nodes, &delete_nodes, &indexes, &valid_count, &op_count) != 0) {
        printf("Failed to create nodes\n");
        return 1;
    }
    report_elapsed_time("Loaded nodes", starttime);

    // Initialize diskarray from temporary buffer
    starttime = get_time_us();
    diskarray_bulk_init(diskarray, temp_buffer, valid_count, g_nodes_sorted);
    report_elapsed_time("  diskarray_bulk_init", starttime);

    // Free temporary buffer since data is now on disk
    free(temp_buffer);

    starttime = get_time_us();
    if (diskarray_balance(diskarray) != 0) {
        printf("Failed to balance\n");
        return 1;
    }
    report_elapsed_time("  diskarray_balance", starttime);
    uint64_t overhead = diskarray->overhead;
    printf("DiskArray overhead:    %s\n", snprintf_bytes(str1, sizeof(str1), overhead));
    uint64_t ram_usage = overhead;
    printf("RAM usage:             %s\n", snprintf_bytes(str1, sizeof(str1), ram_usage));

    // Validate original nodes
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "original", valid_count, (void*)diskarray, diskarray_dump_list) != 0) {
            return 1;
        }
    }

    printf("--- Computing statistics ---\n");

    //// Process DiskArray Statistics ///////////////////////
    Stats stats;
    memset(&stats, 0, sizeof(Stats));
    Node node;
    long bytes = 0;

    long find_extent = log2((double)(valid_count/g_extent_size))*sizeof(Node)*2; // O(log n) but of the extent range
    long search_extent = log2((double)g_extent_size/g_overprovisioning)*sizeof(Node); // O(log n) but of the extent array itself
    long avg_extent_cpy = (g_extent_size/g_overprovisioning)/2*sizeof(Node); // move 1/2 the valid nodes in extent on average

    long sum = 0;
    long count = 0;

    // Get Minimum for diskarray
    bytes = sizeof(void*)*3+sizeof(Node); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        if (diskarray_at(diskarray, 0, &node) != 0) {
            printf("Failed to get minimum value from diskarray\n");
            return 1;
        } else if (node.value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.min, (double)node.value, op_count, bytes, starttime);

    // Get Maximum for diskarray
    bytes = sizeof(void*)*3+sizeof(Node); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        if (diskarray_at(diskarray, valid_count-1, &node) != 0) {
            printf("Failed to get maximum value from diskarray\n");
            return 1;
        } else if (node.value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.max, (double)node.value, op_count, bytes, starttime);

    // Get Median for diskarray
    bytes = find_extent + sizeof(Node); // O(log n/e)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        if (diskarray_at(diskarray, valid_count/2, &node) != 0) {
            printf("Failed to get median value from diskarray\n");
            return 1;
        }
    }
    record_event(&stats.median, (double)node.value, op_count, bytes, starttime);

    // Calculate Mean for diskarray
    bytes = valid_count*sizeof(Node); // O(n)
    starttime = get_time_us();
    sum = 0;
    count = 0;
    diskarray_iter_t* iter = diskarray_iter_create(diskarray, 0);
    if (!iter) {
        printf("Failed to create iterator for mean calculation\n");
        return 1;
    }
    while (diskarray_next(diskarray, iter, &node) == 0) {
        sum += node.value;
        count++;
    }
    diskarray_iter_free(diskarray, iter);
    record_event(&stats.mean, (double)sum / count, 1, bytes, starttime);

    // Calculate standard deviation for diskarray also O(n)
    //  NOTE: not resetting timer because you need to get mean to do stddev
    double sum_sq_diff = 0;
    iter = diskarray_iter_create(diskarray, 0);
    if (!iter) {
        printf("Failed to create iterator for stddev calculation\n");
        return 1;
    }
    while (diskarray_next(diskarray, iter, &node) == 0) {
        double diff = node.value - stats.mean.value;
        sum_sq_diff += diff * diff;
    }
    diskarray_iter_free(diskarray, iter);
    record_event(&stats.stddev, sqrt(sum_sq_diff / count), 1, bytes*2, starttime);

    printf("Calculated basic statics for DiskArray\n");

    //// Benchmarking Search ///////////////////////
    printf("--- Search benchmarks ---\n");

    // diskarray search
    bytes = find_extent + search_extent;
    starttime = get_time_us();
    for (uint64_t i = 0; i < op_count; i++) {
        if (diskarray_search(diskarray, &delete_nodes[i], &node) != 0) {
            printf("Failed to search value %ld in diskarray\n", delete_nodes[i].value);
            diskarray_debug_stats(diskarray);
            return 1;
        }
    }
    record_event(&stats.search, 0, op_count, bytes, starttime);

    printf("Completed Search benchmarks\n");

    //// Benchmarking Index ///////////////////////
    printf("--- Index benchmarks ---\n");

    // diskarray index
    bytes = find_extent + search_extent;
    starttime = get_time_us();
    for (uint64_t i = 0; i < op_count; i++) {
        if (diskarray_at(diskarray, indexes[i], &node) != 0) {
            printf("Failed to get index %zu from diskarray\n", indexes[i]);
            return 1;
        }
    }
    record_event(&stats.index, 0, op_count, bytes, starttime);

    printf("Completed Index benchmarks\n");


    //// Benchmarking Insert ///////////////////////
    printf("--- Insert benchmarks ---\n");

    // DiskArray insert
    bytes = find_extent + search_extent + avg_extent_cpy;
    starttime = get_time_us();
    for (uint64_t i = 0; i < op_count; i++) {
        if (diskarray_insert(diskarray, &insert_nodes[i]) == 0) {
            printf("Failed to insert value %ld into DiskArray tree\n", insert_nodes[i].value);
            return 1;
        }
    }
    record_event(&stats.insert, 0, op_count, bytes, starttime);

    printf("Completed Insert benchmarks\n");

    // Validate
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "insert", valid_count, (void*)diskarray, diskarray_dump_list) != 0) {
            return 1;
        }
    }

    //// Benchmarking delete ///////////////////////
    printf("--- Delete benchmarks ---\n");

    // DiskArray delete
    bytes = find_extent + search_extent + avg_extent_cpy;
    starttime = get_time_us();
    for (uint64_t i = 0; i < op_count; i++) {
        if (diskarray_delete(diskarray, &delete_nodes[i]) != 0) {
            printf("Failed to delete value %ld from DiskArray tree\n", delete_nodes[i].value);
            return 1;
        }
    }
    record_event(&stats.delete, 0, op_count, bytes, starttime);

    printf("Completed Delete benchmarks\n");

    // Validate
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "delete", valid_count, (void*)diskarray, diskarray_dump_list) != 0) {
            return 1;
        }
    }

    // Print results
    printf("\n");
    print_results("DiskArray:", &stats);
    printf("\n");

    stats_write_json(&stats, "diskarray_sync");

    // Cleanup
    free(insert_nodes);
    free(delete_nodes);
    free(indexes);
    diskarray_free(diskarray);

    return 0;
}
