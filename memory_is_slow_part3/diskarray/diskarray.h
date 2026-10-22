#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <liburing.h>
#include <pthread.h>
#include <stdatomic.h>

#define LOOKUP_NODE_SIZE 8

// The directory that keeps extents in key order is itself cut into
// directory extents of DA_DIR_CAP entries with slack (as in nextarray), so a
// split shifts entries inside one directory extent, never the whole array.
// Extents are named by a stable id: everything the I/O engine keeps per
// extent (count, min/max, buffer and dextent mappings) is indexed by id and
// never renumbered; only the directory knows the order.
#define DA_DIR_CAP   4096
#define DA_DIR_BITS  12         // DA_DIR_CAP == 1 << DA_DIR_BITS
#define DA_DIR_FILL  3072       // entries per directory extent after bulk_init / balance (75%)
#define DA_DLT_NODES 273        // 16-ary count tree over 4096 entries: 1 + 16 + 256 nodes

typedef struct {
    _Alignas(64) int dlt[DA_DLT_NODES * 16];   // running counts (of count[id]), one line per node
    int id[DA_DIR_CAP];         // extent ids in key order
    int n;                      // entries in use
    char* max;                  // [DA_DIR_CAP] copy of maxs[id[i]], for a local search
    char* smax1;                // every 8th
    char* smax2;                // every 64th
} diskarray_dir_t;

// Buffer state constants
enum buffer_state {
    BUFFER_UNUSED = -1,        // Empty buffer can be claimed by any user
    BUFFER_PREFETCHING = -2,   // Outstanding IO will fill this buffer, don't touch
    BUFFER_PREFETCHED = -3,    // Buffer is prefetched, but not yet mapped, can reuse
    BUFFER_FLUSHING = -4,      // Buffer is being flushed, don't touch
    BUFFER_FLUSHED = -5,       // Buffer is flushed, can reuse
    BUFFER_FLUSHABLE = -6,     // Buffer is dirty and not mapped can be moved to to FLUSHING
    BUFFER_MAPPED = -7         // Buffer is mapped, not tracked in extent_to_buffer_io[]
};

// I/O error states
enum diskarray_io_error {
    DISKARRAY_IO_SUCCESS = -51,           // Operation completed successfully
    DISKARRAY_IO_ERROR_QUEUE_FULL = -52, // I/O queue is full, cannot submit more operations
    DISKARRAY_IO_ERROR_WAIT_FAILED = -53, // Error waiting for completion (e.g., interrupted by signal)
    DISKARRAY_IO_ERROR_IO_FAILED = -54,   // I/O operation failed (disk error, permission denied, etc.)
    DISKARRAY_IO_ERROR_PARTIAL_IO = -55,  // Partial I/O (read/wrote fewer bytes than expected)
    DISKARRAY_IO_ERROR_INVALID_BUFFER = -56, // Invalid buffer index provided
    DISKARRAY_IO_ERROR_BUFFER_NOT_DIRTY = -57, // Attempted to write clean buffer
    DISKARRAY_IO_ERROR_INCOMPLETE_BATCH = -58, // Batch write completed partially
    DISKARRAY_IO_ERROR_NO_BUFFER = -59, // Can't find a buffer to use
    DISKARRAY_IO_PREFETCHING = -50, // Attempted to prefetch a buffer that is already being prefetched
    DISKARRAY_IO_ERROR_MAPPED = -51, // Already mapped
    DISKARRAY_IO_ERROR_GET_BUFFER = -52, // Failed to get buffer
    DISKARRAY_IO_ERROR_UNKNOWN = -53 // Unknown error
};

// Command states
enum diskarray_cmd_state {
    CMD_LOAD_PENDING = 1, // I/O queued for extent load
    CMD_QUEUED = 2,       // Command queued for processing, can't prefetch, maybe a previous cmd is prefetching
    CMD_DONE = 0,         // Command completed successfully
    CMD_ERROR = -1        // Generic error (specific errors below)
};

