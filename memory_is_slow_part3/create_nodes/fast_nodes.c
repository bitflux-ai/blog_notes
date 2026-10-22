// fast_nodes: write one sorted file of random node values that serves every
// benchmark size (see common/inc/gen_nodes.h for what the benchmarks derive
// from it).  No sort and almost no RAM: each integer is kept with probability
// 1/4 by a seeded ChaCha20 keystream, so the kept integers come out in order.
//
// Two passes over the keystream, split over every cpu: count what each
// 2^26-integer chunk keeps and prefix-sum the counts into file offsets, then
// build each 64 MiB output segment from the chunks it overlaps and write it
// O_DIRECT (aligned segments, so the page cache never sees the file).
//
//   ./fast_nodes -c 64G -o ../data      -> ../data/nodes-gen-5EED0001-512GiB.bin
//
// The file is a 4 KiB header (gen_header_t) followed by the values.

#include "benchmark_helpers.h"
#include "gen_nodes.h"
#include <getopt.h>
#include <stdatomic.h>
#include <sys/statvfs.h>

uint64_t g_count = 0;
char* g_dir = "data";
uint64_t g_seed = 0x5EED0001;
int g_threads = 0;
int debug = 0;
int verbose = 0;

static uint64_t g_nchunks;      // chunks scanned in pass 1
static uint64_t* g_counts;      // values kept per chunk
static uint64_t* g_offsets;     // node offset of each chunk in the file
static uint64_t g_total;        // nodes to write
static uint64_t g_buf_nodes;    // largest chunk
static int g_fd;
static _Atomic uint64_t g_next, g_done_chunks, g_written;
static _Atomic int g_error;

static void* count_worker(void* arg) {
    (void)arg;
    uint64_t c;
    while ((c = atomic_fetch_add(&g_next, 1)) < g_nchunks) {
        g_counts[c] = gen_chunk_count(g_seed, c);
        atomic_fetch_add(&g_done_chunks, 1);
    }
    return NULL;
}

#define SEG_NODES ((64u << 20) / sizeof(Node))

static uint64_t g_last_value;

// the first chunk holding node index i
static uint64_t chunk_of(uint64_t i, uint64_t nchunks) {
    uint64_t lo = 0, hi = nchunks;
    while (hi - lo > 1) {
        uint64_t mid = (lo + hi) / 2;
        if (g_offsets[mid] <= i) lo = mid; else hi = mid;
    }
    return lo;
}

static void* write_worker(void* arg) {
    uint64_t nchunks = *(uint64_t*)arg, nsegs = (g_total + SEG_NODES - 1) / SEG_NODES, seg;
    Node *chunk = malloc(g_buf_nodes * sizeof(Node)), *out = NULL;
    uint64_t have = UINT64_MAX;  // the chunk in `chunk`
    if (!chunk || posix_memalign((void**)&out, GEN_ALIGN, SEG_NODES * sizeof(Node)) != 0) { g_error = 1; return NULL; }
    while ((seg = atomic_fetch_add(&g_next, 1)) < nsegs && !g_error) {
        uint64_t a = seg * SEG_NODES, b = a + SEG_NODES < g_total ? a + SEG_NODES : g_total;
        for (uint64_t c = chunk_of(a, nchunks), at = a; at < b; c++) {
            if (c != have) {
                if (gen_chunk_fill(g_seed, c, chunk) != g_counts[c]) { fprintf(stderr, "chunk %lu miscounted\n", c); g_error = 1; break; }
                have = c;
            }
            uint64_t to = g_offsets[c + 1] < b ? g_offsets[c + 1] : b;
            memcpy(out + (at - a), chunk + (at - g_offsets[c]), (to - at) * sizeof(Node));
            at = to;
        }
        if (b == g_total) g_last_value = out[b - a - 1].value;
        // O_DIRECT: whole 4 KiB blocks; the tail past the last node is cut off by ftruncate
        size_t bytes = ((b - a) * sizeof(Node) + GEN_ALIGN - 1) & ~(size_t)(GEN_ALIGN - 1), done = 0;
        memset((char*)out + (b - a) * sizeof(Node), 0, bytes - (b - a) * sizeof(Node));
        off_t off = (off_t)(GEN_HEADER_SIZE + a * sizeof(Node));
        while (done < bytes) {
            ssize_t rc = pwrite(g_fd, (char*)out + done, bytes - done, off + (off_t)done);
            if (rc <= 0) { perror("pwrite O_DIRECT"); g_error = 1; break; }
            done += (size_t)rc;
        }
        atomic_fetch_add(&g_written, (b - a) * sizeof(Node));
    }
    free(chunk);
    free(out);
    return NULL;
}

// run fn on every thread, printing progress every 10 s until *progress reaches total
static void run_threads(void* (*fn)(void*), void* arg, uint64_t total, _Atomic uint64_t* progress, bool bytes) {
    pthread_t* t = calloc(g_threads, sizeof(pthread_t));
    atomic_store(&g_next, 0);
    for (int i = 0; i < g_threads; i++) pthread_create(&t[i], NULL, fn, arg);
    long start = get_time_us(), last = start;
    while (atomic_load(progress) < total && !g_error) {
        usleep(100000);
        long now = get_time_us();
        if (now - last >= 10000000) {
            double secs = (now - start) / 1e6, p = (double)atomic_load(progress);
            if (bytes)
                printf("    %.1f / %.1f GiB  %.2f GiB/s\n", p / (1ull << 30), total / (double)(1ull << 30), p / (1ull << 30) / secs);
            else
                printf("    %.0f / %lu chunks counted\n", p, total);
            fflush(stdout);
            last = now;
        }
    }
    for (int i = 0; i < g_threads; i++) pthread_join(t[i], NULL);
    free(t);
}

