// Out-of-memory behaviour. The library is built with LZFSE_ZAZZCAT_COUNT_ALLOCATIONS, which sends its allocations
// through the functions below; they fail the Nth allocation on request. Each entry point is run with every one of its
// allocations failing in turn: it must return LZFSE_ZAZZCAT_OUT_OF_MEMORY (or NULL from a create function) and leave
// nothing for the caller to clean up beyond the usual frees. `oom-tests pool` runs only the check that the thread
// pool's start-up survives a failed allocation, which must happen before anything else uses the pool.
// Run `make test-alloc`.
#include <execinfo.h>
#include <stdatomic.h>

#include "bare_functions.h"
#include "buffer.h"
#include "common.h"
#include "format.h"
#include "test_support.h"

// Allocations made since the last reset, the number that fails (-1: none), and whether a failure was injected.
static atomic_long allocation_number;
static atomic_long failing_allocation = -1;
static atomic_bool failure_injected;
// While true, allocations neither count nor fail: an operation's own set-up runs paused.
static atomic_bool paused;

// Failing allocations by call path: with failing_allocation == FAIL_EACH_PATH, an allocation fails the first time it
// is made from a call path (the chain of return addresses) not failed before. The paths already failed are kept in
// `failed_paths` (guarded by a spin lock, since worker threads allocate too).
#define FAIL_EACH_PATH (-2)
#define MAX_PATHS      256
static uint64_t failed_paths[MAX_PATHS];
static size_t failed_path_count;
static atomic_flag paths_lock = ATOMIC_FLAG_INIT;

// True when the call path of this allocation has not been failed yet; remembers it.
static bool new_call_path(void) {
    void *frames[12];
    int count = backtrace(frames, 12);
    uint64_t id = 1469598103934665603ull;
    // Skip this function, fail_this_allocation and the counted_* wrapper; hash the rest.
    for (int i = 3; i < count; i++) id = (id ^ (uint64_t)(uintptr_t)frames[i]) * 1099511628211ull;
    while (atomic_flag_test_and_set(&paths_lock)) {
    }
    bool is_new = true;
    for (size_t i = 0; i < failed_path_count; i++) {
        if (failed_paths[i] == id) is_new = false;
    }
    if (is_new && failed_path_count < MAX_PATHS) failed_paths[failed_path_count++] = id;
    atomic_flag_clear(&paths_lock);
    return is_new;
}

// True when this allocation is the one that should fail.
static bool fail_this_allocation(void) {
    if (atomic_load(&paused)) return false;
    if (atomic_load(&failing_allocation) == FAIL_EACH_PATH) {
        if (!new_call_path()) return false;
        atomic_store(&failure_injected, true);
        return true;
    }
    long number = atomic_fetch_add(&allocation_number, 1);
    if (number != atomic_load(&failing_allocation)) return false;
    atomic_store(&failure_injected, true);
    return true;
}

void *counted_malloc(size_t size) {
    return fail_this_allocation() ? NULL : raw_malloc(size);
}

void *counted_calloc(size_t count, size_t size) {
    return fail_this_allocation() ? NULL : raw_calloc(count, size);
}

void *counted_realloc(void *pointer, size_t size) {
    return fail_this_allocation() ? NULL : raw_realloc(pointer, size);
}

// Make allocation number `number` (counting from now) fail; -1 turns failing off.
static void arm(long number) {
    atomic_store(&allocation_number, 0);
    atomic_store(&failure_injected, false);
    atomic_store(&failing_allocation, number);
}

// An operation under test: returns a status, or LZFSE_ZAZZCAT_OK when it succeeded (a create function reports NULL as
// out of memory).
typedef lzfse_zazzcat_status (*operation)(const void *input, size_t length);

