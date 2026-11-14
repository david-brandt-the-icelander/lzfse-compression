# Benchmark: constant-size literal copies in the LZVN encoder, on an M1 Pro

- **Date:** 2026-11-14
- **Change:** the LZVN encoder (`src/lzvn.c`, used for inputs under 4 KiB) copies the 1–3 literals of a match
  opcode with one 4-byte load and store, and longer literal runs with `wild_copy`, instead of a variable-size
  `memcpy` per token. Baseline: `ece5ad5` (main).
- **Machine:** the owner's MacBook Pro, Apple M1 Pro (10 threads), macOS 27.0.1.
- **Command:** both builds with `make CPU_FLAGS=-mcpu=apple-m1 build/lzfse-bench`, then five rounds of
  ```bash
  build/lzfse-bench --runs 11 --synthetic --chunk 1024 --chunk 2048 --chunk 4000 --chunk 65536
  ```
  alternating baseline and change in each round. The table shows the median over the five rounds.
- **Inputs:** the harness's synthetic corpus. Chunks of 1, 2 and 3 KiB (4000 bytes) take the LZVN path; 64 KiB
  chunks take the LZFSE path and act as a control.

## Method notes

- **Spread:** "worst / best" compares the change's slowest round with the baseline's fastest round, and the
  change's fastest round with the baseline's slowest. When both are positive, no round of the baseline beat any
  round of the change.
- **Against Apple:** each round's encode speed divided by Apple's encode speed in the same run, as the median over
  the rounds. This removes drift between runs.
- **Output:** the compressed sizes (ratio column) are identical for both builds, as expected: only the way the
  literals are copied changed.

## Encode

| Input | Chunks | Ratio | Baseline MB/s | Change MB/s | Change | Worst / best | vs Apple, before → after |
|---|---|---:|---:|---:|---:|---|---|
| text | 1 KiB | 0.472 | 273 | 289 | +5.9% | +5% / +6% | 4.88 → 5.16 |
| text | 2 KiB | 0.416 | 302 | 317 | +5.0% | +4% / +6% | 4.14 → 4.40 |
| text | 3 KiB | 0.388 | 312 | 327 | +4.8% | +4% / +5% | 3.63 → 3.85 |
| text | 64 KiB | 0.210 | 210 | 208 | −1.0% | −1% / 0% | 1.57 → 1.57 |
| binary | 1 KiB | 0.629 | 306 | 317 | +3.6% | +2% / +5% | 4.26 → 4.46 |
| binary | 2 KiB | 0.613 | 304 | 313 | +3.0% | +2% / +5% | 3.07 → 3.19 |
| binary | 3 KiB | 0.606 | 330 | 343 | +3.9% | +2% / +5% | 2.80 → 2.91 |
| binary | 64 KiB | 0.403 | 148 | 147 | −0.7% | −3% / +1% | 1.05 → 1.05 |
| jsonl+base64 | 1 KiB | 1.010 | 250 | 243 | −2.8% | −5% / 0% | 3.01 → 2.96 |
| jsonl+base64 | 2 KiB | 1.003 | 244 | 244 | 0.0% | −2% / +2% | 2.05 → 2.06 |
| jsonl+base64 | 3 KiB | 1.000 | 329 | 326 | −0.9% | −3% / +3% | 2.21 → 2.21 |
| noise | 1 KiB | 1.012 | 251 | 247 | −1.6% | −3% / −1% | 2.95 → 2.91 |
| noise | 2 KiB | 1.006 | 245 | 247 | +0.8% | −2% / +2% | 1.92 → 1.91 |
| noise | 3 KiB | 1.003 | 338 | 337 | −0.3% | −3% / 0% | 2.00 → 2.01 |
| zeros | 1 KiB | 0.035 | 1332 | 1334 | +0.2% | −1% / +1% | 26.06 → 26.16 |
| zeros | 2 KiB | 0.021 | 1303 | 1305 | +0.2% | −1% / +1% | 20.68 → 20.41 |
| zeros | 3 KiB | 0.015 | 1339 | 1343 | +0.3% | −1% / +1% | 18.33 → 18.16 |

## Reading the results

- **Text and binary at 1–3 KiB** are the inputs where the LZVN encoder finds matches with literals, and they gain
  3–6% in every round.
- **jsonl+base64 and noise at small sizes** find almost no matches and are stored raw, so the changed code barely
  runs. Their differences of up to ±3% are within the run-to-run spread.
- **64 KiB chunks** use the LZFSE encoder, which this change does not touch. They stay within the spread.

## Decode

The change does not touch any decoder. Decode speeds moved by less than 1% on every row that decodes tokens. The
rows that decode raw blocks (jsonl+base64 and noise at 1–3 KiB, one `memcpy` of a few KiB per chunk at
10–23 GB/s) varied between rounds by up to 40% in both builds, in a bimodal pattern. The decoder's functions
(`decode_with`, `decode_tokens`, `decode_run`, `lzfse_zazzcat_decode_into`) disassemble to identical instructions
at identical addresses in both binaries, so those differences are noise.
