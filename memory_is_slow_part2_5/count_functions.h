#ifndef COUNT_FUNCTIONS_H
#define COUNT_FUNCTIONS_H

/*
 * The count functions every benchmark in this directory times: how many
 * int32 10s are in a buffer.  Shared by in_ram.c (whole data set in DRAM,
 * or a slice held in L2/L3) and diskbased/benchmark.c (blocks arriving from
 * NVMe-oF).  Buffers are 64-byte aligned; sizes are whole int32s.
 *
 * The AVX-512 functions carry a target attribute so this header compiles
 * without -march=native; GCC then emits them as out-of-line calls.
 */
#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#define AVX512_TARGET __attribute__((target("avx512f")))

typedef size_t (*count_fn_t)(const void* data, size_t bytes);

/* The obvious loop: branch per element.  What a first draft looks like. */
static inline size_t count_simple(const void* data, size_t bytes) {
    const int* p = data;
    const size_t total = bytes / sizeof(int);
    size_t count = 0;
    for (size_t i = 0; i < total; i++)
        if (p[i] == 10) count++;
    return count;
}

/* Branchless with 16 independent accumulators; the compiler vectorizes it
   as far as the enabled ISA allows (AVX2 by default, AVX-512 with -march=native). */
static inline size_t count_unrolled(const void* data, size_t bytes) {
    const int* p = __builtin_assume_aligned(data, 64);
    const size_t total = bytes / sizeof(int);
    uint64_t c[16] = {0};
    size_t i = 0;
    for (; i + 16 <= total; i += 16)
        for (int k = 0; k < 16; k++)
            c[k] += (unsigned)(p[i + k] == 10);
    size_t count = 0;
    for (int k = 0; k < 16; k++) count += c[k];
    for (; i < total; i++) count += (unsigned)(p[i] == 10);
    return count;
}

/* AVX-512F: compare 16 ints to a mask, then a masked subtract keeps the tally
   in the vector unit (one uop per 16 ints).  Cascade Lake has no VPOPCNTDQ. */
static inline AVX512_TARGET size_t count_avx512(const void* data, size_t bytes) {
    const int* p = __builtin_assume_aligned(data, 64);
    const size_t total = bytes / sizeof(int);
    const __m512i ten = _mm512_set1_epi32(10), one = _mm512_set1_epi32(1);
    __m512i a0 = _mm512_setzero_si512(), a1 = a0, a2 = a0, a3 = a0;
    size_t i = 0;
    for (; i + 64 <= total; i += 64) {
        a0 = _mm512_mask_sub_epi32(a0, _mm512_cmpeq_epi32_mask(_mm512_load_si512(p + i), ten), a0, one);
        a1 = _mm512_mask_sub_epi32(a1, _mm512_cmpeq_epi32_mask(_mm512_load_si512(p + i + 16), ten), a1, one);
        a2 = _mm512_mask_sub_epi32(a2, _mm512_cmpeq_epi32_mask(_mm512_load_si512(p + i + 32), ten), a2, one);
        a3 = _mm512_mask_sub_epi32(a3, _mm512_cmpeq_epi32_mask(_mm512_load_si512(p + i + 48), ten), a3, one);
    }
    __m512i sum = _mm512_add_epi32(_mm512_add_epi32(a0, a1), _mm512_add_epi32(a2, a3));
    size_t count = (size_t)(uint32_t)(-_mm512_reduce_add_epi32(sum));
    for (; i < total; i++) count += (unsigned)(p[i] == 10);
    return count;
}

/* Same loads as count_avx512 with no compare: what the core pays just to
   read the bytes.  Returns a checksum so the loads cannot be optimized away. */
static inline AVX512_TARGET size_t touch_block(const void* data, size_t bytes) {
    const __m512i* p = __builtin_assume_aligned(data, 64);
    const size_t n = bytes / sizeof(__m512i);
    __m512i a0 = _mm512_setzero_si512(), a1 = a0, a2 = a0, a3 = a0;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        a0 = _mm512_xor_si512(a0, _mm512_load_si512(p + i));
        a1 = _mm512_xor_si512(a1, _mm512_load_si512(p + i + 1));
        a2 = _mm512_xor_si512(a2, _mm512_load_si512(p + i + 2));
        a3 = _mm512_xor_si512(a3, _mm512_load_si512(p + i + 3));
    }
    for (; i < n; i++) a0 = _mm512_xor_si512(a0, _mm512_load_si512(p + i));
    a0 = _mm512_xor_si512(_mm512_xor_si512(a0, a1), _mm512_xor_si512(a2, a3));
    return (size_t)_mm_cvtsi128_si64(_mm512_castsi512_si128(a0));
}

/* Never reads the buffer: delivery cost only. */
static inline size_t skip_block(const void* data, size_t bytes) {
    (void)data; (void)bytes;
    return 0;
}

typedef struct {
    const char* name;
    count_fn_t fn;
} count_function_t;

static const count_function_t COUNT_FUNCTIONS[] = {
    { "simple",   count_simple },
    { "unrolled", count_unrolled },
    { "avx512",   count_avx512 },
    { "touch",    touch_block },
    { "none",     skip_block },
};
#define NUM_COUNT_FUNCTIONS (sizeof COUNT_FUNCTIONS / sizeof COUNT_FUNCTIONS[0])

static inline count_fn_t count_function_by_name(const char* name) {
    for (size_t i = 0; i < NUM_COUNT_FUNCTIONS; i++)
        if (strcmp(COUNT_FUNCTIONS[i].name, name) == 0) return COUNT_FUNCTIONS[i].fn;
    return NULL;
}

static inline long get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000L + tv.tv_usec;
}

#endif