static void print_usage(const char* program_name) {
    printf("Usage: %s -c COUNT [OPTIONS]\n", program_name);
    printf("Write a sorted file of random nodes for every benchmark size (see common/inc/gen_nodes.h)\n\n");
    printf("Options:\n");
    printf("  -c, --count SIZE         Nodes in the file, e.g. 64G for 512 GiB [REQUIRED]\n");
    printf("  -o, --dir DIR            Output directory (default: data)\n");
    printf("  -s, --seed HEX           Seed (default: 5EED0001)\n");
    printf("  -t, --threads N          Threads (default: online cpus)\n");
    printf("  -h, --help               Show this help message\n");
}

static int parse_arguments(int argc, char* argv[]) {
    static struct option long_options[] = {
        {"count", required_argument, 0, 'c'}, {"dir", required_argument, 0, 'o'},
        {"seed", required_argument, 0, 's'}, {"threads", required_argument, 0, 't'},
        {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "c:o:s:t:h", long_options, NULL)) != -1) {
        switch (c) {
        case 'c': if (parse_size_bin(optarg, &g_count)) return -1; break;
        case 'o': g_dir = optarg; break;
        case 's': g_seed = strtoull(optarg, NULL, 16); break;
        case 't': g_threads = atoi(optarg); break;
        case 'h': print_usage(argv[0]); exit(0);
        default: return -1;
        }
    }
    if (g_count < 1024) {
        fprintf(stderr, "Error: --count must be at least 1K\n");
        return -1;
    }
    return 0;
}

int main(int argc, char* argv[]) {
    if (parse_arguments(argc, argv) != 0) {
        print_usage(argv[0]);
        return 1;
    }
    if (g_threads <= 0) g_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    // every benchmark rounds N up to odd, so hold count | 1
    g_total = g_count | 1;

    char size[32], name[600], str[700];
    no_whitespace(snprintf_bytes(size, sizeof(size), g_count * sizeof(Node)), sizeof(size));
    if (mkdir_p(g_dir) != 0) { fprintf(stderr, "Error: cannot create %s\n", g_dir); return 1; }
    snprintf(name, sizeof(name), "%s/nodes-gen-%08lX-%s.bin", g_dir, g_seed, size);

    uint64_t need = GEN_HEADER_SIZE + g_total * sizeof(Node);
    struct statvfs vfs;
    if (statvfs(g_dir, &vfs) == 0 && (uint64_t)vfs.f_bavail * vfs.f_frsize < need + (16ull << 30)) {
        fprintf(stderr, "Error: %s has %.0f GiB free, need %.0f GiB (+16 GiB headroom)\n", g_dir,
                (double)vfs.f_bavail * vfs.f_frsize / (1ull << 30), need / (double)(1ull << 30));
        return 1;
    }
    printf("%s: %lu nodes, seed %08lX, %d threads\n", name, g_total, g_seed, g_threads);
    long total_start = get_time_us(), start = total_start;

    // pass 1: count.  A chunk keeps 2^24 values on average, give or take a few thousand.
    g_nchunks = g_total / (GEN_CHUNK_INTS / 4) + g_total / (GEN_CHUNK_INTS / 4) / 100 + 4;
    g_counts = calloc(g_nchunks, sizeof(uint64_t));
    g_offsets = calloc(g_nchunks + 1, sizeof(uint64_t));
    run_threads(count_worker, NULL, g_nchunks, &g_done_chunks, false);
    uint64_t nchunks = 0;
    for (uint64_t c = 0; c < g_nchunks && g_offsets[c] < g_total; c++) {
        g_offsets[c + 1] = g_offsets[c] + g_counts[c];
        if (g_counts[c] > g_buf_nodes) g_buf_nodes = g_counts[c];
        nchunks = c + 1;
    }
    if (g_offsets[nchunks] < g_total) { fprintf(stderr, "Error: pass 1 came up short\n"); return 1; }
    snprintf(str, sizeof(str), "Counted %lu chunks", nchunks);
    report_elapsed_time(str, start);

    // pass 2: fill and write
    start = get_time_us();
    g_fd = open(name, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (g_fd < 0) { perror(name); return 1; }
    run_threads(write_worker, &nchunks, g_total * sizeof(Node), &g_written, true);
    if (g_error) return 1;

    gen_header_t h = {.seed = g_seed, .count = g_total, .chunk_bits = GEN_CHUNK_BITS};
    memcpy(h.magic, GEN_MAGIC, 8);
    h.max_value = g_last_value;
    char* header;
    if (posix_memalign((void**)&header, GEN_ALIGN, GEN_HEADER_SIZE) != 0) return 1;
    memset(header, 0, GEN_HEADER_SIZE);
    memcpy(header, &h, sizeof(h));
    if (pwrite(g_fd, header, GEN_HEADER_SIZE, 0) != GEN_HEADER_SIZE || ftruncate(g_fd, (off_t)need) != 0 || fsync(g_fd) != 0) {
        perror("header");
        return 1;
    }
    free(header);
    close(g_fd);
    snprintf(str, sizeof(str), "Wrote %.1f GiB", need / (double)(1ull << 30));
    report_elapsed_time(str, start);
    report_elapsed_time("Total time", total_start);
    return 0;
}
