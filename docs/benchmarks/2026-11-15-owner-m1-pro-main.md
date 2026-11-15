# Benchmark: lzfse-zazzcat vs Apple's libcompression on an M1 Pro, all work to 2026-11-15

- **Date:** 2026-11-15
- **Commit:** `8042dae` (`main`), which includes the LZVN kind-table dispatch (#20), the decoder's simpler refill
  (#21), the encoders' packed words (#25), exact literal copies (#30) and one-store LZVN opcodes (#31).
- **Machine:** the owner's MacBook Pro, Apple M1 Pro (8 performance and 2 efficiency cores, 10 threads),
  macOS 27.0.1, on power, with the system allocator.
- **Command:**
  ```bash
  make clean && make CPU_FLAGS=-mcpu=apple-m1 build/lzfse-bench
  build/lzfse-bench --runs 11 --synthetic <session log>
  ```
- **Inputs:** the harness's synthetic corpus, a real Claude Code session log, and the many-small-files set (586
  files of 512 B to 253 KiB cut from the text, JSON and binary inputs).

## Method notes

- **Comparable with the previous runs.** Apple's own speeds match the
  [2026-11-14 evening run](2026-11-14-owner-m1-pro-main.md) within about 1% (text encode 133/136/137 against
  133/137/138 MB/s, text decode 1325/1342/1351 against 1319/1356/1354), so absolute numbers compare directly.
- **Ratios** below are our speed divided by Apple's in this run; above 1 means faster.
- **Noise and zeros** do different work on each side (we store noise raw; runs are cheap for our match finder), so
  their ratios are listed separately and are not code-speed comparisons.

## Results

| Input | Shape | Contender | Ratio | Encode MB/s | Decode MB/s |
|---|---|---|---:|---:|---:|
| session log | 64 KiB chunks | apple libcompression | 0.473 | 182 | 1023 |
| session log | 64 KiB chunks | lzfse-zazzcat | 0.473 | 304 | 1107 |
| session log | 64 KiB chunks | lzfse-zazzcat parallel | 0.473 | 2004 | 6830 |
| session log | 1 MiB chunks | apple libcompression | 0.354 | 204 | 1397 |
| session log | 1 MiB chunks | lzfse-zazzcat | 0.351 | 254 | 1514 |
| session log | 1 MiB chunks | lzfse-zazzcat parallel | 0.351 | 1797 | 9664 |
| session log | whole | apple libcompression | 0.338 | 208 | 1464 |
| session log | whole | lzfse-zazzcat | 0.335 | 260 | 1583 |
| text | 64 KiB chunks | apple libcompression | 0.229 | 133 | 1325 |
| text | 64 KiB chunks | lzfse-zazzcat | 0.210 | 235 | 1707 |
| text | 64 KiB chunks | lzfse-zazzcat parallel | 0.210 | 1711 | 8586 |
| text | 1 MiB chunks | apple libcompression | 0.225 | 136 | 1342 |
| text | 1 MiB chunks | lzfse-zazzcat | 0.202 | 245 | 1808 |
| text | 1 MiB chunks | lzfse-zazzcat parallel | 0.202 | 1791 | 10473 |
| text | whole | apple libcompression | 0.224 | 137 | 1351 |
| text | whole | lzfse-zazzcat | 0.202 | 249 | 1823 |
| jsonl+base64 | 64 KiB chunks | apple libcompression | 0.761 | 217 | 694 |
| jsonl+base64 | 64 KiB chunks | lzfse-zazzcat | 0.763 | 660 | 774 |
| jsonl+base64 | 64 KiB chunks | lzfse-zazzcat parallel | 0.763 | 4249 | 4452 |
| jsonl+base64 | 1 MiB chunks | apple libcompression | 0.757 | 218 | 689 |
| jsonl+base64 | 1 MiB chunks | lzfse-zazzcat | 0.757 | 366 | 765 |
| jsonl+base64 | 1 MiB chunks | lzfse-zazzcat parallel | 0.757 | 2233 | 5095 |
| jsonl+base64 | whole | apple libcompression | 0.757 | 219 | 693 |
| jsonl+base64 | whole | lzfse-zazzcat | 0.757 | 336 | 775 |
| binary | 64 KiB chunks | apple libcompression | 0.416 | 139 | 1044 |
| binary | 64 KiB chunks | lzfse-zazzcat | 0.403 | 184 | 1200 |
| binary | 64 KiB chunks | lzfse-zazzcat parallel | 0.403 | 1314 | 5541 |
| binary | 1 MiB chunks | apple libcompression | 0.416 | 136 | 1146 |
| binary | 1 MiB chunks | lzfse-zazzcat | 0.406 | 161 | 1256 |
| binary | 1 MiB chunks | lzfse-zazzcat parallel | 0.406 | 1131 | 7974 |
| binary | whole | apple libcompression | 0.416 | 134 | 1161 |
| binary | whole | lzfse-zazzcat | 0.407 | 162 | 1263 |
| noise | 64 KiB chunks | apple libcompression | 1.011 | 223 | 701 |
| noise | 64 KiB chunks | lzfse-zazzcat | 1.000 | 683 | 56680 |
| noise | 64 KiB chunks | lzfse-zazzcat parallel | 1.000 | 3998 | 26546 |
| noise | 1 MiB chunks | apple libcompression | 1.010 | 230 | 706 |
| noise | 1 MiB chunks | lzfse-zazzcat | 1.000 | 706 | 60787 |
| noise | 1 MiB chunks | lzfse-zazzcat parallel | 1.000 | 2368 | 22672 |
| noise | whole | apple libcompression | 1.010 | 234 | 709 |
| noise | whole | lzfse-zazzcat | 1.000 | 729 | 57456 |
| zeros | 64 KiB chunks | apple libcompression | 0.003 | 234 | 16744 |
| zeros | 64 KiB chunks | lzfse-zazzcat | 0.003 | 10565 | 28056 |
| zeros | 64 KiB chunks | lzfse-zazzcat parallel | 0.003 | 36954 | 47393 |
| zeros | 1 MiB chunks | apple libcompression | 0.001 | 228 | 41528 |
| zeros | 1 MiB chunks | lzfse-zazzcat | 0.001 | 20713 | 50840 |
| zeros | 1 MiB chunks | lzfse-zazzcat parallel | 0.001 | 52103 | 46091 |
| zeros | whole | apple libcompression | 0.001 | 146 | 43691 |
| zeros | whole | lzfse-zazzcat | 0.001 | 22550 | 51150 |

### Many small files

| Contender | Ratio | Encode MB/s | Encode files/s | Decode MB/s | Decode files/s |
|---|---:|---:|---:|---:|---:|
| apple libcompression, scratch reused | 0.472 | 153 | 3573 | 939 | 21853 |
| apple libcompression, one-shot | 0.472 | 150 | 3501 | 925 | 21527 |
| lzfse-zazzcat, encoder/decoder reused | 0.461 | 237 | 5526 | 1086 | 25288 |
| lzfse-zazzcat, one-shot | 0.461 | 239 | 5567 | 1082 | 25200 |

Thousands of files per second (encode / decode):

| Contender | 512 B–4 KiB (199 files) | 4–32 KiB (201 files) | 32–256 KiB (186 files) |
|---|---:|---:|---:|
| apple libcompression, scratch reused | 48.2 / 777.3 | 10.0 / 53.3 | 1.3 / 8.2 |
| apple libcompression, one-shot | 37.5 / 525.1 | 9.5 / 51.7 | 1.3 / 8.1 |
| lzfse-zazzcat, encoder/decoder reused | 166.7 / 904.5 | 16.7 / 61.6 | 2.0 / 9.5 |
| lzfse-zazzcat, one-shot | 164.5 / 908.7 | 16.6 / 61.4 | 2.0 / 9.4 |

## Summary

### Single-threaded, against Apple

Our speed divided by Apple's; in brackets, the [2026-11-14 evening run](2026-11-14-owner-m1-pro-main.md) (the
session log was not in that run; its brackets come from the [first C run](2026-11-14-owner-m1-pro-c.md)).

