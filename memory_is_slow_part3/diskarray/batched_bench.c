#include "diskarray.h"
#define IOURING_SUPPORT
#include "benchmark_helpers.h"
#include "gen_nodes.h"
#include "targets.h"
#include <math.h>
#include <getopt.h>

// Default values
// Defaults are the best point of the 2026-10-01 sweep at 1G nodes over 7
// NVMe-oF drives (see ../diskarrayarch.md, "Tuning"): batch about a third of
// the buffer pool, four workers per drive, 4 KiB extents.
#define DEFAULT_EXTENT_SIZE 512 // for 8B nodes this is 4K
#define DEFAULT_OVERPROVISIONING 1.5
#define DEFAULT_NUM_IO_BUFFERS 6144
#define DEFAULT_ALLOWED_DIRTY_PERCENT 50
#define DEFAULT_BATCH_SIZE 2048
#define DEFAULT_WORKERS 28

// Global configuration
char* g_inputfile = NULL;
char* g_listfile = NULL;
char* g_diskfile = "diskarray.dat";
uint64_t g_extent_size = DEFAULT_EXTENT_SIZE;
double g_overprovisioning = DEFAULT_OVERPROVISIONING;
uint64_t g_count = 0;
int g_num_io_buffers = DEFAULT_NUM_IO_BUFFERS;
int g_allowed_dirty_percent = DEFAULT_ALLOWED_DIRTY_PERCENT;
int g_batch = DEFAULT_BATCH_SIZE;
int g_warm_ahead = 0;   // warm the buffer of the command this far ahead (0: off; measured: no help, DDIO already puts the read in L3)
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

// Actual drive traffic from /proc/diskstats for the devices in use, so each
// phase can report real read/write bandwidth rather than the modelled bytes.
typedef struct { double rd_bytes, wr_bytes; } diskstats_t;

static diskstats_t diskstats_now(void) {
    diskstats_t t = {0, 0};
    FILE* f = fopen("/proc/diskstats", "r");
    if (!f) return t;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned maj, min; char name[64]; unsigned long long rd_ios, rd_merges, rd_sectors, rd_ticks, wr_ios, wr_merges, wr_sectors;
        if (sscanf(line, "%u %u %63s %llu %llu %llu %llu %llu %llu %llu", &maj, &min, name, &rd_ios, &rd_merges, &rd_sectors, &rd_ticks, &wr_ios, &wr_merges, &wr_sectors) != 10) continue;
        bool mine = false;
        for (int i = 0; i < g_num_targets; i++)
            if (strncmp(g_target_paths[i], "/dev/", 5) == 0 && strcmp(g_target_paths[i] + 5, name) == 0) mine = true;
        if (!g_num_targets && g_diskfile && strncmp(g_diskfile, "/dev/", 5) == 0 && strcmp(g_diskfile + 5, name) == 0) mine = true;
        if (!mine) continue;
        t.rd_bytes += (double)rd_sectors * 512.0;
        t.wr_bytes += (double)wr_sectors * 512.0;
    }
    fclose(f);
    return t;
}

