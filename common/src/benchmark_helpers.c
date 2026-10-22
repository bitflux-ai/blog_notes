#include "benchmark_helpers.h"
#include "gen_nodes.h"
#include <sched.h>

bool g_nodes_sorted = false;

int stats_write_json(const Stats *s, const char *prefix) {
    if (!s || !prefix) { errno = EINVAL; return -1; }

    enum { N_EVENTS = 10 };
    const char *event_names[N_EVENTS] = {
        "min","max","mean","median","stddev","search","index","insert","delete","clustered"
    };
    const Event *ev[N_EVENTS] = {
        &s->min, &s->max, &s->mean, &s->median, &s->stddev,
        &s->search, &s->index, &s->insert, &s->delete, &s->clustered
    };

    /* Flatten columns (cast to long long for consistent JSON integer printing) */
    double value_col[N_EVENTS];
    long long ops_col[N_EVENTS], bytes_col[N_EVENTS];
    double us_col[N_EVENTS];
    for (int i = 0; i < N_EVENTS; ++i) {
        value_col[i] = ev[i]->value;
        ops_col[i]   = (long long)ev[i]->ops;
        us_col[i]    = ev[i]->us;
        bytes_col[i] = (long long)ev[i]->bytes;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) return -1;
    long long epoch = (long long)now;

    char filename[1024];
    int n = snprintf(filename, sizeof(filename), "%s-%lld.json", prefix, epoch);
    if (n < 0 || n >= (int)sizeof(filename)) { errno = ENAMETOOLONG; return -1; }

    FILE *f = fopen(filename, "w");
    if (!f) return -1;

    /* Begin JSON */
    fputs("{\n", f);

    /* timestamp column (repeated per event) */
    fputs("  \"timestamp\": [\n", f);
    for (int i = 0; i < N_EVENTS; ++i) {
        fputs("    ", f);
        fprintf(f, "%lld", epoch);
        fputs((i + 1 < N_EVENTS) ? ",\n" : "\n", f);
    }
    fputs("  ],\n", f);

    /* test column (repeat escaped prefix per event) */
    fputs("  \"test\": [\n", f);
    for (int i = 0; i < N_EVENTS; ++i) {
        fputs("    \"", f);
        /* JSON-escape prefix inline */
        for (const unsigned char *p = (const unsigned char*)prefix; *p; ++p) {
            switch (*p) {
                case '\\': fputs("\\\\", f); break;
                case '\"': fputs("\\\"", f); break;
                case '\b': fputs("\\b", f);  break;
                case '\f': fputs("\\f", f);  break;
                case '\n': fputs("\\n", f);  break;
                case '\r': fputs("\\r", f);  break;
                case '\t': fputs("\\t", f);  break;
                default:
                    if (*p < 0x20) fprintf(f, "\\u%04x", *p);
                    else fputc(*p, f);
            }
        }
        fputc('\"', f);
        fputs((i + 1 < N_EVENTS) ? ",\n" : "\n", f);
    }
    fputs("  ],\n", f);

    /* event names */
    fputs("  \"event\": [\n", f);
    for (int i = 0; i < N_EVENTS; ++i) {
        fputs("    \"", f);
        fputs(event_names[i], f); /* known-safe ASCII literals */
        fputc('\"', f);
        fputs((i + 1 < N_EVENTS) ? ",\n" : "\n", f);
    }
    fputs("  ],\n", f);

    /* value column (double) */
    fputs("  \"value\": [\n", f);
    for (int i = 0; i < N_EVENTS; ++i) {
        fputs("    ", f);
        fprintf(f, "%.17g", value_col[i]);
        fputs((i + 1 < N_EVENTS) ? ",\n" : "\n", f);
    }
    fputs("  ],\n", f);

    /* ops column */
    fputs("  \"ops\": [\n", f);
    for (int i = 0; i < N_EVENTS; ++i) {
        fputs("    ", f);
        fprintf(f, "%lld", ops_col[i]);
        fputs((i + 1 < N_EVENTS) ? ",\n" : "\n", f);
    }
    fputs("  ],\n", f);

    /* us column */
    fputs("  \"us\": [\n", f);
    for (int i = 0; i < N_EVENTS; ++i) {
        fputs("    ", f);
        fprintf(f, "%.3f", us_col[i]);
        fputs((i + 1 < N_EVENTS) ? ",\n" : "\n", f);
    }
    fputs("  ],\n", f);

    /* bytes column */
    fputs("  \"bytes\": [\n", f);
    for (int i = 0; i < N_EVENTS; ++i) {
        fputs("    ", f);
        fprintf(f, "%lld", bytes_col[i]);
        fputs((i + 1 < N_EVENTS) ? ",\n" : "\n", f);
    }
    fputs("  ],\n", f);

    /* nodes column */
    fputs("  \"nodes\": [\n", f);
    for (int i = 0; i < N_EVENTS; ++i) {
        fputs("    ", f);
        fprintf(f, "%ld", s->nodes);
        fputs((i + 1 < N_EVENTS) ? ",\n" : "\n", f);
    }
    fputs("  ],\n", f);

    /* Notes for custom data (last, no trailing comma) */
    fputs("  \"notes\": [\n", f);
    for (int i = 0; i < N_EVENTS; ++i) {
        fputs("    ", f);
        fprintf(f, "\"%s\"", s->notes);
        fputs((i + 1 < N_EVENTS) ? ",\n" : "\n", f);
    }
    fputs("  ]\n", f);

    /* End JSON */
    fputs("}\n", f);

    if (fclose(f) != 0) return -1;
    return 0;
}

