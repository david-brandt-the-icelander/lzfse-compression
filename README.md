# lzfse-compression

An LZFSE implementation that is faster than Apple's — written from scratch in Rust.

LZFSE is Apple's compression format: `libcompression`'s `COMPRESSION_LZFSE`, Apple Archive, and APFS
transparent file compression all use it. This crate (`lzfse-zazzcat`) reads and writes the same streams. Apple's
decoder reads what it writes and it decodes what Apple's encoder writes; CI checks both directions against
`libcompression` on Apple Silicon and against Apple's open-source reference on Linux.

```rust
let packed = lzfse_zazzcat::encode(data);
let unpacked = lzfse_zazzcat::decode(&packed, data.len())?;
```

## Why it is faster

Measured against `libcompression` in the same process on the owner's M1 Pro (10 threads). The full tables and
method are in [`docs/benchmarks`](docs/benchmarks/2026-11-14-owner-m1-pro.md).

- **Every core.** `encode_chunks` compresses independent chunks on all cores, which is how APFS stores files
  (64 KiB chunks). The chunks are separate streams, so they decode in parallel too. Apple's API is
  single-threaded. On a real Claude Code session log, we encode 7.2–8.8× faster than Apple's and decode 5.6–6.3×
  faster.
- **Faster on one core too.** On that same log, single-threaded encoding is 6–26% faster and decoding is on par,
  at the same compression ratio (within 0.3%). Plain text encodes 34–35% faster and decodes on par or up to 4%
  faster, and it comes out 9–10% smaller.
- **Long repeats are found.** A second match-finder table keyed on 8 bytes keeps track of where long repeats
  start. On logs that re-send long earlier context, the output is up to 69% smaller than Apple's.
- **Incompressible data is stored, not coded.** Blocks that do not shrink become raw blocks:
  - media and already-compressed files decode 46–61× faster than Apple's;
  - they encode 2.1–2.3× faster;
  - and they end up 1% smaller.
- **Cheap where Apple is expensive.**
  - Long runs (zeros, padding) encode 22–54× faster.
  - JSON with base64 attachments encodes 1.2–2.1× faster and decodes 10% faster.

Single-threaded binaries are where Apple's code still wins: about 5–9% on encoding and 5–10% on decoding, even
though our output is 2–3% smaller. Closing that gap is the current work (see [`docs/DESIGN.md`](docs/DESIGN.md),
section 5).

## What is in the box

| Crate / path | What it is |
|---|---|
| `crates/lzfse` (`lzfse-zazzcat`) | The codec: LZFSE and LZVN, encoder and decoder, `no_std` + `alloc` |
| `encode_chunks_into` | Parallel chunked compression on a persistent thread pool, written straight into the APFS resource-fork layout; no allocations once warmed up |
| `lzfse_zazzcat::decmpfs` | APFS transparent compression containers (types 7, 8, 11, 12) |
| `crates/lzfse-cli` | `lzfse-zazzcat encode|decode|inspect` |
| `crates/lzfse-bench` | In-process benchmark against `libcompression` |
| `fuzz/` | cargo-fuzz targets for every decoder entry point |

## How it is checked

- **Interoperability:** CI round-trips a corpus through Apple's `libcompression` in both directions (LZFSE and
  raw LZVN), and through Apple's BSD reference on Linux.
- **The kernel:** CI writes files with our decmpfs containers, sets the compressed flag, and reads them back through
  the macOS kernel; it also decodes thousands of files macOS itself compressed and compares them byte for byte.
- **Hostile input:** the decoder never panics or writes out of bounds. That is covered by property tests,
  truncation and bit-flip sweeps, and four fuzz targets run on every push.
- **Safety:** the crate denies `unsafe` except a few copy kernels. Each checks its bounds once up front and is tested against a safe version.

## Design

[`docs/DESIGN.md`](docs/DESIGN.md) has the goals, the format notes (block types, FSE tables, LZVN opcodes,
decmpfs), the architecture and the benchmark method. The format was implemented from public descriptions and
black-box tests. Apple's BSD-licensed reference was consulted for format facts and, later, studied for its encoder's
design; none of its code was copied.

## License

[MPL-2.0](LICENSE).
