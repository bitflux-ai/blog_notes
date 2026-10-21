/*
 * Stream a striped data set off NVMe-oF drives and count the int32 10s in it
 * on one core.  With DDIO the RDMA writes land in L3, so the consumer core
 * reads them at L3 speed instead of DRAM speed - as long as the buffer pool
 * (workers * depth * block) stays comfortably inside the L3.
 */
#include "io_engine.h"
#include "targets.h"
#include "../count_functions.h"

#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_BLOCK_SIZE (256 * 1024)
#define DEFAULT_DEPTH 8

/* ---- cpu pinning --------------------------------------------------------- */

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

/* The consumer gets cpu 0 and its hyperthread sibling stays idle; workers
   take the following cpus, one each. */
static void pin_threads(ioengine_t* na) {
    const int main_cpu = 0, sibling = ht_sibling(main_cpu);
    const int ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (pin_thread(pthread_self(), main_cpu) != 0) perror("pin main");
    printf("affinity: main cpu %d, sibling %d idle, workers", main_cpu, sibling);
    int cpu = main_cpu;
    for (int i = 0; i < ioengine_num_workers(na); i++) {
        do cpu++; while (cpu < ncpus && cpu == sibling);
        if (cpu >= ncpus) { printf(" (out of cpus at worker %d)", i); break; }
        if (pin_thread(ioengine_worker_thread(na, i), cpu) != 0) perror("pin worker");
        printf(" %d", cpu);
    }
    printf("\n");
}

static double l3_bytes(void) {
    FILE* f = fopen("/sys/devices/system/cpu/cpu0/cache/index3/size", "r");
    if (!f) return 0;
    double v = 0;
    char unit = 0;
    int n = fscanf(f, "%lf%c", &v, &unit);
    fclose(f);
    if (n < 1) return 0;
    return unit == 'M' ? v * (1 << 20) : unit == 'K' ? v * 1024 : v;
}

/* ---- writing the data set onto the drives -------------------------------- */

/* Block i -> drive i % n at offset (i / n) * bs, the layout the reader uses.
   Each pass reads n*RUN blocks, de-interleaves, and writes one run per drive. */
#define LOAD_RUN 64
static int load_datafile(const char* datafile, const char* const* paths, int nd, size_t bs, size_t total_blocks) {
    int rc = -1, src = open(datafile, O_RDONLY);
    if (src < 0) { perror(datafile); return -1; }
    int* fds = calloc((size_t)nd, sizeof(int));
    size_t chunk = (size_t)nd * LOAD_RUN;
    char* in = malloc(chunk * bs);
    char* out = aligned_alloc(4096, LOAD_RUN * bs);
    if (!fds || !in || !out) { fprintf(stderr, "out of memory\n"); goto done; }
    for (int d = 0; d < nd; d++) {
        fds[d] = open(paths[d], O_WRONLY | O_DIRECT);
        if (fds[d] < 0) { perror(paths[d]); goto done; }
    }
    for (size_t b0 = 0; b0 < total_blocks; b0 += chunk) {
        size_t n = total_blocks - b0 < chunk ? total_blocks - b0 : chunk;
        if (pread(src, in, n * bs, (off_t)(b0 * bs)) != (ssize_t)(n * bs)) { perror("read datafile"); goto done; }
        for (int d = 0; d < nd; d++) {
            size_t cnt = 0;
            for (size_t k = (size_t)d; k < n; k += (size_t)nd) memcpy(out + cnt++ * bs, in + k * bs, bs);
            if (cnt && pwrite(fds[d], out, cnt * bs, (off_t)(b0 / (size_t)nd * bs)) != (ssize_t)(cnt * bs)) {
                perror(paths[d]); goto done;
            }
        }
    }
    rc = 0;
done:
    for (int d = 0; fds && d < nd; d++) if (fds[d] > 0) close(fds[d]);
    free(fds); free(in); free(out); close(src);
    return rc;
}

/* ---- main ---------------------------------------------------------------- */

static void usage(const char* prog) {
    fprintf(stderr,
        "usage: %s [options] -t targets.yml <size_bytes>      read <size_bytes> striped across the drives\n"
        "       %s [options] <file_or_disk> <size_bytes>      single file or disk\n"
        "       %s -t targets.yml -l datafile -y              write datafile onto the drives (DESTROYS their contents)\n"
        "options:\n"
        "  -c simple|unrolled|avx512|touch|none   count function run on each block (default avx512)\n"
        "  -w N   workers per drive (default 1)\n"
        "  -q N   queue depth = buffers per worker (default %d)\n"
        "  -b N   block size in bytes, multiple of 4096 (default %d)\n",
        prog, prog, prog, DEFAULT_DEPTH, DEFAULT_BLOCK_SIZE);
}