// Text formatting functions
char* snprintf_value(char* str, size_t n, double value) {
    if (value >= 100.0) {
        snprintf(str, n, " %.0f", value);
    } else if (value >= 10.0) {
        snprintf(str, n, "%.1f", value);
    } else {
        snprintf(str, n, "%.2f", value);
    }
    return str;
}

char* snprintf_bytes(char* str, size_t n, double bytes) {
    static const char* units[] = {" B  ", " KiB", " MiB", " GiB", " TiB"};
    const size_t unit_count = sizeof(units) / sizeof(units[0]);
    char number[16];
    size_t inc = 0;
    double value = bytes;

    while (value >= 1024.0 && inc + 1 < unit_count) {
        value /= 1024.0;
        inc++;
    }
    snprintf_value(number, sizeof(number), value);
    snprintf(str, n, "%s%s", number, units[inc]);
    return str;
}

char* snprintf_count(char* str, size_t n, double count) {
    static const char* units[] = {"", " K", " M", " B", " T"};
    const size_t unit_count = sizeof(units) / sizeof(units[0]);
    char number[16];
    size_t inc = 0;
    double value = count;

    while (value > 1000.0 && inc + 1 < unit_count) {
        value /= 1000.0;
        inc++;
    }
    snprintf_value(number, sizeof(number), value);
    snprintf(str, n, "%s%s", number, units[inc]);
    return str;
}

char* snprintf_time(char* str, size_t n, double us) {
    static const char* units[] = {" ns"," us", " ms", " s "};
    const size_t unit_count = sizeof(units) / sizeof(units[0]);
    char number[16];
    size_t inc = 0;
    double value = us*1000;

    while (value > 1000.0 && inc + 1 < unit_count) {
        value /= 1000.0;
        inc++;
    }
    snprintf_value(number, sizeof(number), value);
    snprintf(str, n, "%s%s", number, units[inc]);
    return str;
}

char* snprintf_speed(char* str, size_t n, double bytes, double us) {
    double seconds = us / 1e6;
    if (seconds <= 0.0) {
        // Avoid division by zero; represent as 0 B/s-equivalent unitized value.
        return snprintf_bytes(str, n, 0.0);
    }
    return snprintf_bytes(str, n, bytes / seconds);
}

char* snprintf_bytes_count(char* str, size_t n, double count, size_t size) {
    char bytesstr[32];
    char countstr[32];
    snprintf_bytes(bytesstr, sizeof(bytesstr), count*size);
    snprintf_count(countstr, sizeof(countstr), count);
    snprintf(str, n, "%s %s", bytesstr, countstr);
    return str;
}

char* no_whitespace(char* s, size_t n) {
    if (!s || n == 0) return s;

    size_t len = strnlen(s, n);
    if (len == n) {                   // ensure termination within n
        s[n - 1] = '\0';
        len = n - 1;
    }

    // Skip leading whitespace
    size_t r = 0;
    while (r < len && isspace((unsigned char)s[r])) r++;

    // Copy only non-whitespace chars
    size_t w = 0;
    for (; r < len; r++) {
        unsigned char c = (unsigned char)s[r];
        if (!isspace(c)) {
            if (w < n - 1) s[w++] = (char)c;
            else break;               // no room for more
        }
    }
    s[w] = '\0';
    return s;
}


void print_performance(char* preamble, Event* event) {
    double us = event->us;
    if (us <= 0) us = 1;
    char spopsstr[32];
    snprintf_time(spopsstr, sizeof(spopsstr), (double)us / (double)event->ops);
    char timestr[32];
    snprintf_time(timestr, sizeof(timestr), (double)us);
    char bytesstr[32];
    snprintf_bytes(bytesstr, sizeof(bytesstr), (double)event->bytes);
    char speedstr[32];
    long total_bytes = event->bytes * event->ops;
    snprintf_speed(speedstr, sizeof(speedstr), (double)total_bytes, (double)us);
    char totalbytesstr[32];
    snprintf_bytes(totalbytesstr, sizeof(totalbytesstr), (double)total_bytes);
    char oppsstr[32];
    snprintf_count(oppsstr, sizeof(oppsstr), (double)(event->ops)/((double)us/(double)1000000));
    printf("%s %s op/s %s/op %s ( %s/s ) processed %s / op - %s total\n",
           preamble, oppsstr, spopsstr, timestr, speedstr, bytesstr, totalbytesstr);
}

void print_results(char* preamble, Stats *stats) {
    char stddevstr[32];
    char meanstr[32];
    snprintf_value(meanstr, sizeof(meanstr), stats->mean.value);
    snprintf_value(stddevstr, sizeof(stddevstr), stats->stddev.value);

    printf("-- %s --\n", preamble);
    printf("    median: %ld, mean:%s, min: %ld, max: %ld, std_dev:%s\n",
           (long)stats->median.value, meanstr,
           (long)stats->min.value, (long)stats->max.value, stddevstr);

    print_performance("    min:        ", &stats->min);
    print_performance("    max:        ", &stats->max);
    print_performance("    median:     ", &stats->median);
    print_performance("    mean:       ", &stats->mean);
    print_performance("    stddev:     ", &stats->stddev);
    print_performance("    search:     ", &stats->search);
    print_performance("    index:      ", &stats->index);
    print_performance("    insert:     ", &stats->insert);
    print_performance("    delete:     ", &stats->delete);
    if (stats->clustered.ops) print_performance("    clustered:  ", &stats->clustered);
}

