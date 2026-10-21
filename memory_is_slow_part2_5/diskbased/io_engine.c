#include "io_engine.h"

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_LINE 64
#define HUGE_PAGE (2u << 20)

/* Requests carry the block index and the buffer slot in one word. */
#define REQ_SLOT_BITS 16
#define REQ_MAKE(block, slot) (((uint64_t)(block) << REQ_SLOT_BITS) | (uint64_t)(slot))
#define REQ_SLOT(e)  ((unsigned)((e) & ((1u << REQ_SLOT_BITS) - 1)))
#define REQ_BLOCK(e) ((size_t)((e) >> REQ_SLOT_BITS))

enum { WORKER_STARTING = 0, WORKER_READY = 1, WORKER_FAILED = -1 };

/* Fields are grouped by who writes them so the two threads never share a
   cache line that either of them writes. */
typedef struct {
    /* main writes, worker reads */
    _Alignas(CACHE_LINE) _Atomic uint32_t req_tail;
    _Atomic bool shutdown;
    uint64_t* req;              /* [depth] filled before req_tail is bumped */

    /* worker writes, main reads */
    _Alignas(CACHE_LINE) _Atomic uint32_t cpl_tail;
    _Atomic int status;
    uint32_t* cpl;              /* [depth] completed slots */

    /* main only */
    _Alignas(CACHE_LINE) uint32_t req_count;
    uint32_t cpl_head;
    int nfree;
    uint32_t* free_slots;       /* stack of idle slots */
    size_t* slot_block;         /* [depth] block each busy slot is reading */

    /* worker only */
    _Alignas(CACHE_LINE) uint32_t req_head;
    uint32_t cpl_count;
    int inflight;
    int id;
    struct io_uring ring;
    pthread_t tid;
    struct ioengine* na;
    char* buffers;              /* depth * block_size, inside na->pool */
} worker_t;

struct ioengine {
    size_t size_bytes;
    int block_size;
    int depth;
    int num_drives;
    int num_workers;
    int* fds;
    char* pool;
    size_t pool_bytes;
    worker_t* workers;
};

static void* alloc_lines(size_t bytes) {
    size_t rounded = (bytes + CACHE_LINE - 1) / CACHE_LINE * CACHE_LINE;
    void* p = aligned_alloc(CACHE_LINE, rounded);
    if (p) memset(p, 0, rounded);
    return p;
}

/* ---- worker thread ------------------------------------------------------ */

static int worker_setup(worker_t* w) {
    ioengine_t* na = w->na;
    int entries = na->depth < 8 ? 8 : na->depth;
    int ret = io_uring_queue_init((unsigned)entries, &w->ring, IORING_SETUP_SINGLE_ISSUER);
    if (ret < 0) {
        fprintf(stderr, "worker %d: io_uring_queue_init: %s\n", w->id, strerror(-ret));
        return ret;
    }
    io_uring_register_ring_fd(&w->ring);

    ret = io_uring_register_files(&w->ring, na->fds, (unsigned)na->num_drives);
    if (ret < 0) {
        fprintf(stderr, "worker %d: io_uring_register_files: %s\n", w->id, strerror(-ret));
        goto fail;
    }

    struct iovec* iov = calloc((size_t)na->depth, sizeof *iov);
    if (!iov) { ret = -ENOMEM; goto fail; }
    for (int s = 0; s < na->depth; s++) {
        iov[s].iov_base = w->buffers + (size_t)s * (size_t)na->block_size;
        iov[s].iov_len = (size_t)na->block_size;
    }
    ret = io_uring_register_buffers(&w->ring, iov, (unsigned)na->depth);
    free(iov);
    if (ret < 0) {
        fprintf(stderr, "worker %d: io_uring_register_buffers: %s\n", w->id, strerror(-ret));
        goto fail;
    }
    return 0;
fail:
    io_uring_queue_exit(&w->ring);
    return ret;
}

/* Turn new requests into SQEs.  Never more than depth outstanding, so the
   ring (>= depth entries) cannot run out. */
