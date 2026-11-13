# lzfse-compression: design

Status: **draft for review**. Nothing here is implemented yet.

## 1. Goals

1. **A complete LZFSE codec written from scratch in Rust.** It covers the encoder and decoder for every block type an LZFSE stream can contain, including LZVN (section 3).
2. **Faster than Apple's.** The bar is measurable and defined in section 6:
   - **Single-threaded parity:** single-threaded encode and decode throughput at least matches `libcompression`'s LZFSE (`COMPRESSION_LZFSE`), with a compression ratio no more than 0.5% worse on the benchmark corpus.
   - **Multi-core:** a block-parallel API beats `libcompression` by roughly the number of performance cores. Apple's API is single-call and single-threaded.
3. **Interoperable both ways:**
   - Apple's decoder reads everything we encode.
   - We decode everything Apple's encoder produces.

   Our output does not have to be byte-identical to Apple's.
4. **Safe on untrusted input.** Malformed or hostile input makes the decoder return an error. It never panics, reads or writes out of bounds, loops forever or allocates without limit.
5. **Usable by `free-disk-space`.** It must cover APFS transparent compression (decmpfs types 11/12), which macOS 26–27 no longer writes through `ditto --hfsCompression`.
6. **A portfolio piece.** That means a readable design, measured claims and reproducible benchmarks.

Non-goals for now: Intel-specific tuning (Intel Macs get the portable path), GPU, Neural Engine or AMX/SME offload (section 5.5), and other Apple formats (LZ4, LZBITMAP, Brotli).

## 2. Clean-room policy

- **Sources:** the format is implemented from public descriptions and from black-box testing against Apple's `libcompression` and the BSD reference ([lzfse/lzfse](https://github.com/lzfse/lzfse)).
- **No ported code:** no reference code is copied or translated line by line. We may consult the reference source as documentation of the format, for example to confirm a constant or a header field, and record that in commit messages.
- **Licensing:** the BSD-3 reference may be built in tests and benchmarks as a comparison target. It is never linked into the library.

## 3. Format notes

These are the facts we need, plus how we will confirm each one. Items marked **verify** are believed correct but must be checked against the reference source and real Apple output before we rely on them.

### 3.1 Stream structure

An LZFSE stream is a sequence of blocks. Each block starts with a 4-byte little-endian magic:

| Magic | Block | Contents |
|---|---|---|
| `bvx-` | Uncompressed | `u32` raw size, then raw bytes |
| `bvxn` | LZVN | `u32` raw size, `u32` payload size, LZVN payload |
| `bvx1` | LZFSE v1 | Header with full-width fields, then payload |
| `bvx2` | LZFSE v2 | Bit-packed header with compressed frequency tables, then payload |
| `bvx$` | End of stream | none |

- **Decoder:** must accept all five block types.
- **Encoder:** emits `bvx2` for normal input, `bvxn` for small input (the reference switches below about 4 KiB, **verify**), `bvx-` when compression does not help, and always `bvx$` at the end.

### 3.2 LZFSE block (`bvx1` / `bvx2`)

- **Two token streams:** a block holds a literal stream and a stream of L/M/D tokens (literal length, match length, match distance). The block limit is about 10,000 matches (**verify**).
- **Literals** are entropy-coded with FSE (finite-state entropy, a table-driven form of asymmetric numeral systems). The codec uses 4 interleaved states over a 256-symbol alphabet with a 1,024-state table (**verify**).
- **L, M and D** are each coded as an FSE symbol plus extra raw bits. The symbol and state counts per table are believed to be L 20/64, M 20/64 and D 64/256 (**verify**), with base-value and extra-bit tables per symbol.
- **Bit order:** bitstreams are written so the decoder reads them backwards from the end (**verify** bit order and how the final word is padded).
- **Headers:**
  - `bvx1` stores counts, sizes, initial FSE states and the frequency tables as plain integers.
  - `bvx2` packs the same fields into three 64-bit words plus variable-length-coded frequency tables.
  - We decode both, and encode `bvx2` only.
- **Limits:** the maximum L, M and D values and the window size are header-level constants. Each is **verify**, with a test that exercises it.

### 3.3 LZVN

- **What it is:** a byte-oriented LZ77 format with no entropy coding. Opcodes encode literal length, match length and distance in a few forms (small, medium and large distance; repeat previous distance; long literal; long match). An end-of-stream opcode `0x06` is followed by 7 zero bytes, and there are no-op opcodes.
- **Where it appears:**
  - inside LZFSE streams as `bvxn` blocks;
  - standalone in decmpfs types 7/8.
- **Verified:** we have already confirmed the opcode set by hand-decoding output from `libcompression` algorithm `0x900`.

### 3.4 APFS transparent compression (decmpfs)