char* compare_values(double a, double b, char* result) {
    char ratio_str[16];
    const char* label;

    if (a > b) {
        snprintf_value(ratio_str, sizeof(ratio_str), a / (b > 0 ? b : 1));
        label = "Slower";
    } else {
        snprintf_value(ratio_str, sizeof(ratio_str), b / (a > 0 ? a : 1));
        label = "Faster";
    }

    char padded_ratio[16];
    snprintf(padded_ratio, sizeof(padded_ratio), "%6s", ratio_str);

    snprintf(result, 32, "%sX %s", padded_ratio, label);
    return result;
}

long get_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000L + tv.tv_usec;
}

void compare_result(char* preamble, Event* a, Event* b){
    char str[64];
    char str1[16];
    char str2[16];
    compare_values(a->us, b->us, str);
    printf("%s%s -- %s vs %s\n", preamble, str, snprintf_time(str1, sizeof(str1), a->us), snprintf_time(str2, sizeof(str2), b->us));
}

void compare_results(const char* preamble, Stats* a, Stats* b) {
    printf("%s\n", preamble);
    compare_result("    min:        ", &a->min, &b->min);
    compare_result("    max:        ", &a->max, &b->max);
    compare_result("    mean:       ", &a->mean, &b->mean);
    compare_result("    stddev:     ", &a->stddev, &b->stddev);
    compare_result("    median:     ", &a->median, &b->median);
    compare_result("    insert:     ", &a->insert, &b->insert);
    compare_result("    delete:     ", &a->delete, &b->delete);
    printf("\n");
}

int validate_statics(const char* alabel, Stats* a, const char* blabel, Stats* b) {
    int result = 0;
    // compare min/max/mean/median/stddev
    if (a->min.value != b->min.value) {
        printf("Min values differ: %s = %f, %s = %f\n", alabel, a->min.value, blabel, b->min.value);
        result = 1;
    }
    if (a->max.value != b->max.value) {
        printf("Max values differ: %s = %f, %s = %f\n", alabel, a->max.value, blabel, b->max.value);
        result = 1;
    }
    if (a->mean.value != b->mean.value) {
        printf("Mean values differ: %s = %f, %s = %f\n", alabel, a->mean.value, blabel, b->mean.value);
        result = 1;
    }
    if (a->stddev.value != b->stddev.value) {
        printf("Stddev values differ: %s = %f, %s = %f\n", alabel, a->stddev.value, blabel, b->stddev.value);
        result = 1;
    }
    return result;
}

void report_elapsed_time(const char* preamble, long start) {
    char str[32];
    snprintf_time(str, sizeof(str), get_time_us() - start);
    printf("%s: %s\n", preamble, str);
}

void record_event(Event* event, double value, long ops, long bytes, long starttime) {
    event->value = value;
    event->ops = ops;
    event->us = get_time_us() - starttime;
    event->bytes = bytes;
}

void phase_begin(Phase* p, long pass_ops) {
    static long min_ops = -1;
    if (min_ops < 0) {
        const char* e = getenv("BENCH_MIN_OPS");
        min_ops = e && *e ? atol(e) : PHASE_MIN_OPS;
    }
    p->ns = 0;
    p->ops = 0;
    p->pass_ops = pass_ops;
    p->min_ops = min_ops;
    p->t0 = 0;
}

void phase_record(const Phase* p, Event* event, long bytes) {
    event->value = 0;
    event->ops = p->ops;
    event->us = (double)p->ns / 1000.0;
    event->bytes = bytes;
    if (p->pass_ops && p->ops > p->pass_ops)
        printf("  %ld passes of %ld ops\n", p->ops / p->pass_ops, p->pass_ops);
}

// Dump to binary file
int dump_binary(const char *name, const void *data, size_t size, size_t offset) {
    FILE *fp = fopen(name, offset == 0 ? "wb" : "r+b"); // first write truncates, later writes update
    if (!fp) {
        fp = fopen(name, "wb+"); // create if missing
        if (!fp) return -1;
    }
    if (fseek(fp, (long)offset, SEEK_SET) != 0) { fclose(fp); return -1; }

    size_t n = fwrite(data, 1, size, fp);
    fclose(fp);
    return (n == size) ? 0 : -1;
}

// Read from a binary file
int load_binary(char *name, void** data, size_t offset, size_t size) {
    FILE* fp = fopen(name, "rb");
    if (!fp) {
        printf("Failed to open %s for reading\n", name);
        return -1;
    }
    if (size == 0) {
        if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
        long end = ftell(fp);
        if (end < 0) { fclose(fp); return -1; }
        size = (size_t)end;
        rewind(fp);
    }

    if (*data == NULL) {
        void *buf = malloc(size);
        if (!buf) {
            char str[32];
            snprintf_bytes(str, sizeof(str), size);
            printf("Failed to allocate memory for %s\n", str);
            fclose(fp);
            return -1;
        }
        *data = buf;
    }
    if (fseek(fp, offset, SEEK_SET) != 0) {
        printf("Failed to seek to offset %ld\n", offset);
        fclose(fp);
        return -1;
    }

    size_t read = fread(*data, 1, size, fp);
    fclose(fp);

    if (read != size) {
        fprintf(stderr, "Failed to read %ld bytes\n", size);
        free(*data);
        return -1;
    }

    return 0;
}

// get file size
int get_file_size(char *name, size_t *size) {
    FILE* fp = fopen(name, "rb");
    if (!fp) {
        printf("Failed to open %s for reading\n", name);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        printf("Failed to seek to end of file\n");
        return -1;
    }
    *size = ftell(fp);
    fclose(fp);
    return 0;
}

