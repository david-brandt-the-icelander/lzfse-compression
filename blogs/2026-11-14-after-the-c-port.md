# After the C port: chasing the speed we lost (and some we didn't)

*2026-11-14 · lzfse-zazzcat*

On 2026-11-14 we converted lzfse-zazzcat, our from-scratch LZFSE implementation, from Rust to C. The C encoder writes byte-identical
streams to the Rust one, and every test passed, so the conversion was correct. Whether it was as fast was a
different question, and answering it took the rest of the day. This post records what we measured, what we found,
and what is still open.

All speeds are measured against Apple's `libcompression` in the same process with `lzfse-bench`. A "ratio" of 1.10
means we are 10% faster than Apple's code in that run. Comparing ratios rather than raw MB/s matters: Apple's own
speed moves by up to 15–20% between runs on the same machine, so only contenders timed back to back in one run can
be compared directly.

## 1. The decoder got slower in C, and C's aliasing rules were why

The owner's first M1 Pro run of the C version decoded more slowly, relative to Apple, than the Rust version had:

| Single-threaded decode vs Apple | Rust | C, as converted |
|---|---:|---:|
| Text | 1.00–1.04 | 0.94–0.97 |
| Binaries | 0.90–0.95 | 0.86–0.92 |

The aarch64 assembly showed why. In C, a store through a `uint8_t *` may legally change *any* memory, so after every
byte the decoder wrote, the compiler had to assume the token count, table pointers and buffer fields might have
changed, and reload them. It also postponed the three FSE state updates until after each token's copies, which ran
out of registers and spilled values to the stack. Rust's `&mut` carries a "nothing else points here" promise to the
compiler; C has to be told.

The fix (commit `2a2729f`):

- copy the header and decoder fields the token loop reads into local variables;
- mark the fast kernel's output and literal pointers `restrict`;
- gather four decoded literals in a register and store them with one 32-bit write;
- look up the *next* token's three FSE table entries right after decoding this token's values, before its copies,
  so the states stay in registers and the loads overlap the copies.

Spill and reload instructions in the token loop dropped from 10 to 4 per iteration, and stack accesses in the function
from 62 to 43. On x86 the same change *added* 8–12% instructions, because x86 has half as many registers and the
early lookups spill there. Since the M1 is the target, the M1 decided:

| Single-threaded vs Apple, M1 Pro | Encode | Decode |
|---|---:|---:|
| Real session log | 1.18–1.37 | 0.99–1.01 |
| Text | **1.56–1.68** | **1.06–1.12** |
| JSON + base64 | 1.39–2.30 | 1.06–1.07 |
| Binaries | **1.02–1.15** | **0.95–0.99** |

The C version now encodes everything faster than Apple's, binaries included (Rust was 5–9% behind there), and decodes
text 6–12% faster. Binary decoding is still 1–5% behind. The full table is in
[`docs/benchmarks/2026-11-14-owner-m1-pro-c.md`](../docs/benchmarks/2026-11-14-owner-m1-pro-c.md).

A side note on CI: the virtual M1 runner's numbers for the same commits were mixed (text better, binaries worse), and
Apple's own speeds dropped about 20% between two runs there. Shared runners are fine for catching large regressions,
not for deciding a 5% change.

## 2. The parallel rows dropped, and the build machine hid the cause

The same M1 run showed several parallel rows (10 threads) below the Rust version's, worst at 1 MiB chunks:

| Parallel, M1 Pro | Rust | C |
|---|---:|---:|
| Binaries, encode, 1 MiB chunks | 850 MB/s | 597 MB/s |
| Text, decode, 1 MiB chunks | 8098 MB/s | 5672 MB/s |

The C thread pool is a straight port of the Rust design: about four tasks per thread, tasks claimed with an atomic
counter, one cached encoder per thread. The benchmark's parallel decode is also the same in both versions: fresh
threads each run, each with its own decoder and output buffer, each taking every Nth stream.

We first rebuilt the last Rust version (`7be57b4`) next to the C one on the development machine and ran them
alternately. No gap. Then a question from the owner changed the investigation: *what CPU is that?* An Intel Xeon in a
4-vCPU cloud VM, compiling with GCC 13. That machine has no efficiency cores, a different compiler from Apple's, and
noisy shared hardware.

Switching to Clang 18, from the same LLVM family as Apple Clang, did two things:

- It explained a puzzle: jsonl+base64 encoded about 40% slower in C than in Rust under GCC, but not under Clang.
  It was a GCC code-generation quirk, irrelevant to the Mac.
- It reproduced a parallel slowdown that GCC had hidden. Single-threaded speed was unchanged, but some parallel
  encode rows were 20–40% lower than GCC's and Rust's.

