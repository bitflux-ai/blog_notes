#include "gen_nodes.h"
#include <openssl/evp.h>
#include <sys/mman.h>

#define KEEP_MASK 0x5555555555555555ull
#define PIECE_BYTES (64 * 1024)                     // keystream generated per step
#define CHUNK_BYTES (GEN_CHUNK_INTS / 4)             // 2 bits per integer
#define BLOCK_BYTES (64ull << 20)                    // O_DIRECT read size

static inline uint64_t splitmix64(uint64_t* s) {
    uint64_t x = (*s += 0x9e3779b97f4a7c15ull);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Scratch memory straight from mmap: going through malloc, the loader's big
// temporaries raised glibc's dynamic mmap threshold and moved where the
// benchmark's own allocations land (AVL search was 18% slower).
static void* scratch_alloc(size_t bytes) {
    void* p = mmap(NULL, bytes ? bytes : 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static void scratch_free(void* p, size_t bytes) { if (p) munmap(p, bytes ? bytes : 1); }

// ── the keystream ─────────────────────────────────────────────────────

// ChaCha20 keyed by the seed, nonce = chunk, counter = 64-byte block
static void keystream_init(EVP_CIPHER_CTX* ctx, uint64_t seed, uint64_t chunk, uint32_t block) {
    uint8_t key[32], iv[16];
    uint64_t s = seed;
    for (int i = 0; i < 4; i++) {
        uint64_t k = splitmix64(&s);
        memcpy(key + 8 * i, &k, 8);
    }
    memcpy(iv, &block, 4);
    memcpy(iv + 4, &chunk, 8);
    memset(iv + 12, 0, 4);
    EVP_EncryptInit_ex(ctx, EVP_chacha20(), NULL, key, iv);
}

static const uint8_t g_zeros[PIECE_BYTES];

// run body for every 64-bit keystream word of a chunk, as (word_index, word)
#define FOR_EACH_WORD(seed, chunk, body)                                        \
    do {                                                                        \
        EVP_CIPHER_CTX* ctx_ = EVP_CIPHER_CTX_new();                            \
        keystream_init(ctx_, (seed), (chunk), 0);                               \
        uint64_t piece_[PIECE_BYTES / 8];                                       \
        for (uint64_t base_ = 0; base_ < CHUNK_BYTES / 8; base_ += PIECE_BYTES / 8) { \
            int outl_;                                                          \
            EVP_EncryptUpdate(ctx_, (uint8_t*)piece_, &outl_, g_zeros, PIECE_BYTES); \
            for (uint64_t i_ = 0; i_ < PIECE_BYTES / 8; i_++) {                 \
                uint64_t word_index = base_ + i_, word = piece_[i_];            \
                body                                                            \
            }                                                                   \
        }                                                                       \
        EVP_CIPHER_CTX_free(ctx_);                                              \
    } while (0)

uint64_t gen_chunk_count(uint64_t seed, uint64_t chunk) {
    uint64_t n = 0;
    FOR_EACH_WORD(seed, chunk, {
        (void)word_index;
        n += __builtin_popcountll(~(word | (word >> 1)) & KEEP_MASK);
    });
    return n;
}

// values are integer + 1, so 0 never appears
uint64_t gen_chunk_fill(uint64_t seed, uint64_t chunk, Node* out) {
    uint64_t n = 0, first = chunk << GEN_CHUNK_BITS;
    FOR_EACH_WORD(seed, chunk, {
        uint64_t m = ~(word | (word >> 1)) & KEEP_MASK;
        uint64_t at = first + word_index * 32 + 1;
        while (m) {
            out[n++].value = at + (__builtin_ctzll(m) >> 1);
            m &= m - 1;
        }
    });
    return n;
}

bool gen_is_kept(uint64_t seed, uint64_t value) {
    uint64_t i = value - 1, chunk = i >> GEN_CHUNK_BITS, j = i & (GEN_CHUNK_INTS - 1);
    uint64_t byte = (j >> 5) * 8;
    uint64_t block[8];
    int outl;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    keystream_init(ctx, seed, chunk, (uint32_t)(byte / 64));
    EVP_EncryptUpdate(ctx, (uint8_t*)block, &outl, g_zeros, 64);
    EVP_CIPHER_CTX_free(ctx);
    uint64_t word = block[(byte % 64) / 8];
    return ((word >> (2 * (j & 31))) & 3) == 0;
}

// ── the file, O_DIRECT ────────────────────────────────────────────────

int gen_read_header(const char* path, gen_header_t* h) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = pread(fd, h, sizeof(*h), 0);
    close(fd);
    if (n != (ssize_t)sizeof(*h) || memcmp(h->magic, GEN_MAGIC, 8) != 0) return -1;
    return 0;
}

bool gen_is_file(const char* path) {
    gen_header_t h;
    return path && gen_read_header(path, &h) == 0;
}

// read aligned [off, off + bytes) into an aligned buffer; short only at EOF
static int read_direct(int fd, void* buf, size_t bytes, off_t off) {
    size_t done = 0;
    while (done < bytes) {
        ssize_t rc = pread(fd, (char*)buf + done, bytes - done, off + (off_t)done);
        if (rc < 0) { perror("pread O_DIRECT"); return -1; }
        if (rc == 0) break;
        done += (size_t)rc;
    }
    return 0;
}

static inline off_t node_offset(uint64_t i) { return (off_t)(GEN_HEADER_SIZE + i * sizeof(Node)); }

// nodes [first, first + n) into out (any alignment) through an aligned bounce block
static int read_nodes(int fd, uint64_t first, uint64_t n, Node* out, void* bounce) {
    off_t start = node_offset(first), end = node_offset(first + n);
    off_t at = start & ~(off_t)(GEN_ALIGN - 1);
    char* dst = (char*)out;
    while (at < end) {
        size_t want = (size_t)(end - at + GEN_ALIGN - 1) & ~(size_t)(GEN_ALIGN - 1);
        if (want > BLOCK_BYTES) want = BLOCK_BYTES;
        if (read_direct(fd, bounce, want, at) != 0) return -1;
        off_t from = at < start ? start : at, to = at + (off_t)want < end ? at + (off_t)want : end;
        memcpy(dst, (char*)bounce + (from - at), (size_t)(to - from));
        dst += to - from;
        at += (off_t)want;
    }
    return 0;
}

// ── picks ─────────────────────────────────────────────────────────────

// a 4-round Feistel permutation of [0, n), cycle-walked: the pool order
typedef struct { uint64_t n, mask, key; int half; } perm_t;

static perm_t perm_new(uint64_t n, uint64_t key) {
    perm_t p = {n, 1, key, 1};
    while ((1ull << (2 * p.half)) < n) p.half++;
    p.mask = (1ull << p.half) - 1;
    return p;
}

static inline uint64_t perm(const perm_t* p, uint64_t x) {
    if (p->n <= 1) return 0;
    do {
        uint64_t l = x >> p->half, r = x & p->mask;
        for (uint64_t round = 0; round < 4; round++) {
            uint64_t s = r ^ p->key ^ (round << 56);
            uint64_t t = l ^ (splitmix64(&s) & p->mask);
            l = r;
            r = t;
        }
        x = (l << p->half) | r;
    } while (x >= p->n);
    return x;
}

// xoshiro256** for the picks
typedef struct { uint64_t s[4]; } rng_t;

static rng_t rng_new(uint64_t seed) {
    rng_t r;
    for (int i = 0; i < 4; i++) r.s[i] = splitmix64(&seed);
    return r;
}

static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static inline uint64_t rng_next(rng_t* r) {
    uint64_t* s = r->s;
    uint64_t out = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t; s[3] = rotl(s[3], 45);
    return out;
}

// uniform in [0, n)
static inline uint64_t rng_below(rng_t* r, uint64_t n) {
    uint64_t limit = UINT64_MAX - UINT64_MAX % n, x;
    do { x = rng_next(r); } while (x >= limit);
    return x % n;
}

typedef struct { uint64_t rank, slot; } pick_t;

// LSD radix sort of n records of `size` bytes by the uint64 at their start.
// Not qsort: glibc's qsort mallocs a merge buffer, and freeing a 27 MB one
// raised the dynamic mmap threshold and changed the benchmark's own heap.
static void radix_sort(void* data, size_t n, size_t size) {
    size_t bytes = n * size;
    char *a = data, *b = scratch_alloc(bytes);
    for (int shift = 0; shift < 64; shift += 8) {
        size_t count[257] = {0};
        for (size_t i = 0; i < n; i++) count[((*(uint64_t*)(a + i * size) >> shift) & 0xff) + 1]++;
        if (count[1 + ((*(uint64_t*)a >> shift) & 0xff)] == n) continue;  // all one digit
        for (int d = 0; d < 256; d++) count[d + 1] += count[d];
        for (size_t i = 0; i < n; i++) {
            char* rec = a + i * size;
            memcpy(b + count[(*(uint64_t*)rec >> shift) & 0xff]++ * size, rec, size);
        }
        char* t = a; a = b; b = t;
    }
    if (a != data) {
        memcpy(data, a, bytes);
        b = a;
    }
    scratch_free(b, bytes);
}

#define PICK_THREADS 32
#define PICK_SPAN (1u << 20)   // picks this close share one read

// values at sorted ranks, split over threads (queue depth for sparse picks);
// nearby picks coalesce into one read of up to PICK_SPAN bytes
typedef struct { const char* path; const pick_t* picks; Node* out; size_t from, to; int error; } pick_job_t;

static void* pick_worker(void* arg) {
    pick_job_t* j = arg;
    int fd = open(j->path, O_RDONLY | O_DIRECT);
    void* buf = scratch_alloc(PICK_SPAN + 2 * GEN_ALIGN);
    if (fd < 0 || !buf) { j->error = 1; goto out; }
    for (size_t i = j->from; i < j->to;) {
        off_t at = node_offset(j->picks[i].rank) & ~(off_t)(GEN_ALIGN - 1);
        size_t k = i;
        while (k < j->to && node_offset(j->picks[k].rank) + (off_t)sizeof(Node) <= at + PICK_SPAN) k++;
        off_t end = node_offset(j->picks[k - 1].rank) + (off_t)sizeof(Node);
        size_t bytes = (size_t)(end - at + GEN_ALIGN - 1) & ~(size_t)(GEN_ALIGN - 1);
        if (read_direct(fd, buf, bytes, at) != 0) { j->error = 1; break; }
        for (; i < k; i++) j->out[j->picks[i].slot] = *(Node*)((char*)buf + (node_offset(j->picks[i].rank) - at));
    }
out:
    if (fd >= 0) close(fd);
    scratch_free(buf, PICK_SPAN + 2 * GEN_ALIGN);
    return NULL;
}

static int read_picks(const char* path, const pick_t* picks, size_t n, Node* out) {
    pthread_t t[PICK_THREADS];
    pick_job_t jobs[PICK_THREADS];
    int threads = n < PICK_THREADS ? 1 : PICK_THREADS, error = 0;
    for (int i = 0; i < threads; i++) {
        jobs[i] = (pick_job_t){path, picks, out, n * i / threads, n * (i + 1) / threads, 0};
        pthread_create(&t[i], NULL, pick_worker, &jobs[i]);
    }
    for (int i = 0; i < threads; i++) { pthread_join(t[i], NULL); error |= jobs[i].error; }
    return error ? -1 : 0;
}

// the pool gather, split over threads: out[j] = sorted[perm(j)]
typedef struct { const Node* sorted; Node* out; const perm_t* p; uint64_t from, to; } gather_t;

static void* gather(void* arg) {
    gather_t* g = arg;
    for (uint64_t j = g->from; j < g->to; j++) g->out[j] = g->sorted[perm(g->p, j)];
    return NULL;
}

int gen_load(const char* path, size_t* valid_count, Node** nodes, gen_mode_t mode,
             Node** insert_nodes, Node** delete_nodes, size_t** indexes, size_t* op_count) {
    char str1[32], str2[128];
    gen_header_t h;
    if (gen_read_header(path, &h) != 0) {
        fprintf(stderr, "Error: %s is not a generated node file\n", path);
        return -1;
    }
    size_t n = *valid_count | 1;  // odd, for a definitive median
    if (n > h.count) {
        fprintf(stderr, "Error: %s holds %lu nodes, %zu requested\n", path, h.count, n);
        return -1;
    }
    *valid_count = n;
    if (*op_count == 0) *op_count = n / 10 > 5000000 ? 5000000 : n / 10;
    size_t ops = *op_count;

    long starttime = get_time_us();
    perm_t pool = perm_new(n, h.seed ^ 0x706f6f6c6f726472ull);
    rng_t rng = rng_new(h.seed ^ 0x7069636b73ull);

    // deletes: the first ops of the pool order, read in file order
    size_t picks_bytes = (ops + 1) * sizeof(pick_t);
    pick_t* picks = scratch_alloc(picks_bytes);
    *delete_nodes = malloc(ops * sizeof(Node));
    for (size_t j = 0; j < ops; j++) picks[j] = (pick_t){perm(&pool, j), j};
    radix_sort(picks, ops, sizeof(pick_t));
    // and the last original, the top of the insert range
    Node top_node;
    picks[ops] = (pick_t){n - 1, 0};
    if (read_picks(path, picks, ops, *delete_nodes) != 0 || read_picks(path, picks + ops, 1, &top_node) != 0) {
        fprintf(stderr, "Error: failed to read %s\n", path);
        return -1;
    }
    scratch_free(picks, picks_bytes);
    printf("  %zu Delete nodes prepared\n", ops);

    // inserts: uniform over the integers in [1, v_N) the generator did not keep
    uint64_t top = top_node.value;
    size_t want = ops + ops / 8 + 64, got = 0, cand_bytes = want * sizeof(uint64_t);
    uint64_t* cand = scratch_alloc(cand_bytes);
    for (;;) {
        while (got < want) {
            uint64_t v = 1 + rng_below(&rng, top - 1);
            if (!gen_is_kept(h.seed, v)) cand[got++] = v;
        }
        radix_sort(cand, got, sizeof(uint64_t));
        size_t u = 0;
        for (size_t i = 0; i < got; i++)
            if (u == 0 || cand[i] != cand[u - 1]) cand[u++] = cand[i];
        got = u;
        if (got >= ops) break;
    }
    // sorted -> random order, then the first ops
    for (size_t i = got - 1; i > 0; i--) {
        size_t j = rng_below(&rng, i + 1);
        uint64_t t = cand[i]; cand[i] = cand[j]; cand[j] = t;
    }
    *insert_nodes = malloc(ops * sizeof(Node));
    for (size_t i = 0; i < ops; i++) (*insert_nodes)[i].value = cand[i];
    scratch_free(cand, cand_bytes);
    printf("  %zu Insert nodes prepared\n", ops);

    // index picks: anything but the first and last
    *indexes = malloc(ops * sizeof(size_t));
    for (size_t i = 0; i < ops; i++) (*indexes)[i] = 1 + rng_below(&rng, n - 2);

    if (mode != GEN_NONE) {
        if (*nodes == NULL) *nodes = malloc(n * sizeof(Node));
        if (!*nodes) {
            fprintf(stderr, "Error: failed to allocate %zu nodes\n", n);
            return -1;
        }
        int fd = open(path, O_RDONLY | O_DIRECT);
        void* bounce = scratch_alloc(BLOCK_BYTES);
        // pool order needs the sorted originals first, then a gather
        size_t sorted_bytes = mode == GEN_POOL ? n * sizeof(Node) : 0;
        Node* sorted = mode == GEN_POOL ? scratch_alloc(sorted_bytes) : *nodes;
        if (fd < 0 || !bounce || !sorted || read_nodes(fd, 0, n, sorted, bounce) != 0) {
            fprintf(stderr, "Error: failed to read %s\n", path);
            return -1;
        }
        close(fd);
        scratch_free(bounce, BLOCK_BYTES);
        if (mode == GEN_POOL) {
            int threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
            pthread_t t[threads];
            gather_t g[threads];
            for (int i = 0; i < threads; i++) {
                g[i] = (gather_t){sorted, *nodes, &pool, n * i / threads, n * (i + 1) / threads};
                pthread_create(&t[i], NULL, gather, &g[i]);
            }
            for (int i = 0; i < threads; i++) pthread_join(t[i], NULL);
            scratch_free(sorted, sorted_bytes);
        }
    }
    snprintf_bytes_count(str1, sizeof(str1), n, sizeof(Node));
    snprintf(str2, sizeof(str2), "Generated nodes (seed %08lX, %s):  %s nodes (%ldB)", h.seed,
             mode == GEN_NONE ? "streamed later" : mode == GEN_SORTED ? "sorted" : "pool order", str1, sizeof(Node));
    report_elapsed_time(str2, starttime);
    return 0;
}

// ── streaming the originals ───────────────────────────────────────────

// Two blocks, each with GEN_STREAM_MAX_NEXT nodes of room in front: when a
// request runs off the end of one block, its tail is copied in front of the
// next so the request comes back contiguous.  The reader thread fills the
// block the consumer is not using.
#define PAD_BYTES (GEN_STREAM_MAX_NEXT * sizeof(Node))

struct gen_stream {
    int fd;
    uint64_t n;              // nodes in the stream
    char* mem[2];            // PAD_BYTES + BLOCK_BYTES each
    uint64_t first[2];       // node index at the start of each block's data
    uint64_t count[2];       // nodes in it
    bool ready[2];
    int cur;                 // the block the consumer reads
    uint64_t pos;            // next node within it
    uint64_t next_first;     // the next block for the reader
    bool stop, error;
    pthread_t reader;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

static void* stream_reader(void* arg) {
    gen_stream_t* s = arg;
    for (int b = 0;; b ^= 1) {
        pthread_mutex_lock(&s->lock);
        while (s->ready[b] && !s->stop) pthread_cond_wait(&s->cond, &s->lock);
        uint64_t first = s->next_first;
        bool done = s->stop || first >= s->n;
        pthread_mutex_unlock(&s->lock);
        if (done) break;
        uint64_t count = s->n - first < BLOCK_BYTES / sizeof(Node) ? s->n - first : BLOCK_BYTES / sizeof(Node);
        // blocks start on node indexes that are multiples of BLOCK_BYTES / 8, so
        // node_offset(first) is aligned: the header is GEN_ALIGN bytes
        size_t bytes = (count * sizeof(Node) + GEN_ALIGN - 1) & ~(size_t)(GEN_ALIGN - 1);
        int rc = read_direct(s->fd, s->mem[b] + PAD_BYTES, bytes, node_offset(first));
        pthread_mutex_lock(&s->lock);
        s->first[b] = first;
        s->count[b] = count;
        s->ready[b] = true;
        s->error |= rc != 0;
        s->next_first = first + count;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
    }
    return NULL;
}

gen_stream_t* gen_stream_open(const char* path, size_t n) {
    gen_header_t h;
    if (gen_read_header(path, &h) != 0 || n > h.count) return NULL;
    gen_stream_t* s = calloc(1, sizeof(*s));
    s->fd = open(path, O_RDONLY | O_DIRECT);
    s->n = n;
    for (int b = 0; b < 2; b++) s->mem[b] = scratch_alloc(PAD_BYTES + BLOCK_BYTES);
    if (s->fd < 0 || !s->mem[0] || !s->mem[1]) {
        perror(path);
        return NULL;
    }
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    pthread_create(&s->reader, NULL, stream_reader, s);
    return s;
}

static void stream_wait(gen_stream_t* s, int b) {
    pthread_mutex_lock(&s->lock);
    while (!s->ready[b]) pthread_cond_wait(&s->cond, &s->lock);
    pthread_mutex_unlock(&s->lock);
    if (s->error) {
        fprintf(stderr, "Error: O_DIRECT read of the node stream failed\n");
        exit(1);
    }
}

const void* gen_stream_next(void* stream, size_t count) {
    gen_stream_t* s = stream;
    if (count > GEN_STREAM_MAX_NEXT) {
        fprintf(stderr, "gen_stream_next: %zu > %d nodes\n", count, GEN_STREAM_MAX_NEXT);
        exit(1);
    }
    stream_wait(s, s->cur);
    char* data = s->mem[s->cur] + PAD_BYTES;
    uint64_t left = s->count[s->cur] - s->pos;
    if (count <= left) {
        const void* p = data + s->pos * sizeof(Node);
        s->pos += count;
        return p;
    }
    // the tail of this block goes in front of the next one, then hand this block back
    int nb = s->cur ^ 1;
    stream_wait(s, nb);
    char* next = s->mem[nb] + PAD_BYTES;
    memcpy(next - left * sizeof(Node), data + s->pos * sizeof(Node), left * sizeof(Node));
    pthread_mutex_lock(&s->lock);
    s->ready[s->cur] = false;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    s->cur = nb;
    s->pos = count - left;
    return next - left * sizeof(Node);
}

void gen_stream_close(gen_stream_t* s) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->stop = true;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->reader, NULL);
    close(s->fd);
    for (int b = 0; b < 2; b++) scratch_free(s->mem[b], PAD_BYTES + BLOCK_BYTES);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    free(s);
}
