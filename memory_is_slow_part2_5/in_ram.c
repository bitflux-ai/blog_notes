/*
 * Count the 10s in a data set held entirely in DRAM, once per count function,
 * then again on slices that fit in L2 and in L3 to show what the same core
 * does when the data is already in cache.
 */
#include "count_functions.h"

#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void report(const char* label, size_t tens, size_t bytes, long elapsed_us) {
    double seconds = (double)elapsed_us / 1e6;
    double gib = (double)bytes / (1 << 30);
    printf("%s %zu 10s processed at %0.2f GiB/s\n", label, tens, gib / seconds);
    printf("%s %zu 10s processed at %0.2f MiB/s\n", label, tens, gib * 1024.0 / seconds);
}

/* Repeat count_avx512 on a buffer that fits in one cache level.  Spin first so
   the core reaches its AVX-512 clock, then time a fixed window. */
static void bench_resident(const char* level, size_t bytes, const void* src, size_t src_bytes) {
    void* buf;
    if (posix_memalign(&buf, 4096, bytes) != 0) {
        fprintf(stderr, "out of memory\n");
        return;
    }
    for (size_t off = 0; off < bytes; off += src_bytes) {
        size_t n = bytes - off < src_bytes ? bytes - off : src_bytes;
        memcpy((char*)buf + off, src, n);
    }

    volatile size_t warmup = 0;
    long until = get_time_us() + 300000L;
    while (get_time_us() < until)
        warmup += count_avx512(buf, bytes);

    int reps = 0;
    size_t tens = 0;
    long start = get_time_us();
    until = start + 200000L;
    do {
        tens += count_avx512(buf, bytes);
        reps++;
    } while (get_time_us() < until);
    long elapsed = get_time_us() - start;

    char label[64];
    snprintf(label, sizeof label, "avx512 %s (%zu KiB)", level, bytes / 1024);
    report(label, tens / (size_t)reps, bytes * (size_t)reps, elapsed);
    free(buf);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <datafile> <size_bytes>\n", argv[0]);
        return 2;
    }
    const char* filename = argv[1];
    size_t size_bytes = strtoull(argv[2], NULL, 10);

    int fd = open(filename, O_RDONLY | O_DIRECT);
    if (fd < 0) { perror(filename); return 1; }
    void* data;
    if (posix_memalign(&data, 4096, size_bytes) != 0) { fprintf(stderr, "out of memory\n"); return 1; }
    for (size_t off = 0; off < size_bytes;) {
        ssize_t n = read(fd, (char*)data + off, size_bytes - off);
        if (n <= 0) { perror("read"); return 1; }
        off += (size_t)n;
    }
    close(fd);

    /* Stay on one core: the L2 run below needs that core's own 1 MiB L2. */
    cpu_set_t set;
    CPU_ZERO(&set);
    int cpu = sched_getcpu();
    if (cpu >= 0) {
        CPU_SET(cpu, &set);
        sched_setaffinity(0, sizeof set, &set);
    }

    for (size_t i = 0; i < NUM_COUNT_FUNCTIONS; i++) {
        if (COUNT_FUNCTIONS[i].fn == skip_block) continue;
        long start = get_time_us();
        size_t tens = COUNT_FUNCTIONS[i].fn(data, size_bytes);
        long elapsed = get_time_us() - start;
        char label[64];
        snprintf(label, sizeof label, "%s loop", COUNT_FUNCTIONS[i].name);
        if (COUNT_FUNCTIONS[i].fn == touch_block)   /* returns a checksum, not a count */
            tens = 0;
        report(label, tens, size_bytes, elapsed);
    }

    /* 512 KiB misses L1 (32 KiB) and fits in L2 (1 MiB).  16 MiB misses L2 and fits in L3 (35.75 MiB). */
    bench_resident("L2", 512UL << 10, data, size_bytes);
    bench_resident("L3", 16UL << 20, data, size_bytes);
    free(data);
    return 0;
}
