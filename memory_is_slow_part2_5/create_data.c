#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>

int main(int argc, char *argv[]) {
    const char* path = argv[1];
    size_t size_bytes = strtoull(argv[2], NULL, 10);
    printf("Creating %s with random data\n", path);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); exit(1); }

    const size_t BS = 4 * 1024 * 1024;
    void *buf = malloc(BS);
    if (!buf) { perror("malloc"); exit(1); }
    memset(buf, 0, BS);

    // fill-and-write loop
    uint64_t remaining = size_bytes;
    while (remaining) {
        size_t chunk = remaining < BS ? (size_t)remaining : BS;

        // fill buffer with pseudorandom bytes
        int* p = (int*)buf;
        for (size_t i = 0; i < chunk/sizeof(int); i += sizeof(int)) {
            p[i] = rand() % 20;
        }

        // write fully
        size_t off = 0;
        while (off < chunk) {
            ssize_t w = write(fd, buf + off, chunk - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                perror("write"); free(buf); close(fd); exit(1);
            }
            off += (size_t)w;
        }
        remaining -= chunk;
    }

    if (fsync(fd) < 0) perror("fsync");
    close(fd);
    free(buf);
    printf("Wrote %" PRIu64 " bytes to %s\n", size_bytes, path);
}

