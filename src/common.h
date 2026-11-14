// Shared helpers for every source file: integer types, compiler hints, little-endian loads and stores, memory
// allocation (with a hook the allocation tests use) and the growable byte buffer.
#ifndef LZFSE_ZAZZCAT_COMMON_H
#define LZFSE_ZAZZCAT_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lzfse_zazzcat.h"

// `static inline` functions are copied into each file that includes this header; ALWAYS_INLINE asks the compiler
// to paste the body into every caller, which hot loops rely on.
#define ALWAYS_INLINE static inline __attribute__((always_inline))
// Keep a function out of line (its callers stay small).
#define NOINLINE __attribute__((noinline))
// Mark a function as rarely called, so the compiler moves it (and the code that calls it) out of hot paths.
#define COLD __attribute__((cold, noinline))
// Branch hints: the condition is almost always true (LIKELY) or false (UNLIKELY).
#define LIKELY(condition)   __builtin_expect(!!(condition), 1)
#define UNLIKELY(condition) __builtin_expect(!!(condition), 0)

// Smaller and larger of two sizes.
ALWAYS_INLINE size_t min_size(size_t a, size_t b) {
    return a < b ? a : b;
}

// Larger of two sizes.
ALWAYS_INLINE size_t max_size(size_t a, size_t b) {
    return a > b ? a : b;
}

// Round `value / divisor` up.
ALWAYS_INLINE size_t div_ceil(size_t value, size_t divisor) {
    return value / divisor + (value % divisor != 0);
}

// Loads and stores of unaligned little-endian integers. `memcpy` with a constant size compiles to one load or
// store instruction; reading through a cast pointer instead would be undefined behaviour in C.
ALWAYS_INLINE uint16_t load_u16(const uint8_t *at) {
    uint16_t value;
    memcpy(&value, at, sizeof value);
    return value;
}

// Load 4 bytes as a little-endian integer.
ALWAYS_INLINE uint32_t load_u32(const uint8_t *at) {
    uint32_t value;
    memcpy(&value, at, sizeof value);
    return value;
}

// Load 8 bytes as a little-endian integer.
ALWAYS_INLINE uint64_t load_u64(const uint8_t *at) {
    uint64_t value;
    memcpy(&value, at, sizeof value);
    return value;
}

// Store a 16-bit little-endian integer.
ALWAYS_INLINE void store_u16(uint8_t *at, uint16_t value) {
    memcpy(at, &value, sizeof value);
}

// Store a 32-bit little-endian integer.
ALWAYS_INLINE void store_u32(uint8_t *at, uint32_t value) {
    memcpy(at, &value, sizeof value);
}

// Store a 64-bit little-endian integer.
ALWAYS_INLINE void store_u64(uint8_t *at, uint64_t value) {
    memcpy(at, &value, sizeof value);
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "lzfse-zazzcat assumes a little-endian CPU (Apple Silicon, x86-64, other little-endian ARM)"
#endif

// Memory allocation. The allocation tests build the library with LZFSE_ZAZZCAT_COUNT_ALLOCATIONS, which routes
// every allocation through counting functions they define; normal builds call the C library directly.
#ifdef LZFSE_ZAZZCAT_COUNT_ALLOCATIONS
void *counted_malloc(size_t size);
void *counted_calloc(size_t count, size_t size);
void *counted_realloc(void *pointer, size_t size);
#define zz_malloc  counted_malloc
#define zz_calloc  counted_calloc
#define zz_realloc counted_realloc
#else
#define zz_malloc  malloc
#define zz_calloc  calloc
#define zz_realloc realloc
#endif
#define zz_free free

// Make room for at least `extra` more bytes after `buffer->length`, growing the capacity geometrically.
// Returns false when memory runs out (the buffer is left unchanged).
bool buffer_reserve(lzfse_zazzcat_buffer *buffer, size_t extra);

// Append `count` bytes; returns false when memory runs out.
bool buffer_append(lzfse_zazzcat_buffer *buffer, const void *bytes, size_t count);

// Append `count` bytes into capacity the caller has already reserved.
ALWAYS_INLINE void buffer_append_reserved(lzfse_zazzcat_buffer *buffer, const void *bytes, size_t count) {
    memcpy(buffer->data + buffer->length, bytes, count);
    buffer->length += count;
}

// Append one byte into reserved capacity.
ALWAYS_INLINE void buffer_push_reserved(lzfse_zazzcat_buffer *buffer, uint8_t byte) {
    buffer->data[buffer->length++] = byte;
}

// Append a little-endian 32-bit integer into reserved capacity.
ALWAYS_INLINE void buffer_push_u32_reserved(lzfse_zazzcat_buffer *buffer, uint32_t value) {
    store_u32(buffer->data + buffer->length, value);
    buffer->length += 4;
}

#endif
