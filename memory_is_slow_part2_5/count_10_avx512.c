#include "count_functions.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

// count_10_avx512: count_10_unrolled's mmap read, counted with the same
// AVX-512 kernel in_ram and diskbased use
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <datafile> <size_bytes>\n", argv[0]);
        return 2;
    }
    char* filename = argv[1];
    size_t size_bytes = strtoull(argv[2], NULL, 10);

    int fd = open(filename, O_RDONLY);
    if (fd < 0) { perror(filename); return 1; }
    void* buffer = mmap(NULL, size_bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (buffer == MAP_FAILED) { perror("mmap"); return 1; }

    long start = get_time_us();
    size_t count = count_avx512(buffer, size_bytes);
    long elapsed = get_time_us() - start;

    printf("avx512 loop found %ld 10s processed at %0.2f GiB/s\n", count, ((double)size_bytes/1073741824)/((double)elapsed/1.0e6));
    return 0;
}
