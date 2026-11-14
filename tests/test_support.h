// A minimal test framework and the data generators the tests share.
//
// Each test is a function; RUN_TEST(name) calls it and reports its name. CHECK(condition, ...) records a failure
// (with file, line and a printf-style message) and lets the test continue; REQUIRE stops the current test.
#ifndef LZFSE_ZAZZCAT_TEST_SUPPORT_H
#define LZFSE_ZAZZCAT_TEST_SUPPORT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bare_functions.h"
#include "lzfse_zazzcat.h"
#include "memory_allocation.h"
#include "memory_operations.h"

// Number of failed checks so far, and the name of the running test.
static int test_failures;
static const char *test_name;

// Record a failure with a message when `condition` is false.
#define CHECK(condition, ...)                                                                                          \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            test_failures++;                                                                                           \
            bare_fprintf(stderr, "FAILED %s (%s:%d): %s: ", test_name, __FILE__, __LINE__, #condition);                \
            bare_fprintf(stderr, __VA_ARGS__);                                                                         \
            bare_fprintf(stderr, "\n");                                                                                \
        }                                                                                                              \
    } while (0)

// Like CHECK, but leave the current test function when the condition is false.
#define REQUIRE(condition, ...)                                                                                        \
    do {                                                                                                               \
        int failures_before = test_failures;                                                                           \
        CHECK(condition, __VA_ARGS__);                                                                                 \
        if (test_failures != failures_before) return;                                                                  \
    } while (0)

// Run one test function and print its name.
#define RUN_TEST(function)                                                                                             \
    do {                                                                                                               \
        test_name = #function;                                                                                         \
        int failures_before = test_failures;                                                                           \
        function();                                                                                                    \
        bare_printf("%s %s\n", test_failures == failures_before ? "ok  " : "FAIL", #function);                         \
    } while (0)

// A deterministic pseudo-random number generator (xorshift), so every run sees the same data.
typedef struct test_random {
    uint32_t state;
} test_random;

// Next pseudo-random 32-bit number.
static inline uint32_t test_random_next(test_random *r) {
    uint32_t x = r->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->state = x;
    return x;
}

// Fill a new buffer with `n` pseudo-random bytes (incompressible "noise").
static inline lzfse_zazzcat_buffer noise(size_t n, uint32_t seed) {
    lzfse_zazzcat_buffer out = {raw_malloc(n + 1), n, n + 1};
    test_random r = {seed | 1};
    for (size_t i = 0; i < n; i++) out.data[i] = (uint8_t)test_random_next(&r);
    return out;
}

// Fill a new buffer with `n` bytes of text-like data: words drawn from a small vocabulary.
static inline lzfse_zazzcat_buffer words(size_t n) {
    static const char *vocabulary[] = {"lorem ",       "ipsum ",      "dolor ",  "sit ", "amet, ",
                                       "consectetur ", "adipiscing ", "elit.\n", "sed ", "do "};
    lzfse_zazzcat_buffer out = {raw_malloc(n + 16), 0, n + 16};
    test_random r = {99};
    while (out.length < n) {
        const char *word = vocabulary[(uint8_t)test_random_next(&r) % 10];
        size_t length = strlen(word);
        copy_bytes(out.data + out.length, word, length);
        out.length += length;
    }
    out.length = n;
    return out;
}

// A buffer holding a copy of `n` bytes.
static inline lzfse_zazzcat_buffer copy_of(const void *data, size_t n) {
    lzfse_zazzcat_buffer out = {raw_malloc(n + 1), n, n + 1};
    if (n > 0) copy_bytes(out.data, data, n);
    return out;
}

// True when two byte ranges are equal (and of equal length).
static inline int same_bytes(const void *a, size_t a_length, const void *b, size_t b_length) {
    return a_length == b_length && (a_length == 0 || compare_bytes(a, b, a_length) == 0);
}

#endif
