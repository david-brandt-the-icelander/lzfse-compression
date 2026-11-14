# Before you tune memcpy, find out who calls it

*2026-11-14. lzfse-zazzcat is an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

The project sends every byte operation through five small wrappers in `src/memory_operations.h`: `copy_bytes`,
`move_bytes`, `fill_bytes`, `clear_bytes` and `compare_bytes`. Each one is a `static inline` call to `memcpy`,
`memmove`, `memset` or `memcmp`. Having one choke point raises an obvious question. Could CPU-specific code in
those five functions make the whole library faster?

A plan for this already existed. It was built from generic advice about fast memory copies: `rep movsb` with
ERMS, AVX2 and AVX-512 loops, non-temporal stores for huge copies, and NEON on arm64. The plan sorted these into
size tiers, from under 32 bytes up to several MiB. We reviewed it against the code before writing any of it, and
almost nothing survived. This post describes how the review went and what it found.

## The method: list the call sites first

The review did not start by asking which technique is fastest. For each wrapper, it asked who calls it, with
what sizes, and on which path. That takes one `grep` per function, and the answers changed most of the plan.

| Wrapper | Real library callers | Verdict |
|---|---|---|
| `copy_bytes` | Constant-size loads and stores. Short literal copies on cold paths. A few large block and chunk copies. | Leave it to libc |
| `move_bytes` | One function, and it never overlaps | Call `copy_bytes` instead |
| `fill_bytes` | The decoder's slow path, for runs of one byte | Nothing to do |
| `clear_bytes` | Struct zeroing, plus table resets sized to the input | Nothing to do |
| `compare_bytes` | One 4-byte magic check | Nothing to do |

## Finding 1: the hot copies never reach the wrapper

The plan's best idea was a wide vector loop for literal and match copies of 32 to 256 bytes. Those copies are
real, and they are hot. They just don't go through `copy_bytes`.

The decoder's fast path, `decode_token_fast` in `src/copy.h`, copies in 16-byte steps with `copy16`. That calls
`copy_bytes(dst, src, 16)` with a constant size, which the compiler turns into one load and one store. Any
"backend" inside the wrapper would either be removed by the compiler or make that single instruction pair
slower. Widening the copies is a real idea, but it means changing the loop in `copy.h`, together with its slack
and room checks. Tuning the wrapper cannot do it.

## Finding 2: `move_bytes` never needs to handle overlap

The plan suggested a fast path for `move_bytes` when the ranges don't overlap. Its only library caller is
`copy_match_exact`, the exact match copy the decoder uses near the end of the output and for long runs with a
short period. That function copies a repeating pattern in growing pieces.

Every call copies `n` bytes from `[p − d, p − d + n)` to `[p + done, p + done + n)`. The ranges overlap only if
`n > d + done`. The piece sizes are chosen so that this never happens:

- The first piece is at most `d`, with `done = 0`.
- After that, each piece is `max(done, d) − done % d`, which is at most `done + d`.

A brute-force check over every distance up to 300 and every length up to 5,000 covered 8.4 million calls and
found no overlap. So `memmove` checks for overlap on every call and never finds any. The "fast path" comes down
to replacing the call with `copy_bytes`. That change still has to earn its place with a benchmark, because the
project's rule is that speed changes land only with a measurement.

## Finding 3: the large copies already run at libc speed

The plan's top tiers (`rep movsb`, non-temporal stores) were aimed at copies of a MiB or more. This codebase has a
few of those: joining the parallel encoder's outputs, writing and reading uncompressed blocks, and copying raw
decmpfs chunks.

On macOS, `memcpy` resolves to Apple's own `_platform_memmove`, which is tuned for Apple's cores. The benchmark
already shows what that achieves. Incompressible "noise" input is stored as uncompressed blocks, so decoding it
is essentially one `copy_bytes`, and it runs at about 34 GB/s on an M1 Pro. A hand-written NEON loop has little
room to beat that. For the parallel join, a bigger lever is to avoid the copy entirely.

The x86 tiers were dropped for a separate reason: this project can only measure on Apple Silicon. Shipping a
code path nobody can benchmark breaks the project's measurement rule.

## Finding 4: the rest are not hot at all

- **`compare_bytes`** has one library caller, a 4-byte magic check. The encoders find matches with their own
  8-bytes-at-a-time XOR loop (`match_length`), not with `memcmp`.
- **`fill_bytes`** writes runs of one repeated byte that the decoder's fast path declined, which are rare.
- **`clear_bytes`** mostly zeroes small structs with constant sizes. The only clears that grow with the input
  are two per-call hash-table resets, and those tables are already sized to the input. The large-input encoder
  avoids clearing altogether by recognizing stale entries. A rough estimate, not measured, puts the clearing at a
  few microseconds against more than 100 µs of encoding for a 64 KiB input. If that ever matters, the fix is to
  clear less, not faster.

## A correction on profiling

The plan said to "use a perf profile", but `perf` doesn't exist on macOS. The replacement is Instruments
(`xcrun xctrace record --template 'Time Profiler'`). There is also a catch: the wrappers are `static inline`, so
they never appear by name in a profile. What appears is libc's `_platform_memmove`, `_platform_memset` and
`_platform_bzero` for the variable-size calls, and you have to trace those back to their callers. Constant-size
copies don't appear at all, because they have become plain loads and stores inside the caller.

## Outcome

The single plan was split into one short document per wrapper under
[`docs/memory-operations/`](../docs/memory-operations/README.md). Each document lists the real call sites, an
assessment, and the measurement that would reopen the question. The headline recommendation went from "add
size-tiered SIMD backends" to:

1. Change `copy_match_exact` to call `copy_bytes`, and keep the change only if the benchmark agrees.
2. Profile before anything else. Unless a wrapper takes more than about 5% of runtime, leave it alone.
3. If wider copies are worth trying, change the `copy16` loops in `copy.h`, not the wrappers.

## Takeaways

- **Generic memcpy advice is about large copies.** Compressors mostly do small, fixed-size ones, and the
  compiler already handles those well.
- **A wrapper used as a choke point is good for auditing, but that's not where the time goes.** The hot loops
  call the wrapper with constant sizes, so the work happens wherever the compiler inlines those calls.
- **Look for the cheaper fix first.** The `move_bytes` fast path came down to calling a different function. The
  parallel join is better solved by not copying, and the hash-table clears by clearing less.
- **Check a proof by brute force.** The non-overlap argument takes three lines. The brute-force check took a
  second to run, and it is what made the claim safe to put in writing.