int main(int argc, char* argv[]) {
    const char* targets_yml = NULL;
    const char* datafile = NULL;
    const char* count_name = "avx512";
    int yes = 0, workers_per_drive = 1, depth = DEFAULT_DEPTH, block_size = DEFAULT_BLOCK_SIZE, opt;
    while ((opt = getopt(argc, argv, "t:l:yc:w:q:b:")) != -1) {
        switch (opt) {
        case 't': targets_yml = optarg; break;
        case 'l': datafile = optarg; break;
        case 'y': yes = 1; break;
        case 'c': count_name = optarg; break;
        case 'w': workers_per_drive = atoi(optarg); break;
        case 'q': depth = atoi(optarg); break;
        case 'b': block_size = atoi(optarg); break;
        default: usage(argv[0]); return 2;
        }
    }
    count_fn_t count_fn = count_function_by_name(count_name);
    if (!count_fn || workers_per_drive < 1 || depth < 1 || block_size < 4096 || block_size % 4096 ||
        (datafile && !targets_yml)) {
        usage(argv[0]);
        return 2;
    }

    /* Which drives */
    target_drive_t* drives = NULL;
    int num_drives = 1;
    const char** paths;
    if (targets_yml) {
        num_drives = targets_read(targets_yml, &drives);
        if (num_drives < 1) { fprintf(stderr, "no drives found in %s\n", targets_yml); return 1; }
        if (targets_resolve(drives, num_drives) != 0) return 1;
        paths = malloc((size_t)num_drives * sizeof *paths);
        for (int i = 0; i < num_drives; i++) paths[i] = drives[i].path;
    } else {
        if (argc - optind != 2) { usage(argv[0]); return 2; }
        paths = malloc(sizeof *paths);
        paths[0] = argv[optind++];
    }

    /* How many bytes */
    size_t size_bytes;
    if (datafile) {
        struct stat st;
        if (stat(datafile, &st) != 0) { perror(datafile); return 1; }
        size_bytes = (size_t)st.st_size;
    } else {
        if (argc - optind != 1) { usage(argv[0]); return 2; }
        size_bytes = strtoull(argv[optind], NULL, 10);
    }
    size_t bs = (size_t)block_size;
    size_t total_blocks = size_bytes / bs;

    /* Blocks per drive */
    size_t share = total_blocks / (size_t)num_drives;
    size_t expect = share * (size_t)num_drives;
    for (int d = 0; drives && d < num_drives; d++) {
        printf("  [%d] %s  %s  %.2f GiB\n", d, drives[d].path, drives[d].nqn,
               (double)(share * bs) / (1 << 30));
        if (share * bs > drives[d].size_bytes) {
            fprintf(stderr, "%s is too small (%llu bytes) for %zu bytes\n",
                    drives[d].path, (unsigned long long)drives[d].size_bytes, share * bs);
            return 1;
        }
    }

    if (datafile) {
        if (!yes) {
            fprintf(stderr, "Loading OVERWRITES the striped prefix of each drive above. Re-run with -y to proceed.\n");
            return 1;
        }
        int rc = load_datafile(datafile, paths, num_drives, bs, total_blocks);
        if (rc == 0) printf("loaded %s (%zu blocks)\n", datafile, total_blocks);
        return rc ? 1 : 0;
    }

    int workers = num_drives * workers_per_drive;
    ioengine_t* na = ioengine_open(paths, num_drives, size_bytes, block_size, workers, depth);
    if (!na) return 1;
    double pool_mib = (double)workers * depth * (double)bs / (1 << 20);
    printf("%d drive(s), %d workers, depth %d, %d-byte blocks, %.2f MiB pool (L3 %.2f MiB), count %s\n",
           num_drives, workers, depth, block_size, pool_mib, l3_bytes() / (1 << 20), count_name);
    pin_threads(na);

    /* Worker w = d + k*num_drives reads only drive d: blocks w, w+W, w+2W, ... */
    size_t* quota = calloc((size_t)workers, sizeof *quota);
    size_t* issued = calloc((size_t)workers, sizeof *issued);
    size_t* next_block = calloc((size_t)workers, sizeof *next_block);
    for (int d = 0; d < num_drives; d++)
        for (int k = 0; k < workers_per_drive; k++) {
            int w = d + k * num_drives;
            quota[w] = share / (size_t)workers_per_drive + ((size_t)k < share % (size_t)workers_per_drive);
            next_block[w] = (size_t)w;
        }

    size_t count = 0, done = 0, block;
    uint64_t busy_tsc = 0, tsc0 = __rdtsc();
    long start = get_time_us();
    while (done < expect) {
        for (int w = 0; w < workers; w++) {
            void* buf;
            while ((buf = ioengine_poll(na, w, &block))) {
                uint64_t t = __rdtsc();
                count += count_fn(buf, bs);
                busy_tsc += __rdtsc() - t;
                ioengine_release(na, w, buf);
                done++;
                /* refill the slot right away so the drive never waits on this drain */
                if (issued[w] < quota[w] && ioengine_issue(na, w, next_block[w])) {
                    next_block[w] += (size_t)workers;
                    issued[w]++;
                }
            }
            while (issued[w] < quota[w] && ioengine_issue(na, w, next_block[w])) {
                next_block[w] += (size_t)workers;
                issued[w]++;
            }
        }
    }
    double seconds = (double)(get_time_us() - start) / 1e6;
    double busy = (double)busy_tsc / (double)(__rdtsc() - tsc0);   /* share of the run spent inside count_fn() */
    size_t bytes = expect * bs;
    double gib = (double)bytes / (1 << 30);
    printf("result: count=%s bytes=%zu seconds=%.3f GiB/s=%.2f busy=%.2f busyGiB/s=%.2f tens=%zu\n",
           count_name, bytes, seconds, gib / seconds, busy, busy > 0 ? gib / (seconds * busy) : 0.0, count);

    ioengine_close(na);
    free(quota); free(issued); free(next_block); free(paths); free(drives);
    return 0;
}