// Command types
enum diskarray_cmd_type {
    DISKARRAY_SEARCH = 1,
    DISKARRAY_AT = 2, 
    DISKARRAY_INSERT = 3,
    DISKARRAY_DELETE = 4
};

// Command structure
typedef struct {
    union {
        void* search_key;  // For _search, _delete
        long index;        // For _at
        void* node;        // For _insert
    };
    void* result;        // Output buffer (node for _search/_at, unused for others)
    int retcode;         // Return code for _search/_at, etc
    int command;         // Enum: _search, _at, _insert, _delete
    int status;          // Status: CMD_PENDING, CMD_LOAD_PENDING, CMD_DONE, or negative error
    uint64_t uid;        // Unique ID for tracking cmds
    size_t slot;         // Position in the pending ring (set by diskarray_queue_cmds)
    // Extent resolved at queue time so execution does not look it up again.
    // Valid only while the matching generation counter is unchanged.
    int extent_idx;
    long local_index;    // for _at
    int pos;             // the extent's directory position (same validity as extent_idx)
    uint64_t gen;
} diskarray_cmd_t;


typedef struct {
    // Disk I/O fields.  Dextent d lives on disk_fds[d % num_drives] at offset
    // (d / num_drives) * extent_disk_size, and is served by worker d % num_workers.
    int* disk_fds;          // One descriptor per drive (or one plain file)
    int num_drives;
    void* buffer;           // Single large buffer allocation for all I/O buffers (huge pages)
    void** io_buffers;      // Pool of aligned I/O buffers (pointers into buffer)
    size_t buffer_stride;   // Size of each I/O buffer in bytes
    bool* dirty_bits;       // Dirty bit array for each I/O buffer
    int* extent_to_buffer;  // Maps extent index to I/O buffer index (-1 if not loaded)
    _Atomic int* buffer_to_extent;  // Maps I/O buffer index to extent index (-1 if unused)
    int num_io_buffers;     // Number of I/O buffers in pool
    int num_dirty_buffers;  // Count of dirty buffers
    int allowed_dirty_percent; // Maximum percentage of dirty buffers allowed
    size_t extent_disk_size; // Size of each extent on disk in bytes
    uint64_t overhead;      // Overhead of the diskarray structures in bytes

    // Background I/O system
    int* extent_to_buffer_io;  // Maps extent index to reserved buffer for background I/O
    int* buffer_to_extent_io;  // Maps buffer index to reserved extent for background I/O
    int num_max_bgio;   // Request ring depth per worker (= its buffer count: one I/O per buffer)
    int flushable;          // tracks how many buffers are flushable
    int watermark_flush;    // how many flushable before we trigger a flush

    // Dextent (disk extent) system
    int* extent_to_dextent;    // Maps extent index to dextent index
    // int* dextent_pool;         // Ring buffer of free dextent indices
    // int dextent_head;          // Next free dextent to allocate
    // int dextent_tail;          // Last freed dextent position  
    int dextent_count;         // Number of free dextents available
    int num_dextents;          // Total dextent capacity

    // Generation counters: structure_gen changes when extent slots shift
    // (split, drained extent); count_gen changes on every count change.
    uint64_t structure_gen;
    uint64_t count_gen;
    // Counters (reset by the caller): commands queued, those whose extent was
    // already in a buffer (or on its way) at queue time, and reads map_extent
    // had to start itself, i.e. the extent was not there when a command ran.
    uint64_t stat_queued;
    uint64_t stat_queue_hits;
    uint64_t stat_sync_loads;

    // Async command system
    diskarray_cmd_t** pending_cmds; // Ring buffer of pending commands
    size_t pending_head;            // Index of next command to process
    size_t pending_tail;            // Index where to add new commands
    size_t pending_capacity;        // Capacity of pending_cmds array
    uint64_t next_uid;              // Next unique ID for commands
    uint64_t next_uid_io;           // Next unique ID for ios


    // Separated extent tracking arrays linear, sorted and aligned to optimize for memory access
    int* count;             // Count of nodes per extent - aligned allocation 512B
    void* mins;             // Array of min node copies - aligned allocation 512B
    void* maxs;             // Array of max node copies - aligned allocation 512B
    void* head;             // Pointer to the first node in the array (buffer-based)
    void* tail;             // Pointer to the last node in the array (buffer-based)

    int num_empty_extents;  // Number of unused extent ids (on free_ids)
    size_t total_count;     // Total number of items across all extents (cached for efficiency)
    int num_extents;        // Total number of extents
    int num_data_extents;   // Number of extents used for data
    size_t buffer_size;     // Total buffer size in nodes (unused in disk mode)
    int node_size;          // Size of each node in bytes
    int extent_size;        // Number of nodes per extent (capacity)
    int (*compare)(const void *a, const void *b);

    // Directory (see diskarray_dir_t): the top level has one entry per
    // directory extent: node count, max (sampled every 8 and 64), and an
    // 8-ary running-count tree for _at().
    diskarray_dir_t** dirs;
    long* tcnt;
    char* tmax;
    char* tsmax1;
    char* tsmax2;
    int ndirs, dirs_cap;
    int* pos_of;                // extent id -> (directory extent << DA_DIR_BITS) | entry, -1 if free
    int* free_ids;              // stack of unused extent ids
    long* lookup_tree;          // the top count tree, root first, levels contiguous
    int tree_height;
    int total_tree_nodes;
    int level_offset[16];

    // Multithreaded I/O fields
    int num_worker_threads;             // Number of worker threads
    struct worker_thread_data* workers; // Array of worker data
    int dextents_per_worker;            // Dextents per worker shard
    int num_io_buffers_per_worker;      // Number of I/O buffers per worker
} diskarray_t;

