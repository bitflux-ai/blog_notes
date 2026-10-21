#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>

int main(int argc, char *argv[]) {
    char* filename = argv[1];
    size_t size_bytes = strtoull(argv[2], NULL, 10);
    size_t total_ints = size_bytes / sizeof(int);
    size_t count = 0;

    int fd = open(filename, O_RDONLY);
    int* data = (int*)mmap(NULL, size_bytes, PROT_READ, MAP_SHARED, fd, 0);
 
    for (size_t i = 0; i < total_ints; ++i) {
        if (data[i] == 10) count++;
    }

    printf("Found %ld 10s\n", count);
}