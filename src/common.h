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

// Multi-value loads and stores for fields that sit next to each other. On a little-endian CPU an array of
// integers has exactly the byte layout of the integers written one after another, so a single copy does it.

// Load three consecutive 16-bit little-endian integers (6 bytes).
ALWAYS_INLINE void load_u16_x3(const uint8_t *at, uint16_t *first, uint16_t *second, uint16_t *third) {
    uint16_t values[3];
    memcpy(values, at, sizeof values);
    *first = values[0];
    *second = values[1];
    *third = values[2];
}

// Store three consecutive 16-bit little-endian integers (6 bytes).
ALWAYS_INLINE void store_u16_x3(uint8_t *at, uint16_t first, uint16_t second, uint16_t third) {
    const uint16_t values[3] = {first, second, third};
    memcpy(at, values, sizeof values);
}

// Store two consecutive 32-bit little-endian integers (8 bytes).
ALWAYS_INLINE void store_u32_x2(uint8_t *at, uint32_t first, uint32_t second) {
    const uint32_t values[2] = {first, second};
    memcpy(at, values, sizeof values);
}

// Store three consecutive 32-bit little-endian integers (12 bytes).
ALWAYS_INLINE void store_u32_x3(uint8_t *at, uint32_t first, uint32_t second, uint32_t third) {
    const uint32_t values[3] = {first, second, third};
    memcpy(at, values, sizeof values);
}

// Store three consecutive 64-bit little-endian integers (24 bytes).
ALWAYS_INLINE void store_u64_x3(uint8_t *at, uint64_t first, uint64_t second, uint64_t third) {
    const uint64_t values[3] = {first, second, third};
    memcpy(at, values, sizeof values);
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

// Append two bytes into reserved capacity. Use this (or the 3-byte version) instead of several
// buffer_push_reserved calls: a store through a `uint8_t *` is allowed to change any object, `buffer` included,
// so after every one-byte push the compiler must reload `data` and `length`. Here they are read once.
ALWAYS_INLINE void buffer_push_2_reserved(lzfse_zazzcat_buffer *buffer, uint8_t first, uint8_t second) {
    size_t length = buffer->length;
    uint8_t *at = buffer->data + length;
    at[0] = first;
    at[1] = second;
    buffer->length = length + 2;
}

// Append three bytes into reserved capacity (see buffer_push_2_reserved).
ALWAYS_INLINE void buffer_push_3_reserved(lzfse_zazzcat_buffer *buffer, uint8_t first, uint8_t second, uint8_t third) {
    size_t length = buffer->length;
    uint8_t *at = buffer->data + length;
    at[0] = first;
    at[1] = second;
    at[2] = third;
    buffer->length = length + 3;
}

// Append a little-endian 32-bit integer into reserved capacity.
ALWAYS_INLINE void buffer_push_u32_reserved(lzfse_zazzcat_buffer *buffer, uint32_t value) {
    store_u32(buffer->data + buffer->length, value);
    buffer->length += 4;
}

// Append two little-endian 32-bit integers into reserved capacity.
ALWAYS_INLINE void buffer_push_u32_x2_reserved(lzfse_zazzcat_buffer *buffer, uint32_t first, uint32_t second) {
    store_u32_x2(buffer->data + buffer->length, first, second);
    buffer->length += 8;
}

// Append a little-endian 64-bit integer into reserved capacity.
ALWAYS_INLINE void buffer_push_u64_reserved(lzfse_zazzcat_buffer *buffer, uint64_t value) {
    store_u64(buffer->data + buffer->length, value);
    buffer->length += 8;
}

#endif
