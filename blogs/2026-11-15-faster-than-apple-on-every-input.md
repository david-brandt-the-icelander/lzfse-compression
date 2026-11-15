# Faster than Apple's on every input: where the combined work stands

*2026-11-15. lzfse-zazzcat is an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

Two days ago the project was a Rust library that beat Apple's encoder but lost to its decoder on binaries. Since
then it has been converted to C, several sessions have worked on it in parallel, and a dozen pull requests have
landed. Tonight the owner ran the benchmark on `main` on an M1 Pro. For the first time, every input encodes and
decodes faster than with Apple's code, on one core and on ten. This post puts that run next to the earlier ones,
says which changes moved which numbers, and says what the numbers do not show.

The full tables are in [the benchmark report](../docs/benchmarks/2026-11-15-owner-m1-pro-main.md).

## Comparing runs fairly

Every run times our library and Apple's `libcompression` in the same process, with the contenders interleaved run
by run, so both see the same machine. Comparing *between* runs is harder: Apple's own speeds have moved by up to
15–20% from one session to the next. That's why earlier reports compared ratios (our speed divided by Apple's)
rather than MB/s.

This run is unusually clean. Apple's speeds matched the previous evening's run within about 1% (text encoding at
133/136/137 MB/s against 133/137/138), so this time the absolute numbers compare directly too.

## The result

Our speed divided by Apple's, single-threaded, with the previous evening's run in brackets:

| Input | Size vs Apple's output | Encode | Decode |
|---|---|---:|---:|
| Real session log | as small, or up to 1% smaller | 1.25–1.67 (1.18–1.37) | 1.08 (0.99–1.01) |
| Text | 8–10% smaller | 1.77–1.82 (1.56–1.67) | 1.29–1.35 (1.17–1.25) |
| JSON + base64 | equal (0.3% larger at 64 KiB) | 1.53–3.04 (1.38–2.32) | 1.11–1.12 (1.06–1.07) |
| Binary | 2–3% smaller | 1.18–1.32 (1.06–1.13) | 1.09–1.15 (1.02–1.07) |
| Many small files | 2% smaller | 1.55 | 1.16 |

(The session log's brackets come from the first C run, which was the last to include it.)

Ten threads run 8.8–19.6 times as fast as Apple's single thread on encoding and 5.3–7.8 times on decoding.
Apple's API has no parallel mode, so those numbers say what our chunked API gives a caller like
`free-disk-space`, not that our code is that much faster.

## What moved which numbers

Because Apple's speeds held still between the two runs, each change can be read off the absolute numbers.

**Decoding** rose by 4–10%. Only one decoder change landed in between, the bit-reader refill and the removed length
checks ([the binary decode margin](2026-11-15-the-binary-decode-margin.md)). Its instruction counts had predicted
text −8.1%, binary −7.3% and JSON −4.6%; the M1 showed text +8–10%, binary +5–7% and JSON +4–5%. That close a
match is the best evidence so far that counting instructions on x86, plus reading the arm64 assembly, predicts the
M1 well for this decoder.

**Encoding** rose by 8–31%, all from other sessions' work in the same period:

- packing hash buckets and FSE fields into words;
- copying the last literals of an input inline instead of calling `memcpy`, which also stopped the parse loop
  spilling registers around a call that rarely ran ([a rare call still costs](2026-11-15-a-rare-call-still-costs.md));
- writing each LZVN opcode with one store ([one store per opcode](2026-11-15-one-store-per-opcode.md)).

**Small files under 4 KiB** decode 9% faster than the evening before (829 to 905 thousand files per second), on
top of the 29% from the table-driven LZVN dispatch
([small files and the LZVN decoder](2026-11-14-small-files-and-the-lzvn-decoder.md)). Below 4 KiB we now encode
3.5 times as fast as Apple's and decode 1.16 times as fast as Apple's best case.

## What the numbers don't show

- **One machine.** These are an M1 Pro's numbers. The M3 has only been measured on the Rust version.
- **Synthetic inputs, mostly.** Apart from one real session log, the inputs are generated: English-like text, JSON
  lines with base64 attachments, x86-like instruction records, noise and zeros. Real binaries and documents will
  land somewhere in the same range, but not on these exact numbers.
- **Noise and zeros aren't in the table** on purpose. We store incompressible data raw, so "decoding" noise is a
  memory copy (81–86 times Apple's speed), and long runs are cheap for our match finder (45–154 times Apple's
  encoding). Those are real for files we write, but they measure a different job, not faster code.
- **Single runs.** One run per state, and run-to-run noise is about 5%. The binary-decode margin (9–15%) is now
  clear of it, but a 1% difference anywhere in the table is not.

## What's next

The library is ready to be used. The next step is the reason it exists: `free-disk-space`'s `compress` command,
which can't compress anything on macOS 27 because `ditto --hfsCompression` quietly writes uncompressed copies.
The plan is to give the library one more function, `lzfse_zazzcat_decmpfs_apply`, that puts compressed data onto a
file and has the kernel accept it, taken from the code the library's macOS test already uses to verify that. Then
`free-disk-space` calls the library instead of `ditto`, and the one write path that has to be exactly right lives
in one tested place.