// One background I/O request, handed from the main thread to a worker.
typedef struct {
    int buffer_idx;
    int extent_idx;
    int dextent_idx;        // where on disk
    int old_dextent_idx;    // >= 0: this is a flush (copy-on-write); free the old one when done
} diskarray_io_t;

// One finished request, handed back from the worker to the main thread.
typedef struct {
    int buffer_idx;
    int old_dextent_idx;    // >= 0: a flush finished, free this dextent
} diskarray_done_t;

enum worker_status { WORKER_STARTING = 0, WORKER_READY = 1, WORKER_FAILED = -1 };

// Per-worker state.  The main thread and the worker talk through two
// single-producer/single-consumer rings; fields are grouped by who writes
// them so neither side ever writes a cache line the other is spinning on.
typedef struct worker_thread_data {
    // main writes, worker reads
    _Alignas(64) _Atomic uint32_t req_tail;    // requests published so far
    _Atomic bool shutdown;
    diskarray_io_t* req;                       // [num_max_bgio] filled before req_tail is bumped

    // worker writes, main reads
    _Alignas(64) _Atomic uint32_t cpl_tail;    // completions published so far
    _Atomic int status;                        // enum worker_status
    diskarray_done_t* cpl;                     // [num_max_bgio] finished requests

    // main only
    _Alignas(64) uint32_t req_count;           // requests written (== req_tail after publish)
    uint32_t cpl_head;                         // completions drained
    int inflight;                              // issued but not yet drained
    int* dextent_pool;                         // ring of this worker's free dextents (d % workers == id)
    int dextent_head;
    int dextent_tail;
    int dextent_count;
    int num_dextents;
    int* buffer_pool;                          // LRU order of this worker's buffers: [0]=LRU
    int num_io_buffers;                        // buffers owned by this worker
    int buffer_start_idx;                      // first of them in na->io_buffers

    // worker only
    _Alignas(64) uint32_t req_head;            // requests consumed
    uint32_t cpl_count;                        // completions written
    int outstanding;                           // submitted, not yet completed
    diskarray_io_t* ops;                       // [num_io_buffers] the request in flight on each buffer
    int worker_id;
    int num_workers;
    void* na_ptr;
    struct io_uring ring;
    pthread_t thread_id;
    uint64_t io_completions;
    uint64_t errors;
} worker_thread_data_t;

