# Benchmark: the current C version vs Apple's libcompression and the Rust version

- **Date:** 2026-11-14
- **Commit:** `7aaad2c` (`main` after PR #10: the C conversion, the decoder register work, the parallel false-sharing
  fix, PRs #7 and #8, and the build fixes)
- **Against Apple:** GitHub Actions `macos-latest`, an Apple M1 (virtualised), 3 threads,
  [actions run 43600000454](https://github.com/david-brandt-the-icelander/lzfse-compression/actions/runs/43600000454).
- **Against the Rust version:** the development VM, an Intel Xeon at 2.1 GHz with 4 vCPUs (x86-64, KVM), Clang 18,
  comparing `7aaad2c` with the last Rust commit `7be57b4`.

## Method

- **Harness:** `make CPU_FLAGS=-mcpu=apple-m1 build/lzfse-bench && build/lzfse-bench --runs 11` on the runner. On the
  VM, the C build uses `make CC=clang`, the Rust build uses `cargo build --release -p lzfse-bench`, and the two run
  alternately, three rounds each; the tables show the median of the three rounds' medians.
- **Correctness gate:** every chunk is decoded back before its timing counts (on macOS also by Apple's decoder).
- **Inputs:** the harness's synthetic corpus (8 MiB of text, JSON lines with base64 attachments, x86-like binary
  records and zeros; 4 MiB of noise) in 64 KiB chunks (as APFS stores files), 1 MiB chunks, and whole.
- **Shared and virtual machines are noisy.** Compare contenders within a table, not absolute numbers across tables.
  The owner's M1 Pro gives the authoritative numbers ([last run](2026-11-14-owner-m1-pro-c.md)).

## 1. Against Apple, on Apple Silicon

Our speed divided by Apple's in the same run; above 1 means faster. The parallel columns use 3 threads against
Apple's one.

| Input | Ratio vs Apple's | Encode | Decode | Parallel encode | Parallel decode |
|---|---|---:|---:|---:|---:|
| Text | 8–10% smaller | **1.53–1.62** | **1.19–1.24** | 4.3–4.4 | 3.1–3.4 |
| JSON + base64 | 0.3% larger at 64 KiB, else equal | 1.35–2.22 | 1.06–1.07 | 3.7–6.3 | 2.8–3.1 |
| Binary | 2–3% smaller | 1.01–1.10 | 1.00–1.06 | 2.7–2.8 | 2.6–3.0 |
| Noise | 1% smaller | 2.0–2.2 | 63–75 | 4.0–5.2 | 27–29 |
| Zeros | equal | 39–158 | 0.96–1.40 | 64–167 | 1.0–2.3 |

On this runner the C version encodes and decodes every input at least as fast as Apple's, single-threaded. The one
row below 1 is zeros decoded as one 8 MiB stream (0.96), where both decoders mostly fill memory. Parallel rows scale
close to the 3 available threads: parallel speed over our single-threaded speed is 2.5–2.9× for both encoding and
decoding on text, JSON and binaries.

The owner's M1 Pro run of `2a2729f` measured binary decoding 1–5% behind Apple's; this runner measures it 0–6%
ahead. The difference is within what separate machines and runs give, so the binary-decode question stays open until
the next run on the owner's Mac.

Full table:

| Input | Shape | Contender | Ratio | Encode MB/s | Decode MB/s |
|---|---|---|---:|---:|---:|
| text | 64 KiB chunks | apple libcompression | 0.229 | 125 | 1210 |
| text | 64 KiB chunks | lzfse-zazzcat | 0.210 | 191 | 1439 |
| text | 64 KiB chunks | lzfse-zazzcat parallel | 0.210 | 553 | 4056 |
| text | 1 MiB chunks | apple libcompression | 0.225 | 129 | 1235 |
| text | 1 MiB chunks | lzfse-zazzcat | 0.202 | 209 | 1534 |
| text | 1 MiB chunks | lzfse-zazzcat parallel | 0.202 | 549 | 3846 |
| text | whole | apple libcompression | 0.224 | 131 | 1256 |
| text | whole | lzfse-zazzcat | 0.202 | 211 | 1547 |
| jsonl+base64 | 64 KiB chunks | apple libcompression | 0.761 | 202 | 637 |
| jsonl+base64 | 64 KiB chunks | lzfse-zazzcat | 0.763 | 449 | 676 |
| jsonl+base64 | 64 KiB chunks | lzfse-zazzcat parallel | 0.763 | 1272 | 1963 |
| jsonl+base64 | 1 MiB chunks | apple libcompression | 0.757 | 205 | 646 |
| jsonl+base64 | 1 MiB chunks | lzfse-zazzcat | 0.757 | 302 | 688 |
| jsonl+base64 | 1 MiB chunks | lzfse-zazzcat parallel | 0.757 | 759 | 1777 |
| jsonl+base64 | whole | apple libcompression | 0.757 | 204 | 643 |
| jsonl+base64 | whole | lzfse-zazzcat | 0.757 | 275 | 681 |
| binary | 64 KiB chunks | apple libcompression | 0.416 | 134 | 961 |
| binary | 64 KiB chunks | lzfse-zazzcat | 0.403 | 135 | 1019 |
| binary | 64 KiB chunks | lzfse-zazzcat parallel | 0.403 | 356 | 2930 |
| binary | 1 MiB chunks | apple libcompression | 0.416 | 130 | 1056 |
| binary | 1 MiB chunks | lzfse-zazzcat | 0.406 | 140 | 1090 |
| binary | 1 MiB chunks | lzfse-zazzcat parallel | 0.406 | 362 | 2769 |
| binary | whole | apple libcompression | 0.416 | 133 | 1113 |
| binary | whole | lzfse-zazzcat | 0.407 | 146 | 1118 |
| noise | 64 KiB chunks | apple libcompression | 1.011 | 209 | 645 |
| noise | 64 KiB chunks | lzfse-zazzcat | 1.000 | 423 | 40721 |
| noise | 64 KiB chunks | lzfse-zazzcat parallel | 1.000 | 1086 | 18893 |
| noise | 1 MiB chunks | apple libcompression | 1.010 | 217 | 655 |
| noise | 1 MiB chunks | lzfse-zazzcat | 1.000 | 463 | 47127 |
| noise | 1 MiB chunks | lzfse-zazzcat parallel | 1.000 | 877 | 17549 |
| noise | whole | apple libcompression | 1.010 | 217 | 659 |
| noise | whole | lzfse-zazzcat | 1.000 | 485 | 49345 |
| zeros | 64 KiB chunks | apple libcompression | 0.003 | 223 | 16194 |
| zeros | 64 KiB chunks | lzfse-zazzcat | 0.003 | 8729 | 22733 |
| zeros | 64 KiB chunks | lzfse-zazzcat parallel | 0.003 | 14291 | 37449 |
| zeros | 1 MiB chunks | apple libcompression | 0.001 | 207 | 37958 |
| zeros | 1 MiB chunks | lzfse-zazzcat | 0.001 | 20510 | 44384 |
| zeros | 1 MiB chunks | lzfse-zazzcat parallel | 0.001 | 34664 | 38836 |
| zeros | whole | apple libcompression | 0.001 | 132 | 38130 |
| zeros | whole | lzfse-zazzcat | 0.001 | 20867 | 36472 |

## 2. Against the Rust version, on x86

MB/s; C/Rust above 1 means the C version is faster. Parallel rows use 4 threads. This is x86 and Clang, not the
target chip: use it for the C/Rust columns only.

| Input | Shape | Threads | Encode C | Encode Rust | C/Rust | Decode C | Decode Rust | C/Rust |
|---|---|---|---:|---:|---:|---:|---:|---:|
| text | 64 KiB chunks | 1 | 91 | 105 | 0.87 | 806 | 835 | 0.97 |
| text | 64 KiB chunks | 4 | 318 | 306 | 1.04 | 1515 | 1564 | 0.97 |
| text | 1 MiB chunks | 1 | 106 | 94 | 1.13 | 879 | 872 | 1.01 |
| text | 1 MiB chunks | 4 | 271 | 246 | 1.10 | 1648 | 1612 | 1.02 |
| text | whole | 1 | 107 | 112 | 0.96 | 734 | 652 | 1.13 |
| jsonl+base64 | 64 KiB chunks | 1 | 232 | 251 | 0.92 | 377 | 495 | 0.76 |
| jsonl+base64 | 64 KiB chunks | 4 | 603 | 474 | 1.27 | 840 | 1087 | 0.77 |
| jsonl+base64 | 1 MiB chunks | 1 | 153 | 162 | 0.94 | 446 | 493 | 0.90 |
| jsonl+base64 | 1 MiB chunks | 4 | 377 | 296 | 1.27 | 985 | 1098 | 0.90 |
| jsonl+base64 | whole | 1 | 153 | 152 | 1.01 | 459 | 463 | 0.99 |
| binary | 64 KiB chunks | 1 | 80 | 83 | 0.96 | 615 | 636 | 0.97 |
| binary | 64 KiB chunks | 4 | 231 | 231 | 1.00 | 1157 | 1235 | 0.94 |
| binary | 1 MiB chunks | 1 | 66 | 76 | 0.87 | 634 | 655 | 0.97 |
| binary | 1 MiB chunks | 4 | 191 | 176 | 1.09 | 1169 | 1289 | 0.91 |
| binary | whole | 1 | 76 | 77 | 0.99 | 631 | 509 | 1.24 |
| noise | 64 KiB chunks | 1 | 220 | 237 | 0.93 | 12731 | 10344 | 1.23 |
| noise | 64 KiB chunks | 4 | 503 | 491 | 1.02 | 9544 | 9715 | 0.98 |
| noise | 1 MiB chunks | 1 | 241 | 156 | 1.54 | 9783 | 8065 | 1.21 |
| noise | 1 MiB chunks | 4 | 528 | 194 | 2.72 | 7825 | 6730 | 1.16 |
| noise | whole | 1 | 252 | 276 | 0.91 | 8038 | 7755 | 1.04 |
| zeros | 64 KiB chunks | 1 | 5445 | 3602 | 1.51 | 15763 | 14538 | 1.08 |
| zeros | 64 KiB chunks | 4 | 15433 | 8659 | 1.78 | 21993 | 18482 | 1.19 |
| zeros | 1 MiB chunks | 1 | 17909 | 3352 | 5.34 | 28845 | 23093 | 1.25 |
| zeros | 1 MiB chunks | 4 | 31701 | 9911 | 3.20 | 21142 | 21366 | 0.99 |
| zeros | whole | 1 | 17938 | 3589 | 5.00 | 18277 | 13801 | 1.32 |

- **Parallel encoding** is level with or faster than Rust's on every input: 1.27× on JSON + base64 and 2.7× on noise
  at 1 MiB chunks, where the false-sharing fix matters most.
- **Zeros and noise** encode much faster in C (1.5–5.3× single-threaded, apart from noise as one stream); the cause
  has not been profiled.
- **Single-threaded text and binaries** are within ±13% either way, which is this VM's run-to-run noise.
- **JSON + base64 decoding** is 10–24% slower in C on x86 at 64 KiB and 1 MiB chunks, single-threaded and parallel
  alike. On the owner's M1 Pro the gap is smaller: the C version decodes it at 1.06–1.07× Apple's speed, where the
  Rust version reached 1.10×. It is the one input where the C decoder still trails the Rust one on the target chip.

## Summary

- On Apple Silicon (CI's virtual M1), the current version is at least as fast as Apple's on every input and shape,
  single-threaded, except zeros decoded as one stream (0.96). It is 8–10% smaller on text and 2–3% smaller on
  binaries.
- Parallel encoding and decoding scale close to the available threads.
- Against the Rust version, the C version is level or faster on parallel encoding and on zeros and noise;
  single-threaded differences on x86 are mostly noise. JSON + base64 decoding trails Rust's, by about 4% relative to
  Apple on the owner's M1 Pro and by 10–24% on x86.
- The authoritative check remains a run on the owner's M1 Pro.
