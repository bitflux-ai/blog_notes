/*
 * Batched single-consumer driver for the B+ tree: bptree's peer of
 * nextarray2's `-p pipeline` mode.
 *
 * Operations run `group` at a time.  The group is resolved together
 * (bptree_locate_group: one tree level per round, every operation's next
 * node prefetched before any is read, then the leaf binary search one probe
 * per round), and then executed in order at the resolved locations.  A
 * location is used only while the tree's generation still matches and the
 * leaf's neighbours still bracket the key; otherwise the operation does its
 * full lookup (counted as a fallback).  Before executing operation i, the
 * leaf range its memmove will shift is prefetched for operation i + D.
 */
#include "bptree.h"
#include "benchmark_helpers.h"
#include "gen_nodes.h"

#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_LEAF_SIZE 512
#define DEFAULT_BRANCH_FACTOR 16
#define DEFAULT_FILL 0.75
#define DEFAULT_GROUP 32
#define DEFAULT_TAIL_AHEAD 4

char* g_inputfile = NULL;
char* g_listfile = NULL;
uint64_t g_leaf_size = DEFAULT_LEAF_SIZE;
uint64_t g_branch_factor = DEFAULT_BRANCH_FACTOR;
uint64_t g_count = 0;
double g_fill = DEFAULT_FILL;
int g_group = DEFAULT_GROUP;
int g_tail_ahead = DEFAULT_TAIL_AHEAD;   // operations ahead whose memmove range is prefetched
int g_cpu = 0;                           // consumer cpu, as nextarray2 (-1: don't pin)
int debug = 0;
int verbose = 0;

enum { OP_SEARCH, OP_INDEX, OP_INSERT, OP_DELETE };

static void pin(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0) perror("pin");
}

// Warm what execution will touch beyond the resolve: for insert/delete the
// leaf tail from pos to the end that the memmove shifts.
static inline void warm(const bptree_loc_t* loc, int kind) {
    if ((kind != OP_INSERT && kind != OP_DELETE) || !loc->leaf) return;
    const char* p = (const char*)((const uint64_t*)loc->leaf->u.leaf.keys + loc->pos);
    const char* end = (const char*)((const uint64_t*)loc->leaf->u.leaf.keys + loc->leaf->n + 1);
    for (p = (const char*)((uintptr_t)p & ~(uintptr_t)63); p < end; p += 64) __builtin_prefetch(p, 1);
}

static int execute(bptree_t* t, const bptree_loc_t* loc, int kind, const uint64_t* key, long index) {
    switch (kind) {
    case OP_SEARCH: return bptree_search_resolved(t, loc, key) ? 0 : -1;
    case OP_INDEX:  return bptree_at_resolved(t, loc, index) ? 0 : -1;
    case OP_INSERT: return bptree_insert_resolved(t, loc, key) == 1 ? 0 : -1;
    case OP_DELETE: return bptree_delete_resolved(t, loc, key);
    }
    return -1;
}

// One pass of op_count operations: ctx is Node* (keys) or size_t* (ranks).
static void run_pipeline(bptree_t* t, const char* name, size_t op_count, int kind, const void* ctx) {
    static bptree_loc_t loc[BPTREE_MAX_GROUP];   // static: nothing is allocated in the timed path
    static bptree_path_t paths[BPTREE_MAX_GROUP];
    const Node* nodes = ctx;
    const size_t* ranks = ctx;
    for (size_t start = 0; start < op_count; start += (size_t)g_group) {
        int n = (int)(op_count - start < (size_t)g_group ? op_count - start : (size_t)g_group);
        const uint64_t* keys = kind == OP_INDEX ? NULL : &nodes[start].value;
        // size_t and long ranks have the same bits; the ranks are < 2^63
        const long* idx = kind == OP_INDEX ? (const long*)&ranks[start] : NULL;
        if (kind == OP_INDEX) bptree_locate_index_group(t, idx, n, loc);
        else bptree_locate_group(t, keys, n, loc, kind == OP_SEARCH ? NULL : paths);
        for (int i = 0; i < n && i < g_tail_ahead; i++) warm(&loc[i], kind);
        for (int i = 0; i < n; i++) {
            if (i + g_tail_ahead < n) warm(&loc[i + g_tail_ahead], kind);
            if (execute(t, &loc[i], kind, keys ? &keys[i] : NULL, idx ? idx[i] : 0) != 0) {
                fprintf(stderr, "%s: operation %zu failed\n", name, start + (size_t)i);
                exit(1);
            }
        }
    }
}

