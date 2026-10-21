#ifndef IO_ENGINE_H
#define IO_ENGINE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Striped O_DIRECT reader built for DDIO: one io_uring issuer thread per
 * worker, a small fixed pool of registered buffers per worker, and a single
 * consumer (the caller's thread) that drains completed buffers.  The pool is
 * the only DMA target, so keeping workers * depth * block_size under the L3
 * size keeps the data the NIC just wrote resident for the consumer.
 *
 * Layout: logical block b lives on drive b % num_drives at byte offset
 * (b / num_drives) * block_size.  Worker w is expected to be handed blocks
 * with b % num_workers == w; with num_workers a multiple of num_drives that
 * keeps each ring on one drive.
 *
 * Main-thread protocol per worker (all single-producer/single-consumer):
 *   while (ioengine_issue(na, w, block)) ...   // false when all depth slots are busy
 *   while ((buf = ioengine_poll(na, w, &block))) { use(buf); ioengine_release(na, w, buf); }
 */
typedef struct ioengine ioengine_t;

/* A single path is opened O_CREAT and ftruncated to size_bytes if it is a
   plain file.  Several paths are opened as-is and striped.  Blocks until the
   worker threads have registered their rings, or returns NULL. */
ioengine_t* ioengine_open(const char* const* paths, int num_paths, size_t size_bytes,
                          int block_size, int num_workers, int depth);
void ioengine_close(ioengine_t* na);

bool  ioengine_issue(ioengine_t* na, int worker, size_t block);
void* ioengine_poll(ioengine_t* na, int worker, size_t* block);
void  ioengine_release(ioengine_t* na, int worker, void* buf);

int       ioengine_num_workers(const ioengine_t* na);
int       ioengine_num_drives(const ioengine_t* na);
int       ioengine_block_size(const ioengine_t* na);
int       ioengine_depth(const ioengine_t* na);
size_t    ioengine_size(const ioengine_t* na);
pthread_t ioengine_worker_thread(const ioengine_t* na, int worker);

#endif