// Run `op` with its first allocation failing, then its second, and so on until no failure is injected; every failed
// run must report out of memory and the last must succeed. With `keep_state` the operation changes lasting state (the
// thread pool and its scratch arrays), so an allocation fails the first time it is made from each call path instead:
// what an earlier run allocated is kept, so counting allocations would skip some.
static void fail_each_allocation(const char *name, operation op, const void *input, size_t length, bool keep_state) {
    failed_path_count = 0;
    for (long number = 0; number < 400; number++) {
        arm(keep_state ? FAIL_EACH_PATH : number);
        lzfse_zazzcat_status status = op(input, length);
        bool injected = atomic_load(&failure_injected);
        arm(-1);
        if (!injected) {
            CHECK(status == LZFSE_ZAZZCAT_OK, "%s: succeeded with no failure but returned %s", name,
                  lzfse_zazzcat_status_string(status));
            return;
        }
        CHECK(status == LZFSE_ZAZZCAT_OUT_OF_MEMORY, "%s: allocation %ld failed but the status is %s", name, number,
              lzfse_zazzcat_status_string(status));
    }
    CHECK(0, "%s: still allocating after 400 allocations", name);
}

// --- Operations ---

static lzfse_zazzcat_status op_encode(const void *input, size_t length) {
    lzfse_zazzcat_buffer out = {0};
    lzfse_zazzcat_status status = lzfse_zazzcat_encode(input, length, NULL, &out);
    lzfse_zazzcat_buffer_free(&out);
    return status;
}

// Encode with an encoder made beforehand and used once, so its tables are all still to be allocated.
static lzfse_zazzcat_status op_encoder_encode(const void *input, size_t length) {
    atomic_store(&paused, true);
    lzfse_zazzcat_encoder *encoder = lzfse_zazzcat_encoder_create(NULL);
    atomic_store(&paused, false);
    lzfse_zazzcat_buffer out = {0};
    lzfse_zazzcat_status status =
        encoder == NULL ? LZFSE_ZAZZCAT_OUT_OF_MEMORY : lzfse_zazzcat_encoder_encode(encoder, input, length, &out);
    lzfse_zazzcat_encoder_destroy(encoder);
    lzfse_zazzcat_buffer_free(&out);
    return status;
}

static lzfse_zazzcat_status op_encoder_create(const void *input, size_t length) {
    (void)input;
    (void)length;
    lzfse_zazzcat_encoder *encoder = lzfse_zazzcat_encoder_create(NULL);
    lzfse_zazzcat_status status = encoder == NULL ? LZFSE_ZAZZCAT_OUT_OF_MEMORY : LZFSE_ZAZZCAT_OK;
    lzfse_zazzcat_encoder_destroy(encoder);
    return status;
}

static lzfse_zazzcat_status op_decoder_create(const void *input, size_t length) {
    (void)input;
    (void)length;
    lzfse_zazzcat_decoder *decoder = lzfse_zazzcat_decoder_create();
    lzfse_zazzcat_status status = decoder == NULL ? LZFSE_ZAZZCAT_OUT_OF_MEMORY : LZFSE_ZAZZCAT_OK;
    lzfse_zazzcat_decoder_destroy(decoder);
    return status;
}

static lzfse_zazzcat_status op_decode(const void *input, size_t length) {
    lzfse_zazzcat_buffer out = {0};
    lzfse_zazzcat_status status = lzfse_zazzcat_decode(input, length, SIZE_MAX / 2, &out);
    lzfse_zazzcat_buffer_free(&out);
    return status;
}

// decode_into allocates only the literal buffer; a size that is wrong would make it fail for a reason of its own.
static size_t decoded_size_for_op;

static lzfse_zazzcat_status op_decode_into(const void *input, size_t length) {
    uint8_t *room = raw_malloc(decoded_size_for_op + 64);
    size_t written;
    lzfse_zazzcat_status status = lzfse_zazzcat_decode_into(input, length, room, decoded_size_for_op + 64, &written);
    raw_free(room);
    return status;
}

static lzfse_zazzcat_status op_decoder_decode(const void *input, size_t length) {
    atomic_store(&paused, true);
    lzfse_zazzcat_decoder *decoder = lzfse_zazzcat_decoder_create();
    atomic_store(&paused, false);
    if (decoder == NULL) return LZFSE_ZAZZCAT_OUT_OF_MEMORY;
    uint8_t *room = raw_malloc(decoded_size_for_op + 64);
    size_t written;
    lzfse_zazzcat_status status =
        lzfse_zazzcat_decoder_decode(decoder, input, length, room, decoded_size_for_op + 64, &written);
    raw_free(room);
    lzfse_zazzcat_decoder_destroy(decoder);
    return status;
}

