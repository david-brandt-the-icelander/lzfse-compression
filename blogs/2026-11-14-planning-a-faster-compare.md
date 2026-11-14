# Planning a faster `compare_bytes`, and finding one 4-byte compare

*2026-11-14 · lzfse-zazzcat*

Every memory function in this code base goes through a wrapper in `src/memory_operations.h`. `compare_bytes`
wraps `memcmp`, and `equal_bytes` wraps `compare_bytes` for callers that only need "equal or not". With the
wrappers in place, the obvious next question was how to make each one faster, using CPU-specific instructions
where they help. This post follows that question for `compare_bytes`: the plan, a checklist of common `memcmp`
tricks held up against the code, and the conclusion. The library only used it once, to compare 4 bytes, and that
needed nothing more than an integer compare.

Compiler output below is from Apple clang 21 at `-O3`, targeting an Apple M1 Pro.

## Step zero: who calls it?

A search for callers found:

- **One library caller:** the decmpfs header parser, which checks that an extended attribute starts with the
  4-byte magic `"fpmc"`.
- **Everything else is outside the library:** tests, the fuzz targets, and the benchmark's check that decoded
  output matches the input. That check runs outside the timed part of the benchmark.

The encoder's search for matches compares far more bytes than all of these together, but it doesn't use
`compare_bytes`. It uses `match_length` in `src/copy.h`, which XORs 8 bytes at a time and finds the first
differing byte with a count of trailing zero bits.

So the plan split in two: a little cleanup for `compare_bytes`, and the real speed work on `match_length`.

## The plan for `compare_bytes`

Apple Silicon offers little that would help. It has NEON (16-byte vectors), but no SVE, and SME's streaming mode
is meant for matrix work, not short compares. The project's rule is that CPU-specific code is written only for
Apple Silicon, because that is the only machine it can be measured on. Apple's own `memcmp` already uses NEON for
long buffers, so a hand-written NEON loop could at best tie it there, and would lose on short buffers to what the
compiler already does.

That left one small idea: have `equal_bytes` call `bcmp` instead of `memcmp`. `bcmp` only answers "equal or
not", so it can stop at the first difference without working out which buffer is larger. Only the tests and fuzz
targets would notice.

## Checking a checklist

A widely shared list of `memcmp` optimizations made a useful cross-check. Each item, against this code:

| Idea | What we found |
|---|---|
| Only ask for equality | Already done: every caller uses `equal_bytes`. |
| Make the length a compile-time constant | Already happening: the one library call has length 4. |
| Inline the comparison | Already done: the wrappers are `static inline`, `match_length` is always inlined. |
| SIMD (NEON) | Only useful in `match_length`, and only for long matches. |
| Compare cached hashes first | Already done in the encoder (see below). |
| Compare the likely-different bytes first | New, and applies to `match_length` (see below). |
| Alignment and cache locality | Nothing to gain (see below). |

Two of the checklist's claims didn't hold on this toolchain, so they were tested.

**"Clang turns `memcmp(...) == 0` into `bcmp` by itself."** Not here. With a variable length, Apple clang still
calls `_memcmp`:

```
bl   _memcmp
cmp  w0, #0x0
cset w0, eq
```

Only an explicit `bcmp` call produces `bl _bcmp`. So if `equal_bytes` should use `bcmp`, it has to say so.

**"A constant 16-byte compare becomes SIMD instructions."** The checklist's example is x86 vector code. On ARM64,
clang uses paired 8-byte integer loads (`ldp`) and a chain of compares. `cmp` compares the first pair, and each
`ccmp` (conditional compare) compares the next pair only if everything so far was equal:

```
ldp  x8, x9, [x0]        ; 16 bytes from a
ldp  x10, x11, [x1]      ; 16 bytes from b
cmp  x8, x10
ccmp x9, x11, #0x0, eq
```

A 64-byte constant compare is four `ldp` pairs and one `cmp` followed by seven `ccmp`. Still no NEON.

### Already done: cached values before the real compare

The encoder's hash buckets store each earlier position together with the first 4 bytes found there. A candidate
whose stored 4 bytes don't match is rejected without reading the input at all. The table of long matches does the
same with 8 bytes. That is the "compare hashes first" idea, with the bytes themselves standing in for the hash.

### New: compare the byte that decides the result

zlib uses this trick when it searches for matches. Once the encoder has a best match of length `L`, a later
candidate can only replace it if it is longer, which means it must also match at byte `L`. Checking that one byte
first rejects candidates that pass the 4-byte check but can't win, without running `match_length` on them. The
output can't change: a candidate rejected this way is at most `L` bytes long, and only a strictly longer match
replaces the best one. It isn't measured yet.

### Nothing to gain: alignment

Matches start at arbitrary byte positions, so their alignment can't be chosen, and Apple cores load unaligned
words at full speed. For cache locality, the buckets already keep values beside positions, so rejections stay
inside the table.

## And then the conclusion

All of this planning was for a function whose only use in the library was this line:

```c
if (xattr_length < DECMPFS_HEADER_SIZE || compare_bytes(xattr, decmpfs_magic, 4) != 0) return LZFSE_ZAZZCAT_CORRUPT;
```

That compares the 4 bytes at one pointer with the 4 bytes at another. It doesn't need a SIMD backend, a `bcmp`
switch, or a dispatch table. It needs one 32-bit load from each pointer and one integer comparison:

```c
if (xattr_length < DECMPFS_HEADER_SIZE || load_u32(xattr) != load_u32(decmpfs_magic)) return LZFSE_ZAZZCAT_CORRUPT;
```

`load_u32` copies 4 bytes into an integer through `copy_bytes`, which is the valid C way to read a possibly
unaligned word. Because `decmpfs_magic` is a constant, its load folds into an immediate value, and the line
becomes `ldr`, `mov`/`movk` and `cmp`. At `-O3`, clang was already producing those instructions from the
`memcmp` call. The rewrite says it in the source, and it no longer depends on the optimizer recognising `memcmp`.
The companion post, [Comparing four bytes, and where XOR does (not)
help](2026-11-14-comparing-four-bytes-and-xor.md), covers this change in detail. It also explains why a pointer
cast and an XOR are no better.

With that line changed, `compare_bytes` has no callers left in the library.

## Takeaways

- **Count the callers before planning a backend.** "Optimize `compare_bytes`" sounded like a hot-path problem.
  It was one 4-byte check, run once per decmpfs header.
- **A fixed-size compare is an integer compare.** For 4 or 8 bytes, load both sides into integers and compare
  them. No library call, no vector code.
- **Test the checklist on your own compiler.** Two of its claims, automatic `bcmp` and SIMD for constant sizes,
  didn't hold for Apple clang on ARM64.
- **The comparison that matters may not go through the function.** The encoder's comparison work lives in
  `match_length`, which never called `compare_bytes`.

## Open items

These belong to `match_length`, not `compare_bytes`, and each needs a measurement before it lands:

- Profile how much encode time `match_length` takes, and how long its matches usually are.
- Try checking the deciding byte first in the encoder's candidate loops.
- Try doing the first 8-byte compare before entering the loop, and then 16 bytes per step with paired loads.
- Use NEON only if the profile shows many long matches.

Extending matches backward 8 bytes at a time was already tried and measured slower (see the companion post), so
it is off the list.