static int worker_submit(worker_t* w) {
    ioengine_t* na = w->na;
    uint32_t tail = atomic_load_explicit(&w->req_tail, memory_order_acquire);
    int n = 0;
    while (w->req_head != tail) {
        uint64_t e = w->req[w->req_head % (uint32_t)na->depth];
        unsigned slot = REQ_SLOT(e);
        size_t block = REQ_BLOCK(e);
        int drive = (int)(block % (size_t)na->num_drives);
        off_t offset = (off_t)(block / (size_t)na->num_drives) * na->block_size;

        struct io_uring_sqe* sqe = io_uring_get_sqe(&w->ring);
        io_uring_prep_read_fixed(sqe, drive, w->buffers + (size_t)slot * (size_t)na->block_size,
                                 (unsigned)na->block_size, offset, (int)slot);
        sqe->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data64(sqe, slot);
        w->req_head++;
        n++;
    }
    if (n) {
        int ret = io_uring_submit(&w->ring);
        if (ret < 0) {
            fprintf(stderr, "worker %d: io_uring_submit: %s\n", w->id, strerror(-ret));
            exit(1);
        }
        w->inflight += n;
    }
    return n;
}

static int worker_reap(worker_t* w) {
    ioengine_t* na = w->na;
    struct io_uring_cqe* cqe;
    unsigned head, n = 0;
    io_uring_for_each_cqe(&w->ring, head, cqe) {
        if (cqe->res != na->block_size) {
            fprintf(stderr, "worker %d: read of slot %llu returned %d (%s)\n", w->id,
                    (unsigned long long)cqe->user_data, cqe->res,
                    cqe->res < 0 ? strerror(-cqe->res) : "short read");
            exit(1);
        }
        w->cpl[(w->cpl_count + n) % (uint32_t)na->depth] = (uint32_t)cqe->user_data;
        n++;
    }
    if (n) {
        io_uring_cq_advance(&w->ring, n);
        w->inflight -= (int)n;
        w->cpl_count += n;
        atomic_store_explicit(&w->cpl_tail, w->cpl_count, memory_order_release);
    }
    return (int)n;
}

static void* worker_main(void* arg) {
    worker_t* w = arg;
    if (worker_setup(w) != 0) {
        atomic_store(&w->status, WORKER_FAILED);
        return NULL;
    }
    atomic_store(&w->status, WORKER_READY);

    for (;;) {
        int busy = worker_submit(w) + worker_reap(w);
        if (busy) continue;
        if (w->inflight == 0 && atomic_load_explicit(&w->shutdown, memory_order_acquire))
            break;
        __builtin_ia32_pause();
    }
    io_uring_queue_exit(&w->ring);
    return NULL;
}

/* ---- main-thread API ---------------------------------------------------- */

bool ioengine_issue(ioengine_t* na, int worker, size_t block) {
    worker_t* w = &na->workers[worker];
    if (w->nfree == 0) return false;
    uint32_t slot = w->free_slots[--w->nfree];
    w->slot_block[slot] = block;
    w->req[w->req_count % (uint32_t)na->depth] = REQ_MAKE(block, slot);
    w->req_count++;
    atomic_store_explicit(&w->req_tail, w->req_count, memory_order_release);
    return true;
}

void* ioengine_poll(ioengine_t* na, int worker, size_t* block) {
    worker_t* w = &na->workers[worker];
    if (w->cpl_head == atomic_load_explicit(&w->cpl_tail, memory_order_acquire))
        return NULL;
    uint32_t slot = w->cpl[w->cpl_head % (uint32_t)na->depth];
    w->cpl_head++;
    *block = w->slot_block[slot];
    return w->buffers + (size_t)slot * (size_t)na->block_size;
}

void ioengine_release(ioengine_t* na, int worker, void* buf) {
    worker_t* w = &na->workers[worker];
    uint32_t slot = (uint32_t)(((char*)buf - w->buffers) / (size_t)na->block_size);
    w->free_slots[w->nfree++] = slot;
}

int       ioengine_num_workers(const ioengine_t* na) { return na->num_workers; }
int       ioengine_num_drives(const ioengine_t* na)  { return na->num_drives; }
int       ioengine_block_size(const ioengine_t* na)  { return na->block_size; }
int       ioengine_depth(const ioengine_t* na)       { return na->depth; }
size_t    ioengine_size(const ioengine_t* na)        { return na->size_bytes; }
pthread_t ioengine_worker_thread(const ioengine_t* na, int worker) { return na->workers[worker].tid; }

/* ---- setup / teardown --------------------------------------------------- */