int compare_to_binary_file(char *name, void* data, size_t size) {
    // loop through data 1MB at a time
    size_t chunk_size = 1024 * 1024;
    int num_chunks = size / chunk_size;
    int read = 0;
    if (size % chunk_size != 0) {
        num_chunks++;
    }
    FILE* fp = fopen(name, "rb");
    if (!fp) {
        printf("Failed to open %s for reading\n", name);
        return -1;
    }
    void* data2 = malloc(chunk_size);
    printf("Comparing %d chunks of %ld bytes each\n", num_chunks, chunk_size);
    for (int i = 0; i < num_chunks; i++) {
        if (fseek(fp, i * chunk_size, SEEK_SET) != 0) {
            printf("Failed to seek to offset %ld\n", i * chunk_size);
            return -1;
        }
        int r = fread(data2, chunk_size, 1, fp);
        if (r < 0) {
            char str[32];
            snprintf_bytes(str, sizeof(str), chunk_size);
            printf("Failed to read %s bytes\n", str);
            return -1;
        }
        int result = memcmp(data, data2, chunk_size);
        if (result != 0) {
            printf("Mismatch at offset %d\n", read);
            return result;
        }
        read += r;
        data += r;
    }
    free(data2);
    fclose(fp);
    if ((size_t)read != size) {
        printf("Failed to read %ld bytes\n", size);
        return -1;
    }
    return 0;
}

#if defined(IOURING_SUPPORT)

#include <liburing.h>
#include <inttypes.h>
#include <string.h>
#include <sys/types.h>

#define CHUNK (1u << 20)  // 1 MiB
#define NUM_BUFFERS 4     // total buffers per file; >2

typedef struct {
    unsigned char *ptr;
    size_t cap;
    size_t len;
    int    in_use;   // reserved for an inflight/ready read
    int    ready;    // completion received
    int    chunk;    // which chunk this buffer holds
} Buf;

typedef struct {
    int   fd;
    off_t size;
    int   file_id; // 1 or 2
    int   inflight;     // number of buffers currently in use (FILLING or READY)
    int   next_submit;  // next chunk index to submit
    Buf   bufs[NUM_BUFFERS];
} FileCtx;

static inline uint64_t make_udata(int file_id, int buf_idx, int chunk) {
    // [63:56]=file_id, [55:48]=buf_idx, [47:0]=chunk
    return ((uint64_t)(file_id & 0xFF) << 56) |
           ((uint64_t)(buf_idx  & 0xFF) << 48) |
           ((uint64_t)chunk & 0x0000FFFFFFFFFFFFULL);
}

static inline void parse_udata(uint64_t u, int *file_id, int *buf_idx, int *chunk) {
    *file_id = (int)((u >> 56) & 0xFF);
    *buf_idx = (int)((u >> 48) & 0xFF);
    *chunk   = (int)(u & 0x0000FFFFFFFFFFFFULL);
}

static int find_free_buf(FileCtx *f) {
    for (int i = 0; i < NUM_BUFFERS; ++i)
        if (!f->bufs[i].in_use) return i;
    return -1;
}

static int find_ready_chunk_slot(FileCtx *f, int chunk) {
    for (int i = 0; i < NUM_BUFFERS; ++i)
        if (f->bufs[i].in_use && f->bufs[i].ready && f->bufs[i].chunk == chunk) return i;
    return -1;
}

static int submit_read(struct io_uring *ring, FileCtx *f, int chunk_idx, size_t chunk_size) {
    if (f->inflight >= NUM_BUFFERS) return 0; // no room (shouldn't happen with our policy)
    int b = find_free_buf(f);
    if (b < 0) return 0; // no buffer available

    off_t off = (off_t)chunk_idx * (off_t)chunk_size;
    size_t remaining = (off >= f->size) ? 0 : (size_t)((f->size - off) < (off_t)chunk_size ? (f->size - off) : (off_t)chunk_size);
    if (remaining == 0) return 0;

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;

    f->bufs[b].len   = remaining;
    f->bufs[b].chunk = chunk_idx;
    f->bufs[b].in_use= 1;
    f->bufs[b].ready = 0;

    io_uring_prep_read(sqe, f->fd, f->bufs[b].ptr, remaining, off);
    sqe->user_data = make_udata(f->file_id, b, chunk_idx);

    int ret = io_uring_submit(ring);
    if (ret < 0) {
        errno = -ret;
        perror("io_uring_submit");
        f->bufs[b].in_use = 0;
        return -1;
    }
    f->inflight++;
    f->next_submit = chunk_idx + 1;
    return 1;
}

static int wait_for_both_ready(struct io_uring *ring, FileCtx *f1, FileCtx *f2, int chunk) {
    while (1) {
        int s1 = find_ready_chunk_slot(f1, chunk);
        int s2 = find_ready_chunk_slot(f2, chunk);
        if (s1 >= 0 && s2 >= 0) return 0;

        struct io_uring_cqe *cqe = NULL;
        int w = io_uring_wait_cqe(ring, &cqe);
        if (w < 0) {
            errno = -w;
            perror("io_uring_wait_cqe");
            return -1;
        }
        int res = cqe->res;
        int file_id, buf_idx, cchunk;
        parse_udata(cqe->user_data, &file_id, &buf_idx, &cchunk);
        io_uring_cqe_seen(ring, cqe);

        FileCtx *fx = (file_id == 1) ? f1 : f2;
        if (res < 0) {
            errno = -res;
            perror("read (uring)");
            return -1;
        }
        // For safety: allow short reads only if it's the last chunk and matches computed size
        if ((size_t)res != fx->bufs[buf_idx].len) {
            fprintf(stderr, "Short read: expected %zu, got %d\n", fx->bufs[buf_idx].len, res);
            return -1;
        }
        fx->bufs[buf_idx].ready = 1;
    }
}

