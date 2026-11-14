# Zeroing memory faster: five ideas, measured

*2026-11-14 · lzfse-zazzcat*

Clearing memory looks like a natural place to speed up a compressor. Hash tables have to start empty, FSE
decoding tables are rebuilt for every block, and headers are assembled in zeroed scratch space. One afternoon we
went through the common tricks for making zero-fills faster and measured each one before writing any code.
Most of them lost. One idea removed clears we never needed, and that change landed.

All numbers come from an M1 Pro (8 performance and 2 efficiency cores, 16 KiB pages, 128-byte cache lines,
64-byte `dc zva` blocks) running macOS 27. Each one is the best of many runs of a small C program built with
`clang -O2`. Clears go through the project's `clear_bytes` wrapper, which calls the C library's `memset`.

## First, how big are our clears?

Every answer below depends on this. The library's clears are small:

| What is cleared | Size | When |
|---|---|---|
| Structs and block headers | under 1 KB | per call or per block |
| FSE decoding tables | 4 KiB + 3 KiB | per LZFSE block |
| LZVN hash table (inputs under 4 KiB) | at most 4 KiB | per call |
| Long-match slots, 64 KiB input path | at most 32 KiB | per call |
| Buckets, 64 KiB input path (a fill loop, not `memset`) | 192 KiB | per call |
| Large-input hash tables | 768 KiB by default, up to 192 MiB | only when they grow, or about once per 4 GiB of input |

A 64 KiB clear takes about **0.5–0.7 µs** on one core. Encoding a 64 KiB chunk takes 145–510 µs on real
data.

## 1. Split big clears across threads?

The idea comes from parallel-loop models such as OpenMP, Cilk Plus, OpenCilk and ISPC: hand each thread a
slice of the buffer.

We timed one thread against two, with a worker parked on a condition variable (µs):

| Size | One thread | Two threads |
|---|---|---|
| 4 KiB | 0.04–0.09 | 2.4–3.9 |
| 64 KiB | 0.55–0.70 | 3.2–3.7 |
| 1 MiB | 8.9–10.9 | 12.2–13.1 |
| 16 MiB | 143 | 98–136 |

Then we timed the real thing, `#pragma omp parallel for` with libomp from Homebrew LLVM 21 (µs):

| Size | Serial | Idle policy ×2 | Idle policy ×8 | Spinning ×2 | Spinning ×8 |
|---|---|---|---|---|---|
| 4 KiB | 0.04 | 3.6 | 33 | 0.3 | 1.8 |
| 64 KiB | 0.5 | 4.2 | 34 | 1.4 | 1.8 |
| 1 MiB | 8 | 12 | 34 | 5.7 | 5.5 |
| 16 MiB | 140 | 137 | 103 | 76 | 68 |

"Idle policy" is OpenMP's default, where waiting threads sleep. "Spinning" is `OMP_WAIT_POLICY=active`, where
they busy-wait.

Waking sleeping threads costs 3.5–33 µs. Spinning workers cut that to 0.3–1.8 µs, but a 64 KiB clear still
ends up about three times slower. Splitting only starts to pay somewhere between 64 KiB and 1 MiB, and it
reaches about 2× at 16 MiB.

Besides, the clears already run inside `parallel.c`'s per-chunk tasks, with every core busy encoding. An
OpenMP region inside those tasks would start a second set of threads that compete for the same cores.
Cilk-style work stealing handles nesting better, but with every worker busy nobody is free to steal. ISPC's
SIMD side adds nothing either, because Apple's `memset` already uses `dc zva`, which zeroes a whole block
per instruction.

**Verdict: no.** It would pay only for clears of several MiB on idle cores, and we have neither.

## 2. Swap in a pre-cleared buffer from a pool?

The idea: rather than clearing a dirty table on the hot path, take a clean one from a pool, return the dirty
one, and let a background thread clear it.

We timed the per-call setup on the 64 KiB input path:

| Setup | Time |
|---|---|
| Bucket fill, 192 KiB | 4.1–4.3 µs |
| Long-slot clear, 32 KiB | 0.21–0.25 µs |
| LZVN table clear, at most 4 KiB | about 0.05 µs |

Together that is 1–3% of encoding a 64 KiB chunk of real data. It reaches about a third for all-zero chunks,
which encode in roughly 13 µs.

A pool doesn't fit:

- **The costly part depends on the input.** Each bucket starts as "position 0, holding the input's first
  4 bytes", so an empty slot is still a real candidate and the match finder never checks for empty slots. A
  buffer prepared ahead of time can't know those bytes.
- **It moves the work rather than removing it.** Most 64 KiB chunks are encoded in parallel with every core
  busy, so a background clearer takes time from the encoders.
- **It costs memory and cache warmth.** It needs a thread inside the library, a version for builds without
  threads, and twice the table memory. The pooled buffer would also arrive cleared by another core, so it
  wouldn't be in this core's L1 cache, whereas clearing in place leaves the table warm.

The better tool is **generation tagging**. The large-input path already uses it: stored positions are offset
by a running `position_base`, so entries left from earlier inputs look stale and the tables are never
cleared. Moving the 64 KiB path to it would mean widening 16-bit positions to 32 bits. That grows its
tables from 224 KiB to 320 KiB, which may cost more in lookups than the 4 µs it saves. Only a benchmark can
settle that, and we haven't tried it yet.

**Verdict: no.** At most, an experiment with generation tags is worth running.

## 3. Are we clearing bytes nobody reads?