static lzfse_zazzcat_status op_stats(const void *input, size_t length) {
    lzfse_zazzcat_stream_stats stats;
    clear_bytes(&stats, sizeof stats);
    return lzfse_zazzcat_stream_stats_add(&stats, input, length);
}

static lzfse_zazzcat_status op_decmpfs_compress(const void *input, size_t length) {
    lzfse_zazzcat_decmpfs c;
    lzfse_zazzcat_status status = lzfse_zazzcat_decmpfs_compress(input, length, &c);
    if (status == LZFSE_ZAZZCAT_OK) lzfse_zazzcat_decmpfs_free(&c);
    return status;
}

static lzfse_zazzcat_status op_decmpfs_decompress(const void *input, size_t length) {
    lzfse_zazzcat_decmpfs c;
    // Only the decompression's allocations are of interest: the compression runs paused.
    atomic_store(&paused, true);
    lzfse_zazzcat_status status = lzfse_zazzcat_decmpfs_compress(input, length, &c);
    atomic_store(&paused, false);
    if (status != LZFSE_ZAZZCAT_OK) return status;
    lzfse_zazzcat_buffer out = {0};
    status =
        lzfse_zazzcat_decmpfs_decompress(c.xattr.data, c.xattr.length, c.fork.data, c.fork.length, UINT64_MAX, &out);
    lzfse_zazzcat_buffer_free(&out);
    lzfse_zazzcat_decmpfs_free(&c);
    return status;
}

static lzfse_zazzcat_status op_lzvn_encode(const void *input, size_t length) {
    lzfse_zazzcat_buffer out = {0};
    lzfse_zazzcat_status status = lzfse_zazzcat_lzvn_encode(input, length, &out);
    lzfse_zazzcat_buffer_free(&out);
    return status;
}

static lzfse_zazzcat_status op_encode_chunks(const void *input, size_t length) {
    lzfse_zazzcat_buffer out = {0};
    lzfse_zazzcat_offsets offsets = {0};
    lzfse_zazzcat_status status = lzfse_zazzcat_encode_chunks(input, length, 65536, NULL, &out, &offsets);
    lzfse_zazzcat_buffer_free(&out);
    lzfse_zazzcat_offsets_free(&offsets);
    return status;
}

// Chunks of a size that gives many small tasks, and the empty input (one offset to allocate).
static lzfse_zazzcat_status op_encode_small_chunks(const void *input, size_t length) {
    lzfse_zazzcat_buffer out = {0};
    lzfse_zazzcat_offsets offsets = {0};
    lzfse_zazzcat_status status = lzfse_zazzcat_encode_chunks(input, length, 1000, NULL, &out, &offsets);
    lzfse_zazzcat_buffer_free(&out);
    lzfse_zazzcat_offsets_free(&offsets);
    return status;
}

// Chunks of 70000 bytes with a short last one: the last chunk is encoded by LZVN, which allocates its own table, and
// shares a task with chunks before it.
static lzfse_zazzcat_status op_encode_short_tail(const void *input, size_t length) {
    lzfse_zazzcat_buffer out = {0};
    lzfse_zazzcat_offsets offsets = {0};
    lzfse_zazzcat_status status = lzfse_zazzcat_encode_chunks(input, length, 70000, NULL, &out, &offsets);
    lzfse_zazzcat_buffer_free(&out);
    lzfse_zazzcat_offsets_free(&offsets);
    return status;
}

static lzfse_zazzcat_status op_encode_no_chunks(const void *input, size_t length) {
    (void)length;
    lzfse_zazzcat_buffer out = {0};
    lzfse_zazzcat_offsets offsets = {0};
    lzfse_zazzcat_status status = lzfse_zazzcat_encode_chunks(input, 0, 1000, NULL, &out, &offsets);
    lzfse_zazzcat_buffer_free(&out);
    lzfse_zazzcat_offsets_free(&offsets);
    return status;
}