// Put the tree back after a pass of inserts or deletes (untimed), so the
// next pass starts from the same contents.
static void undo_insert(bptree_t* t, size_t n, const void* ctx) {
    for (size_t i = 0; i < n; i++) bptree_delete(t, (void*)&((const Node*)ctx)[i]);
}
static void undo_delete(bptree_t* t, size_t n, const void* ctx) {
    for (size_t i = 0; i < n; i++) bptree_insert(t, (void*)&((const Node*)ctx)[i]);
}

// Time a phase: passes of op_count operations until Phase has enough (see
// benchmark_helpers.h), with undo between passes.
static void run_phase(bptree_t* t, const char* name, size_t op_count, int kind, const void* ctx,
                      void (*undo)(bptree_t*, size_t, const void*), long bytes_model, Event* ev) {
    printf("--- %s benchmarks ---\n", name);
    uint64_t fb = t->fallbacks;
    long passes = 0;
    Phase ph;
    phase_begin(&ph, (long)op_count);
    for (;;) {
        phase_start(&ph);
        run_pipeline(t, name, op_count, kind, ctx);
        phase_stop(&ph);
        passes++;
        if (!phase_again(&ph)) break;
        if (undo) undo(t, op_count, ctx);
    }
    phase_record(&ph, ev, bytes_model);
    printf("  %s: %lu of %zu resolved operations fell back to a full lookup\n", name,
           (unsigned long)(t->fallbacks - fb), (size_t)passes * op_count);
}

static void print_usage(const char* prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Batched B+ tree benchmark (bptree2): group resolve, then execute in order\n\n");
    printf("  -i, --input FILE         Path file with pregenerated nodes [REQUIRED]\n");
    printf("  -l, --list FILE          Path file with sorted nodes for debug\n");
    printf("  -c, --count N            How many nodes to use\n");
    printf("  -e, --leaf-size SIZE     Max keys per leaf (default: %d)\n", DEFAULT_LEAF_SIZE);
    printf("  -b, --branch-factor N    Max children per internal node (default: %d)\n", DEFAULT_BRANCH_FACTOR);
    printf("  -F, --fill FRACTION      Fill leaves/internal nodes to this at bulk load (default: %.2f)\n", DEFAULT_FILL);
    printf("  -g, --group N            Operations resolved together (default: %d, max %d)\n", DEFAULT_GROUP, BPTREE_MAX_GROUP);
    printf("  -D, --tail-ahead N       Prefetch the memmove range N operations ahead (default: %d, 0 = off)\n", DEFAULT_TAIL_AHEAD);
    printf("  -C, --cpu N              Pin to cpu N (default: 0, -1 = don't pin)\n");
    printf("  -d, --debug LEVEL        Validate against the list files\n");
    printf("  -v, --verbose LEVEL      Verbose output\n");
    printf("  -h, --help               Show this help message\n");
    printf("\nOne of --list or --count must be specified\n");
}

static int parse_arguments(int argc, char* argv[]) {
    static struct option opts[] = {
        {"input", required_argument, 0, 'i'},  {"list", required_argument, 0, 'l'},
        {"count", required_argument, 0, 'c'},  {"leaf-size", required_argument, 0, 'e'},
        {"branch-factor", required_argument, 0, 'b'}, {"fill", required_argument, 0, 'F'},
        {"group", required_argument, 0, 'g'},  {"tail-ahead", required_argument, 0, 'D'},
        {"cpu", required_argument, 0, 'C'},    {"debug", required_argument, 0, 'd'},
        {"verbose", required_argument, 0, 'v'}, {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "i:l:c:e:b:F:g:D:C:d:v:h", opts, NULL)) != -1) {
        switch (c) {
        case 'i': g_inputfile = optarg; break;
        case 'l': g_listfile = optarg; break;
        case 'c': if (parse_size_bin(optarg, &g_count) != 0) return -1; break;
        case 'e': if (parse_size_bin(optarg, &g_leaf_size) != 0) return -1; break;
        case 'b': if (parse_size_bin(optarg, &g_branch_factor) != 0) return -1; break;
        case 'F': g_fill = atof(optarg); if (!(g_fill > 0.0 && g_fill <= 1.0)) return -1; break;
        case 'g': g_group = atoi(optarg); if (g_group < 1 || g_group > BPTREE_MAX_GROUP) return -1; break;
        case 'D': g_tail_ahead = atoi(optarg); if (g_tail_ahead < 0) return -1; break;
        case 'C': g_cpu = atoi(optarg); break;
        case 'd': debug = atoi(optarg); break;
        case 'v': verbose = atoi(optarg); break;
        case 'h': print_usage(argv[0]); exit(0);
        default: return -1;
        }
    }
    if (!g_inputfile || (g_count == 0) == (g_listfile == NULL)) return -1;
    return 0;
}

static int dump_list(void* ctx, const char* filename) { return bptree_dump_nodes_to_file(ctx, filename); }