- **Header:** the `com.apple.decmpfs` xattr holds a 16-byte header: magic `fpmc`, a `u32` compression type and a `u64` uncompressed size.
- **Types:** type 11 stores LZFSE data inline in the xattr; type 12 stores it in the resource fork.
- **Resource fork layout:** a fork header, a table of 64 KiB block offsets, the blocks themselves, and a fixed resource map. This is the same as the zlib type 4 layout prototyped in the `free-disk-space` branch (since removed).
- **Per block:** each 64 KiB block is believed to be a complete LZFSE stream, ending with `bvx$` (**verify**). An incompressible block is stored raw behind a one-byte marker (**verify** the marker value for LZFSE).
- **How we check:** parse already-compressed files that ship with macOS (under `/System` and `/usr`) and read files we compress back through the kernel.

## 4. Architecture

### 4.1 Workspace

```
crates/
  lzfse/         library: format, FSE, LZVN, match finder, block and stream codecs
  lzfse-cli/     encode / decode / inspect command line tool
  lzfse-bench/   benchmark harness (section 6), with libcompression and reference FFI
  lzfse-sys-ref/ test-only build of the BSD reference, fetched in CI and never shipped
fuzz/            cargo-fuzz targets
docs/            this design, benchmark reports
```

### 4.2 Library modules

| Module | Responsibility |
|---|---|
| `format` | Block magics, header parsing and writing for v1 and v2 |
| `bits` | Forward and backward bit writers and readers |
| `fse` | Normalising frequency tables, building encode and decode tables, encoding and decoding |
| `lzvn` | LZVN encoder and decoder |
| `matchfind` | Hash-chain match finder, which has the most tuning knobs |
| `block` | One LZFSE block: tokenise, then entropy-code, or the reverse |
| `stream` | Splitting into blocks, choosing the block type, the end marker, the public API |
| `parallel` | Block-parallel compression of independent chunks (decmpfs-style) |
| `kernels` | Hot loops behind a narrow interface (section 5) |

**Public API**, sketch only:
- Buffer encode and decode, mirroring `compression_encode_buffer`.
- A streaming encoder and decoder.
- `compress_chunks(input, chunk_size, threads)` for decmpfs-style independent 64 KiB blocks.
- Every decode path enforces an explicit output-size limit.

### 4.3 Safety boundary

- **Safe reference path:** the library's portable path is safe Rust. The decoder crate root uses `#![deny(unsafe_code)]`, with exceptions allowed only inside `kernels`.
- **Rules for any `unsafe`, C or assembly kernel:**
  - It has a `// SAFETY:` justification.
  - It sits behind the same interface as a safe Rust reference kernel.
  - It is differentially fuzzed against that reference (section 7).
  - Decoder kernels must enforce bounds themselves, because they process untrusted input.

## 5. Performance plan and hot-loop backends

### 5.1 Kernels

| Kernel | Where | Likely accelerations |
|---|---|---|
| Hash and insert | Match finder | CRC32 instruction as the hash, table size tuned to L1 or L2, prefetch |
| Match-length extension | Match finder | 8-byte XOR with count-trailing-zeros, 16-byte NEON compare |
| Literal and match copy | LZVN and LZFSE decode | 16- and 32-byte NEON or overlapping wide copies, short-distance special cases |
| Histogram | FSE encode | 4 interleaved scalar tables or NEON |
| FSE encode and decode loops | Both | Branchless bit buffering, 64-bit refills, interleaved states |
| Table reset | Per block | Smaller tables, generation counters instead of clearing |

### 5.2 Backends

- **Kernel variants:** each kernel has a portable Rust reference and optional variants:
  - `rust-simd` (`std::arch::aarch64` NEON);
  - `c` (built with the `cc` crate);
  - `asm` (`core::arch::asm!` or `.S` files).
- **Selection:** at compile time through Cargo features. Runtime CPU detection is used only where a variant needs more than the Apple M1 baseline.
- **Same tests for every variant:** the test suite runs once per enabled backend.

### 5.3 Parallelism

- **The decmpfs case:** chunks are independent, so we use scoped threads over a work queue.
- **Thread priority:** chosen through macOS QoS classes. User-initiated work runs on the P-cores. The `free-disk-space` daemon runs at utility or background priority on the E-cores.
- **Large single streams:** blocks are encoded in parallel and concatenated. This is valid because LZFSE blocks are self-contained (**verify**: no state carried across blocks).

### 5.4 Build

- `-C target-cpu=apple-m1` for Apple Silicon release builds.
- Universal binaries keep a portable x86-64 slice.
- Profile-guided optimisation (PGO) is an optional later step.

### 5.5 Ruled out, for now

- **AMX/SME and GPU (Metal):** compression is serial and branchy within a block, and both have high entry costs.
- **Neural Engine and media engines:** fixed-function hardware.
- **Dedicated LZ hardware:** Apple does not expose any to apps.

Each can be revisited as a documented experiment once the CPU path is mature.

## 6. Benchmark methodology

### 6.1 Contenders

