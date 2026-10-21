#ifndef _BENCHMARK_HELPERS
#define _BENCHMARK_HELPERS
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>

typedef struct {
    double value;
    long ops;
    double us;
    long bytes;
} Event;

typedef struct {
    Event min;
    Event max;
    Event mean;
    Event median;
    Event stddev;
    Event search;
    Event index;
    Event insert;
    Event delete;
    char notes[1024];
    uint64_t nodes;
} Stats;

// Text formatting functions
char* snprintf_value(char* str, size_t n, double value);
char* snprintf_bytes(char* str, size_t n, double bytes);
char* snprintf_count(char* str, size_t n, double count);
char* snprintf_time(char* str, size_t n, double us);
char* snprintf_speed(char* str, size_t n, double bytes, double us);
char* snprintf_bytes_count(char* str, size_t n, double count, size_t size);
char* no_whitespace(char* s, size_t n);
void print_performance(char* preamble, Event* event);
void print_results(char* preamble, Stats *stats);
char* compare_values(double a, double b, char* result);
long get_time_us();

// Monotonic nanoseconds, for the timed phases.
static inline long get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

// One timed phase (search/index/insert/delete).  A pass is the op_count ops;
// at small sizes that is only a few hundred ops, so the phase repeats its pass
// until it has done PHASE_MIN_OPS (env BENCH_MIN_OPS) ops or PHASE_MAX_NS of
// timed work.  Inserts and deletes are undone between passes, untimed:
//
//     Phase ph; phase_begin(&ph, op_count);
//     for (;;) {
//         phase_start(&ph);  ...the timed pass...  phase_stop(&ph);
//         if (!phase_again(&ph)) break;
//         ...undo the pass...
//     }
//     phase_record(&ph, &stats.insert, bytes);
#define PHASE_MIN_OPS 100000L
#define PHASE_MAX_NS  1000000000L
typedef struct {
    long ns;          // timed so far
    long ops;         // ops done so far
    long pass_ops;
    long min_ops;
    long t0;
} Phase;
void phase_begin(Phase* p, long pass_ops);
static inline void phase_start(Phase* p) { p->t0 = get_time_ns(); }
static inline void phase_stop(Phase* p) { p->ns += get_time_ns() - p->t0; p->ops += p->pass_ops; }
static inline bool phase_again(const Phase* p) { return p->ops < p->min_ops && p->ns < PHASE_MAX_NS; }
void phase_record(const Phase* p, Event* event, long bytes);
void compare_results(const char* preamble, Stats* a, Stats* b);
int validate_statics(const char* alabel, Stats* a, const char* blabel, Stats* b);
void report_elapsed_time(const char* preamble, long start);
void record_event(Event* event, double value, long ops, long bytes, long starttime);
int stats_write_json(const Stats *s, const char *prefix);

// Dump to binary file
int dump_binary(const char *name, const void *data, size_t size, size_t offset);

// Read from a binary file
int load_binary(char *name, void** data, size_t offset, size_t size);
int get_file_size(char *name, size_t *size);
int compare_to_binary_file(char *name, void* data, size_t size);
int compare_files(const char *file1, const char *file2);
int parse_size_bin(const char *s, uint64_t *out);

// Writes an 8-char lowercase hex string (plus NUL) into out[9].
void hash8(const void *buf, char out[9]);
int mkdir_p(const char *p);

// Pin the calling (consumer) thread to cpu 0, leave cpu 0's hyperthread
// sibling idle, and give each worker thread its own following cpu.  Prints
// the layout.  Returns the number of workers pinned.
int pin_main_and_workers(const pthread_t* workers, int num_workers);

// Strips trailing extension (text after last '.' in the basename), then splits on `delim`.
size_t split_char_strip_ext(char *s, char delim, char *out[], size_t max_out);

///// NODES /////////////////////////////////

// The node structure we're sorting on
typedef struct Node {
    uint64_t value;
} Node;

// Compare function for qsort and bsearch
static inline int compare_nodes(const void *a, const void *b) {
    const Node *na = (const Node *)a;
    const Node *nb = (const Node *)b;

    if (na->value < nb->value) {
        return -1;
    } else if (na->value > nb->value) {
        return 1;
    } else {
        return 0;
    }
}


static inline int rand_bits_per_call(void) {
    unsigned x = RAND_MAX;
    int b = 0;
    while (x) { b++; x >>= 1; }
    return b;                // e.g. 15 on MSVC, 31 on glibc
}

static inline uint64_t rand64(void) {
    int b = rand_bits_per_call();
    unsigned mask = (b >= 31) ? 0x7FFFFFFFu : ((1u << b) - 1u);

    uint64_t r = 0;
    int produced = 0;
    while (produced < 64) {
        r = (r << b) | (uint64_t)(rand() & mask);
        produced += b;
    }
    if (produced > 64) r >>= (produced - 64);  // trim excess high bits
    return r;
}

// unbiased [0, n) using rejection sampling
static inline uint64_t rand64_bounded(uint64_t n) {
    if (n == 0) return 0;
    uint64_t limit = UINT64_MAX - (UINT64_MAX % n);
    uint64_t r;
    do { r = rand64(); } while (r >= limit);
    return r % n;
}

static inline void generate_random_node(Node *node, uint64_t range) {
    node->value = rand64_bounded(range);
    // I want to filter out 0 as a value
    if (node->value == 0) {
        while (node->value == 0) {
            node->value = rand64_bounded(range);
        }
    }
}

int load_nodes_from_file(char *nodefilename, Node **nodes, size_t count, size_t offset);
// true when load_nodes filled the caller's buffer with sorted nodes (a list
// file or generated data); pass it to bulk_init as "sorted"
extern bool g_nodes_sorted;
int load_nodes(char *nodefilename, char *debuglistfilename, Node **nodes, Node** insert_nodes, Node **delete_nodes, size_t** indexes, size_t *valid_count, size_t *op_count);
int validate_nodes_by_file(char* nodesfilename, char* stage, size_t count, void* context, int(*dump_nodes)(void*, const char*));

#endif