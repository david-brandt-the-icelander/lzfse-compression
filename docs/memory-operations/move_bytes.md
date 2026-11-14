# `move_bytes`: tuning notes

Shared rules and decisions: [README](README.md).

## Where it is called

Its only library caller is `copy_match_exact` (`src/copy.h`), which the decoder's slow path uses. Matches reach
it only near the end of the output, or when the period is 2–7 bytes and the length is over 64 (distance 1 goes
to `fill_bytes`). The only other caller is one unit test (`tests/unit_tests.c`), and its ranges do not overlap.

## Finding: no library call needs overlap handling

Both calls copy `n` bytes from `[p − d, p − d + n)` to `[p + done, p + done + n)`. The ranges overlap only when
`n > d + done`, and that never happens:

- The direct call (`distance >= length`) copies `n = length ≤ d` with `done = 0`.
- In the loop, the first piece is `d` with `done = 0`. Each later piece is `max(done, d) − done % d`, which is at
  most `done` when `done ≥ d` and `d − done` when `done < d`. In both cases it is at most `d + done`.

So `memmove`'s overlap check and direction choice are wasted work there. The old plan's idea of a fast path for
the non-overlapping case comes down to calling `copy_bytes` instead.

## Steps

1. Change both calls in `copy_match_exact` to `copy_bytes`. Add a comment that states the bound above.
2. Add a unit test that runs `copy_match_exact` for every distance from 1 to 64 and every length from 0 to 256,
   and compares it with a byte-by-byte reference. Run it under `make test-sanitize`.
3. Measure the change with the benchmark's binary input (short periods are common there). Land it only if the
   change wins or is neutral, as AGENTS.md requires.
4. Once that lands, the library has no `move_bytes` caller left, and it needs no backend. Keep the wrapper for
   future overlapping uses.
