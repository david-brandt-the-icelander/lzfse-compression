# lzfse-compression: design

Status: **approved** (owner decisions in section 9). Implementation follows the milestones in section 8.

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

Non-goals for now:

- Intel Macs: not supported. The portable Rust path still builds on x86-64 Linux for CI.
- GPU, Neural Engine and AMX/SME offload (section 5.5).
- Other Apple formats (LZ4, LZBITMAP, Brotli).

## 2. Clean-room policy

- **Sources:** the format is implemented from public descriptions and from black-box testing against Apple's `libcompression` and the BSD reference ([lzfse/lzfse](https://github.com/lzfse/lzfse)).
- **No ported code:** no reference code is copied or translated line by line. We may consult the reference source as documentation of the format, for example to confirm a constant or a header field, and record that in commit messages.
- **Design ideas:** since 2026-11-14 (owner's decision), the reference encoder's design may also be studied. Ideas are not covered by its BSD-3-Clause license; copied or translated code would be, so we still write our own. So far this means the deferred ("pending") match and indexing every position, as described in M. Hron, *Compression method LZFSE*, CTU Prague 2018, section 4.1.1.
- **Licensing:** the BSD-3 reference may be built in tests and benchmarks as a comparison target. It is never linked into the library.

## 3. Format notes

These are the facts we need, plus how we will confirm each one. Items marked **verify** are believed correct but must be checked against the reference source and real Apple output before we rely on them.

### 3.1 Stream structure

Confirmed from the reference source. Interoperability is tested against Apple's codec in CI (section 7).

An LZFSE stream is a sequence of blocks. Each block starts with a 4-byte little-endian magic:

| Magic | Block | Contents |
|---|---|---|
| `bvx-` | Uncompressed | `u32` raw size, then raw bytes |
| `bvxn` | LZVN | `u32` raw size, `u32` payload size, then an LZVN payload that ends with its own end-of-stream opcode |
| `bvx1` | LZFSE v1 | 772-byte header with full-width fields (770 bytes of fields plus 2 bytes of C struct padding), then the payload |
| `bvx2` | LZFSE v2 | Bit-packed header with compressed frequency tables, then the payload |
| `bvx$` | End of stream | none |

- **Decoder:** must accept all five block types.
- **Encoder:** Apple's emits `bvx-` below 8 bytes and `bvxn` below 4,096 bytes, falls back to `bvx-` when compression does not help, uses `bvx2` otherwise, and always ends with `bvx$`. We follow the same policy.
- **Cross-block matches:** a match may refer to output from earlier blocks, up to the maximum distance. Each block starts with no previous distance.

### 3.2 LZFSE block (`bvx1` / `bvx2`)

**Token streams**
- **L/M/D tokens:** a block holds up to 10,000 tokens. Each token is a literal length L, a match length M and a match distance D.
- **Literals:** up to 40,000 literal bytes per block, counted after padding to a multiple of 4 with zero bytes.
- **Limits:**
  - L ≤ 315 and M ≤ 2,359 per token. Longer runs are split. A literal-only token has M = 0 and D = 1 (D is unused in that case).
  - D ≤ 262,139.
  - D = 0 means "repeat the previous distance". The previous distance resets at every block.

**Symbols**
- **Literals:** one 256-symbol FSE alphabet with 1,024 states, coded with 4 interleaved states. Literal *i* uses state *i* mod 4.
- **L, M and D:** each value is coded as an FSE symbol plus raw extra bits, with a fixed base and extra-bit count per symbol. The alphabets are L 20 symbols / 64 states, M 20 / 64 and D 64 / 256.

**FSE (tANS) tables**
- **State ranges:** each used symbol owns a contiguous range of `f` decoder states, where `f` is its normalised frequency. Symbols are laid out in ascending order, with no state spreading.
- **Bit counts:** for a symbol with frequency `f` and `N` total states, `k` is chosen so that `N ≤ f·2^k < 2N`. The first `j0 = 2N/2^k − f` states of the range read `k` bits; the rest read `k−1` bits.
- **Next state:** the next state is a per-state base plus the bits read.

**Bitstreams**
- **Writing:** the encoder pushes bits least-significant first into a 64-bit accumulator and flushes whole bytes forward. It encodes the last symbol first.
- **Reading:** the decoder reads the stream backwards from its end, taking the most recently pushed bits first.
- **Padding:** the final partial byte is zero-padded. The header stores how much (`bits` in [−7, 0], stored +7 in v2).
- **Token payload:** starts with 8 zero bytes, so Apple's decoder can read 8 bytes at a time without running off the start.
- **Literal payload:** has no such padding. Apple's decoder may read into the header bytes before it, but never uses those bits.

**Initial states**
The encoder starts every state at 0. The final encoder states are stored in the header and become the decoder's initial states.

**Order within a token**
For each token, from last to first, the encoder pushes D extra bits, D state bits, M extra bits, M state bits, L extra bits and L state bits. The decoder therefore reads L, then M, then D.

**Headers**
- **`bvx1`:** stores the counts, payload sizes, final states and four frequency tables as plain little-endian integers.
- **`bvx2`:**
  - **Packed fields:** three packed 64-bit words follow the magic and the raw size:
    - word 0: literal count (20 bits), literal payload bytes (20), match count (20), `literal_bits + 7` (3);
    - word 1: the 4 literal states (10 bits each), token payload bytes (20), `lmd_bits + 7` (3);
    - word 2: total header size (32), then the L, M and D states (10 bits each).
  - **Frequency tables:** follow as a variable-length bit string, least-significant bit first, in the order L (20), M (20), D (64), literals (256). Codes, written low bits first:
    - `0` → `00`, `1` → `10` (2 bits);
    - `2` → `001`, `3` → `101` (3 bits);
    - 4–7 → 5 bits ending in `011`;
    - 8–23 → 8 bits: `0111` then the value − 8 in 4 bits;
    - 24–1047 → 14 bits: `1111` then the value − 24 in 10 bits.

### 3.3 LZVN

LZVN is a byte-oriented LZ77 format with no entropy coding. Each opcode carries up to 3 literal bytes (L), a match length (M) and a distance (D).

**Opcode forms**

| Form | Bytes | Fields |
|---|---|---|
| Small distance `sml_d` | `LLMMMDDD DDDDDDDD` | L 0–3, M 3–10, D < 1,536 (the 3 high bits must be 0–5) |
| Medium distance `med_d` | `101LLMMM DDDDDDMM DDDDDDDD` | L 0–3, M 3–34, D < 16,384 |
| Large distance `lrg_d` | `LLMMM111` + 16-bit D | L 0–3, M 3–10, D < 65,536 |
| Previous distance `pre_d` | `LLMMM110` | L 1–3, M 3–10, D = previous distance |
| Small match `sml_m` | `1111MMMM` | M 1–15, previous distance |
| Large match `lrg_m` | `11110000` + byte | M 16–271, previous distance |
| Small literal `sml_l` | `1110LLLL` | L 1–15 |
| Large literal `lrg_l` | `11100000` + byte | L 16–271 |

**Reserved opcodes**
- End of stream: `0x06`, followed by 7 zero bytes (8 bytes in total).
- No-op: `0x0E` and `0x16`.
- Undefined, so a decode error: `0x1E`, `0x26`, `0x2E`, `0x36`, `0x3E`, `0x70`–`0x7F` and `0xD0`–`0xDF`.
- Because of those, some (L, M) combinations have no opcode in the forms that store M in 3 bits (`sml_d`, `lrg_d`, `pre_d`):
  - L = 1 allows M ≤ 8;
  - L = 2 allows M ≤ 6;
  - L = 3 allows M ≤ 4;
  - `pre_d` with L = 0 is not available.
  The encoder must avoid the missing combinations.

**Decoder rules**
- Every opcode except end-of-stream needs at least one more input byte after its literals.
- A distance of 0, or one larger than the output written so far, is an error.

**Verified:** we confirmed the opcode set by hand-decoding output from `libcompression` algorithm `0x900`.

### 3.4 APFS transparent compression (decmpfs)

Verified on Apple Silicon in CI (`tests/apple.rs`).

- **Header:** the `com.apple.decmpfs` xattr starts with a 16-byte header: magic `fpmc`, a little-endian `u32`
  compression type and a `u64` uncompressed size.
- **Inline types (7 LZVN, 11 LZFSE):** the compressed data follows the header in the same xattr, as long as the
  whole attribute fits in 3,802 bytes.
- **Resource-fork types (8 LZVN, 12 LZFSE):** `com.apple.ResourceFork` holds a table of little-endian `u32`
  offsets, one per 64 KiB chunk plus one for the end, followed by the chunks. Each chunk is a complete stream. This
  is simpler than zlib's type 4, which wraps its chunks in a classic resource map.
- **Verified both ways:**
  - The kernel reads back files we write with types 11 and 12 and accepts the compressed flag.
  - Our parser decodes 3,494 type-8 files that macOS itself compressed (on the runner image) to exactly what the
    kernel returns.
- **Raw chunks:** we never need a raw-chunk marker. An incompressible chunk becomes an LZFSE stream with a `bvx-`
  block. The parser also accepts a one-byte marker followed by raw data, as older tools write.

## 4. Architecture

### 4.1 Workspace

```
crates/
  lzfse/         library: format, FSE, LZVN, match finder, block and stream codecs
  lzfse-cli/     encode / decode / inspect command line tool
  lzfse-bench/   benchmark harness (section 6), with libcompression and reference FFI
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
| `encode` | Match finder: two hash tables (section 4.2.1), lazy evaluation, skip-ahead; `encode::block` collects tokens and entropy-codes each block |
| `block` | One LZFSE block: tokenise, then entropy-code, or the reverse |
| `stream` | Splitting into blocks, choosing the block type, the end marker, the public API |
| `parallel` | Block-parallel compression of independent chunks (decmpfs-style) |
| `kernels` | Hot loops behind a narrow interface (section 5) |

**Public API**, sketch only:
- Buffer encode and decode, mirroring `compression_encode_buffer`.
- A streaming encoder and decoder.
- `compress_chunks(input, chunk_size, threads)` for decmpfs-style independent 64 KiB blocks.
- Every decode path enforces an explicit output-size limit.

#### 4.2.1 Match finder

Two tables are probed at every position, after the previous distance:
- **Short table:** 4-way buckets keyed on 4 bytes. It finds the short matches binaries and source code live on.
- **Long table:** one position per slot, keyed on 8 bytes, with four times as many slots.

Common short strings (frequent words, JSON keys) refill the short buckets constantly. They push out the one
candidate that starts a long repeat, so with the short table alone the encoder settled for many 10–15-byte matches.
The long table keeps those starts.

**Held-back match.** The parser holds the last match back instead of emitting it at once. If the next match extends
backward over all of it, the held one is dropped: one token instead of two. This is the common case in JSON logs,
where a short match on a shared prefix (`{"parentUuid":"`) is followed by a long copy that includes the prefix. The
reference encoder keeps a pending match for the same reason.

**Indexing inside matches.** Every position inside a match goes into the long table. Inside matches longer than 64
bytes, only the first and last 16 positions also go into the short table, so that long copies do not flush its
buckets. Content that repeats later in edited form (successive versions of a file in a session log) is then found
again right after each edit, at its nearest earlier copy.

On the owner's 552 MB Claude Code session log (M3 Pro, `lzfse-bench --stats`), Apple stores 0.1784 at 1 MiB chunks
and 0.1604 whole:

| Encoder | 1 MiB chunks | Whole |
|---|---:|---:|
| Both tables | 0.1854 | 0.1722 |
| Plus the held-back match | 0.1837 | 0.1704 |
| Plus indexing inside matches (shipped) | **0.1747** | **0.1567** |

Tried and not shipped:
- **Indexing interiors in the short table too:** 0.4% smaller on that log, but it flushes the short buckets.
- **Larger tables (16 bits) with lazy matching up to 32 bytes:** another 1% smaller, for 5–20% slower encoding.
- **Trying every position inside a short held match:** worse in both ratio and speed, because a replaced match's
  bytes became literals.

Hashing more bytes in the single table was tried first and rejected. It helped repetitive text but cost real
binaries up to 8% of ratio, because they depend on 4–6-byte matches.

Measured on a 4 MiB session-log-like input (`tests/apple.rs`, `session_like`):

| Encoder | 1 MiB chunks | Whole |
|---|---:|---:|
| Apple's | 328,753 bytes | 289,447 bytes |
| Short table only | 595,996 bytes | 588,593 bytes |
| Both tables | 120,709 bytes | 89,207 bytes |

On real binaries, libraries, man pages, JSON and source, the change was 0.2–3% smaller with both tables, never
larger.

**64 KiB fast path** (`encode/small.rs`). Inputs of at most 64 KiB, which covers every APFS chunk and most files,
use their own finder:
- **16-bit positions:** the short table is 192 KiB and the long table 32 KiB, against 512 + 256 KiB for large inputs.
- **Cleared on each call**, so no stale-entry checks are needed.
- **No distance-limit check:** a distance can never exceed 262,139 bytes.
- **Sized to the input:** the tables shrink to 1/8 of that for 8 KiB and below. Each size is a separate
  compile-time variant, because variable hash shifts cost about 8%.

At 64 KiB chunks on real files it encodes 30–45% faster than the general finder, at most 0.2% larger. On 6 KB and
20 KB inputs it is 30–50% faster.

### 4.3 Safety boundary

- **Safe reference path:** the library's portable path is safe Rust. The crate root uses `#![deny(unsafe_code)]`; the only exceptions are the copy kernels in `kernels`, which check their bounds once and are tested against safe references.
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

- **The decmpfs case:** chunks are independent, so they are encoded on `rayon`'s persistent work-stealing pool.
  - **Why `rayon`:** it keeps its threads alive between calls and can safely borrow the caller's input, which std's
    scoped threads can only do by spawning fresh threads each call. Like every crate dependency, it is statically
    linked.
  - **Per-worker reuse:** each worker thread keeps its own `Encoder`, and chunk outputs go through a pool of reused
    buffers.
  - **`encode_chunks_into`:** writes the streams back to back plus an offset table, which is the resource-fork
    layout. Once warmed up, it allocates nothing (`tests/alloc.rs`).
- **One-shot functions:** `encode()` and `decode()` deliberately keep no global state. Long-lived callers reuse an
  `Encoder` or `Decoder`, which allocates nothing per call.
- **Thread priority:** chosen through macOS QoS classes. User-initiated work runs on the P-cores. The `free-disk-space` daemon runs at utility or background priority on the E-cores.
- **Large single streams:** blocks are **not** self-contained. A match may reach back into earlier blocks, because the decoder checks the distance against the whole output so far, up to 262,139 bytes. The repeat-distance state does reset at every block. So a single stream can only be encoded in parallel by cutting it into independent segments, at some cost in ratio. The decmpfs case does not have this problem, since every 64 KiB chunk is its own stream.

### 5.4 Build

- `-C target-cpu=apple-m1` for Apple Silicon release builds. The M1 is the minimum supported chip.
- Later, if measurements show a gain, add separate builds tuned for M2, M3 and newer, picked at runtime. That is not part of the first version.
- No Intel slice.
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

| # | Deliverable | Done when | Status |
|---|---|---|---|
| M0 | This document, workspace skeleton, CI on macOS and Linux, lint rules | The plan is approved and CI is green | Done |
| M1 | Decoder: `bvx-`, `bvxn`, `bvx1`, `bvx2`, `bvx$` | It decodes Apple's output in CI and the decoder fuzz and property tests are clean | Done |
| M2 | Scalar encoder: LZVN and LZFSE `bvx2` | The compatibility matrix is green and the ratio is within 2% of Apple's | Done: within ±0.6% |
| M3 | Benchmark harness and first report | A reproducible baseline report against `libcompression` | Done: `docs/benchmarks` |
| M4 | Optimisation: parallel chunks, then kernels (Rust, then NEON, then C or assembly) | The goal-2 targets are met. Every change shows a measured gain and passes differential tests | In progress: parallel and literal-heavy cases beat Apple; single-threaded text and binary do not yet |
| M5 | decmpfs writer and `free-disk-space` integration | Type 11/12 files read back through the kernel | Writer done and kernel-verified; wiring into `free-disk-space` waits for this crate to be merged |

## 9. Owner decisions

1. **Chips:** the M1 is the minimum (the owner uses an M1 at home and an M3 at work).
2. **Intel Macs:** not supported.
3. **Ratio target:** no more than 0.5% worse than Apple's.
4. **Crate name:** `lzfse-zazzcat`.
5. **`no_std`:** supported. The library builds without `std`, using `alloc`. Threads and other OS-dependent parts need the default `std` feature.
6. **Per-chip builds:** M2/M3-tuned builds come later, only if measured to help (section 5.4).