// Context for iterator style usage with _next
typedef struct {
    int extent_idx;         // id of the current extent
    int idx;                // Index in the current extent
    int prefetch;           // Number of extents to prefetch ahead (0 = no prefetching)
    int dir, entry;         // directory position of the current extent
    int look_dir, look_entry; // directory position of the last extent prefetched or scanned
    int ahead;              // extents prefetched beyond the current one
    void* buffer;           // Buffer for the current extent
} diskarray_iter_t;

//// Lookup tree functions ///////////////////




///// DEBUG HELPERS ////////////////////
bool diskarray_debug_stats(diskarray_t* na);
void diskarray_debug_extent(diskarray_t* na, int extent_index);
int diskarray_dump_nodes_to_file(diskarray_t* na, const char* file);
void diskarray_debug_queue(diskarray_t* na);
void diskarray_debug_buffers(diskarray_t* na);
int diskarray_map_extent_exportable(diskarray_t* na, void** io_buffer, int extent_idx);
void diskarray_unmap_buffer_exportable(diskarray_t* na, void** io_buffer);

///// API ////////////////////

// Search for a node in the diskarray
int diskarray_search(diskarray_t* na, void* search_key, void* result);

// Get the node at a specific index
int diskarray_at(diskarray_t* na, long index, void* result);

// Insert a node into the diskarray
int diskarray_insert(diskarray_t* na, void* node);

// Delete function that takes pointer to actual node (like avl_delete)
int diskarray_delete(diskarray_t* na, void* search_key);

// Iterator management
diskarray_iter_t* diskarray_iter_create(diskarray_t* na, int prefetch);
void diskarray_iter_free(diskarray_t* na, diskarray_iter_t* iter);

// Allows iterator style usage
int diskarray_next(diskarray_t* na, diskarray_iter_t* iter, void* result);
int diskarray_prefetch_next(diskarray_t* na, diskarray_iter_t* iter);

// Create an empty diskarray on one plain file or block device.
diskarray_t* diskarray_alloc(const char* disk_file, size_t max_nodes, int extent_size,
int node_size, int num_io_buffers, int allowed_dirty_percent,
int workers, int (*compare)(const void *a, const void *b));

// Create an empty diskarray striped over several block devices (dextent d on
// drive d % num_paths).  workers is rounded up to a multiple of num_paths so
// every worker's io_uring talks to exactly one drive.
diskarray_t* diskarray_alloc_striped(const char* const* disk_paths, int num_paths, size_t max_nodes,
int extent_size, int node_size, int num_io_buffers, int allowed_dirty_percent,
int workers, int (*compare)(const void *a, const void *b));

// Thread handle of worker i, for CPU pinning by the caller.
pthread_t diskarray_worker_thread(const diskarray_t* na, int worker);

// Free a diskarray
void diskarray_free(diskarray_t* na);

// Initialize a diskarray with an array of nodes from a temporary buffer, sorted or not
void diskarray_bulk_init(diskarray_t* na, void* temp_buffer, size_t count, bool sorted);
// bulk_init from a source of sorted nodes: next(ctx, n) returns the next n contiguously
void diskarray_bulk_init_stream(diskarray_t* na, size_t count, const void* (*next)(void*, size_t), void* ctx);

// Redistributes data evenly across extents
int diskarray_balance(diskarray_t* na);

// Async command functions
int diskarray_queue_cmds(diskarray_t* na, diskarray_cmd_t* cmds, size_t count);
int diskarray_cmd_wait(diskarray_t* na, diskarray_cmd_t* cmd);
// Hint: if cmd's extent has already been read into a buffer, start that
// buffer's lines on their way into this core's caches, so cmd's search runs
// on L1/L2 hits when it executes (call it a few commands ahead).
void diskarray_cmd_warm(diskarray_t* na, const diskarray_cmd_t* cmd);

// Worker submission and synchronization
int diskarray_submit_and_wait_all(diskarray_t* na);