| Input | Size vs Apple's output | Encode | Decode |
|---|---|---:|---:|
| Session log (real) | as small, or up to 1% smaller | **1.25–1.67** (1.18–1.37) | **1.08** (0.99–1.01) |
| Text | 8–10% smaller | **1.77–1.82** (1.56–1.67) | **1.29–1.35** (1.17–1.25) |
| JSON + base64 | 0.3% larger at 64 KiB, else equal | **1.53–3.04** (1.38–2.32) | **1.11–1.12** (1.06–1.07) |
| Binary | 2–3% smaller | **1.18–1.32** (1.06–1.13) | **1.09–1.15** (1.02–1.07) |
| Small files, all | 2% smaller | 1.55 | 1.16 |
| Small files, 512 B–4 KiB | | 3.46 | 1.16 (1.10) |

Every row is faster than Apple's, on encoding and decoding. Binary decoding, which was 1–5% behind two runs ago,
is now 9–15% ahead.

### What moved since the evening run (absolute MB/s, same machine, Apple's numbers unchanged)

| Single-threaded | Encode | Decode |
|---|---:|---:|
| Text | +8–13% (208/228/230 → 235/245/249) | +8–10% (1545/1678/1688 → 1707/1808/1823) |
| JSON + base64 | +10–31% (503/332/304 → 660/366/336) | +4–5% (740/745/745 → 774/765/775) |
| Binary | +7–23% (149/151/156 → 184/161/162) | +5–7% (1126/1192/1195 → 1200/1256/1263) |
| Small files under 4 KiB | +2% | +9% (829 → 905 thousand files/s) |

- **Decoding** moved by about what the decoder's instruction counts predicted for PR #21 (text −8.1%, binary
  −7.3%, JSON −4.6% instructions): text +8–10%, binary +5–7%, JSON +4–5%. No other decoder change landed in between.
- **Encoding** gains come from the other sessions' encoder work in the same period (packed words, exact literal
  copies, one-store opcodes).

### Parallel (10 threads), against Apple's single thread

| Input | Encode | Decode |
|---|---:|---:|
| Session log | 8.8–11.0× | 6.7–6.9× |
| Text | 12.9–13.2× | 6.5–7.8× |
| JSON + base64 | 10.2–19.6× | 6.4–7.4× |
| Binary | 8.3–9.5× | 5.3–7.0× |

### Noise and zeros (different work, not code speed)

Noise: we store it raw, so our decode is a memory copy (81–86× Apple's decode, which entropy-decodes Apple's
1%-larger output); encode 3.1×. Zeros: encode 45–154× (runs are cheap for our match finder), decode 1.17–1.68×.
