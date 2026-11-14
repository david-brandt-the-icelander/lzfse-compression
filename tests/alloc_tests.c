// Allocation behaviour of each entry point. The library is built with LZFSE_ZAZZCAT_COUNT_ALLOCATIONS, which
// sends its allocations through the counting functions below. Reused encoders and decoders must not allocate at
// all, the one-shot functions only allocate what they return plus bounded scratch, and warmed-up chunked encoding
// allocates nothing. Run `make test-alloc`; the numbers are printed.
#include <pthread.h>
#include <stdatomic.h>

#include "bare_functions.h"
#include "buffer.h"
#include "common.h"
#include "test_support.h"

// Allocations (including reallocations) and the bytes they requested; frees are not counted.
static atomic_size_t allocation_calls;
static atomic_size_t allocation_bytes;
// Count every thread's allocations (for code that uses the worker threads), or only the measuring thread's.
static atomic_bool count_all_threads;
// Whether this thread is being measured. `_Thread_local` gives each thread its own copy of the variable.
static _Thread_local bool thread_measured;

// Record one allocation of `size` bytes if it should be counted.
static void count_allocation(size_t size) {
    if (atomic_load(&count_all_threads) || thread_measured) {
        atomic_fetch_add(&allocation_calls, 1);
        atomic_fetch_add(&allocation_bytes, size);
    }
}

void *counted_malloc(size_t size) {
    count_allocation(size);
    return raw_malloc(size);
}

void *counted_calloc(size_t count, size_t size) {
    count_allocation(count * size);
    return raw_calloc(count, size);
}

void *counted_realloc(void *pointer, size_t size) {
    count_allocation(size);
    return raw_realloc(pointer, size);
}

// Allocations made while measuring.
typedef struct allocation_count {
    size_t calls;
    size_t bytes;
} allocation_count;

// Start measuring this thread (or all threads).
static allocation_count start_counting(bool all_threads) {
    allocation_count start = {atomic_load(&allocation_calls), atomic_load(&allocation_bytes)};
    atomic_store(&count_all_threads, all_threads);
    thread_measured = true;
    return start;
}

// Stop measuring and return what was allocated since `start`.
static allocation_count stop_counting(allocation_count start) {
    thread_measured = false;
    atomic_store(&count_all_threads, false);
    allocation_count end = {atomic_load(&allocation_calls) - start.calls, atomic_load(&allocation_bytes) - start.bytes};
    return end;
}

// Text-like input of `n` bytes.
static lzfse_zazzcat_buffer text(size_t n) {
    static const char phrase[] = "the quick brown fox jumps over a lazy dog and of ";
    lzfse_zazzcat_buffer out = {raw_malloc(n + 1), n, n + 1};
    for (size_t i = 0; i < n; i++) out.data[i] = (uint8_t)(phrase[i % (sizeof phrase - 1)] ^ ((i / 997) & 1));
    return out;
}

// Reused encoders and decoders allocate nothing once warmed up, at every input size and block type.
static void test_reuse_is_allocation_free(void) {
    lzfse_zazzcat_encoder *encoder = lzfse_zazzcat_encoder_create(NULL);
    lzfse_zazzcat_decoder *decoder = lzfse_zazzcat_decoder_create();
    size_t sizes[] = {5, 1000, 4095, 65536, 1 << 20};
    lzfse_zazzcat_buffer inputs[5], out = {0};
    uint8_t *decoded = raw_malloc(1 << 20);
    size_t written;
    // Warm up on every size first, so every buffer reaches its final capacity.
    for (int i = 0; i < 5; i++) {
        inputs[i] = text(sizes[i]);
        out.length = 0;
        bare_lzfse_zazzcat_encoder_encode(encoder, inputs[i].data, inputs[i].length, &out);
        bare_lzfse_zazzcat_decoder_decode(decoder, out.data, out.length, decoded, 1 << 20, &written);
    }
    for (int i = 0; i < 5; i++) {
        out.length = 0;
        allocation_count start = start_counting(false);
        bare_lzfse_zazzcat_encoder_encode(encoder, inputs[i].data, inputs[i].length, &out);
        allocation_count encode = stop_counting(start);
        start = start_counting(false);
        bare_lzfse_zazzcat_decoder_decode(decoder, out.data, out.length, decoded, 1 << 20, &written);
        allocation_count decode = stop_counting(start);
        bare_printf("    %8zu bytes: encoder (%zu, %zu)  decoder (%zu, %zu)\n", sizes[i], encode.calls, encode.bytes,
                    decode.calls, decode.bytes);
        CHECK(encode.calls == 0, "reused encoder allocated for %zu bytes", sizes[i]);
        CHECK(decode.calls == 0, "reused decoder allocated for %zu bytes", sizes[i]);
        lzfse_zazzcat_buffer_free(&inputs[i]);
    }
    raw_free(decoded);
    lzfse_zazzcat_buffer_free(&out);
    lzfse_zazzcat_encoder_destroy(encoder);
    lzfse_zazzcat_decoder_destroy(decoder);
}