// Find first differing byte within n; returns n if equal
static size_t first_diff(const unsigned char *a, const unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return i;
    return n;
}

int compare_files(const char *file1, const char *file2) {
    int rc = -1;
    int fd1 = -1, fd2 = -1;
    struct io_uring ring;
    memset(&ring, 0, sizeof(ring));

    // Open files
    fd1 = open(file1, O_RDONLY | O_CLOEXEC);
    fd2 = open(file2, O_RDONLY | O_CLOEXEC);
    if (fd1 < 0 || fd2 < 0) { perror("open"); goto done; }

    // Stat sizes
    struct stat st1, st2;
    if (fstat(fd1, &st1) != 0 || fstat(fd2, &st2) != 0) { perror("fstat"); goto done; }
    if (!S_ISREG(st1.st_mode) || !S_ISREG(st2.st_mode)) {
        fprintf(stderr, "Both paths must be regular files\n"); goto done;
    }
    if (st1.st_size != st2.st_size) { rc = 1; goto done; }
    if (st1.st_size == 0) { rc = 0; goto done; }

    // Init ring
    if (io_uring_queue_init(2 * NUM_BUFFERS + 4, &ring, 0) != 0) { perror("io_uring_queue_init"); goto done; }

    // File contexts + buffer pools
    FileCtx f1 = {.fd = fd1, .size = st1.st_size, .file_id = 1, .inflight = 0, .next_submit = 0};
    FileCtx f2 = {.fd = fd2, .size = st2.st_size, .file_id = 2, .inflight = 0, .next_submit = 0};

    for (int i = 0; i < NUM_BUFFERS; ++i) {
        if (posix_memalign((void**)&f1.bufs[i].ptr, 4096, CHUNK) != 0 ||
            posix_memalign((void**)&f2.bufs[i].ptr, 4096, CHUNK) != 0) {
            fprintf(stderr, "OOM\n"); goto done;
        }
        f1.bufs[i].cap = f2.bufs[i].cap = CHUNK;
        f1.bufs[i].in_use = f2.bufs[i].in_use = 0;
        f1.bufs[i].ready  = f2.bufs[i].ready  = 0;
        f1.bufs[i].chunk  = f2.bufs[i].chunk  = -1;
    }

    const size_t chunk_size = CHUNK;
    const int64_t total_chunks = (st1.st_size + chunk_size - 1) / chunk_size;

    // Initial submits: keep one free buffer for prefetch (so inflight <= NUM_BUFFERS-1)
    int initial = (NUM_BUFFERS - 1);
    if (initial < 1) initial = 1; // always submit chunk 0
    int to_submit = (total_chunks < initial) ? (int)total_chunks : initial;

    for (int c = 0; c < to_submit; ++c) {
        if (submit_read(&ring, &f1, c, chunk_size) < 0) goto done;
        if (submit_read(&ring, &f2, c, chunk_size) < 0) goto done;
    }

    int64_t cur_chunk = 0;
    while (cur_chunk < total_chunks) {
        // Ensure both current chunks are ready
        if (wait_for_both_ready(&ring, &f1, &f2, (int)cur_chunk) != 0) goto done;

        int s1 = find_ready_chunk_slot(&f1, (int)cur_chunk);
        int s2 = find_ready_chunk_slot(&f2, (int)cur_chunk);
        if (s1 < 0 || s2 < 0) { fprintf(stderr, "ready slot sync error\n"); goto done; }

        unsigned char *b1 = f1.bufs[s1].ptr;
        unsigned char *b2 = f2.bufs[s2].ptr;
        size_t len = f1.bufs[s1].len;
        if (f2.bufs[s2].len != len) { fprintf(stderr, "length mismatch\n"); goto done; }

        // Two-phase compare with ~3/4 prefetch trigger
        size_t prefetch_at = len - (len / 4); // ~75%
        if (prefetch_at > len) prefetch_at = len;

        // Phase 1: compare prefix [0, prefetch_at)
        int cmp = memcmp(b1, b2, prefetch_at);
        if (cmp != 0) {
            // Find exact differing byte within the prefix
            size_t off = first_diff(b1, b2, prefetch_at);
            fprintf(stderr, "Mismatch at byte offset %jd\n",
                    (intmax_t)((off_t)cur_chunk * (off_t)chunk_size + (off_t)off));
            rc = 1;
            goto done;
        }

        // Trigger prefetch (top-up one chunk ahead) if we have room and more chunks remain
        if (f1.next_submit < total_chunks && f1.inflight < NUM_BUFFERS) {
            if (submit_read(&ring, &f1, f1.next_submit, chunk_size) < 0) goto done;
        }
        if (f2.next_submit < total_chunks && f2.inflight < NUM_BUFFERS) {
            if (submit_read(&ring, &f2, f2.next_submit, chunk_size) < 0) goto done;
        }

        // (Optional CPU prefetch of next in-order chunk buffers, if already allocated)
        if (f1.next_submit < total_chunks) __builtin_prefetch(f1.bufs[(s1+1) % NUM_BUFFERS].ptr, 1, 1);
        if (f2.next_submit < total_chunks) __builtin_prefetch(f2.bufs[(s2+1) % NUM_BUFFERS].ptr, 1, 1);

        // Phase 2: compare suffix [prefetch_at, len)
        size_t tail = len - prefetch_at;
        if (tail) {
            cmp = memcmp(b1 + prefetch_at, b2 + prefetch_at, tail);
            if (cmp != 0) {
                size_t off2 = first_diff(b1 + prefetch_at, b2 + prefetch_at, tail);
                fprintf(stderr, "Mismatch at byte offset %jd\n",
                        (intmax_t)((off_t)cur_chunk * (off_t)chunk_size + (off_t)prefetch_at + (off_t)off2));
                rc = 1;
                goto done;
            }
        }

        // Done with this chunk: free the two buffers for reuse
        f1.bufs[s1].in_use = f1.bufs[s1].ready = 0;
        f2.bufs[s2].in_use = f2.bufs[s2].ready = 0;
        f1.inflight--;
        f2.inflight--;

        // After finishing a chunk, optionally top-up so next loop has current+lookahead ready
        // Keep at most NUM_BUFFERS-1 inflight per file (leave 1 free for the next prefetch trigger).
        while (f1.inflight < (NUM_BUFFERS - 1) && f1.next_submit < total_chunks) {
            if (submit_read(&ring, &f1, f1.next_submit, chunk_size) <= 0) break;
        }
        while (f2.inflight < (NUM_BUFFERS - 1) && f2.next_submit < total_chunks) {
            if (submit_read(&ring, &f2, f2.next_submit, chunk_size) <= 0) break;
        }

        cur_chunk++;
    }

    rc = 0; // equal

done:
    if (ring.ring_fd > 0) io_uring_queue_exit(&ring);
    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);
    // Free buffers (if allocated)
    // We must not double-free; they were allocated even if ring init failed.
    // Use local loops guarded by NULL checks.
    // Note: We only have FileCtx locals if ring init succeeded; else f1/f2 are still valid zeros because of initializer use above.
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        // These NULL checks are safe even if not posix_memalign'd
        extern void free(void*);
        if (i < NUM_BUFFERS && ((void*)0) != NULL) {
            if (f1.bufs[i].ptr) free(f1.bufs[i].ptr);
            if (f2.bufs[i].ptr) free(f2.bufs[i].ptr);
        }
    }
    return rc; // 0 == equal, 1 == different, -1 == error
}

