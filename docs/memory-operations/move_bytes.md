# `move_bytes`: tuning notes

Shared rules and decisions: [README](README.md).

Status: done (2026-11-14). `copy_match_exact` now uses `copy_bytes`, and the library has no `move_bytes` caller
left. Measurement: [docs/benchmarks/2026-11-14-owner-m1-pro-copy-match-exact.md](../benchmarks/2026-11-14-owner-m1-pro-copy-match-exact.md).

## Where it was called

Its only library caller was `copy_match_exact` (`src/copy.h`), which both decoders' slow paths use. In the LZFSE
decoder (`decode_token_slow`, `src/decode.c`), matches reach it only near the end of the output, or when the
period is 2–7 bytes and the length is over 64 (distance 1 goes to `fill_bytes`). The LZVN decoder (`decode_run`,
`src/lzvn.c`) calls it for every match that `decode_token_fast` declines, including distance 1. The only other
caller is one unit test (`tests/unit_tests.c`), and its ranges do not overlap.

## Finding: no library call needs overlap handling

Both calls copy `n` bytes from `[p − d, p − d + n)` to `[p + done, p + done + n)`. The ranges overlap only when
`n > d + done`, and that never happens:

- The direct call (`distance >= length`) copies `n = length ≤ d` with `done = 0`.
- In the loop, the first piece is `d` with `done = 0`. After it `done ≥ d`, and each later piece is
  `max(done, d) − done % d = done − done % d`, which is at most `done`. (So `piece` is never 0 and the
  `if (piece == 0)` guard never fires.)

So `memmove`'s overlap check and direction choice are not needed there. The old plan's idea of a fast path for
the non-overlapping case comes down to calling `copy_bytes` instead.

On macOS this saves nothing: `memcpy` and `memmove` are both re-exports of `_platform_memmove`, which handles
overlap either way. The gain is limited to C libraries whose `memcpy` is a separate, check-free routine (glibc,
musl). The change also states the non-overlap promise in the code, and the sanitizer build now checks it,
because AddressSanitizer reports `memcpy-param-overlap` and has no such check for `memmove`.

## Steps

1. Done. Both calls in `copy_match_exact` use `copy_bytes`, with a comment that states the bound above.
2. Done. `test_copy_match_exact` runs every distance from 1 to 64 and every length from 0 to 256 against a
   byte-by-byte reference, and passes under `make test-sanitize`.
3. Done. Neutral on the benchmark's binary input (+0.1% at 4000-byte chunks, +0.7% at 64 KiB) and within the
   spread on every other row, as expected on macOS (see above).
4. The library has no `move_bytes` caller left, and it needs no backend. The wrapper stays for future
   overlapping uses.