- this library, one variant per backend, both single-threaded and parallel;
- `libcompression` LZFSE (`0x801`) through FFI, in-process;
- the BSD reference built with `clang -O3`;
- for context: `libcompression` LZVN (`0x900`) and zlib (`0x205`).

### 6.2 Corpus

Corpus files are downloaded by a script and never committed (licensing and size). Checksums are pinned.

- **[Silesia](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia):** a standard mixed corpus.
- **Text and code:** a slice of enwik8 (a standard Wikipedia text benchmark) and source code.
- **Real-world:** Claude Code session logs (JSON lines with base64 attachments), because that is where `free-disk-space` found its biggest candidates. They are synthetic or user-supplied, never committed.
- **Edge cases:** binaries, already-compressed media, random bytes, all zeros, a highly repetitive file and tiny files.

### 6.3 Shapes

- independent 64 KiB chunks, the decmpfs case;
- 1 MiB buffers;
- whole files up to 256 MiB.

### 6.4 Metrics

- **Throughput** in MB/s of uncompressed data (10^6 bytes), for encode and decode separately.
- **Ratio:** compressed divided by original size.
- **Peak memory.**
- **Spread:** median of at least 10 timed runs after warm-up, with the minimum and maximum reported.

### 6.5 Controls

- in-process timing with no Python or subprocess overhead;
- inputs preloaded in memory, so file I/O is excluded;
- on AC power with Low Power Mode off;
- a cool-down between contenders, and the order of contenders rotated each run;
- user-initiated QoS for single-thread runs.

Every report records the chip model, core counts, macOS version, Rust version and commit hash.

### 6.6 Correctness gate

A result only counts if every output decodes byte-identically with Apple's decoder.

### 6.7 Where benchmarks run

- **Developer Mac:** these are the authoritative runs.
- **CI:** runs a small benchmark smoke test for regressions only, because shared runners are too noisy to gate on.
- **Reports:** results go to `docs/benchmarks/<date>-<machine>.md`.

### 6.8 Baseline already measured

On the owner's Mac (macOS 27.2.1, 64 KiB chunks of a 47 MiB session log, Python harness), `libcompression` LZFSE ran at 193 MB/s and the BSD reference at 180 MB/s, both with a ratio of 0.729. The in-process harness will replace these numbers.

## 7. Testing

- **Unit tests:** per module, such as FSE table construction, bit I/O, header round-trips and every LZVN opcode form.
- **Round-trip property tests:** `proptest` over random and structured inputs, at every boundary (empty input, 1 byte, block-size multiples, maximum L, M and D).
- **Compatibility matrix:** run in CI on macOS against `libcompression`, and on Linux against the BSD reference.

  | Encoder ↓ / Decoder → | Ours | Apple | BSD ref |
  |---|---|---|---|
  | Ours | ✔ | ✔ | ✔ |
  | Apple | ✔ | — | — |
  | BSD ref | ✔ | — | — |

- **Golden vectors:** small committed streams produced by Apple's encoder, covering `bvx-`, `bvxn`, `bvx1` (if reachable) and `bvx2`.
- **Fuzzing:**
  - decoder on arbitrary bytes: errors only, no panics;
  - round-trip encode then decode;
  - differential kernel fuzzing, each backend against the reference;
  - differential decoding against the BSD reference.

  Each runs for a short time in CI and for long runs locally.
- **decmpfs integration:** run on macOS CI and the owner's Mac. Write type 12 files, read them back through the kernel, and parse the system's own compressed files (section 3.4).

## 8. Milestones

| # | Deliverable | Done when |
|---|---|---|
| M0 | This document, workspace skeleton, CI on macOS and Linux, lint rules | The plan is approved and CI is green on an empty workspace |
| M1 | Decoder: `bvx-`, `bvxn`, `bvx1`, `bvx2`, `bvx$` | It decodes Apple's golden vectors, decoder fuzzing is clean, and every **verify** in sections 3.1–3.3 is resolved |
| M2 | Scalar encoder: LZVN and LZFSE `bvx2` | The compatibility matrix is green and the ratio is within 2% of Apple's |
| M3 | Benchmark harness and first report | A reproducible baseline report against `libcompression` and the reference |
| M4 | Optimisation: parallel chunks, then kernels (Rust, then NEON, then C or assembly) | The goal-2 targets are met. Every change shows a measured gain and passes differential fuzzing |
| M5 | decmpfs writer and `free-disk-space` integration | Type 12 files read back through the kernel. The section 3.4 **verify** items are resolved |

## 9. Open questions for the owner

1. **Baseline chip:** M1 as the minimum, with tuning on your own machine? Which M chip is that?
2. **Intel Macs:** portable path only, or worth an SSE/AVX2 backend?
3. **Ratio target:** is "no more than 0.5% worse than Apple's" the right trade-off, or should speed win outright?
4. **Crate name on crates.io:** the names `lzfse` and `lzfse_rust` are taken.
5. **`no_std` support** (for embedded use or other operating systems): worth designing in now?
