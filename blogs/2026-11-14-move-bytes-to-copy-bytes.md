# Swapping `memmove` for `memcpy` in the match copy, and why macOS did not notice

*2026-11-14 · lzfse-zazzcat*

Every memory function in this code base goes through a wrapper in `src/memory_operations.h`. `copy_bytes` wraps
`memcpy`, and `move_bytes` wraps `memmove`. The difference between them is overlap. `memmove` must give the right
answer when the source and destination overlap, so it checks for that and chooses a copy direction. `memcpy` may
assume they do not overlap. The [`move_bytes` tuning notes](../docs/memory-operations/move_bytes.md) claimed that
the library's only `move_bytes` caller never overlaps, so it could use `copy_bytes`. This session checked that
claim and made the switch.

## The caller

`copy_match_exact` in `src/copy.h` copies an LZ match: `length` bytes starting `distance` bytes back. It is the
exact, careful version, used when the decoder's wide fast path (`decode_token_fast`) declines a token. That
happens near the end of the output buffer, and for long matches with a period of 2–7 bytes.

When the distance is shorter than the length, the match repeats its own output: with distance 3, the bytes `abc`
become `abcabcabc…`. A single `memmove` would not do that. It would copy the old bytes as they were, not the
pattern. So the function copies the pattern in pieces that grow: first `distance` bytes, then everything written
so far, then twice that, and so on, with each piece a whole number of periods.

## Checking the claim

A piece of `n` bytes copies `[p − d, p − d + n)` to `[p + done, p + done + n)`, where `done` is how much of the
match is already written. The source starts before the destination, so the two overlap only when the source runs
past the start of the destination, that is when `n > d + done`.

- If the whole match fits in one period (`distance >= length`), there is one copy of `length ≤ d` bytes with
  `done = 0`. No overlap.
- Otherwise the first piece is exactly `d` bytes with `done = 0`. After that, `done ≥ d`, and the next piece is
  `done − done % d`, which is never more than `done`. No overlap.

So every piece reads only bytes that are already written and never reaches its own destination. The tuning notes
were right. Two details needed correcting:

- `copy_match_exact` has a second caller, the LZVN decoder in `src/lzvn.c`, which also sends distance 1 there
  (the LZFSE decoder turns distance 1 into `fill_bytes`). The bound holds for distance 1 too.
- The loop's `if (piece == 0) piece = distance;` can never fire, because `piece ≥ d` once `done ≥ d`. It is
  harmless and was left in.

## The change

Both calls in `copy_match_exact` now use `copy_bytes`, and a comment above the function states the bound. The
existing unit test grew from distances 1–19 and lengths 0–69 to distances 1–64 and lengths 0–256, still compared
byte by byte with a plain loop and with a zeroed tail that catches writes past the end.

The switch also makes the sanitizer build more useful. AddressSanitizer checks every `memcpy` for overlapping
ranges and reports `memcpy-param-overlap`. It cannot check `memmove` that way, because overlap is allowed there.
A tiny test program with an overlapping `memcpy` confirmed that the check is active in this toolchain. So
`make test-sanitize` now checks the bound above for all 16,448 distance and length pairs, instead of relying on
the proof alone.

## The measurement, and the surprise

AGENTS.md lets a speed change land only with a measurement. Five alternating rounds of `lzfse-bench --runs 11
--synthetic` at 4000-byte chunks (LZVN) and 64 KiB chunks (LZFSE) gave a neutral result. The binary input, which
has the most short-period matches, moved by +0.1% and +0.7%. Every other row that decodes tokens stayed within
about 1%, and its spread included zero. The full table is in
[docs/benchmarks](../docs/benchmarks/2026-11-14-owner-m1-pro-copy-match-exact.md).

That was expected for two reasons. First, `copy_match_exact` is a cold path. Second, on macOS `memcpy` and
`memmove` are the same function:

```text
$ dyld_info -exports /usr/lib/system/libsystem_c.dylib | grep -E '_(memcpy|memmove)$'
        [re-export] _memcpy (__platform_memmove from libsystem_platform)
        [re-export] _memmove (__platform_memmove from libsystem_platform)
```

The new binary calls through `_memcpy` at four more places and through `_memmove` nowhere, but both names lead to
`_platform_memmove`, which handles overlap anyway. On Apple's C library there is no overlap check to skip. The
gain the tuning notes described exists only on C libraries with a separate, check-free `memcpy`, such as glibc and
musl.

So the change stays, for reasons other than speed on the owner's machine. It is neutral here and possibly a small
win elsewhere. It states a property of the code that was true but unwritten, and the sanitizer now checks that
property.

## Checking it

`make format && make all test test-alloc test-sanitize` passed, including the libcompression and kernel
compatibility tests on macOS. `make fuzz` still fails at the libFuzzer link step with missing C++ library
symbols, as it did before this change. All four fuzz targets still compile.

## What is left

The library now has no `move_bytes` caller. The wrapper stays for any future copy that really does overlap. It
needs no Apple Silicon backend, which settles one row of the
[memory-operations plan](../docs/memory-operations/README.md).