// --- Tests ---

// Compressible text and incompressible noise, of the sizes that take each path through the encoder.
static void test_every_allocation_can_fail(void) {
    size_t sizes[] = {0, 5, 50, 700, 5000, 70000, 400000};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        lzfse_zazzcat_buffer texts[2] = {words(sizes[k]), noise(sizes[k], (uint32_t)k + 1)};
        for (int kind = 0; kind < 2; kind++) {
            lzfse_zazzcat_buffer *data = &texts[kind];
            char name[64];
            snprintf(name, sizeof name, "encode %s %zu", kind ? "noise" : "text", sizes[k]);
            fail_each_allocation(name, op_encode, data->data, data->length, false);
            snprintf(name, sizeof name, "encoder_encode %s %zu", kind ? "noise" : "text", sizes[k]);
            fail_each_allocation(name, op_encoder_encode, data->data, data->length, false);
            lzfse_zazzcat_buffer stream = {0};
            REQUIRE(lzfse_zazzcat_encode(data->data, data->length, NULL, &stream) == LZFSE_ZAZZCAT_OK, "encode");
            decoded_size_for_op = data->length;
            snprintf(name, sizeof name, "decode %s %zu", kind ? "noise" : "text", sizes[k]);
            fail_each_allocation(name, op_decode, stream.data, stream.length, false);
            fail_each_allocation("decode_into", op_decode_into, stream.data, stream.length, false);
            fail_each_allocation("decoder_decode", op_decoder_decode, stream.data, stream.length, false);
            fail_each_allocation("stream_stats_add", op_stats, stream.data, stream.length, false);
            fail_each_allocation("lzvn_encode", op_lzvn_encode, data->data, data->length, false);
            lzfse_zazzcat_buffer_free(&stream);
        }
        lzfse_zazzcat_buffer_free(&texts[0]);
        lzfse_zazzcat_buffer_free(&texts[1]);
    }
    fail_each_allocation("encoder_create", op_encoder_create, NULL, 0, false);
    fail_each_allocation("decoder_create", op_decoder_create, NULL, 0, false);
}

// The chunked encoder's lasting state (scratch arrays, per-thread encoders) is built up one failure at a time.
static void test_chunked_encoding_survives_failures(void) {
    lzfse_zazzcat_buffer big = words(1 << 20), noisy = noise(300000, 8);
    fail_each_allocation("encode_chunks empty", op_encode_no_chunks, big.data, 0, false);
    lzfse_zazzcat_buffer tail = words(70000 * 40 + 1000);
    fail_each_allocation("encode_chunks short tail", op_encode_short_tail, tail.data, tail.length, true);
    lzfse_zazzcat_buffer_free(&tail);
    fail_each_allocation("encode_chunks", op_encode_chunks, big.data, big.length, true);
    fail_each_allocation("encode_chunks noise", op_encode_chunks, noisy.data, noisy.length, true);
    fail_each_allocation("encode_chunks small chunks", op_encode_small_chunks, big.data, 200000, true);
    lzfse_zazzcat_buffer small = words(20000);
    fail_each_allocation("decmpfs_compress inline", op_decmpfs_compress, small.data, 300, false);
    fail_each_allocation("decmpfs_compress fork", op_decmpfs_compress, big.data, big.length, false);
    fail_each_allocation("decmpfs_compress noise", op_decmpfs_compress, noisy.data, noisy.length, false);
    fail_each_allocation("decmpfs_decompress inline", op_decmpfs_decompress, small.data, 300, false);
    fail_each_allocation("decmpfs_decompress fork", op_decmpfs_decompress, big.data, big.length, false);
    lzfse_zazzcat_buffer_free(&big);
    lzfse_zazzcat_buffer_free(&noisy);
    lzfse_zazzcat_buffer_free(&small);
}

