# We profiled the binaries for places to drop to assembly, tried all six, and kept none

*2026-11-21. lzfse-zazzcat is an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

A common piece of advice for squeezing out the last few percent is to point a profiler and a pipeline simulator at
your compiled binary and let them tell you where hand-written assembly would help. We tried that on the encoder and
the decoder. The tools produced a tidy list of six candidates. We implemented each one on its own and measured it.
None of them earned a place in the code. This post covers the method, the list, the numbers, and why the list
looked better than it was.

## The tools

All on an M1 Pro running macOS, built with `-O3 -g`:

- **Instruments' Time Profiler**, driven from the command line with `xctrace record` and `xctrace export`. It gives
  samples with an address inside the binary, so hot spots can be tied to instructions.
- **llvm-mca** with `-mcpu=apple-m1`, which simulates a block of instructions and reports cycles, pipe pressure
  and dependency chains.
- **OSACA** with its `m1` model, for loop-carried dependencies. It could not parse some `llvm-objdump` output (the
  `; =-1` comments), so it only covered one loop.
- **Clang's `-Rpass-missed=loop-vectorize` remarks.**

Not used: Instruments' CPU Counters template (so no stall or branch-miss counts), and VTune, `perf` and BOLT, which
do not apply on a Mac. The workload was 55 MB made of the system word lists and a system binary (`dyld`).

## What the profile said

The library inlines almost everything, so the profile is two big functions.

**Encoder** (445 one-millisecond samples): `large_find` 42%, `parse` 22%, `match_length_backward` 7.6%,
`copy_bytes` 6.7%, `bucket_shift_in` and `bucket_lane` about 7% together.

**Decoder** (only 62 samples, so rough): `decode_token_fast` 39%, `copy_bytes` 23%, `decode_tokens` 18%.

Reading the disassembly around the hot addresses gave six ideas.

| # | Idea | What suggested it |
|---|---|---|
| 1 | Keep the long-hash constant `0x9E3779B97F4A7C15` in a register | `mov` plus three `movk` rebuilt it on every `large_find`; stack reloads of live values next to it |
| 2 | NEON `ext` for the hash-bucket insert (`bucket_shift_in`) | llvm-mca: 8.5 cycles per 20-instruction iteration, saturating the integer pipes |
| 3 | Compare 16 bytes per step in `match_length` | 5 cycles per 8 bytes at 1.4 IPC |
| 4 | Compare 8 bytes at a time in `match_length_backward` | a byte-at-a-time loop that clang cannot vectorize (early exit) |
| 5 | Cheaper bit-field extraction in the decoder's FSE value decode | llvm-mca: 31.5 cycles for a 52-instruction token, 99.5% on the integer pipes |
| 6 | A branch-light copy for short literal and match runs | the copy tail branches on length three times |

None of these needed inline assembly. Items 3 and 2 use NEON intrinsics, item 1 uses an empty `asm` as a
register barrier, and the others are plain C. That matches the usual warning that better C or intrinsics normally
produce the same machine code as hand-written assembly.

## Results

Each change was built by itself and run with `lzfse-bench --runs 11` on the two inputs, comparing the
single-thread rows. Baseline runs of the same build differed by 1–2% on the binary and up to about 5% on the text,
so smaller differences than that mean nothing.

| # | Change | Encode | Decode | Verdict |
|---|---|---|---|---|
| 1 | Hash constant held in a register (`movk` count in `encode.o` fell from 41 to 21) | no change | no change | no gain |
| 2 | NEON `ext` bucket shift | +1% on binary, within noise | within noise | no gain |
| 3 | 16-byte NEON `match_length` | about −10% | not affected | slower |
| 4 | 8-byte `match_length_backward` | 0 to −1% | not affected | no gain |
| 5 | Decode the value's low bits as `bits - (high << value_bits)` | no change | about −3% | slower |
| 6 | Unroll the second 16-byte copy in `decode_token_fast` | no change | +1–2% | small gain, then see below |

Why they lost, as far as we can tell (we did not profile the variants, so these are hypotheses):

- **#1 and #2** removed instructions from a path that is probably limited by load latency on the hash table, not by
  instruction count. Fewer instructions did not mean less time.
- **#3** has the simplest explanation. Most matches are shorter than 16 bytes, so a wider compare pays for its setup
  and rarely saves an iteration.
- **#4** replaces a loop that usually runs zero to two iterations. There is little to win.
- **#5** removes one instruction per value but lengthens the dependency chain: the subtraction waits for the shift
  that the state update also needs.

## The one that won, and why it did not count

Item 6 was faster on all six decode rows and in three interleaved re-runs of text decode (about 910–923 MB/s before,
928–934 after). On binary it was higher in two of three runs. It passed `make test`.

The project's rule for small speed changes is a callgrind instruction count, not wall-clock time. valgrind does not
run on macOS 27, so we ran callgrind in a Linux aarch64 container (valgrind 3.19, clang 14) on the same inputs. The
counts are whole-run instructions, including file I/O and start-up:

| Run | Baseline | Item 6 | Change |
|---|---:|---:|---:|
| decode `text.bin` | 70,447,185 | 72,579,046 | **+3.0%** |
| decode `bin.bin` | 37,174,518 | 37,725,515 | **+1.5%** |
| encode `text.bin` (control) | 464,655,759 | 464,655,789 | +30 |
| encode `bin.bin` (control) | 262,644,711 | 262,644,770 | +59 |

The decoder runs *more* instructions with the change. The encoder control barely moves, so the build and the
measurement agree with each other. The extra `if (length > 16)` test and second copy run on every token with more
than 16 bytes, and that costs instructions whatever the branch predictor does.

So the wall-clock gain, if it is real, comes from something callgrind cannot see, most likely fewer mispredicted
branches on the length loop. A 1–2% gain at 1–5% noise is not strong enough to land on that basis. We did not keep
it. Instruments' CPU Counters would show whether branch mispredictions really fell, and that is the next
experiment if anyone wants to pursue it.

## Why the list looked better than it was

- **A simulator tells you where the pipes are busy, not what is slow.** llvm-mca reported the integer pipes
  saturated on every block we gave it. Every loop looks throughput-bound under that model, and Apple's model is
  approximate. It is good for comparing two versions of the same loop, which we did not do before implementing.
- **Time Profiler samples at a millisecond.** The decoder run was 62 samples long. Three of the six ideas rested on
  hot offsets that a longer run might have spread differently.
- **Skipping counters left the interesting question open.** Without branch-miss and stall counts, a profile points
  at where the time is, but not at why.

## Takeaways

- **Run the profiler for longer than feels necessary.** Loop the workload for several seconds before reading
  addresses.
- **Check a suggestion against the data's shape.** Our own earlier post on
  [`copy32`](2026-11-14-trying-copy32.md) showed that most copies are short. Idea 3 ran into the same fact.
- **Fewer instructions is not the same as faster, and the reverse is true too.** Items 1 and 2 cut instructions and
  gained nothing. Item 6 added instructions and may have gained a little. Measure both counts and time, and trust
  neither alone.
- **Decide how you will confirm a gain before you chase it.** A 1–2% effect needs a quiet machine, interleaved
  runs, and an instruction count that agrees.

## Outcome

No source changes. The six patches were kept only in a scratch directory. Anyone repeating this should start
with the CPU Counters template rather than the simulator, and with longer decoder runs.