#else

int compare_files(const char *file1, const char *file2) {
    const size_t CHUNK = 1u << 20;  // 1 MiB
    int rc = -1;
    FILE *fp1 = NULL, *fp2 = NULL;
    unsigned char *buf1 = NULL, *buf2 = NULL;

    fp1 = fopen(file1, "rb");
    fp2 = fopen(file2, "rb");
    if (!fp1 || !fp2) { fprintf(stderr, "Failed to open files\n"); goto done; }

    // Get sizes using fseeko/ftello for large files
    if (fseeko(fp1, 0, SEEK_END) != 0 || fseeko(fp2, 0, SEEK_END) != 0) goto done;
    off_t size1 = ftello(fp1);
    off_t size2 = ftello(fp2);
    if (size1 < 0 || size2 < 0) goto done;

    if (size1 != size2) { rc = 1; goto done; }  // different sizes => not equal
    if (fseeko(fp1, 0, SEEK_SET) != 0 || fseeko(fp2, 0, SEEK_SET) != 0) goto done;

    buf1 = (unsigned char*)malloc(CHUNK);
    buf2 = (unsigned char*)malloc(CHUNK);
    if (!buf1 || !buf2) { fprintf(stderr, "OOM\n"); goto done; }

    off_t remaining = size1;
    off_t offset = 0;
    while (remaining > 0) {
        size_t to_read = (remaining > (off_t)CHUNK) ? CHUNK : (size_t)remaining;

        size_t n1 = fread(buf1, 1, to_read, fp1);
        size_t n2 = fread(buf2, 1, to_read, fp2);
        if (n1 != to_read || n2 != to_read) goto done;

        int cmp = memcmp(buf1, buf2, to_read);
        if (cmp != 0) {
            // Find first differing byte (optional, for diagnostics)
            size_t i = 0;
            while (i < to_read && buf1[i] == buf2[i]) ++i;
            fprintf(stderr, "Mismatch at byte offset %jd\n", (intmax_t)(offset + (off_t)i));
            rc = cmp ? cmp : 1;  // nonzero
            goto done;
        }

        remaining -= to_read;
        offset    += to_read;
    }

    rc = 0;  // equal

done:
    if (fp1) fclose(fp1);
    if (fp2) fclose(fp2);
    free(buf1);
    free(buf2);
    return rc;  // 0 == equal, 1/!=0 == different, -1 == error
}

#endif

int parse_size_bin(const char *s, uint64_t *out) {
    if (!s || !*s || !out) { errno = EINVAL; return -1; }

    char *end;
    errno = 0;
    unsigned long long base = strtoull(s, &end, 0);  // supports "0x1000", "0777", etc.
    if (errno == ERANGE) return -1;

    while (isspace((unsigned char)*end)) end++;

    uint64_t mul = 1;
    if (*end) {
        char u = *end++;
        if (u >= 'a' && u <= 'z') u -= 32; // toupper
        switch (u) {
            case 'B': mul = 1; break;
            case 'K': mul = 1024ULL; break;
            case 'M': mul = 1024ULL * 1024; break;
            case 'G': mul = 1024ULL * 1024 * 1024; break;
            case 'T': mul = 1024ULL * 1024 * 1024 * 1024; break;
            default: errno = EINVAL; return -1;
        }
        // optional trailing i/B: "MiB", "MB", "mib", etc.
        if (*end == 'i' || *end == 'I') end++;
        if (*end == 'b' || *end == 'B') end++;

        while (isspace((unsigned char)*end)) end++;
    }

    if (*end != '\0') { errno = EINVAL; return -1; }

    if (mul != 1 && base > (UINT64_MAX / mul)) { errno = ERANGE; return -1; }

    *out = (uint64_t)base * mul;
    return 0;
}

