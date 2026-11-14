// The growable output buffer (lzfse_zazzcat_buffer, declared in the public header) and its append helpers.
#ifndef LZFSE_ZAZZCAT_BUFFER_H
#define LZFSE_ZAZZCAT_BUFFER_H

#include "common.h"

// Make room for at least `extra` more bytes after `buffer->length`, growing the capacity geometrically.
// Returns false when memory runs out (the buffer is left unchanged).
bool buffer_reserve(lzfse_zazzcat_buffer *buffer, size_t extra);

// Append `count` bytes; returns false when memory runs out.
bool buffer_append(lzfse_zazzcat_buffer *buffer, const void *bytes, size_t count);

// Append `count` bytes into capacity the caller has already reserved.
ALWAYS_INLINE void buffer_append_reserved(lzfse_zazzcat_buffer *buffer, const void *bytes, size_t count) {
    copy_bytes(buffer->data + buffer->length, bytes, count);
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