Yes. We went through every clear and asked whether something later overwrites what it zeroes, or never reads
it:

- **The one-shot decoder cleared its whole ~7 KB state on every call.** Only the literal-buffer pointer and
  capacity need a starting value, because the FSE tables are rebuilt for every LZFSE block before use. Streams
  made only of LZVN or uncompressed blocks never use the tables at all.
- **The FSE decoding tables were cleared and then filled.** Every state up to the sum of the frequencies gets
  written anyway, so only the states past that sum need zeroing. In streams from our encoder there are none.
  A hostile stream can still have frequencies that sum to less than the state count, and the decoder can reach
  those states, so they still get a defined zero entry.
- **Header parsers cleared a ~760-byte struct and then set every field.** The exception is a v2 header with no
  frequency tables, which now zeroes its frequencies itself.
- **The block encoder cleared its header and then set every field.** One catch: `write_v2_header` reads the
  stream sizes, padding and final states before they are known, to write placeholders. Without the clear
  those would be garbage, and `padding_bits + 7` could even overflow under the sanitizer. They are now set to
  0 explicitly.
- **Small stuff:** header placeholders that are always overwritten or dropped, encoder entries for symbols
  that never occur, and a decmpfs offset table that is always fully written.

One test also needed a change. `test_headers_round_trip` compares parsed headers byte for byte, including the
struct's 2 padding bytes, which the parsers no longer zero, so the test now clears the destination itself.

Each removed clear stays in the source as a commented-out line, with a note above it explaining why removing
it is safe. The change landed in PR #8.

**Caveat.** This went in without a benchmark, by the owner's decision, which is an exception to the project
rule that speed changes need a measurement. Most of these clears cost about 10 ns per block. The one that might
show up is the 7 KB decoder clear, worth roughly 0.1 µs per one-shot decode. That estimate comes from the clear
timings above and has not been measured end to end.

## 4. Are our zero-fills honouring alignment?

The concern: is clearing a misaligned address unsafe, or slow?

It is safe. `memset` accepts any address, and every typed pointer we pass is naturally aligned. Unaligned
multi-byte loads and stores elsewhere go through `copy_bytes`, never through pointer casts.

It isn't slow either. We cleared at offsets 0, 1, 8, 16, 64 and 100 bytes from a cache-line boundary:

| Size | Spread across offsets |
|---|---|
| 764 B | 9–10 ns |
| 4 KiB | 44–46 ns |
| 32 KiB | 340–361 ns |
| 64 KiB | 648–698 ns |

Apple's `memset` covers a ragged start and end with a few overlapping stores, then zeroes the middle with
`dc zva`. Constant-size clears are compiled into paired 16-byte stores, which Apple cores handle at full speed
even when misaligned. One run showed a 2× outlier at 7 KiB, but it did not reproduce.

**Verdict: nothing to fix.**

## 5. Release pages instead of writing zeros?

The idea: for very large buffers, ask the OS for fresh pages (remap, `madvise`, or `calloc` on new memory)
rather than writing zeros.

Times in µs. "Dense" writes every 64-byte line afterwards, as a filling hash table would. "Sparse" touches only
8 spots:

| Buffer | Dense: `memset` | Dense: `calloc` | Dense: remap | Sparse: `memset` | Sparse: remap |
|---|---|---|---|---|---|
| 768 KiB | 13 | 14 | 52 | 5 | 9 |
| 8 MiB | 144 | 144 | 826 | 47 | 52 |
| 192 MiB | 5,785 | 6,448 | 21,373 | 2,026 | 1,966 |

Releasing pages is cheap, but the first write to each fresh 16 KiB page traps into the kernel, which then
zeroes it. That cost about 1–1.7 µs per page, against about 0.2–0.5 µs for `memset` to cover the same 16 KiB.
The trick wins only when most pages are never touched again. Our hash tables are sized to the input, so
encoding touches nearly every page. Even the sparse case only broke even at 192 MiB, because releasing that
many dirty pages costs about as much as zeroing them.

The large-input tables already get the free version of this. They come from `calloc` rather than being
cleared, so memory new to the process arrives zeroed for nothing. Recycled memory gets zeroed by `calloc` at
`memset` speed, which is why those two columns match.

**Verdict: no** for speed. It could be useful for shrinking memory: an idle encoder created with a large
`hash_bits` holds up to 192 MiB that `madvise(MADV_FREE_REUSABLE)` could hand back to the OS.

## What we took away

- **Measure the size first.** At under 64 KiB, a clear finishes before any thread or system call can help.
  Every "make zeroing faster" trick has a fixed cost of 0.3–30 µs, and that is the whole budget.
- **The cheapest clear is the one you skip.** The only change that landed removed clears instead of speeding
  them up.
- **To keep a table empty, tag its entries rather than clearing it.** Generation tags already keep the
  large-input tables from ever being cleared, and they are the one idea here still worth an experiment, on the
  64 KiB path.
- **Removing a clear needs a careful read.** Two of the "unneeded" clears were quietly protecting something:
  fields read before they were set in the block encoder, and states a hostile stream can reach in the FSE
  tables.

## Open items

- Benchmark the removed clears, especially the one-shot decode of small chunks.
- Prototype generation tags on the 64 KiB encoder path and compare encode speed on 64 KiB chunks.
- Fix `make fuzz` on macOS 27. With the default toolchain it fails to link. With Homebrew LLVM 21's own libc++
  it links, but its AddressSanitizer runtime locks up during startup, before any library code runs. The cleanup
  above was fuzzed for 45 s per target under UBSan only.