// Writes an 8-char lowercase hex string (plus NUL) into out[9].
void hash8(const void *buf, char out[9]) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t n = (1u << 20);

    uint32_t h = 2166136261u;      // FNV-1a 32-bit offset basis
    const uint32_t prime = 16777619u;

    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= prime;
    }

    // zero-padded 8 hex digits
    (void)snprintf(out, 9, "%08X", h);
}

// Strips trailing extension (text after last '.' in the basename), then splits on `delim`.
size_t split_char_strip_ext(char *s, char delim, char *out[], size_t max_out) {
    if (!s || !out || max_out == 0) return 0;

    // Find start of basename (after last path separator)
    char *base = s;
    char *slash = strrchr(s, '/');
    char *bslash = strrchr(s, '\\');            // handle Windows paths too
    if (slash && (!bslash || slash > bslash)) base = slash + 1;
    if (bslash && (!slash || bslash > slash)) base = bslash + 1;

    // Strip extension: last '.' in basename, not the first char of basename
    char *dot = strrchr(base, '.');
    if (dot && dot > base) *dot = '\0';

    if (delim == '\0') { out[0] = s; return 1; }

    char d[2] = { delim, '\0' };
    size_t count = 0;
    for (char *tok = strtok(s, d); tok && count < max_out; tok = strtok(NULL, d)) {
        out[count++] = tok;
    }
    return count;
}

static int is_dir(const char *p){ struct stat st; return stat(p,&st)==0 && S_ISDIR(st.st_mode); }
int mkdir_p(const char *p){
    if (mkdir(p, 0777) == 0) return 0;        // perms filtered by process umask
    if (errno == EEXIST && is_dir(p)) return 0;
    return -1;
}

// static int rand_bits_per_call(void) {
//     unsigned x = RAND_MAX;
//     int b = 0;
//     while (x) { b++; x >>= 1; }
//     return b;                // e.g. 15 on MSVC, 31 on glibc
// }

// uint64_t rand64(void) {
//     int b = rand_bits_per_call();
//     unsigned mask = (b >= 31) ? 0x7FFFFFFFu : ((1u << b) - 1u);

//     uint64_t r = 0;
//     int produced = 0;
//     while (produced < 64) {
//         r = (r << b) | (uint64_t)(rand() & mask);
//         produced += b;
//     }
//     if (produced > 64) r >>= (produced - 64);  // trim excess high bits
//     return r;
// }

// // unbiased [0, n) using rejection sampling
// uint64_t rand64_bounded(uint64_t n) {
//     if (n == 0) return 0;
//     uint64_t limit = UINT64_MAX - (UINT64_MAX % n);
//     uint64_t r;
//     do { r = rand64(); } while (r >= limit);
//     return r % n;
// }

int load_nodes_from_file(char *nodefilename, Node **nodes, size_t count, size_t offset) {
    if (count == 0 && nodefilename == NULL) {
        return -1;
    }
    size_t nodebytes = 0;
    if (get_file_size(nodefilename, &nodebytes) != 0) {
        printf("Failed to get file size\n");
        return -1;
    }
    size_t size_bytes = count*sizeof(Node);
    size_t offset_bytes = offset*sizeof(Node);
    if ((size_bytes+offset_bytes) > nodebytes) {
        printf("Error: node count too large for file\n");
        return -1;
    }
    int rc = load_binary(nodefilename, (void **)nodes, offset_bytes, size_bytes);
    if (rc != 0) {
        return rc;
    }
    return 0;
}