// The one-shot functions allocate only their result plus bounded scratch.
static void test_one_shot_allocations_are_bounded(void) {
    size_t sizes[] = {1000, 65536, 1 << 20};
    for (int i = 0; i < 3; i++) {
        size_t n = sizes[i];
        lzfse_zazzcat_buffer data = text(n), packed = {0}, out = {0};
        bare_lzfse_zazzcat_encode(data.data, n, NULL, &packed);
        uint8_t *into = raw_malloc(n);
        size_t written;
        allocation_count start = start_counting(false);
        bare_lzfse_zazzcat_encode(data.data, n, NULL, &out);
        allocation_count encode = stop_counting(start);
        lzfse_zazzcat_buffer_free(&out);
        start = start_counting(false);
        bare_lzfse_zazzcat_decode(packed.data, packed.length, n, &out);
        allocation_count decode = stop_counting(start);
        lzfse_zazzcat_buffer_free(&out);
        start = start_counting(false);
        bare_lzfse_zazzcat_decode_into(packed.data, packed.length, into, n, &written);
        allocation_count decode_into = stop_counting(start);
        bare_printf("    %8zu bytes: encode (%zu, %zu)  decode (%zu, %zu)  decode_into (%zu, %zu)\n", n, encode.calls,
                    encode.bytes, decode.calls, decode.bytes, decode_into.calls, decode_into.bytes);
        // Encoder: itself, the output, the hash tables (at most 512 + 256 KiB) and one block's buffers (~200 KB).
        CHECK(encode.calls <= 12, "encode made %zu allocations", encode.calls);
        CHECK(encode.bytes <= 2 * n + 1100000, "encode requested %zu bytes", encode.bytes);
        // Decoder: the output plus the literal buffer (at most 40 KB); the tables live on the stack.
        CHECK(decode.calls <= 2 && decode.bytes <= n + 41000 + 64, "decode (%zu, %zu)", decode.calls, decode.bytes);
        CHECK(decode_into.calls <= 1 && decode_into.bytes <= 41000, "decode_into (%zu, %zu)", decode_into.calls,
              decode_into.bytes);
        raw_free(into);
        lzfse_zazzcat_buffer_free(&data);
        lzfse_zazzcat_buffer_free(&packed);
    }
    // Small inputs never pay for the LZFSE buffers.
    lzfse_zazzcat_buffer small = text(1000), out = {0};
    allocation_count start = start_counting(false);
    bare_lzfse_zazzcat_encode(small.data, small.length, NULL, &out);
    allocation_count encode = stop_counting(start);
    CHECK(encode.bytes < 8000 + sizeof(void *) * 1024, "a 1000-byte encode requested %zu bytes", encode.bytes);
    lzfse_zazzcat_buffer_free(&small);
    lzfse_zazzcat_buffer_free(&out);
}

// Warmed-up chunked encoding allocates nothing, on any thread.
static void test_chunked_encoding(void) {
    lzfse_zazzcat_buffer big = text(4 << 20), out = {0};
    lzfse_zazzcat_offsets offsets = {0};
    for (int i = 0; i < 3; i++) {
        out.length = 0;
        bare_lzfse_zazzcat_encode_chunks(big.data, big.length, 65536, NULL, &out, &offsets);
    }
    out.length = 0;
    allocation_count start = start_counting(true);
    lzfse_zazzcat_status status = lzfse_zazzcat_encode_chunks(big.data, big.length, 65536, NULL, &out, &offsets);
    allocation_count chunks = stop_counting(start);
    bare_printf("    encode_chunks, warmed up, %zu threads: (%zu, %zu)\n", lzfse_zazzcat_parallelism(), chunks.calls,
                chunks.bytes);
    CHECK(status == LZFSE_ZAZZCAT_OK && offsets.length == 65, "64 chunks");
    CHECK(chunks.calls == 0, "warmed-up chunked encoding made %zu allocations", chunks.calls);
    lzfse_zazzcat_buffer_free(&big);
    lzfse_zazzcat_buffer_free(&out);
    lzfse_zazzcat_offsets_free(&offsets);
}

int main(void) {
    RUN_TEST(test_reuse_is_allocation_free);
    RUN_TEST(test_one_shot_allocations_are_bounded);
    RUN_TEST(test_chunked_encoding);
    if (test_failures) {
        bare_printf("%d check(s) failed\n", test_failures);
        return 1;
    }
    bare_printf("all allocation tests passed\n");
    return 0;
}
