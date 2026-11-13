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

Measured against `libcompression` in the same process on GitHub's Apple Silicon runners. The full table and
method are in [`docs/benchmarks`](docs/benchmarks/2026-11-13-github-m2pro.md).

- **Every core.** Independent chunks are compressed and decompressed on all cores (`encode_chunks`). That is how
  APFS stores files, in 64 KiB chunks. Apple's API is single-threaded. On a 5-thread M2 Pro runner, text encodes
  about 4× faster than Apple's and decodes about 3× faster.
- **Incompressible data is stored, not coded.** Blocks that do not shrink become raw blocks:
  - media and already-compressed files decode at memory speed, about 40× faster than Apple's;
  - they encode about 2× faster;
  - and they end up 1% smaller.
- **Cheap where Apple is expensive.**
  - Long runs (zeros, padding) encode 25–50× faster.
  - Literal-heavy data (JSON with base64 attachments) encodes about 2× faster and decodes at the same speed.
- **Same ratio.** On every input, our output is within 0.3% of Apple's size.

Single-threaded encoding of plain text and binaries is 10–20% behind Apple's, and decoding them is 20–30% behind.
Closing that gap is the current work (see [`docs/DESIGN.md`](docs/DESIGN.md), section 5).

## What is in the box

| Crate / path | What it is |
|---|---|
| `crates/lzfse` (`lzfse-zazzcat`) | The codec: LZFSE and LZVN, encoder and decoder, `no_std` + `alloc` |
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
black-box tests; Apple's reference source was consulted for format facts only, never copied.

## License

[MPL-2.0](LICENSE).