int load_nodes(char *nodefilename, char *debuglistfilename, Node **nodes, Node** insert_nodes, Node **delete_nodes, size_t** indexes, size_t *valid_count, size_t *op_count) {
    char str1[32];
    char str2[64];
    bool sorted = false;

    // A generated file (fast_nodes): N comes from -c, and a caller that hands
    // over a buffer gets the sorted originals, as it would with a list file.
    if (gen_is_file(nodefilename)) {
        if (debuglistfilename != NULL) {
            fprintf(stderr, "Error: %s is generated data; give the size with -c, not -l\n", nodefilename);
            return -1;
        }
        g_nodes_sorted = *nodes != NULL;
        return gen_load(nodefilename, valid_count, nodes, g_nodes_sorted ? GEN_SORTED : GEN_POOL,
                        insert_nodes, delete_nodes, indexes, op_count);
    }
    if (debuglistfilename != NULL) {
        // Using listfile as source of node count, so debug works
        size_t listfile_bytes = 0;
        if (get_file_size(debuglistfilename, &listfile_bytes) != 0) {
            return -1;
        }
        *valid_count = listfile_bytes / sizeof(Node);
        if (*valid_count % 2 == 0) {
            printf("Error: node count must be odd %s invalid\n", debuglistfilename);
            return -1;
        }
        sorted = *nodes != NULL;
    } else {
        if (*valid_count % 2 == 0) {
            *valid_count += 1; // rounding up to make sure we have an odd number of nodes
        }
    }

    if (*op_count == 0) {
        *op_count = *valid_count / 10;
        if (*op_count > 5000000) {
            *op_count = 5000000;
        }
    }

    long starttime = get_time_us();
    if (load_nodes_from_file(nodefilename, nodes, *valid_count, 0) != 0) {
        printf("Failed to load nodes from file\n");
        return 11;
    }
    snprintf_bytes_count(str1, sizeof(str1), *valid_count, sizeof(Node));
    snprintf(str2, sizeof(str2), "Loaded Node Pool:  %s nodes (%ldB)", str1, sizeof(Node));
    report_elapsed_time(str2, starttime);

    // prepare nodes for insertion benchmark
    //  need to be nodes that are NOT in the structures
    if (load_nodes_from_file(nodefilename, insert_nodes, *op_count, *valid_count) != 0) {
        printf("Failed to load nodes from file\n");
        return 11;
    }
    printf("  %zu Insert nodes prepared\n", *op_count);

    // prepare nodes for deletion benchmark
    //  need to be nodes that ARE in the structures
    *delete_nodes = malloc((*op_count) * sizeof(Node));
    memcpy(*delete_nodes, *nodes, (*op_count) * sizeof(Node));
    printf("  %zu Delete nodes prepared\n", *op_count);

    *indexes = malloc((*op_count) * sizeof(size_t));
    if (*indexes == NULL) {
        printf("Failed to allocate memory for indexes\n");
        return 1;
    }
    size_t i = 0;
    size_t j = 0;
    while (i < *op_count) {
        (*indexes)[i] = (*nodes)[j].value % *valid_count;
        // don't want to check first or last node, we test for those separately
        if ((*indexes)[i] < (*valid_count-1) && (*indexes)[i] > 0) {
            i++;
        }
        j++;
        if (j >= *valid_count) {
            printf("Error: Failed to find %zu valid indexes\n", *op_count);
            return 1;
        }
    }

    if (sorted) {
        starttime = get_time_us();
        if (load_nodes_from_file(debuglistfilename, nodes, *valid_count, 0) != 0) {
            printf("Failed to load nodes from file\n");
            return 11;
        }
        report_elapsed_time("  loaded sorted nodes", starttime);
    }
    g_nodes_sorted = sorted;

    return 0;
}


int validate_nodes_by_file(char* nodesfilename, char* stage, size_t count, void* context, int(*dump_nodes)(void*, const char*)) {
    if (gen_is_file(nodesfilename)) {
        fprintf(stderr, "Error: -d validates against the old list files; %s is generated data\n", nodesfilename);
        return 1;
    }
    char str1[32];
    char str2[64];
    char* fnameparts[3];
    char* tempfilename = strdup(nodesfilename);
    if (split_char_strip_ext(tempfilename, '-', fnameparts, sizeof(fnameparts)/sizeof(fnameparts[0])) != 3) {
        fprintf(stderr, "Error: Failed to split input file name\n");
        return 1;
    }
    if (count %2 != 0) {
        // hack, we're not rounding up with the filenames
        count -= 1;
    }
    char* pathname = fnameparts[0];
    char* hash = fnameparts[1];
    no_whitespace(snprintf_bytes(str1, sizeof(str1), count * sizeof(Node)), sizeof(str1));
    snprintf(str2, sizeof(str2), "-sorted-%s-%s", hash, str1);
    char* basefilename = malloc(strlen(pathname) + sizeof(str2) + 1);
    int filename_len = strlen(pathname) + sizeof(str2) + 1;
    snprintf(basefilename, filename_len, "%s%s", pathname, str2);
    free(tempfilename);
    filename_len += sizeof(str2);
    char* filename = malloc(filename_len+1);
    snprintf(filename, filename_len, "%s-%s.bin", basefilename, stage);
    free(basefilename);

    // confirm that the file exists
    if (access(filename, F_OK) != 0) {
        fprintf(stderr, "Error: File %s does not exist\n", filename);
        free(filename);
        return 1;
    }

    if (mkdir_p("data") != 0) {
        fprintf(stderr, "Error: Failed to create directory: data\n");
        return 1;
    }

    char* tempfile = "data/temp.bin";
    if (dump_nodes(context, tempfile) != 0) {
        fprintf(stderr, "Error: Failed to dump nodes to file %s\n", filename);
        free(filename);
        return 1;
    }

    if (compare_files(filename, tempfile) != 0) {
        fprintf(stderr, "Error: Files %s and %s do not match\n", filename, tempfile);
        free(filename);
        return 1;
    }

    // delete tempfile
    unlink(tempfile);

    free(filename);
    printf("Nodes validated\n");
    return 0;
}


///// CPU PINNING /////////////////////////////////

static int ht_sibling(int cpu) {
    char path[128];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int a = -1, b = -1;
    int n = fscanf(f, "%d,%d", &a, &b);
    fclose(f);
    return n == 2 ? (a == cpu ? b : a) : -1;
}

static int pin_thread(pthread_t tid, int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(tid, sizeof set, &set);
}

int pin_main_and_workers(const pthread_t* workers, int num_workers) {
    const int main_cpu = 0, sibling = ht_sibling(main_cpu);
    const int ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (pin_thread(pthread_self(), main_cpu) != 0) perror("pin main");
    printf("affinity: main cpu %d, sibling %d idle, workers", main_cpu, sibling);
    int cpu = main_cpu, pinned = 0;
    for (int i = 0; i < num_workers; i++) {
        do cpu++; while (cpu < ncpus && cpu == sibling);
        if (cpu >= ncpus) { printf(" (out of cpus at worker %d)", i); break; }
        if (pin_thread(workers[i], cpu) == 0) pinned++; else perror("pin worker");
        printf(" %d", cpu);
    }
    printf("\n");
    return pinned;
}