// An input past the encoder's 1 GiB segment size, so that a failure in the first segment ends a loop with a segment
// to go. Zeros compress to almost nothing, so this is quick.
static void test_failure_in_the_first_of_two_segments(void) {
    size_t n = ((size_t)1 << 30) + 4096;
    uint8_t *zeros = raw_calloc(n, 1);
    REQUIRE(zeros != NULL, "1 GiB of zeros");
    fail_each_allocation("encode 1 GiB", op_encode, zeros, n, false);
    raw_free(zeros);
}

// The growable buffer and the header writers: a failed allocation and sizes too large to add.
static void test_buffer_limits(void) {
    lzfse_zazzcat_buffer b = {0};
    CHECK(!buffer_append(&b, "x", SIZE_MAX), "an append of SIZE_MAX bytes fails");
    // More than can be added to a length of 16 without wrapping.
    uint8_t *sixteen = raw_malloc(16);
    lzfse_zazzcat_buffer full = {sixteen, 16, 16};
    CHECK(!buffer_reserve(&full, SIZE_MAX), "a reservation of SIZE_MAX fails");
    raw_free(sixteen);
    arm(0);
    CHECK(!buffer_append(&b, "x", 1), "a failed allocation fails the append");
    arm(-1);
    CHECK(b.data == NULL && b.length == 0 && b.capacity == 0, "a failed append leaves the buffer as it was");
    // A buffer whose capacity is already above half of SIZE_MAX doubles to SIZE_MAX, which cannot be allocated; the
    // old block stays valid. The length and capacity here are not true, but nothing reads the bytes.
    uint8_t *block = raw_malloc(16);
    lzfse_zazzcat_buffer huge = {block, SIZE_MAX / 2 + 1, SIZE_MAX / 2 + 1};
    CHECK(!buffer_reserve(&huge, 1), "growing past what can be allocated fails");
    CHECK(huge.data == block, "the old block is kept");
    raw_free(block);
    block_header h;
    clear_bytes(&h, sizeof h);
    arm(0);
    CHECK(!write_v1_header(&h, &b), "write_v1_header fails when it cannot allocate");
    arm(0);
    CHECK(write_v2_header(&h, &b) == 0, "write_v2_header fails when it cannot allocate");
    arm(-1);
    CHECK(b.length == 0, "nothing was written");
    lzfse_zazzcat_decoder_destroy(NULL);
    lzfse_zazzcat_encoder_destroy(NULL);
}

// The thread pool is started by the first chunked call. A failed allocation there leaves a pool without encoders,
// which reports out of memory for every call.
static void test_pool_start_failure(void) {
    lzfse_zazzcat_buffer data = words(300000), out = {0};
    lzfse_zazzcat_offsets offsets = {0};
    arm(0);
    lzfse_zazzcat_status status = lzfse_zazzcat_encode_chunks(data.data, data.length, 65536, NULL, &out, &offsets);
    arm(-1);
    CHECK(status == LZFSE_ZAZZCAT_OUT_OF_MEMORY, "pool start-up failure: %s", lzfse_zazzcat_status_string(status));
    status = lzfse_zazzcat_encode_chunks(data.data, data.length, 65536, NULL, &out, &offsets);
    CHECK(status == LZFSE_ZAZZCAT_OUT_OF_MEMORY || status == LZFSE_ZAZZCAT_OK, "a later call: %s",
          lzfse_zazzcat_status_string(status));
    lzfse_zazzcat_buffer_free(&data);
    lzfse_zazzcat_buffer_free(&out);
    lzfse_zazzcat_offsets_free(&offsets);
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == 'p') {
        RUN_TEST(test_pool_start_failure);
    } else {
        // Start the thread pool now: a failure while starting it is permanent (see test_pool_start_failure).
        bare_lzfse_zazzcat_parallelism();
        RUN_TEST(test_every_allocation_can_fail);
        RUN_TEST(test_chunked_encoding_survives_failures);
        RUN_TEST(test_buffer_limits);
        RUN_TEST(test_failure_in_the_first_of_two_segments);
    }
    if (test_failures) {
        bare_printf("%d check(s) failed\n", test_failures);
        return 1;
    }
    bare_printf("all out-of-memory tests passed\n");
    return 0;
}
