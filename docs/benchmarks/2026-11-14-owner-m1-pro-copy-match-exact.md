# Benchmark: `copy_match_exact` copies with `copy_bytes` instead of `move_bytes`, on an M1 Pro

- **Date:** 2026-11-14
- **Change:** both calls in `copy_match_exact` (`src/copy.h`) use `copy_bytes` (`memcpy`) instead of
  `move_bytes` (`memmove`). Its pieces never overlap (see [move_bytes.md](../memory-operations/move_bytes.md)).
  Baseline: `ece5ad5` (main).
- **Machine:** the owner's MacBook Pro, Apple M1 Pro (10 threads), macOS 27.0.1.
- **Command:** both builds with `make CPU_FLAGS=-mcpu=apple-m1 build/lzfse-bench`, then five rounds of
  ```bash
  build/lzfse-bench --runs 11 --synthetic --chunk 4000 --chunk 65536
  ```
  alternating baseline and change in each round. The table shows the median over the five rounds.
- **Inputs:** the harness's synthetic corpus. 4000-byte chunks take the LZVN path, whose decoder calls
  `copy_match_exact` for every distance including 1. 64 KiB chunks take the LZFSE path, whose decoder sends
  distance 1 to `fill_bytes`.

## Why no gain was expected on macOS

On macOS, `memcpy` and `memmove` are the same function. `libsystem_c` re-exports both as `_platform_memmove`
from `libsystem_platform` (`dyld_info -exports /usr/lib/system/libsystem_c.dylib`). The change binary has four
more calls through the `_memcpy` stub and none left through `_memmove`, but both stubs end in the same code. The
measurement checks that nothing else changed. A gain would show only on a C library whose `memcpy` skips the
overlap check that `memmove` does (glibc, musl).

## Decode

| Input | Chunks | Ratio | Baseline MB/s | Change MB/s | Change | Worst / best |
|---|---|---:|---:|---:|---:|---|
| text | 4000 B | 0.388 | 648 | 649 | +0.2% | −2% / +3% |
| text | 64 KiB | 0.210 | 1558 | 1542 | −1.0% | −4% / +2% |
| binary | 4000 B | 0.606 | 1061 | 1062 | +0.1% | −4% / +5% |
| binary | 64 KiB | 0.403 | 1098 | 1106 | +0.7% | −3% / +2% |
| jsonl+base64 | 4000 B | 1.000 | 18124 | 18443 | +1.8% | −30% / +37% |
| jsonl+base64 | 64 KiB | 0.763 | 717 | 727 | +1.4% | −3% / +6% |
| noise | 4000 B | 1.003 | 29127 | 36792 | +26.3% | −39% / +89% |
| noise | 64 KiB | 1.000 | 47662 | 52429 | +10.0% | −24% / +36% |
| zeros | 4000 B | 0.015 | 5153 | 5178 | +0.5% | −2% / +4% |
| zeros | 64 KiB | 0.003 | 23564 | 23432 | −0.6% | −3% / +2% |

"Worst / best" compares the change's slowest round with the baseline's fastest, and the change's fastest with the
baseline's slowest. The compressed sizes are identical for both builds.

## Reading the results

- **Neutral.** Every row that decodes tokens (text, binary, zeros and jsonl+base64 at 64 KiB) moved by less than
  2%, with a spread that includes zero. The binary input, which has the most short-period matches, moved by
  +0.1% and +0.7%.
- **jsonl+base64 at 4000 B and noise** are stored as raw blocks and never reach `copy_match_exact`. Each chunk
  takes microseconds to decode, and their rounds varied by up to 89% in both builds, so those rows are noise.
- **Encode** is unchanged (the encoders do not call `copy_match_exact`), within 2% on every row.
