#ifndef GEN_NODES_H
#define GEN_NODES_H

// Generated node data: one sorted file of random values that serves every
// size, plus inserts, deletes and index picks derived from its seed.
//
// The file is a random subset of the integers: each integer is kept with
// probability 1/4, decided by 2 bits of a ChaCha20 keystream keyed by the seed
// (both bits zero = kept).  Kept integers are written in order, so the file is
// sorted and unique by construction, and its first N values are themselves a
// random subset of [1, v_N) for every N.  A benchmark of size N uses
//   originals   the first N values
//   deletes     N's pool order is a seeded permutation of the originals and
//               the deletes are its first ops entries, as with create_nodes' pool
//   inserts     integers below v_N that the keystream did not keep, drawn
//               uniformly (exactly the distribution of random new values)
//   indexes     uniform in [1, N - 2]
//   clustered   (g_clustered_nodes) ops integers absent from both the
//               originals and the inserts, counting up from a random original:
//               one narrow key range, in increasing order (a sorted batch, or
//               time-ordered keys landing in one region)
// Everything reads the file O_DIRECT so the page cache never holds it.
// See create_nodes/fast_nodes.c for the writer.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "benchmark_helpers.h"

#define GEN_MAGIC "BFNODES1"
#define GEN_HEADER_SIZE 4096
#define GEN_ALIGN 4096                          // O_DIRECT alignment
#define GEN_CHUNK_BITS 26                       // integers per keystream chunk: 2^26
#define GEN_CHUNK_INTS (1ull << GEN_CHUNK_BITS)

typedef struct {
    char magic[8];
    uint64_t seed;
    uint64_t count;       // values in the file
    uint64_t max_value;   // the last one
    uint32_t chunk_bits;
} gen_header_t;

// what gen_load hands back for the originals
typedef enum {
    GEN_NONE,    // nothing: the caller streams them with gen_stream_*
    GEN_SORTED,  // sorted, into *nodes (allocated if NULL)
    GEN_POOL,    // in pool order (a random permutation), allocated if NULL
} gen_mode_t;

bool gen_is_file(const char* path);
int gen_read_header(const char* path, gen_header_t* h);

// The keystream behind the file: how many integers chunk c keeps, and fill out
// with their values (out must hold gen_chunk_count() nodes).
uint64_t gen_chunk_count(uint64_t seed, uint64_t chunk);
uint64_t gen_chunk_fill(uint64_t seed, uint64_t chunk, Node* out);
bool gen_is_kept(uint64_t seed, uint64_t value);

// Keys for the clustered-insert phase, set by gen_load (count 0 when the
// data did not come from a generated file).
extern Node* g_clustered_nodes;
extern size_t g_clustered_count;

// load_nodes for a generated file.  *valid_count is the requested N (rounded
// up to odd like load_nodes); *op_count 0 means min(N / 10, 5M).
int gen_load(const char* path, size_t* valid_count, Node** nodes, gen_mode_t mode,
             Node** insert_nodes, Node** delete_nodes, size_t** indexes, size_t* op_count);

// The first n originals in order, O_DIRECT and double-buffered by a reader
// thread: each gen_stream_next returns the next `count` nodes contiguously
// (count <= GEN_STREAM_MAX_NEXT).
#define GEN_STREAM_MAX_NEXT (64 * 1024)
typedef struct gen_stream gen_stream_t;
gen_stream_t* gen_stream_open(const char* path, size_t n);
const void* gen_stream_next(void* stream, size_t count);
void gen_stream_close(gen_stream_t* s);

#endif