void ioengine_close(ioengine_t* na) {
    if (!na) return;
    if (na->workers) {
        for (int i = 0; i < na->num_workers; i++)
            atomic_store_explicit(&na->workers[i].shutdown, true, memory_order_release);
        for (int i = 0; i < na->num_workers; i++) {
            worker_t* w = &na->workers[i];
            if (w->tid) pthread_join(w->tid, NULL);
            free(w->req);
            free(w->cpl);
            free(w->free_slots);
            free(w->slot_block);
        }
        free(na->workers);
    }
    if (na->fds) {
        for (int i = 0; i < na->num_drives; i++) close(na->fds[i]);
        free(na->fds);
    }
    if (na->pool) munmap(na->pool, na->pool_bytes);
    free(na);
}

static int open_drives(ioengine_t* na, const char* const* paths, int num_paths) {
    na->fds = malloc((size_t)num_paths * sizeof(int));
    if (!na->fds) return -1;
    int flags = O_RDWR | O_DIRECT | (num_paths == 1 ? O_CREAT : 0);
    for (int i = 0; i < num_paths; i++) {
        na->fds[i] = open(paths[i], flags, 0644);
        if (na->fds[i] < 0) {
            fprintf(stderr, "open %s: %s\n", paths[i], strerror(errno));
            return -1;
        }
        na->num_drives = i + 1;
    }
    if (num_paths == 1) {
        /* Grow a plain file to size if needed; never shrink it, and leave block devices alone. */
        struct stat st;
        if (fstat(na->fds[0], &st) != 0) return -1;
        if (S_ISREG(st.st_mode) && (size_t)st.st_size < na->size_bytes &&
            ftruncate(na->fds[0], (off_t)na->size_bytes) < 0) {
            fprintf(stderr, "ftruncate %s: %s\n", paths[0], strerror(errno));
            return -1;
        }
    }
    return 0;
}

ioengine_t* ioengine_open(const char* const* paths, int num_paths, size_t size_bytes,
                          int block_size, int num_workers, int depth) {
    if (num_paths < 1 || num_workers < 1 || depth < 1 || depth >= (1 << REQ_SLOT_BITS) ||
        block_size < 4096 || block_size % 4096) {
        fprintf(stderr, "ioengine_open: bad parameters\n");
        return NULL;
    }
    ioengine_t* na = calloc(1, sizeof *na);
    if (!na) return NULL;
    na->size_bytes = size_bytes;
    na->block_size = block_size;
    na->depth = depth;
    na->num_workers = num_workers;

    if (open_drives(na, paths, num_paths) != 0) goto fail;

    /* One pool for every worker's buffers, on huge pages where available. */
    na->pool_bytes = (size_t)num_workers * (size_t)depth * (size_t)block_size;
    na->pool_bytes = (na->pool_bytes + HUGE_PAGE - 1) / HUGE_PAGE * HUGE_PAGE;
    na->pool = mmap(NULL, na->pool_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (na->pool == MAP_FAILED) {
        na->pool = NULL;
        fprintf(stderr, "mmap %zu bytes: %s\n", na->pool_bytes, strerror(errno));
        goto fail;
    }
    madvise(na->pool, na->pool_bytes, MADV_HUGEPAGE);
    memset(na->pool, 0, na->pool_bytes);

    na->workers = alloc_lines((size_t)num_workers * sizeof(worker_t));
    if (!na->workers) goto fail;
    for (int i = 0; i < num_workers; i++) {
        worker_t* w = &na->workers[i];
        w->id = i;
        w->na = na;
        w->buffers = na->pool + (size_t)i * (size_t)depth * (size_t)block_size;
        w->req = alloc_lines((size_t)depth * sizeof *w->req);
        w->cpl = alloc_lines((size_t)depth * sizeof *w->cpl);
        w->free_slots = alloc_lines((size_t)depth * sizeof *w->free_slots);
        w->slot_block = alloc_lines((size_t)depth * sizeof *w->slot_block);
        if (!w->req || !w->cpl || !w->free_slots || !w->slot_block) goto fail;
        for (int s = 0; s < depth; s++) w->free_slots[s] = (uint32_t)(depth - 1 - s);
        w->nfree = depth;
    }
    for (int i = 0; i < num_workers; i++) {
        worker_t* w = &na->workers[i];
        int ret = pthread_create(&w->tid, NULL, worker_main, w);
        if (ret != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(ret));
            goto fail;
        }
    }
    for (int i = 0; i < num_workers; i++) {
        int st;
        while ((st = atomic_load(&na->workers[i].status)) == WORKER_STARTING)
            usleep(100);
        if (st != WORKER_READY) goto fail;
    }
    return na;
fail:
    ioengine_close(na);
    return NULL;
}
