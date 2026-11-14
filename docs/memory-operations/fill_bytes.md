# `fill_bytes`: tuning notes

Shared rules and decisions: [README](README.md).

## Where it is called

Its one library caller is `decode_token_slow` (`src/decode.c`). It writes runs of one repeated byte (distance 1)
that the fast path declined, which means runs longer than 64 bytes or runs near the end of the output. The only
other caller is one unit test.

## Assessment

These runs are rare, and each one is a single variable-size `memset`. Apple's libc `memset` is already tuned for
its cores, so no backend could do much better. No work is planned.

## Revisit if

A profile of run-heavy input (sparse files, zero-padded images) shows `_platform_memset` called from the decoder
above the 5% threshold. Even then, check first whether raising the fast path's 64-byte limit for distance 1
removes the calls.
