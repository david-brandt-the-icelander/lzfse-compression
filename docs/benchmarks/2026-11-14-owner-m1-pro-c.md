# Benchmark: lzfse-zazzcat (C) vs Apple's libcompression on an M1 Pro

- **Date:** 2026-11-14
- **Commit:** `2a2729f` (the C conversion plus the decoder's register and early-lookup changes)
- **Machine:** the owner's MacBook Pro, Apple M1 Pro (10 threads), macOS 27.0.1.
- **Command:**
  ```bash
  make CPU_FLAGS=-mcpu=apple-m1 build/lzfse-bench
  build/lzfse-bench --runs 11 --synthetic <session log>
  ```
- **Inputs:** the harness's synthetic corpus, plus the same real Claude Code session log as the
  [Rust-era run](2026-11-14-owner-m1-pro.md).

## Method notes

- **Comparing runs:** compare by the ratios in the summary, which come from contenders timed back to back within
  one run. Apple's own speeds move by up to 15% between runs.
- **Correctness gate:** every chunk was decoded by both decoders before its timing counted.
- **The C conversion alone** (`1d4219e`, measured the same day without a saved table) decoded text at 0.94–0.97
  and binaries at 0.86–0.92 of Apple's speed. Commit `2a2729f` keeps the token loop's state in registers on
  aarch64; that alone moved text to 1.06–1.12 and binaries to 0.95–0.99.

## Results

| Input | Shape | Contender | Ratio | Encode MB/s | Decode MB/s |
|---|---|---|---:|---:|---:|
| session log | 64 KiB chunks | apple libcompression | 0.473 | 177 | 1003 |
| session log | 64 KiB chunks | lzfse-zazzcat | 0.473 | 243 | 1012 |
| session log | 64 KiB chunks | lzfse-zazzcat parallel | 0.473 | 1462 | 5909 |
| session log | 1 MiB chunks | apple libcompression | 0.354 | 198 | 1361 |
| session log | 1 MiB chunks | lzfse-zazzcat | 0.351 | 234 | 1354 |
| session log | 1 MiB chunks | lzfse-zazzcat parallel | 0.351 | 1495 | 7583 |
| session log | whole | apple libcompression | 0.338 | 202 | 1426 |
| session log | whole | lzfse-zazzcat | 0.335 | 239 | 1430 |
| text | 64 KiB chunks | apple libcompression | 0.229 | 127 | 1262 |
| text | 64 KiB chunks | lzfse-zazzcat | 0.210 | 198 | 1332 |
| text | 64 KiB chunks | lzfse-zazzcat parallel | 0.210 | 1204 | 6251 |
| text | 1 MiB chunks | apple libcompression | 0.225 | 130 | 1300 |
| text | 1 MiB chunks | lzfse-zazzcat | 0.202 | 219 | 1429 |
| text | 1 MiB chunks | lzfse-zazzcat parallel | 0.202 | 925 | 5672 |
| text | whole | apple libcompression | 0.224 | 134 | 1306 |
| text | whole | lzfse-zazzcat | 0.202 | 223 | 1463 |
| jsonl+base64 | 64 KiB chunks | apple libcompression | 0.761 | 212 | 662 |
| jsonl+base64 | 64 KiB chunks | lzfse-zazzcat | 0.763 | 488 | 708 |
| jsonl+base64 | 64 KiB chunks | lzfse-zazzcat parallel | 0.763 | 1785 | 3408 |
| jsonl+base64 | 1 MiB chunks | apple libcompression | 0.757 | 214 | 675 |
| jsonl+base64 | 1 MiB chunks | lzfse-zazzcat | 0.757 | 319 | 713 |
| jsonl+base64 | 1 MiB chunks | lzfse-zazzcat parallel | 0.757 | 1045 | 3265 |
| jsonl+base64 | whole | apple libcompression | 0.757 | 212 | 681 |
| jsonl+base64 | whole | lzfse-zazzcat | 0.757 | 295 | 727 |
| binary | 64 KiB chunks | apple libcompression | 0.416 | 138 | 1026 |
| binary | 64 KiB chunks | lzfse-zazzcat | 0.403 | 144 | 1011 |
| binary | 64 KiB chunks | lzfse-zazzcat parallel | 0.403 | 888 | 4660 |
| binary | 1 MiB chunks | apple libcompression | 0.416 | 128 | 1085 |
| binary | 1 MiB chunks | lzfse-zazzcat | 0.406 | 130 | 1032 |
| binary | 1 MiB chunks | lzfse-zazzcat parallel | 0.406 | 597 | 4655 |
| binary | whole | apple libcompression | 0.416 | 130 | 1132 |
| binary | whole | lzfse-zazzcat | 0.407 | 149 | 1073 |
| noise | 64 KiB chunks | apple libcompression | 1.011 | 217 | 679 |
| noise | 64 KiB chunks | lzfse-zazzcat | 1.000 | 455 | 34100 |
| noise | 64 KiB chunks | lzfse-zazzcat parallel | 1.000 | 1656 | 23432 |
| noise | 1 MiB chunks | apple libcompression | 1.010 | 221 | 667 |
| noise | 1 MiB chunks | lzfse-zazzcat | 1.000 | 485 | 41528 |
| noise | 1 MiB chunks | lzfse-zazzcat parallel | 1.000 | 994 | 22550 |
| noise | whole | apple libcompression | 1.010 | 223 | 696 |
| noise | whole | lzfse-zazzcat | 1.000 | 507 | 40330 |
| zeros | 64 KiB chunks | apple libcompression | 0.003 | 228 | 16352 |
| zeros | 64 KiB chunks | lzfse-zazzcat | 0.003 | 8314 | 23046 |
| zeros | 64 KiB chunks | lzfse-zazzcat parallel | 0.003 | 25811 | 39756 |
| zeros | 1 MiB chunks | apple libcompression | 0.001 | 223 | 38836 |
| zeros | 1 MiB chunks | lzfse-zazzcat | 0.001 | 21183 | 47393 |
| zeros | 1 MiB chunks | lzfse-zazzcat parallel | 0.001 | 55554 | 42582 |
| zeros | whole | apple libcompression | 0.001 | 142 | 40721 |
| zeros | whole | lzfse-zazzcat | 0.001 | 21732 | 49345 |

## Summary

Each cell is our speed divided by Apple's, with the Rust version's run in brackets. Above 1 means faster.

| Input | Encode | Decode | Parallel encode (10 threads) | Parallel decode (10 threads) |
|---|---:|---:|---:|---:|
| Session log (real) | 1.18–1.37 (1.06–1.26) | 0.99–1.01 (1.00–1.02) | 7.6–8.3 (7.2–8.8) | 5.6–5.9 (5.6–6.3) |
| Text | **1.56–1.68** (1.34–1.35) | **1.06–1.12** (1.00–1.04) | 7.1–9.5 (9.0–9.7) | 4.4–5.0 (5.0–6.0) |
| JSON + base64 | 1.39–2.30 (1.24–2.10) | 1.06–1.07 (1.10) | 4.9–8.4 (6.3–10.1) | 4.8–5.1 (6.1–7.0) |
| Binary | **1.02–1.15** (0.91–0.95) | **0.95–0.99** (0.90–0.95) | 4.7–6.4 (6.2–6.4) | 4.3–4.5 (5.0–5.6) |
| Noise | 2.1–2.3 (2.1–2.3) | 50–62 (46–61) | 4.5–7.6 (4.0–9.2) | 34–35 (27–32) |
| Zeros | 36–153 (22–54) | 1.2–1.4 (1.1–2.3) | 113–249 (81–113) | 1.1–2.4 (1.1–4.1) |

- **Ratios** are unchanged from the Rust version: the C encoder writes byte-identical streams.
- **Single-threaded:** the C version encodes everything faster than Apple's, binaries included (they were 5–9%
  behind), and decodes text 6–12% faster. Binary decoding is 1–5% behind (was 5–10%).
- **Parallel:** lower than the Rust version's on several rows, most at 1 MiB chunks (binary encode 850 → 597 MB/s,
  text decode 8098 → 5672 MB/s). The cause is not measured yet. With only a few 1 MiB chunks per thread, a
  slow chunk or one of the M1 Pro's two efficiency cores holds up the whole row, so these rows vary more between
  runs than the single-threaded ones.
