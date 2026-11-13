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

Measured against `libcompression` in the same process on GitHub's Apple Silicon runner. The full tables and
method are in [`docs/benchmarks`](docs/benchmarks).

- **Every core.** Independent chunks are compressed on all cores (`encode_chunks`), which is how APFS stores
  files (64 KiB chunks). Apple's API is single-threaded.
- **Incompressible data is stored, not coded.** Blocks that do not shrink become raw blocks, so media and
  already-compressed files decode at memory speed instead of through the entropy decoder.
- **Cheap where Apple is expensive.** Long runs (zeros, padding) encode about 25× faster, and literal-heavy data
  (JSON with base64 attachments) encodes about 1.6× faster and decodes faster than Apple's.
- **Same ratio.** On the test corpus our output is within ±0.6% of Apple's size, often smaller.

Single-threaded encoding and decoding of plain text and binaries is still 10–30% behind Apple's; closing that gap
is the current work (see [`docs/DESIGN.md`](docs/DESIGN.md), section 5).

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
- **Safety:** the crate denies `unsafe` except two bounds-checked copy kernels, each tested against a safe version.

## Design

[`docs/DESIGN.md`](docs/DESIGN.md) has the goals, the format notes (block types, FSE tables, LZVN opcodes,
decmpfs), the architecture and the benchmark method. The format was implemented from public descriptions and
black-box tests; Apple's reference source was consulted for format facts only, never copied.

## License

[MPL-2.0](LICENSE).
