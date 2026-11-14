# `equal_bytes`: saying "are these the same?" directly

*2026-11-14 · lzfse-zazzcat*

Every memory function in this code base goes through a wrapper in `src/memory_operations.h`, and `memcmp` is
wrapped by `compare_bytes`. Like `memcmp`, it answers a three-way question. It returns zero when the two regions
match, and otherwise a negative or positive number depending on which region has the smaller byte at the first
difference. This session added a second wrapper, `equal_bytes`, for the question every caller was actually
asking: are these bytes the same?

## Why bother

A three-way compare used as an equality test is a well-known way to write C that reads backwards. Every call
looked like one of these:

```c
CHECK(compare_bytes(a, b, sizeof a) == 0, "d=%zu m=%zu", d, m);

if (xattr_length < DECMPFS_HEADER_SIZE || compare_bytes(xattr, decmpfs_magic, 4) != 0) return LZFSE_ZAZZCAT_CORRUPT;
```

To read either line you have to remember that `compare_bytes` returns zero for a match, so `== 0` means "equal"
and `!= 0` means "different". The shorter form, `!memcmp(a, b, n)`, is worse: the `!` reads as "not equal" but
means "equal". The project writes for readers who have not used C for a while, so the function's name should give
the meaning instead.

Before writing anything, we checked how the result was used. All 18 calls, in the library, the tests, the
benchmark and the fuzz targets, compared it with zero. None used the ordering.

## The change

The new wrapper sits directly below `compare_bytes` and is defined in terms of it:

```c
// True when the first `count` bytes of `a` and `b` are the same. Use this for equality checks and keep
// compare_bytes for callers that need the order.
static inline bool equal_bytes(const void *a, const void *b, size_t count) {
    return compare_bytes(a, b, count) == 0;
}
```

Calling `compare_bytes` instead of `memcmp` keeps `memcmp` in a single place in the header. The header now
includes `<stdbool.h>`, because before C23 `bool` is a macro from that header rather than a keyword.

The call sites were rewritten mechanically. `compare_bytes(...) == 0` became `equal_bytes(...)`, and
`compare_bytes(...) != 0` became `!equal_bytes(...)`. The two lines above now read:

```c
CHECK(equal_bytes(a, b, sizeof a), "d=%zu m=%zu", d, m);

if (xattr_length < DECMPFS_HEADER_SIZE || !equal_bytes(xattr, decmpfs_magic, 4)) return LZFSE_ZAZZCAT_CORRUPT;
```

The calls were in seven files: `src/decmpfs.c` (the only one in the library), `bench/bench.c` (the benchmark's
check that both decoders return the original data), the two fuzz targets, and three test files. `AGENTS.md` now
lists `equal_bytes` among the allowed memory functions, and the `compare_bytes` tuning notes say that callers go
through it.

## What stayed the same

Several callers guard the compare with a length check:

```c
return a_length == b_length && (a_length == 0 || equal_bytes(a, b, a_length));
```

The guard is there for a C rule that surprises people. The pointers passed to `memcmp` must be valid even when
the count is zero, so in C11 `memcmp(NULL, NULL, 0)` is undefined behaviour. An empty buffer, such as a
zero-initialised `lzfse_zazzcat_buffer`, has a null data pointer. Moving the guard into `equal_bytes` would have been
tidier, but it would also change what the wrapper means for every future caller, which is more than this change
set out to do. The guards stay at the call sites.

Speed should not change. Both wrappers are `static inline`, so the compiler sees the same `memcmp` call compared
with zero as before. Because this is not a speed change, we took no measurements for it.

## Checking it

`make format && make all test test-alloc test-sanitize` passed, including the libcompression and kernel
compatibility tests on macOS. `make fuzz` did not: the libFuzzer link step fails with missing C++ library
symbols. It fails the same way on the tree without this change (the problem described at the end of
[Zeroing memory faster](2026-11-14-zeroing-memory.md)), so we checked that both fuzz targets still compile.

## What it led to

Once the four-byte magic check read `!equal_bytes(xattr, decmpfs_magic, 4)`, the next question was how fast that
compare could be. That is the subject of [Planning a faster
`compare_bytes`](2026-11-14-planning-a-faster-compare.md) and [Comparing four bytes, and where XOR does (not)
help](2026-11-14-comparing-four-bytes-and-xor.md).
