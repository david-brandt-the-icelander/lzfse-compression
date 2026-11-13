# Benchmark: lzfse-zazzcat vs Apple's libcompression

- **Date:** 2026-11-13
- **Commit:** `59c0fed`
- **Machine:** GitHub Actions `macos-latest` runner, an Apple M2 Pro (virtualised), with 5 threads available
- **Run:** [actions run 43499042029](https://github.com/david-brandt-the-icelander/lzfse-compression/actions/runs/43499042029)

## Method

- **Harness:** `crates/lzfse-bench`, run as `cargo run --release -p lzfse-bench -- --runs 11` with `-C target-cpu=apple-m1`.
- **In-process timing:** Apple's `libcompression` is called through FFI with reused scratch buffers. Our encoder and decoder are reused across chunks the same way.
- **Interleaved contenders:** each of the 11 timed runs measures every contender back to back, so noise from other tenants of the shared runner hits all of them alike. The table shows the median.
- **Correctness gate:** before any timing counts, every chunk we encode is decoded by our decoder and by Apple's, and every chunk Apple encodes is decoded by ours.
- **Inputs:** synthetic, generated deterministically by the harness:
  - English-like text;
  - JSON lines with large base64 attachments, like the Claude Code session logs `free-disk-space` found;
  - x86-like binary records;
  - random noise;
  - zeros.
  Each is 4–8 MiB.
- **Shapes:**
  - 64 KiB chunks, as APFS transparent compression stores files;
  - 1 MiB chunks;
  - the whole input as one stream.
- **Shared runners are noisy.** Use these numbers for relative comparison. The owner's M1 and M3 Macs give the authoritative ones: run `cargo run --release -p lzfse-bench` there.

## Results

| Input | Shape | Contender | Ratio | Encode MB/s | Decode MB/s |
|---|---|---|---:|---:|---:|
| text | 64 KiB chunks | apple libcompression | 0.229 | 140 | 1325 |
| text | 64 KiB chunks | lzfse-zazzcat | 0.229 | 132 | 961 |
| text | 64 KiB chunks | lzfse-zazzcat parallel | 0.229 | 622 | 4392 |
| text | 1 MiB chunks | apple libcompression | 0.225 | 145 | 1383 |
| text | 1 MiB chunks | lzfse-zazzcat | 0.225 | 134 | 952 |
| text | 1 MiB chunks | lzfse-zazzcat parallel | 0.225 | 515 | 3507 |
| text | whole | apple libcompression | 0.224 | 144 | 1377 |
| text | whole | lzfse-zazzcat | 0.224 | 133 | 942 |
| jsonl+base64 | 64 KiB chunks | apple libcompression | 0.761 | 204 | 691 |
| jsonl+base64 | 64 KiB chunks | lzfse-zazzcat | 0.763 | 416 | 650 |
| jsonl+base64 | 64 KiB chunks | lzfse-zazzcat parallel | 0.763 | 1620 | 2751 |
| jsonl+base64 | 1 MiB chunks | apple libcompression | 0.757 | 197 | 739 |
| jsonl+base64 | 1 MiB chunks | lzfse-zazzcat | 0.757 | 379 | 717 |
| jsonl+base64 | 1 MiB chunks | lzfse-zazzcat parallel | 0.757 | 1445 | 2637 |
| jsonl+base64 | whole | apple libcompression | 0.757 | 234 | 773 |
| jsonl+base64 | whole | lzfse-zazzcat | 0.757 | 397 | 813 |
| binary | 64 KiB chunks | apple libcompression | 0.416 | 128 | 910 |
| binary | 64 KiB chunks | lzfse-zazzcat | 0.416 | 105 | 721 |
| binary | 64 KiB chunks | lzfse-zazzcat parallel | 0.416 | 566 | 3441 |
| binary | 1 MiB chunks | apple libcompression | 0.416 | 133 | 1079 |
| binary | 1 MiB chunks | lzfse-zazzcat | 0.417 | 120 | 835 |
| binary | 1 MiB chunks | lzfse-zazzcat parallel | 0.417 | 511 | 3222 |
| binary | whole | apple libcompression | 0.416 | 138 | 1116 |
| binary | whole | lzfse-zazzcat | 0.417 | 124 | 891 |
| noise | 64 KiB chunks | apple libcompression | 1.011 | 211 | 709 |
| noise | 64 KiB chunks | lzfse-zazzcat | 1.000 | 451 | 28001 |
| noise | 64 KiB chunks | lzfse-zazzcat parallel | 1.000 | 2076 | 31497 |
| noise | 1 MiB chunks | apple libcompression | 1.010 | 218 | 725 |
| noise | 1 MiB chunks | lzfse-zazzcat | 1.000 | 500 | 41562 |
| noise | 1 MiB chunks | lzfse-zazzcat parallel | 1.000 | 1906 | 20594 |
| noise | whole | apple libcompression | 1.010 | 201 | 717 |
| noise | whole | lzfse-zazzcat | 1.000 | 468 | 36605 |
| zeros | 64 KiB chunks | apple libcompression | 0.003 | 195 | 8988 |
| zeros | 64 KiB chunks | lzfse-zazzcat | 0.003 | 4975 | 18497 |
| zeros | 64 KiB chunks | lzfse-zazzcat parallel | 0.003 | 16357 | 45191 |
| zeros | 1 MiB chunks | apple libcompression | 0.001 | 195 | 35370 |
| zeros | 1 MiB chunks | lzfse-zazzcat | 0.001 | 6279 | 42057 |
| zeros | 1 MiB chunks | lzfse-zazzcat parallel | 0.001 | 18562 | 38065 |
| zeros | whole | apple libcompression | 0.001 | 130 | 31815 |
| zeros | whole | lzfse-zazzcat | 0.001 | 6356 | 33695 |

Ratio is compressed size divided by original size, so lower is better.

## Summary

Each cell is our speed divided by Apple's, single-threaded unless stated. Above 1 means faster.

| Input | Encode | Decode | Parallel encode (5 threads) | Parallel decode (5 threads) |
|---|---:|---:|---:|---:|
| Text | 0.92–0.94 | 0.68–0.73 | 3.6–4.4 | 2.5–3.3 |
| JSON + base64 | 1.7–2.0 | 0.94–1.05 | 7.3–7.9 | 3.6–4.0 |
| Binary | 0.82–0.90 | 0.77–0.80 | 3.8–4.4 | 3.0–3.8 |
| Noise | 2.1–2.3 | 40–57 | 8.7–9.8 | 28–44 |
| Zeros | 25–49 | 1.06–2.1 | 84–95 | 1.1–5.0 |

- **Ratio:** within 0.3% of Apple's on every input. JSON + base64 is 0.763 vs 0.761 at 64 KiB, and identical at the larger shapes.
- **Where we win:**
  - **Incompressible data:** we store raw blocks, while Apple entropy-codes data that doesn't shrink and ends up 1% larger.
  - **Long runs:** much faster encoding.
  - **Literal-heavy data:** about twice as fast to encode.
  - **Every case that can use more than one core.** `libcompression` is single-threaded.
- **Where we lose:** single-threaded encoding and decoding of text and binaries, by 10–30%. The decoder's per-token cost is the bottleneck. Section 5 of `docs/DESIGN.md` lists the next steps: NEON literal decoding, fewer bounds checks in the token loop, and profiling on real hardware.
