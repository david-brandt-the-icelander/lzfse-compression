# lzfse-compression

An LZFSE implementation that is faster than Apple's — written from scratch in C.

LZFSE is Apple's compression format: `libcompression`'s `COMPRESSION_LZFSE`, Apple Archive, and APFS
transparent file compression all use it. This library (`lzfse-zazzcat`) reads and writes the same streams. Apple's
decoder reads what it writes and it decodes what Apple's encoder writes; CI checks both directions against
`libcompression` on Apple Silicon and against Apple's open-source reference on Linux.

```c
#include "lzfse_zazzcat.h"

lzfse_zazzcat_buffer packed = {0}, unpacked = {0};
lzfse_zazzcat_encode(data, data_length, NULL, &packed);   // NULL: default options
lzfse_zazzcat_decode(packed.data, packed.length, data_length, &unpacked);
lzfse_zazzcat_buffer_free(&packed);
lzfse_zazzcat_buffer_free(&unpacked);
```

Build with `make` (C11, a C compiler and `make`; no other dependencies). The API is documented in
[`include/lzfse_zazzcat.h`](include/lzfse_zazzcat.h).

The project started in Rust and was converted to C on 2026-11-14. The C encoder produces byte-identical streams to
the Rust one.

## Why it is faster

Measured against `libcompression` in the same process on the owner's M1 Pro (10 threads). The full tables and
method are in [`docs/benchmarks`](docs/benchmarks/2026-11-14-owner-m1-pro-c.md).

- **Every core.** `encode_chunks` compresses independent chunks on all cores, which is how APFS stores files
  (64 KiB chunks). The chunks are separate streams, so they decode in parallel too. Apple's API is
  single-threaded. On a real Claude Code session log, we encode 7.6–8.3× faster than Apple's and decode 5.6–5.9×
  faster.
- **Faster on one core too.** On that same log, single-threaded encoding is 18–37% faster and decoding is on par,
  with output as small or up to 1% smaller. Plain text encodes 56–68% faster, decodes 6–12% faster, and comes out
  9–10% smaller. Binaries encode 2–15% faster and come out 2–3% smaller.
- **Long repeats are found.** A second match-finder table keyed on 8 bytes keeps track of where long repeats
  start. On logs that re-send long earlier context, the output is up to 69% smaller than Apple's.
- **Incompressible data is stored, not coded.** Blocks that do not shrink become raw blocks:
  - media and already-compressed files decode 50–62× faster than Apple's;
  - they encode 2.1–2.3× faster;
  - and they end up 1% smaller.
- **Cheap where Apple is expensive.**
  - Long runs (zeros, padding) encode 36–153× faster.
  - JSON with base64 attachments encodes 1.4–2.3× faster and decodes 6–7% faster.

Single-threaded binary decoding is where Apple's code still wins, by 1–5%, even though our output is 2–3% smaller.
Closing that gap is the current work (see [`docs/DESIGN.md`](docs/DESIGN.md), section 5).

## What is in the box

| Path | What it is |
|---|---|
| `include/lzfse_zazzcat.h`, `src/` | The codec: LZFSE and LZVN, encoder and decoder (`build/liblzfse_zazzcat.a`) |
| `lzfse_zazzcat_encode_chunks` | Parallel chunked compression on a persistent pthread pool, written straight into the APFS resource-fork layout; no allocations once warmed up |
| `lzfse_zazzcat_decmpfs_*` | APFS transparent compression containers (types 7, 8, 11, 12) |
| `tools/cli.c` | `build/lzfse-zazzcat encode|decode|inspect` |
| `bench/bench.c` | In-process benchmark against `libcompression` (`build/lzfse-bench`) |
| `tests/` | Unit, round-trip, hostile-input, allocation and (on macOS) Apple interoperability tests |
| `fuzz/` | libFuzzer targets for every decoder entry point |

## How it is checked

- **Interoperability:** CI round-trips a corpus through Apple's `libcompression` in both directions (LZFSE and
  raw LZVN), and through Apple's BSD reference on Linux.
- **The kernel:** CI writes files with our decmpfs containers, sets the compressed flag, and reads them back through
  the macOS kernel; it also decodes thousands of files macOS itself compressed and compares them byte for byte.
- **Hostile input:** the decoder never crashes or reads or writes out of bounds. That is covered by randomized
  tests, truncation and bit-flip sweeps run under AddressSanitizer and UndefinedBehaviorSanitizer, and four fuzz
  targets run on every push.
- **Wide copies:** the decoder's fast copies check their room once per token and are tested against byte-by-byte
  versions, including that they never write past their slack.

## Design

[`docs/DESIGN.md`](docs/DESIGN.md) has the goals, the format notes (block types, FSE tables, LZVN opcodes,
decmpfs), the architecture and the benchmark method. The format was implemented from public descriptions and
black-box tests. Apple's BSD-licensed reference was consulted for format facts and, later, studied for its encoder's
design; none of its code was copied.

## License

[MPL-2.0](LICENSE).
