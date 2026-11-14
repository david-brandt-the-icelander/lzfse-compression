# Many small files: the allocator didn't matter, the LZVN decoder did

*2026-11-14. lzfse-zazzcat is an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

`free-disk-space` will compress whole directories, and real directories are mostly small files. Our benchmark had
only ever timed 8 MiB inputs, cut into 64 KiB or 1 MiB chunks, with an encoder and decoder that are created once
and reused. So we asked two questions about small files. Does the allocator matter, and should we swap the system
`malloc` for jemalloc or mimalloc? Does the one-shot API, which sets up its state again for every file, cost
anything? The answers were no and no. Measuring them turned up a third thing that did matter: on files under
4 KiB, Apple's decoder was 17% faster than ours. A table-driven opcode dispatch fixed that, and now we are 10% ahead.

All M1 numbers come from an M1 Pro running macOS 27, the others from a 4-vCPU x86 cloud VM with Clang 18.

## A benchmark for many small files

`lzfse-bench` now ends with a many-small-files section. It cuts the synthetic text, JSON and binary inputs into 586
files of 512 bytes to 256 KiB. The sizes are spread evenly on a log scale, so each octave from 512 B to 256 KiB holds
about the same number of files. Four contenders run on them:

- **Ours, reused:** one encoder and one decoder for all files.
- **Ours, one-shot:** `lzfse_zazzcat_encode` and `lzfse_zazzcat_decode_into`, which set up their encoder or decoder
  state for every file.
- **Apple's, reused:** one scratch buffer for all files.
- **Apple's, one-shot:** a NULL scratch buffer, so `libcompression` allocates its own every time.

Output buffers are reused in every row, so the only difference between one-shot and reused is the library's own
setup. Results come in files per second, overall and in three size classes. The report also names the allocator in
use. When mimalloc is loaded, it says whether mimalloc really replaced `malloc` for the whole process.

## The allocator doesn't matter

The library never allocates in a hot loop. Reused encoders and decoders make no allocations at all, and the
allocation tests enforce that. So a faster allocator can only speed up setup, and setup turned out to be tiny.

Runs with glibc's `malloc` and mimalloc 2.1.2 on x86, and with the system allocator and Homebrew's mimalloc on the
M1, agreed within about 3% on small files, within run-to-run noise. On the M1, our rows were 2–3% *slower* with
mimalloc. Some of the main-table rows, the noise and zeros inputs that finish in well under a millisecond, swung
40–80% between runs in both directions, which is timing noise, not the allocator.

One trap is worth knowing if you swap allocators anyway. The library grows caller-owned `lzfse_zazzcat_buffer`
memory with `realloc`. If only the library's allocation macros point at mimalloc while the caller still uses the
system `malloc`, one side ends up freeing the other's memory. Replace the allocator for the whole process, or let
only the library create, grow and free those buffers.

## The one-shot API is free

| Thousands of files per second, encode / decode (M1) | 512 B–4 KiB | 4–32 KiB | 32–256 KiB |
|---|---:|---:|---:|
| Ours, reused | 163.1 / 644 | 13.5 / 58.2 | 1.8 / 8.9 |
| Ours, one-shot | 160.9 / 644 | 13.4 / 57.8 | 1.8 / 8.9 |
| Apple's, reused | 48.0 / 777 | 10.0 / 53.4 | 1.3 / 8.1 |
| Apple's, one-shot | 37.1 / 532 | 9.4 / 51.5 | 1.3 / 8.0 |

Our one-shot calls are within about 1% of reused ones in every class. Callgrind found where the small difference
goes: one-shot encoding runs 4.7% more instructions, nearly all of them zeroing the 768 KiB of hash tables that each
call over 64 KiB allocates fresh. One-shot decoding runs exactly the same instructions per file. Apple's one-shot
path costs 23% on encoding and 32% on decoding for files under 4 KiB, so `free-disk-space` can call our one-shot
API per file and come out ahead.