int main(int argc, char* argv[]) {
    if (parse_arguments(argc, argv) != 0) { print_usage(argv[0]); return 1; }

    Node* nodes = NULL;
    Node* insert_nodes = NULL;
    Node* delete_nodes = NULL;
    size_t* indexes = NULL;
    size_t valid_count = g_count, op_count = 0;
    if (valid_count == 0) {
        if (get_file_size(g_listfile, &valid_count) != 0) return 1;
        valid_count /= sizeof(Node);
    }
    // load_nodes only reloads the sorted list into a buffer that already exists (see bptree/benchmark.c)
    if (g_listfile != NULL || gen_is_file(g_inputfile)) {
        nodes = calloc(valid_count, sizeof(Node));
        if (!nodes) { printf("Failed to allocate node buffer\n"); return 1; }
    }

    bptree_t* tree = bptree_alloc((int)g_leaf_size, (int)g_branch_factor, sizeof(Node), compare_nodes);
    if (!tree) { printf("Failed to create B+ tree\n"); return 1; }
    tree->bulk_fill = g_fill;

    long start = get_time_us();
    if (load_nodes(g_inputfile, g_listfile, &nodes, &insert_nodes, &delete_nodes, &indexes, &valid_count, &op_count) != 0) return 1;
    report_elapsed_time("Loaded nodes", start);
    start = get_time_us();
    if (bptree_bulk_init(tree, nodes, valid_count, g_nodes_sorted) != 0) { printf("Failed to bulk_init B+ tree\n"); return 1; }
    report_elapsed_time("  bptree_bulk_init", start);
    if (debug > 0 && validate_nodes_by_file(g_inputfile, "original", valid_count, tree, dump_list) != 0) return 1;

    if (g_cpu >= 0) pin(g_cpu);
    printf("bptree2: %zu nodes, leaf_size %lu, branch_factor %lu, fill %.2f, group %d, tail ahead %d, cpu %d\n",
           valid_count, (unsigned long)g_leaf_size, (unsigned long)g_branch_factor, g_fill, g_group, g_tail_ahead, g_cpu);

    Stats stats;
    memset(&stats, 0, sizeof stats);
    snprintf(stats.notes, sizeof stats.notes,
             "leaf_size=%lu,branch_factor=%lu,fill=%.2f,keys=u64,mode=pipeline,group=%d,tail_ahead=%d,cpu=%d",
             (unsigned long)g_leaf_size, (unsigned long)g_branch_factor, g_fill, g_group, g_tail_ahead, g_cpu);
    stats.nodes = valid_count;

    // the same byte model as bptree/benchmark.c
    double leaf_keys = (double)g_leaf_size * g_fill;
    double n_leaves = (double)valid_count / leaf_keys < 1.0 ? 1.0 : (double)valid_count / leaf_keys;
    double fanout = (double)g_branch_factor * g_fill < 2.0 ? 2.0 : (double)g_branch_factor * g_fill;
    long find_path = (long)(log(n_leaves) / log(fanout) + 1.0) * (long)(sizeof(void*) * (size_t)g_branch_factor);
    long search_leaf = (long)(log2(leaf_keys) * sizeof(Node));
    long avg_leaf_cpy = (long)((leaf_keys / 2) * sizeof(Node));

    run_phase(tree, "search", op_count, OP_SEARCH, delete_nodes, NULL, find_path + search_leaf, &stats.search);
    run_phase(tree, "index", op_count, OP_INDEX, indexes, NULL, find_path + (long)sizeof(Node), &stats.index);
    run_phase(tree, "insert", op_count, OP_INSERT, insert_nodes, undo_insert, find_path + search_leaf + avg_leaf_cpy, &stats.insert);
    if (debug > 0 && validate_nodes_by_file(g_inputfile, "insert", valid_count, tree, dump_list) != 0) return 1;
    run_phase(tree, "delete", op_count, OP_DELETE, delete_nodes, undo_delete, find_path + search_leaf + avg_leaf_cpy, &stats.delete);
    if (debug > 0 && validate_nodes_by_file(g_inputfile, "delete", valid_count, tree, dump_list) != 0) return 1;
    // one narrow key range in increasing order (see gen_nodes.h)
    if (g_clustered_count)
        run_phase(tree, "clustered", g_clustered_count, OP_INSERT, g_clustered_nodes, undo_insert,
                  find_path + search_leaf + avg_leaf_cpy, &stats.clustered);

    printf("\n");
    print_results("B+ tree (bptree2, batched):", &stats);
    printf("\n");
    stats_write_json(&stats, "bptree2");

    free(insert_nodes);
    free(delete_nodes);
    free(indexes);
    free(nodes);
    bptree_free(tree);
    return 0;
}
