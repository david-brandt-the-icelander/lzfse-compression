# Benchmark: the current C version vs Apple's libcompression on an M1 Pro

- **Date:** 2026-11-14 (evening)
- **Commit:** `ece5ad5` (`main` after PR #12; the code is that of `7aaad2c`: the C conversion, the decoder register
  work, the parallel false-sharing fix, PRs #7 and #8 and the build fixes)
- **Machine:** the owner's MacBook Pro, Apple M1 Pro (8 performance and 2 efficiency cores, 10 threads), macOS 27.0.1.
- **Command:**
  ```bash
  make CPU_FLAGS=-mcpu=apple-m1 build/lzfse-bench && build/lzfse-bench --runs 11
  ```
- **Inputs:** the harness's synthetic corpus only (no session log this time).

## Method notes

- **Comparable with the Rust-era run.** Apple's own speeds in this run match the
  [Rust-era run](2026-11-14-owner-m1-pro.md) within 1–3% (text encode 133/137/138 against 133/137/137 MB/s, text decode
  1319/1356/1354 against 1317/1351/1356). So for once the absolute numbers of the two runs can be compared directly,
  including the parallel rows.
- **Correctness gate:** every chunk was decoded by both decoders before its timing counted.

## Results

| Input | Shape | Contender | Ratio | Encode MB/s | Decode MB/s |
|---|---|---|---:|---:|---:|
| text | 64 KiB chunks | apple libcompression | 0.229 | 133 | 1319 |
| text | 64 KiB chunks | lzfse-zazzcat | 0.210 | 208 | 1545 |
| text | 64 KiB chunks | lzfse-zazzcat parallel | 0.210 | 1447 | 7133 |
| text | 1 MiB chunks | apple libcompression | 0.225 | 137 | 1356 |
| text | 1 MiB chunks | lzfse-zazzcat | 0.202 | 228 | 1678 |
| text | 1 MiB chunks | lzfse-zazzcat parallel | 0.202 | 1303 | 10131 |
| text | whole | apple libcompression | 0.224 | 138 | 1354 |
| text | whole | lzfse-zazzcat | 0.202 | 230 | 1688 |
| jsonl+base64 | 64 KiB chunks | apple libcompression | 0.761 | 217 | 693 |
| jsonl+base64 | 64 KiB chunks | lzfse-zazzcat | 0.763 | 503 | 740 |
| jsonl+base64 | 64 KiB chunks | lzfse-zazzcat parallel | 0.763 | 3374 | 4175 |
| jsonl+base64 | 1 MiB chunks | apple libcompression | 0.757 | 219 | 700 |
| jsonl+base64 | 1 MiB chunks | lzfse-zazzcat | 0.757 | 332 | 745 |
| jsonl+base64 | 1 MiB chunks | lzfse-zazzcat parallel | 0.757 | 2072 | 5037 |
| jsonl+base64 | whole | apple libcompression | 0.757 | 221 | 702 |
| jsonl+base64 | whole | lzfse-zazzcat | 0.757 | 304 | 745 |
| binary | 64 KiB chunks | apple libcompression | 0.416 | 141 | 1051 |
| binary | 64 KiB chunks | lzfse-zazzcat | 0.403 | 149 | 1126 |
| binary | 64 KiB chunks | lzfse-zazzcat parallel | 0.403 | 968 | 5912 |
| binary | 1 MiB chunks | apple libcompression | 0.416 | 138 | 1162 |
| binary | 1 MiB chunks | lzfse-zazzcat | 0.406 | 151 | 1192 |
| binary | 1 MiB chunks | lzfse-zazzcat parallel | 0.406 | 824 | 7564 |
| binary | whole | apple libcompression | 0.416 | 138 | 1176 |
| binary | whole | lzfse-zazzcat | 0.407 | 156 | 1195 |
| noise | 64 KiB chunks | apple libcompression | 1.011 | 222 | 702 |
| noise | 64 KiB chunks | lzfse-zazzcat | 1.000 | 468 | 51782 |
| noise | 64 KiB chunks | lzfse-zazzcat parallel | 1.000 | 2994 | 26214 |
| noise | 1 MiB chunks | apple libcompression | 1.010 | 225 | 688 |
| noise | 1 MiB chunks | lzfse-zazzcat | 1.000 | 496 | 45590 |
| noise | 1 MiB chunks | lzfse-zazzcat parallel | 1.000 | 1762 | 25891 |
| noise | whole | apple libcompression | 1.010 | 229 | 694 |
| noise | whole | lzfse-zazzcat | 1.000 | 521 | 51782 |
| zeros | 64 KiB chunks | apple libcompression | 0.003 | 232 | 16481 |
| zeros | 64 KiB chunks | lzfse-zazzcat | 0.003 | 8766 | 23563 |
| zeros | 64 KiB chunks | lzfse-zazzcat parallel | 0.003 | 28056 | 45590 |
| zeros | 1 MiB chunks | apple libcompression | 0.001 | 225 | 40137 |
| zeros | 1 MiB chunks | lzfse-zazzcat | 0.001 | 21509 | 48771 |
| zeros | 1 MiB chunks | lzfse-zazzcat parallel | 0.001 | 54828 | 51150 |
| zeros | whole | apple libcompression | 0.001 | 144 | 42799 |
| zeros | whole | lzfse-zazzcat | 0.001 | 22490 | 49932 |

## Summary

### Single-threaded, against Apple

Our speed divided by Apple's in this run; above 1 means faster. In brackets: the [first C run](2026-11-14-owner-m1-pro-c.md)
(`2a2729f`).

| Input | Size vs Apple's output | Encode | Decode |
|---|---|---:|---:|
| Text | 8–10% smaller | **1.56–1.67** (1.56–1.68) | **1.17–1.25** (1.06–1.12) |
| JSON + base64 | 0.3% larger at 64 KiB, else equal | 1.38–2.32 (1.39–2.30) | 1.06–1.07 (1.06–1.07) |
| Binary | 2–3% smaller | **1.06–1.13** (1.02–1.15) | **1.02–1.07** (0.95–0.99) |

Every like-for-like row is now faster than Apple's. Binary decoding, the last row where Apple's code won (by 1–5%), is
now 2–7% ahead, and text decoding rose from 6–12% to 17–25% ahead. No decoder change since `2a2729f` explains that
much: PRs #7 and #8 cut the decoder's instruction count by only 0.3–0.6% on x86. Part of it is this run's conditions
(Apple's text decoding is 4–5% faster here than in the first C run too); the rest may come from code layout after
PR #8's restructuring. A repeat run would tell whether it holds.

Noise and zeros do different work on each side (we store noise raw; runs are cheap for our match finder), so their
ratios are not code-speed comparisons: noise decodes 66–75× and zeros encode 38–156× faster than Apple's, as files
written by our encoder.

### Parallel (10 threads), against the Rust version and the first C run

MB/s. Apple's numbers match the Rust-era run's, so these absolute numbers compare directly.

| Input | Shape | Encode: now / first C / Rust | Decode: now / first C / Rust |
|---|---|---:|---:|
| Text | 64 KiB | **1447** / 1204 / 1293 | **7133** / 6251 / 6564 |
| Text | 1 MiB | **1303** / 925 / 1232 | **10131** / 5672 / 8098 |
| JSON + base64 | 64 KiB | **3374** / 1785 / 2175 | 4175 / 3408 / 4219 |
| JSON + base64 | 1 MiB | **2072** / 1045 / 1387 | **5037** / 3265 / 4909 |
| Binary | 64 KiB | **968** / 888 / 908 | **5912** / 4660 / 5275 |
| Binary | 1 MiB | 824 / 597 / 850 | **7564** / 4655 / 6495 |
| Noise | 64 KiB | **2994** / 1656 / 2043 | **26214** / 23432 / 22310 |
| Noise | 1 MiB | **1762** / 994 / 931 | **25891** / 22550 / 19040 |

- **The parallel regression is gone.** Parallel encoding is 1.1–2.0× the first C run's and level with or ahead of
  Rust's everywhere (binaries at 1 MiB: 0.97×, within noise). The biggest gains are on the inputs that write the
  most output, JSON + base64 (1.9–2.0×) and noise (1.8×), as expected from the false-sharing fix.
- **The parallel decode gap was not a decoder problem.** Parallel decoding is 1.1–1.8× the first C run's and level
  with or ahead of Rust's (0.99–1.36×), although nothing in the decode path changed beyond the 0.3–0.6% fewer
  instructions above. The earlier low rows came from that run's conditions: it was about 5% slower overall, and rows
  with fewer chunks than threads depend on which cores the chunks land on.
- **Scaling:** 10 threads run text, JSON and binaries 5.5–7.0× faster than one for encoding and 4.6–6.8× for
  decoding, on 8 performance and 2 efficiency cores.
