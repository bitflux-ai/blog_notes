#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>

long get_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000L + tv.tv_usec;
}

// count_10_loop
int main(int argc, char *argv[]) {
    char* filename = argv[1];
    size_t size_bytes = strtoull(argv[2], NULL, 10);
    size_t total_ints = size_bytes / sizeof(int);
    size_t count = 0;

    int fd = open(filename, O_RDONLY);
    int* data = (int*)mmap(NULL, size_bytes, PROT_READ, MAP_SHARED, fd, 0);
 
    long start = get_time_us();
    for (size_t i = 0; i < total_ints; ++i) {
        if (data[i] == 10) count++;
    }
    long elapsed = get_time_us() - start;

    printf("simple loop found %ld 10s processed at %0.2f GiB/s\n", count, ((double)size_bytes/1073741824)/((double)elapsed/1.0e6));
}