static void report_drive_bw(const char* phase, double rd_bytes, double wr_bytes, double s) {
    double rd = rd_bytes / 1073741824.0, wr = wr_bytes / 1073741824.0;
    printf("  drives during %s: read %.2f GiB (%.2f GiB/s)  write %.2f GiB (%.2f GiB/s)  total %.2f GiB/s\n",
           phase, rd, rd / s, wr, wr / s, (rd + wr) / s);
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
    printf("  --batch-size NUM         Batch size for operations (default: %d)\n", DEFAULT_BATCH_SIZE);
    printf("  -W, --warm-ahead N       Warm the buffer of the command N ahead before executing (default: 0, off)\n");
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
        {"batch-size", required_argument, 0, 's'},
        {"warm-ahead", required_argument, 0, 'W'},
        {"workers", required_argument, 0, 'w'},
        {"debug",      required_argument, 0, 'd'},
        {"verbose",    required_argument, 0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "i:l:f:e:o:c:b:p:s:w:d:v:ht:W:", long_options, NULL)) != -1) {
        switch (c) {
            case 'i':
                g_inputfile = optarg;
                break;
            case 'W':
                g_warm_ahead = atoi(optarg);
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
            case 's':
                g_batch = atoi(optarg);
                if (g_batch <= 0) {
                    fprintf(stderr, "Error: batch size must be positive\n");
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

///// PHASE RUNNER /////////////////////////////////

typedef struct {
    Node* keys;         // search / insert / delete operands
    size_t* indexes;    // _at operands
    Node* result;       // where results land (shared; the benchmark only checks retcode)
} fill_ctx_t;

typedef void (*fill_fn)(diskarray_cmd_t* cmd, size_t op, const fill_ctx_t* ctx);

static void fill_search(diskarray_cmd_t* c, size_t op, const fill_ctx_t* x) {
    c->command = DISKARRAY_SEARCH; c->search_key = &x->keys[op]; c->result = x->result;
}
static void fill_index(diskarray_cmd_t* c, size_t op, const fill_ctx_t* x) {
    c->command = DISKARRAY_AT; c->index = (long)x->indexes[op]; c->result = x->result;
}
static void fill_insert(diskarray_cmd_t* c, size_t op, const fill_ctx_t* x) {
    c->command = DISKARRAY_INSERT; c->node = &x->keys[op]; c->result = NULL;
}
static void fill_delete(diskarray_cmd_t* c, size_t op, const fill_ctx_t* x) {
    c->command = DISKARRAY_DELETE; c->search_key = &x->keys[op]; c->result = NULL;
}

// Fill cmds[0..n) with ops [next, next+n) and queue them.  Returns n.
static size_t queue_half(diskarray_t* na, diskarray_cmd_t* cmds, size_t half, size_t* next, size_t op_count,
                         fill_fn fill, const fill_ctx_t* ctx) {
    size_t n = op_count - *next < half ? op_count - *next : half;
    for (size_t i = 0; i < n; i++) {
        fill(&cmds[i], *next + i, ctx);
        cmds[i].status = CMD_QUEUED;
    }
    if (n && diskarray_queue_cmds(na, cmds, n) < 0) {
        fprintf(stderr, "Error queueing commands\n");
        exit(1);
    }
    *next += n;
    return n;
}

static void wait_half(diskarray_t* na, const char* phase, diskarray_cmd_t* cmds, size_t n, int ok_retcode) {
    for (size_t i = 0; i < n && i < (size_t)g_warm_ahead; i++) diskarray_cmd_warm(na, &cmds[i]);
    for (size_t i = 0; i < n; i++) {
        if (g_warm_ahead && i + (size_t)g_warm_ahead < n) diskarray_cmd_warm(na, &cmds[i + (size_t)g_warm_ahead]);
        if (diskarray_cmd_wait(na, &cmds[i]) != CMD_DONE || cmds[i].retcode != ok_retcode) {
            fprintf(stderr, "%s: command %zu failed (status %d retcode %d)\n", phase, i, cmds[i].status, cmds[i].retcode);
            diskarray_debug_stats(na);
            exit(1);
        }
    }
}

// One pass: op_count operations of one kind with g_batch in flight, as two
// half-batches.  ok_retcode is what a successful command returns: 1 for
// insert, 0 otherwise.
static void run_pass(diskarray_t* na, const char* phase, int ok_retcode, size_t op_count,
                     fill_fn fill, const fill_ctx_t* ctx, diskarray_cmd_t* a, diskarray_cmd_t* b, size_t half) {
    size_t next = 0;
    size_t na_n = queue_half(na, a, half, &next, op_count, fill, ctx);
    size_t nb_n = queue_half(na, b, half, &next, op_count, fill, ctx);
    while (na_n || nb_n) {
        wait_half(na, phase, a, na_n, ok_retcode);
        na_n = queue_half(na, a, half, &next, op_count, fill, ctx);
        wait_half(na, phase, b, nb_n, ok_retcode);
        nb_n = queue_half(na, b, half, &next, op_count, fill, ctx);
    }
}

// Time a phase: passes until Phase has enough (see benchmark_helpers.h).
// Between passes, undo (if set) runs the opposite operation over the same
// keys, untimed, and the array is rebalanced, so every pass starts from the
// same contents.  The drive traffic reported is that of the timed passes;
// /proc/diskstats is read outside the timed window.
static void run_phase(diskarray_t* na, const char* phase, int ok_retcode, size_t op_count, long bytes_model,
                      fill_fn fill, fill_fn undo, const fill_ctx_t* ctx, Event* event) {
    printf("--- %s benchmarks ---\n", phase);
    size_t half = (size_t)g_batch / 2 ? (size_t)g_batch / 2 : 1;
    diskarray_cmd_t* a = malloc(half * sizeof(diskarray_cmd_t));
    diskarray_cmd_t* b = malloc(half * sizeof(diskarray_cmd_t));

    na->stat_queued = na->stat_queue_hits = na->stat_sync_loads = 0;
    Phase ph;
    phase_begin(&ph, (long)op_count);
    double rd = 0, wr = 0;
    for (;;) {
        diskstats_t before = diskstats_now();
        phase_start(&ph);
        run_pass(na, phase, ok_retcode, op_count, fill, ctx, a, b, half);
        phase_stop(&ph);
        diskstats_t after = diskstats_now();
        rd += after.rd_bytes - before.rd_bytes;
        wr += after.wr_bytes - before.wr_bytes;
        if (!phase_again(&ph) || !undo) break;
        run_pass(na, phase, 1 - ok_retcode, op_count, undo, ctx, a, b, half);
        if (diskarray_balance(na) != 0) {
            fprintf(stderr, "%s: balance between passes failed\n", phase);
            exit(1);
        }
    }
    phase_record(&ph, event, bytes_model);
    report_drive_bw(phase, rd, wr, (double)ph.ns / 1e9);
    printf("  extents during %s: %lu queued, %.1f%% already in a buffer, %lu read again while executing (%.2f%% of ops)\n",
           phase, (unsigned long)na->stat_queued, na->stat_queued ? 100.0 * na->stat_queue_hits / na->stat_queued : 0.0,
           (unsigned long)na->stat_sync_loads, na->stat_queued ? 100.0 * na->stat_sync_loads / na->stat_queued : 0.0);
    printf("Completed %s benchmarks\n", phase);
    free(a);
    free(b);
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

    // The clustered phase splits extents in one region over and over: about
    // ops / (extent / 2) new extents, where ops = min(N / 10, 5M) as gen_load
    // picks it.  Up to ~50M nodes that is more than the 10% spare, so add
    // spare extents (and keep bulk_init's fill where it was) rather than
    // force a balance in the middle of a timed phase.
    size_t ops_expected = (valid_count | 1) / 10 > 5000000 ? 5000000 : (valid_count | 1) / 10;
    size_t base_extents = (max_nodes + g_extent_size - 1) / g_extent_size;
    size_t base_data = base_extents >= 10 ? (size_t)(base_extents * 0.90) : base_extents - 1;
    size_t want_spare = 4 * ops_expected / g_extent_size + 16;
    if (base_extents - base_data < want_spare) max_nodes += (want_spare - (base_extents - base_data)) * g_extent_size;

    diskarray_t* diskarray = open_diskarray(max_nodes, g_extent_size, sizeof(Node), 
                                                    g_num_io_buffers, g_allowed_dirty_percent, g_workers, compare_nodes);
    if (!diskarray) {
        printf("Failed to create diskarray with batch size %d\n", g_batch);
        return 1;
    }
    if ((size_t)diskarray->num_data_extents > base_data) diskarray->num_data_extents = (int)base_data;
    snprintf_count(str1, sizeof(str1), diskarray->num_extents);
    snprintf_bytes(str2, sizeof(str2), diskarray->extent_size * sizeof(Node));
    snprintf_bytes(str3, sizeof(str3), diskarray->num_io_buffers * diskarray->extent_disk_size);
    printf("DiskArray:\n  extents %s of %s total I/O buffer size %s (buffers: %d)\n", 
           str1, str2, str3, diskarray->num_io_buffers);
    snprintf_count(str1, sizeof(str1), diskarray->total_tree_nodes);
    snprintf_bytes(str2, sizeof(str2), LOOKUP_NODE_SIZE*sizeof(*diskarray->lookup_tree));
    printf("  lookup height: %d lt_nodes: %s size %s\n", diskarray->tree_height, str1, str2);

    // Load nodes into temporary buffer
    // Generated data streams O_DIRECT into bulk_init and is never all in RAM;
    // otherwise load the nodes into a temporary buffer
    long starttime = get_time_us();
    if (gen_is_file(g_inputfile)) {
        if (gen_load(g_inputfile, &valid_count, NULL, GEN_NONE, &insert_nodes, &delete_nodes, &indexes, &op_count) != 0) {
            printf("Failed to create nodes\n");
            return 1;
        }
        report_elapsed_time("Loaded nodes", starttime);
        starttime = get_time_us();
        gen_stream_t* stream = gen_stream_open(g_inputfile, valid_count);
        if (!stream) {
            printf("Failed to open %s\n", g_inputfile);
            return 1;
        }
        diskarray_bulk_init_stream(diskarray, valid_count, gen_stream_next, stream);
        gen_stream_close(stream);
        report_elapsed_time("  diskarray_bulk_init", starttime);
    } else {
        Node* temp_buffer = malloc(valid_count * sizeof(Node));
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
    }

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
        starttime = get_time_us();
        if (validate_nodes_by_file(g_inputfile, "original", valid_count, (void*)diskarray, diskarray_dump_list) != 0) {
            return 1;
        }
        report_elapsed_time("  validate_nodes_by_file", starttime);
    }

    printf("--- Computing statistics ---\n");

    //// Process DiskArray Statistics ///////////////////////
    Stats stats;
    memset(&stats, 0, sizeof(Stats));
    Node node;
    long bytes = 0;

    snprintf(stats.notes, sizeof(stats.notes), "extent_size=%ld,batch_size=%d,workers=%d,io_buffers=%d,overprovisioning=%.2f,warm_ahead=%d,file=%s", g_extent_size, g_batch, g_workers, g_num_io_buffers, g_overprovisioning, g_warm_ahead, g_diskfile);
    stats.nodes = (uint64_t)valid_count;

    long find_extent = log2((double)(valid_count/g_extent_size))*sizeof(Node)*2; // O(log n) but of the extent range
    long search_extent = log2((double)g_extent_size/g_overprovisioning)*sizeof(Node); // O(log n) but of the extent array itself
    long avg_extent_cpy = (g_extent_size/g_overprovisioning)/2*sizeof(Node); // move 1/2 the valid nodes in extent on average

    long sum = 0;
    long count = 0;

    // Get Minimum for diskarray
    Node* node_ptr = NULL;
    bytes = sizeof(void*)*3+sizeof(Node); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        node_ptr = (Node*)diskarray->head;
        if (node_ptr == NULL) {
            printf("Failed to get minimum value from diskarray\n");
            return 1;
        } else if (node_ptr->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.min, (double)node_ptr->value, op_count, bytes, starttime);

    // Get Maximum for diskarray
    bytes = sizeof(void*)*3+sizeof(Node); // O(1)
    starttime = get_time_us();
    for (size_t i=0; i<op_count; i++) {
        node_ptr = (Node*)diskarray->tail;
        if (node_ptr == NULL) {
            printf("Failed to get maximum value from diskarray\n");
            return 1;
        } else if (node_ptr->value == 0) {
            printf("Test cases don't have 0, hack to prevent compiler from optimizing out the loop\n");
            return 1;
        }
    }
    record_event(&stats.max, (double)node_ptr->value, op_count, bytes, starttime);

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
    diskarray_iter_t* iter = diskarray_iter_create(diskarray, 100);
    if (!iter) {
        printf("Failed to create iterator for mean calculation\n");
        return 1;
    }
    printf("start mean calculation\n");
    while (diskarray_next(diskarray, iter, &node) == 0) {
        sum += node.value;
        count++;
    }
    diskarray_iter_free(diskarray, iter);
    record_event(&stats.mean, (double)sum / count, 1, bytes, starttime);

    // Calculate standard deviation for diskarray also O(n)
    //  NOTE: not resetting timer because you need to get mean to do stddev
    double sum_sq_diff = 0;
    iter = diskarray_iter_create(diskarray, 100);
    if (!iter) {
        printf("Failed to create iterator for stddev calculation\n");
        return 1;
    }
    printf("start stddev calculation\n");
    while (diskarray_next(diskarray, iter, &node) == 0) {
        double diff = node.value - stats.mean.value;
        sum_sq_diff += diff * diff;
    }
    diskarray_iter_free(diskarray, iter);
    record_event(&stats.stddev, sqrt(sum_sq_diff / count), 1, bytes*2, starttime);

    printf("Calculated basic statics for DiskArray\n");

    //// Benchmarks ///////////////////////
    // Each phase keeps g_batch commands in flight as two half-batches: while
    // one half executes, the other half's extents are already being read, so
    // the drives never drain between batches.  Still one consumer thread.
    run_phase(diskarray, "search", 0, op_count, find_extent + search_extent,
              fill_search, NULL, &(fill_ctx_t){ .keys = delete_nodes, .result = &node }, &stats.search);
    run_phase(diskarray, "index", 0, op_count, find_extent + search_extent,
              fill_index, NULL, &(fill_ctx_t){ .indexes = indexes, .result = &node }, &stats.index);
    run_phase(diskarray, "insert", 1, op_count, find_extent + search_extent + avg_extent_cpy,
              fill_insert, fill_delete, &(fill_ctx_t){ .keys = insert_nodes }, &stats.insert);

    if (debug > 0) {
        if (validate_nodes_by_file(g_inputfile, "insert", valid_count, (void*)diskarray, diskarray_dump_list) != 0) {
            return 1;
        }
    }

    run_phase(diskarray, "delete", 0, op_count, find_extent + search_extent + avg_extent_cpy,
              fill_delete, fill_insert, &(fill_ctx_t){ .keys = delete_nodes }, &stats.delete);

    // one narrow key range in increasing order (see gen_nodes.h): splits
    if (g_clustered_count)
        run_phase(diskarray, "clustered", 1, g_clustered_count, find_extent + search_extent + avg_extent_cpy,
                  fill_insert, fill_delete, &(fill_ctx_t){ .keys = g_clustered_nodes }, &stats.clustered);

    // Validate
    if (debug>0) {
        if (validate_nodes_by_file(g_inputfile, "delete", valid_count, (void*)diskarray, diskarray_dump_list) != 0) {
            return 1;
        }
    }

    // Print results
    printf("\n");
    if (verbose > 0) {
        // Full ordered scan: every node in increasing order, and as many as the
        // array says it holds.  Reads the whole array back, so only on -v.
        diskarray_iter_t* vit = diskarray_iter_create(diskarray, 100);
        Node prev = {0}, cur;
        size_t seen = 0, bad = 0;
        while (diskarray_next(diskarray, vit, &cur) == 0) {
            if (seen && cur.value <= prev.value && bad++ < 5)
                printf("verify: node %zu = %lu after %lu\n", seen, (unsigned long)cur.value, (unsigned long)prev.value);
            prev = cur;
            seen++;
        }
        diskarray_iter_free(diskarray, vit);
        if (bad || seen != diskarray->total_count) {
            printf("verify: FAILED (%zu out of order, %zu nodes scanned, %zu expected)\n", bad, seen, diskarray->total_count);
            return 1;
        }
        printf("verify: %zu nodes in order\n", seen);
    }
    print_results("DiskArray:", &stats);
    printf("\n");

    stats_write_json(&stats, "diskarray");

    // Cleanup
    free(insert_nodes);
    free(delete_nodes);
    free(indexes);
    //printf("Completed Delete benchmarks\n");
    diskarray_free(diskarray);

    return 0;
}
