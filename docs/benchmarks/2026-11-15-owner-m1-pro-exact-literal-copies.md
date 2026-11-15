# Benchmark: exact literal copies without `memcpy` in the encoders, on an M1 Pro

- **Date:** 2026-11-15
- **Change:** both encoders copy literals with `wild_copy` except within 16 bytes of the end of the input, where
  they used `copy_bytes` (a variable-size `memcpy` call). They now use `copy_exact_inline` (`src/copy.h`):
  16-byte steps plus one last overlapping 16-byte step, or two overlapping 8- or 4-byte steps, or three single
  bytes, all inline. The call rarely ran, but it sat inside the parse loops. In the LZVN encoder `emit_literals`
  is now always inlined (with the larger copy inside, the compiler had stopped inlining it). Baseline: `6fa5b84`
  (main).
- **Machine:** the owner's MacBook Pro, Apple M1 Pro (10 threads), macOS 27.0.1.
- **Command:** both builds with `make CPU_FLAGS=-mcpu=apple-m1 build/lzfse-bench`, then five rounds of
  ```bash
  build/lzfse-bench --runs 11 --synthetic --chunk 1024 --chunk 2048 --chunk 4000 --chunk 16384 --chunk 65536 --chunk 1048576
  ```
  alternating baseline and change in each round. The table shows the median over the five rounds.
- **Inputs:** the harness's synthetic corpus. Chunks of 1, 2 and 3 KiB (4000 bytes) take the LZVN encoder; 16 and
  64 KiB the small-input LZFSE encoder (`encode_small`); 1 MiB the main LZFSE encoder.

## Method notes

- **Raw speeds, not relative to Apple.** Apple's encoder ran at one of two speeds from run to run (text at
  64 KiB: 104 or 126–130 MB/s), in both builds' runs. Dividing by it, as earlier notes did, adds that jump to our
  numbers, while our own speeds stayed within about 1–2% across rounds. So the table compares our MB/s directly.
- **Spread:** "worst / best" compares the change's slowest round with the baseline's fastest round, and the
  change's fastest round with the baseline's slowest. When both are positive, no round of the baseline beat any
  round of the change.
- **Output:** the compressed sizes (ratio column) are identical for both builds: only how literals are copied
  changed.

## Generated code

Counted with `objdump -d -r` on `build/obj/*.o`.

| | `memcpy` calls | Code size (bytes) | Stack loads and stores in the encoder function |
|---|---|---|---|
| `lzvn.o` (`lzvn_encode_with_table`) | 5 → 3 (the rest are outside the encoder) | 3528 → 3764 | 30 → 12 (only its prologue and epilogue) |
| `encode.o` (`lzfse_zazzcat_encoder_encode`) | 9 → 1 | 7160 → 8044 | 104 → 85 |
| `encode_small.o` (`encode_small`) | 29 → 0 | 21872 → 25160 | 300 → 326 |

Before the change, the LZVN parse loop stored 5 values to the stack before every literal run of 4 or more bytes
and loaded 7 back after it, whether or not the `memcpy` ran. After it, the loop does not touch the stack.

## Encode