The cause was **false sharing**. Each encoding task writes into its own output buffer, but the buffers' `{data,
length, capacity}` fields sat side by side in one shared array. The bit writer updates `length` on every flush, every
few bytes. In C that is a real store each time, for the same aliasing reason as in section 1. Neighbouring tasks run
on different cores, so those cores kept taking the same cache line from each other. Apple Silicon's cache lines are
128 bytes, so five buffers share each line. The Rust version kept `length` in a register and never paid this.

The fix is a few lines: each task works on a copy of its buffer's fields on its own stack and writes them back once.

| Parallel encode, Clang, 4-core x86, MB/s | Before | After | Rust |
|---|---:|---:|---:|
| jsonl+base64, 64 KiB chunks | 297–344 | **575–678** | 404–506 |
| jsonl+base64, 1 MiB chunks | 238–290 | **346–384** | 268–306 |
| noise, 64 KiB chunks | 310–359 | **516–578** | 488–515 |
| noise, 1 MiB chunks | 211–260 | **401–609** | 170–215 |

Inputs that write the most output per unit of work gain the most; text and binaries did not change on that machine.
CI's 3-thread virtual Mac did not show a clear change either, which fits: three threads share fewer cache lines than
ten. How much of the M1 gap this closes still needs a run on the M1 itself.

## 3. Parallel decoding: no gap where we can look

The decode side has no shared output buffers, so false sharing cannot explain it. A diagnostic that times each
parallel-decode run in parts found, on Linux:

- per-thread setup (decoder and buffer allocation) under 5 µs;
- starting and joining the threads about 140 µs, 3% of a run;
- the rest is decoding; keeping decoders and buffers between runs changed nothing;
- C and Rust scale alike; C's parallel rows simply track its single-threaded speed.

So the gap is either specific to macOS or not real. Two things make the 1 MiB rows unreliable: the synthetic inputs
are 8 MiB, so there are only 8 streams for 10 threads, and the M1 Pro has 8 performance cores and 2 efficiency cores.
When one stream lands on an efficiency core, the whole row waits for it, and nothing can rebalance a single stream.
Also, the Rust and C numbers came from runs in different sessions.

The decisive measurement is a paired run: the Rust and C benchmarks built side by side on the M1 and run
alternately. If C still trails, the first candidate fix is to let decode threads claim streams one at a time, as the
encoder's pool does, instead of each taking every Nth stream. That helps at 64 KiB chunks, where a fast core can take
over streams a slow one would otherwise hold. It cannot help when there are fewer streams than threads.

## 4. Interlude: `main` went red

While this was going on, other sessions merged two pull requests into `main`. One of them, which added `bare_`
wrappers for functions whose results are deliberately ignored, was merged with failing CI. `main` then no longer built
on Linux:

- `lstat` and `mode_t` are hidden by glibc under `-std=c11` unless `_DEFAULT_SOURCE` is defined before any system
  header. macOS shows them anyway, which is why the macOS job and the owner's builds kept working. The Makefile now
  defines it for every build.
- The no-threads build left out the pthread wrappers, but the benchmark starts its own threads. A program that does
  can now ask for them with `LZFSE_ZAZZCAT_PROGRAM_USES_THREADS`.
- One test file was not clang-formatted.

The lesson is an old one: a red check on a pull request is a broken `main` waiting to happen.

## 5. Did the merges cost speed?

With everything merged, we compared the result with the version just before the other sessions' merges. On a shared
VM, wall-clock timings flagged zeros encoding as 12–33% slower. Instruction counts, which are exact, said otherwise:

| Single-threaded, current vs before the merges | Encode instructions | Decode instructions |
|---|---|---|
| Text, jsonl, binaries, noise | identical | 0.3–0.6% fewer |
| Zeros, 64 KiB chunks | +0.1% | 4.8% fewer |

Both versions write the same number of compressed bytes for every input and chunk size, and every chunk decodes back
to its original. A zeros encode takes well under a millisecond, so the timing "regression" was noise. When timing and
instruction counts disagree on a tiny workload, believe the counts.

## What we learned

- **C needs to be told what Rust knows.** Byte stores alias everything. Hot loops need locals, `restrict` and grouped
  stores, and shared structures that hot code writes need to stay off shared cache lines.
- **Measure on the target's compiler family.** GCC hid a real problem and invented a fake one. Clang reproduced what
  Apple Clang does closely enough to find the cause.
- **Compare within one run.** Ratios between contenders timed back to back are stable; absolute numbers across
  sessions and machines are not.
- **Few tasks on mixed cores is a lottery.** Rows with fewer chunks than threads measure core placement as much as
  code.
- **Count instructions when timings are tiny or noisy.** They separate real changes from scheduler noise.

## Still open

- The owner's paired C/Rust run on the M1, to see how much of the parallel gap the false-sharing fix closed and
  whether the parallel decode gap is real.
- Single-threaded binary decoding, 1–5% behind Apple.
- Wiring the decmpfs writer into `free-disk-space`.