Encoding was never in doubt: we encoded files under 4 KiB 3.4–4.4 times faster than Apple's. But one cell was
wrong: decoding files under 4 KiB, 644 thousand files per second against Apple's 777.

## Where the decoder spent its time

Below 4 KiB both encoders write LZVN, LZFSE's simpler sibling format, rather than LZFSE's entropy-coded blocks. So the
gap was in our LZVN decoder. A dedicated test decoded 13,834 files of 512 B to 4 KiB, and Callgrind showed:

- **Per-call setup:** under 1% of the decode.
- **The slow, exact copy path** (taken near the end of a block, where wide copies would overrun): only 2% of
  opcodes.
- **The opcode loop:** about 100 instructions per opcode, 36 of them in the copies and **65 deciding what the opcode
  is.**

An LZVN opcode is a single byte whose value range selects one of about a dozen layouts. The opcode can carry 0–3
literals and a match with an 11-, 14- or 16-bit distance, reuse the previous distance, or be a literal-only or
match-only opcode, among others. Our decoder tested the byte against each range in turn, ran a separate check for
the reserved values, and stored "no match" and "previous distance" as special marker values in a shared struct,
which a later step turned back into real lengths and distances. Each of those steps was a few instructions and a
branch, and together they cost twice as much as the copies.

## A kind table and a switch

Each of the 256 possible opcode bytes now has its kind in a constant table: small, medium or large distance,
previous distance, small or large match, small or large literals, no-op, end of stream, or reserved. The decoder
looks up the kind and `switch`es on it, and each case computes the opcode's length, literal length, match length
and distance directly. Literal-only opcodes simply have match length 0, so the marker values are gone. The copies
still go through the shared `decode_token_fast` routine the LZFSE decoder uses.

Apple's reference decoder is table-dispatched too. Its license allows studying it, and we took that design idea
from it. We copied none of its code.

On x86 the change cut the decode's instructions by 27% (901 M to 657 M) and made tiny-file decoding 45–52% faster.
On arm64 Clang compiles the `switch` into a short tree of three or four comparisons on the kind rather than a jump
table, so only the M1 could tell how much would carry over.

## Checking that nothing changed

A decoder rewrite is where bugs hide, and LZVN streams come from untrusted files. We compiled the old and new
library each into its own shared library, loaded both into one test program, and compared them on 2,345,000 cases:

- valid streams from five inputs, with output capacities that were roomy, exact and 10 bytes too small;
- the same streams with one to four bytes changed and with their ends cut off;
- 2 million random byte strings.

Status, decoded length and every output byte agreed in all of them. The tests passed under AddressSanitizer and
UndefinedBehaviorSanitizer, and the LZVN and LZFSE decode fuzz targets ran for a minute each without findings. The
check was run again after merging other work into the branch.

## The result

| Files under 4 KiB, thousands per second, decode (M1) | Before | After |
|---|---:|---:|
| Ours, reused | 644 | **829** |
| Ours, one-shot | 644 | **816** |
| Apple's, reused | 777 | 757 |
| Apple's, one-shot | 532 | 514 |

Our tiny-file decoding is 29% faster and now 10% ahead of Apple's best case, where it was 17% behind. Apple's own
numbers moved by 2–4% between the runs, which is ordinary variation. The larger size classes didn't change, as
expected, because they use LZFSE blocks.

## What we learned

- **Measure the workload you will run.** Large chunked inputs never showed the LZVN decoder. Small files did,
  within one run.
- **Look for setup cost before replacing the allocator.** A library that doesn't allocate in its hot loops gains
  nothing from a faster `malloc`, and the benchmark showed it in minutes.
- **Count the decision, not just the work.** Our copies were already fast; working out what to copy cost twice as
  much.
- **Compare old and new decoders directly.** Loading both versions into one program and diffing their output on
  millions of valid, damaged and random inputs is a cheap check for a rewrite.