| Input | Chunks | Ratio | Baseline MB/s | Change MB/s | Change | Worst / best |
|---|---|---:|---:|---:|---:|---|
| text | 1 KiB | 0.472 | 283 | 288 | +1.8% | −1% / +4% |
| text | 2 KiB | 0.416 | 310 | 317 | +2.3% | 0% / +4% |
| text | 3 KiB | 0.388 | 317 | 323 | +1.9% | 0% / +4% |
| text | 16 KiB | 0.227 | 185 | 186 | +0.5% | 0% / +4% |
| text | 64 KiB | 0.210 | 198 | 226 | +14.1% | +13% / +16% |
| text | 1 MiB | 0.202 | 237 | 236 | −0.4% | −1% / +1% |
| binary | 1 KiB | 0.629 | 305 | 318 | +4.3% | +3% / +5% |
| binary | 2 KiB | 0.613 | 303 | 313 | +3.3% | +1% / +7% |
| binary | 3 KiB | 0.606 | 330 | 345 | +4.5% | +3% / +7% |
| binary | 16 KiB | 0.430 | 153 | 154 | +0.7% | −1% / +1% |
| binary | 64 KiB | 0.403 | 173 | 178 | +2.9% | +2% / +3% |
| binary | 1 MiB | 0.406 | 157 | 157 | 0.0% | −3% / +1% |
| jsonl+base64 | 1 KiB | 1.010 | 240 | 246 | +2.5% | +1% / +5% |
| jsonl+base64 | 2 KiB | 1.003 | 234 | 241 | +3.0% | +1% / +4% |
| jsonl+base64 | 3 KiB | 1.000 | 317 | 324 | +2.2% | −1% / +5% |
| jsonl+base64 | 16 KiB | 0.768 | 593 | 594 | +0.2% | −4% / +1% |
| jsonl+base64 | 64 KiB | 0.763 | 638 | 638 | 0.0% | −1% / +1% |
| jsonl+base64 | 1 MiB | 0.757 | 355 | 356 | +0.3% | −1% / +1% |
| noise | 1 KiB | 1.012 | 241 | 247 | +2.5% | −5% / +6% |
| noise | 2 KiB | 1.006 | 236 | 241 | +2.1% | −2% / +4% |
| noise | 3 KiB | 1.003 | 326 | 334 | +2.5% | +1% / +4% |
| noise | 16 KiB | 1.001 | 577 | 581 | +0.7% | −5% / +2% |
| noise | 64 KiB | 1.000 | 657 | 664 | +1.1% | −1% / +3% |
| noise | 1 MiB | 1.000 | 681 | 679 | −0.3% | −2% / +1% |
| zeros | 1 KiB | 0.035 | 1313 | 1302 | −0.8% | −3% / +2% |
| zeros | 2 KiB | 0.021 | 1281 | 1276 | −0.4% | −1% / +6% |
| zeros | 3 KiB | 0.015 | 1325 | 1307 | −1.4% | −4% / +3% |
| zeros | 16 KiB | 0.009 | 7966 | 7682 | −3.6% | −7% / +3% |
| zeros | 64 KiB | 0.003 | 10565 | 10525 | −0.4% | −2% / +7% |
| zeros | 1 MiB | 0.001 | 21454 | 21676 | +1.0% | −6% / +5% |

## Reading the results

- **The LZVN encoder (1–3 KiB)** gains 2–4.5% on text, binary, jsonl+base64 and noise, in most rounds. Even
  inputs that end up stored raw run the encoder first, and their whole input is one literal run.
- **Text at 64 KiB** gains 14% in every round (226–227 MB/s against 195–200), and binary at 64 KiB 3%. This is
  `encode_small`, the path every APFS chunk takes. The gain is larger than the stack counts suggest: that
  function's stack traffic went *up* (300 → 326). Some of it is likely code layout rather than the removed call,
  and a later unrelated change could move it again.
- **16 KiB and 1 MiB** stay within the spread.
- **Zeros** compress to almost nothing and run at 1–22 GB/s, where a few microseconds of jitter move a row by
  several percent; the −3.6% at 16 KiB swings from −7% to +3% between rounds.

## Earlier measurement

The change was first measured on top of `4413871` (a branch with two other LZVN encoder commits, not on main),
with two more variants in the same rounds. It showed the same pattern, smaller: +2–3% for binary at 1–3 KiB,
+6% for binary and +3.5% for text at 64 KiB, and −0.8% (binary) at 16 KiB. The variants:

- **LZVN only** (`emit_literals` without the LZFSE change): the same LZVN gains, nothing at 64 KiB.
- **A cold out-of-line helper** (`COLD PRESERVE_REGISTERS`, still calling `memcpy`) in `emit_literals` instead
  of the inline copy: similar LZVN gains, but the loop still saves and reloads values around the call. For the
  LZFSE encoders this approach had been ruled out before: marking `block_writer_flush` the same way made them
  slower (`blogs/2026-11-14-keeping-values-in-registers.md`).

## Decode

The change does not touch any decoder. Decode speeds moved by up to 6% on some rows in either direction, within
the run-to-run spread of the decode columns.
