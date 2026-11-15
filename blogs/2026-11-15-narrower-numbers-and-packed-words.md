# Narrower numbers rarely helped; packing them into words did

*2026-11-15. lzfse-zazzcat is an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

The question for this session had two parts. Where in this code base is a numeric variable wider than it needs to be,
and would fewer bits make it faster? And where could several values, such as a function's parameters, be combined
into one with a little bit-trickery to save work?

The short answer: making numbers narrower almost never helped. Two of the four narrowings we built made the code
slower. But packing several small values into one 64-bit word helped in three places, and the biggest of those gains
was 8–10% on encoding 64 KiB inputs. A second experiment showed that the packing, not the smaller size, made that one
faster. With a fourth change, the four that landed make encoding 12–26% faster on 64 KiB chunks and 5–9% faster on
1 MiB chunks.

## Step 1: what "narrower" can buy on Apple Silicon

On a 64-bit Arm CPU a value held in a register costs the same whether it is 8, 32 or 64 bits wide. Every general
register is 64 bits, and a 32-bit add takes as long as a 64-bit one. A narrower *local variable* therefore can't make
anything faster. At best it compiles to the same code, and at worst the compiler has to add instructions that zero-
or sign-extend the value back to 64 bits. That left two places where width can matter:

- **Data in memory.** A smaller table or array touches fewer cache lines and fits better in the 128 KiB L1 data
  cache of an M1 Pro performance core.
- **How the data is laid out.** Several small fields in one word can be loaded with one instruction and updated with
  one shift, where separate fields need separate loads and stores.

We read every file in `src/` and listed each numeric value that is wider than its range needs, plus every place where
several values travel together. Then we profiled the library to see which of them sit in code that takes measurable
time.

## Step 2: where the time goes

We built a small harness that encodes or decodes 16 MiB inputs in 64 KiB and 1 MiB chunks, and sampled it with
macOS's `sample` tool. The inputs were the same two as in earlier posts: a 16 MiB slice from the middle of
`libclang.dylib` and 16 MiB of macOS SDK headers.

| Encoder (64 KiB chunks) | Share | Decoder (64 KiB chunks) | Share |
|---|---:|---|---:|
| Parse loop and match finder (`encode_small`) | 84% | `decode_tokens` | 60% |
| Block writer (`block_writer_flush`: entropy coding) | 13% | `decode_literals` | 32% |
| `qsort` and its comparison function, from `fse_normalize` | 3% | Building the FSE tables | 5% |
| | | Reading the block header (`parse_v2_header`) | 3% |

The `qsort` share was a surprise: 3% of encoding goes into rounding symbol frequencies, about four times per block.

## Step 3: the candidates

Every numeric value that could be narrower, or packed with others, falls into one of these rows. The verdict column
gives the result of steps 4 and 5.

| Where | What | Could be | Verdict |
|---|---|---|---|
| Encoder `token` (`encode_internal.h`) | 3 × `uint32_t` + 3 symbol bytes = 16 B, 10,000 per block (160 KB) | 8 B: lengths are below 2^9 and 2^12, distances below 2^18 | Built (B). No gain |
| Small finder bucket (`encode_small.c`) | `uint16_t positions[4]` + `uint32_t values[4]` = 24 B; 192 KiB for 64 KiB inputs | 16 B: positions in one word, 16-bit hash tags in another | Built (D). **+8–10%** |
| Large finder bucket (`encode.c`) | `uint32_t positions[4]` + `uint32_t values[4]` = 32 B | Same fields packed two per word (L); or values as 16-bit tags (L16) | L: **+1.5–3%**. L16: slower |
| FSE encoder entry (`fse.h`) | threshold, bits and two deltas, 4 × 32 bits = 16 B | Two fields: 8 B (A1), or 4 B with 16-bit fields (A2) | A1: **+1–2%**. A2: lost most of it |
| `fse_normalize` (`fse.c`) | `{uint64_t remainder; size_t symbol}` = 16 B, sorted with `qsort` | One 64-bit key, `remainder << 8 \| (255 - symbol)` | Built (C). **+1–2%** |
| Frequency codes in the block header (`format.c`) | Up to six compares per code, 360 codes per block | One 16-entry table, each entry packing 4 fields into one `uint32_t` | Built (I). No gain |
| Decoder token values (`decode_tokens`) | `size_t` lengths and distances in registers | `uint32_t` | Built (R). Same code |
| LZVN encoder hash table (`lzvn.c`) | `uint32_t` per slot, at most 4 KiB for the inputs under 4 KiB it gets | `uint16_t` | Not built: saves clearing 2 KiB, about 30 ns per file |
| Literal histogram (`count_literals`) | `uint32_t[4][256]`, 4 KiB cleared per block | `uint16_t` (each lane counts at most 10,000) | Not built: saves clearing 2 KiB once per block |
| Value base tables (`format.c`) | `uint32_t` bases, largest 60 and 312 for two of the three tables | `uint8_t`, `uint16_t` | Not built: 160 bytes, always in L1, a narrow load costs the same |
| FSE decoder entries, frequencies, `opcode_kinds` | 8 B, 4 B, `uint16_t`, `uint8_t` | Nothing | Already minimal |
| Block header, options, block writer, thread pool, buffers | One per call or per block | Anything | Never on a hot path |

**Combining function parameters.** The arm64 calling convention passes up to eight integer arguments in registers
(x0 to x7), so only a ninth argument would go through the stack. No function here that is actually called takes more
than eight. The biggest, the decoder's cold `decode_token_slow`, takes exactly eight. The functions with more, such
as `decode_token_fast` (nine) and `parse`, are always inlined, so their parameters disappear when they are compiled.
Packing, say, three lengths into one register for the cold call would add packing work to every token in the hot
loop, to save work on a call that almost never happens. We didn't build it.

## Step 4: the experiments

Each change went into its own git worktree off the same commit. All of them passed `make test`, including the Apple
and kernel compatibility tests on macOS. Each produced output identical to the baseline, byte for byte, on both
inputs (we compared a hash of the compressed output). Two measurements were taken:

- **Instructions and cycles** per pass over the input, from `/usr/bin/time -l`. A run with one pass was subtracted
  from a run with four, which removes the setup, and each figure is the median of three. Instruction counts are
  steady: a build whose change doesn't touch the measured path moves by 0.1% or less. Cycles move by about ±2%.
- **Speed**, with the builds run in turn, five to seven rounds of 1.5 seconds each per input and chunk size. The
  baseline's spread between rounds was 1–3% in the quieter run.

### A. The FSE encoder: one field instead of a compare

To encode a symbol, FSE writes the low bits of the current state and moves to a new state. Each symbol's entry said
"states at or above `threshold` write `bits` bits, smaller ones write `bits - 1`", with a different `delta` for each
case. In C:

```c
bool high = s >= entry->threshold;
uint32_t bits = high ? entry->bits : entry->bits - 1;
int32_t delta = high ? entry->delta_high : entry->delta_low;
```

That compiled to 15 instructions per literal, and the longest dependency chain ran from the state through a compare, a
conditional select and a load of the chosen delta before the next state was known.

The fast entropy coders (zstd's FSE is the best-known one) fold the threshold and the bit count into one number. If
the state is kept as `x = state + state_count`, in the range [N, 2N) instead of [0, N), then

```c
uint32_t bits = (x + entry->bits_delta) >> 16;   // bits_delta = (k << 16) - (frequency << k)
x = (x >> bits) + entry->state_delta;            // the same delta for both cases
```

The sum's top half is `k` when the state is above the threshold and `k - 1` below it, so a shift does what the compare
did. And because LZFSE gives each symbol one contiguous range of states, a little algebra shows the two old deltas
become the same number once states carry the offset. The entry shrinks from four fields to two, from 16 bytes to 8,
and the loop from 15 instructions per literal to 12, with no compare and no dependent load. The only other change is
that the encoder starts from `x = N` and subtracts N from the final states it writes into the header. The two unit
tests that call `fse_encode` directly needed the same adjustment.

Then we tried to go narrower. With a shift of 11 instead of 16, both fields fit in 16 bits and the entry in 4 bytes
(A2). That made it worse: clang can't load two 16-bit fields of different signedness with one instruction, so the
single `ldp` became an `ldrsh` and an `ldrh`. A2 kept only a fifth of A1's saving in instructions.

The listing also showed a `cmp`/`csel` pair in every bit-writer push. It comes from `if (count != 0)` in
`bit_writer_push`, a guard that does nothing: a value of zero bits is zero, and OR-ing it in changes nothing. A3 is A1
without the guard.

### B. The token: 16 bytes to 8

A token holds a literal length (at most 315, so 9 bits), a match length (at most 2359, 12 bits) and a distance (at
most 262,139, 18 bits), plus three symbol bytes the block writer fills in later. They fit in 8 bytes: the distance and
two of the symbols share a `uint32_t`, and the literal length shares a `uint16_t` with its symbol. That halves the
token array from 160 KB to 80 KB, which then fits in L1 across the block writer's three passes over it.

### C. One key instead of a comparison function

`fse_normalize` gives each symbol the rounded-down share of the states and hands the leftovers to the symbols with the
largest remainders, lower symbols first on a tie. It sorted 16-byte `{remainder, symbol}` pairs with `qsort` and a
two-field comparison function. A remainder is smaller than the total count, which is below 2^40, so the pair packs
into one 64-bit key, `remainder << 8 | (255 - symbol)`, whose plain integer order is exactly the order wanted. With
plain integers there's no need for a callback. And because each symbol lost less than one state to rounding, fewer
leftovers remain than there are symbols: only the top `left` keys matter, not their order. A quickselect finds them in
linear time. The keys are all different, so the chosen set, and the output, are the same as before.

### D. The small finder's bucket: two words

The match finder for inputs up to 64 KiB keeps, per hash bucket, the last four positions (16 bits each) and the four
bytes found at each (32 bits each), 24 bytes in all. Inserting a position shifted the four entries along one at a
time. The listing showed five loads and six overlapping stores for every input position, partly over bytes it had
just loaded.

D stores the four positions in one `uint64_t` and, instead of the four bytes, a 16-bit tag in another: the 16 hash
bits just below the bucket index. Inserting becomes one shift and one OR per word:

```c
b->positions = (b->positions << 16) | (uint16_t)position;
b->tags = (b->tags << 16) | small_tag(four_bytes, bits);
```

A matching tag is no longer proof that the bytes match, so a hit now loads the candidate's 4 bytes from the input to
check. The bucket is 16 bytes, never straddles a cache line, and the table for a 64 KiB input shrinks from 192 KiB to
128 KiB.

That made two changes at once, a smaller table and a packed layout. Three control builds pulled them apart: D with
padding back to 24 bytes (D24), to 32 bytes (D32), and a packed bucket that keeps the full 32-bit values in two words
without tags, also 24 bytes (DV).

### L and L16. The large finder's bucket

Inputs over 64 KiB use a finder whose buckets hold four 32-bit positions and four 32-bit values (32 bytes). Its
search loads the bucket into eight registers, in a loop that already runs short of registers and reloads values from
the stack. L keeps every field at 32
bits but packs them two per 64-bit word and inserts with a shift across the pair. L16 then narrows the values to
16-bit tags, as D does, which makes the bucket 24 bytes.

### I and R. Two in the decoder

I replaces the ladder of compares that decodes one frequency code in a block header with a 16-entry table. Each entry
packs the code's length, base value, shift and mask into one `uint32_t`. R changes the decoder's per-token lengths and
distances from `size_t` to `uint32_t`.

## Step 5: results

Instructions and cycles per pass, as a change from the baseline (the range covers both inputs):

| Change | 64 KiB chunks: instructions | cycles | 1 MiB chunks: instructions | cycles |
|---|---:|---:|---:|---:|
| A1: FSE entry, two 32-bit fields | −0.8 to −1.2% | +1.2 to −1.6% | −0.7 to −1.2% | +0.6 to −2.6% |
| A2: FSE entry, two 16-bit fields | −0.2 to −0.3% | +0.4 to −1.2% | −0.1 to −0.3% | −0.5 to −2.0% |
| A3: A1 without the push guard | −2.3 to −2.9% | −1.4 to −2.1% | −2.0 to −2.7% | −0.4 to −1.0% |
| B: 8-byte token | −0.1 to −0.3% | +0.0 to +0.5% | +0.7% | −0.4 to +1.9% |
| C: one key, quickselect | −0.7 to −1.2% | −1.4 to −2.5% | −0.3 to −0.7% | +0.2 to −1.8% |
| D: small bucket, 16 B, tags | **−6.2 to −6.6%** | **−8.0 to −9.4%** | unchanged | unchanged |
| D24: D padded to 24 B | −5.6 to −5.9% | −6.7 to −7.2% | | |
| D32: D padded to 32 B | −5.9 to −6.4% | −7.0 to −7.4% | | |
| DV: packed, 32-bit values, 24 B | −4.6 to −5.4% | −7.6 to −8.6% | | |
| L: large bucket, packed, 32 B | | | −1.3 to −2.1% | −1.5 to −3.5% |
| L16: L with 16-bit tags, 24 B | | | +1.7 to +2.2% | +1.6 to +1.8% |
| A3 + C + D | **−9.1 to −10.6%** | **−10.4 to −13.4%** | −2.4 to −3.5% | −2.0 to −3.7% |
| A3 + C + D + B | −8.4 to −9.9% | | −1.4 to −2.7% | |

Speed from the timed runs, as a change from the baseline:

| Change | clang.bin 64 KiB | headers 64 KiB | clang.bin 1 MiB | headers 1 MiB |
|---|---:|---:|---:|---:|
| A3 | +2.2% | +1.3% | +1.7% | +0.9% |
| C | +2.5% | +1.1% | +1.0% | +0.6% |
| D | **+10.1%** | **+8.5%** | −0.1% | −0.0% |
| A3 + C + D | **+15.6%** | **+11.4%** | +3.0% | +1.8% |

I made no difference to decoding: −0.03% to −0.19% instructions, and speeds within ±1.7%, the noise of that run. R
changed `decode_tokens` from 316 instructions to 315. Its cycle counts came out 0.5–9.6% *higher*, but they were
measured after another job had started loading the machine (a load average of 31 on ten cores), so we don't trust
them. The instruction counts, which load doesn't affect, show the two versions do the same work.

### What the numbers say

- **D's gain comes from packing, not from the smaller table.** Padding D back to 24 or 32 bytes kept 85–90% of the
  gain, and DV, which narrows nothing, kept 75–84% of the instruction saving and 91–95% of the cycle saving. One
  load of the whole bucket, and an insert of a shift and an OR per word, replaced five loads and six overlapping
  stores per position. The tags add about one more point.
- **Narrowing the large bucket's values made it slower.** L16 is about 3.5% behind L in instructions. Its tag needs a
  shift that varies with the table size, held in yet another register in a loop already short of them, and every tag
  hit costs an extra load to check the bytes. Halving the values to 16 bits saved 8 bytes of a bucket that sits in
  L2 either way.
- **The narrow FSE entry gave back most of the gain.** Going from 8 bytes to 4 split one paired load into two, and A2
  kept a fifth of A1's saving in instructions.
- **The 8-byte token did nothing.** Unpacking the fields costs instructions in two of the three passes, and the
  smaller array saved no measurable time. Our guess, not measured: each pass reads the tokens in order, which the
  hardware prefetcher handles well, so the 160 KB were never really waiting on L2.
- **The bit-trick in the FSE encoder pays, but the block writer is only 13% of encoding.** Cutting the literal loop
  from 15 instructions per literal to 12 (A1), or 10 (A3), is worth about 1–2% overall.

## Outcome

Four changes landed, one commit each, all with output identical to before, byte for byte:

1. **D**, the packed 16-byte small-finder bucket (`src/encode_small.c`), for inputs up to 64 KiB, which covers every
   APFS chunk.
2. **A3**, the two-field FSE encoder entry and the push without a guard (`src/fse.h`, `src/fse.c`, `src/bits.h`).
3. **C**, the single-key normalization with a quickselect instead of `qsort` (`src/fse.c`). It comes with a new test
   that checks the result against a plain reference on 3,000 random count tables. The test passes on the old
   `qsort` code and fails on a deliberately broken select.
4. **L**, the packed large-finder bucket (`src/encode.c`), for inputs over 64 KiB.

The full benchmark (`lzfse-bench --runs 7`, three rounds of each build in turn on an M1 Pro, encoding speed relative
to Apple's in the same run, median of the rounds) shows the four together:

| Input | Chunks | Before (vs Apple) | After (vs Apple) | Change |
|---|---|---:|---:|---:|
| clang.bin | 64 KiB | 1.300 | 1.508 | +16.0% |
| clang.bin | 1 MiB | 1.238 | 1.302 | +5.1% |
| headers.txt | 64 KiB | 1.366 | 1.530 | +12.0% |
| headers.txt | 1 MiB | 1.270 | 1.335 | +5.1% |
| text | 64 KiB | 1.556 | 1.752 | +12.6% |
| text | 1 MiB | 1.646 | 1.795 | +9.1% |
| binary | 64 KiB | 1.059 | 1.316 | +24.3% |
| binary | 1 MiB | 1.105 | 1.180 | +6.8% |
| jsonl+base64 | 64 KiB | 2.404 | 3.033 | +26.2% |
| jsonl+base64 | 1 MiB | 1.533 | 1.676 | +9.3% |

The three rounds agreed to about 1% (binary at 64 KiB: 142–144 MB/s before, 178–179 after). The compression ratios
are unchanged, and decoding, which none of this touches, stayed within ±1.1%. The synthetic noise and zeros inputs
are left out, as in earlier posts: noise is stored raw and its speeds jump around, and zeros encode at gigabytes per
second either way. On 1 MiB chunks the gain is larger than our harness measured for L alone (1.5–3.5%), because the
FSE and normalization changes apply to every block too.

B, A2, L16, I and R were dropped. The candidates we didn't build (the LZVN hash table, the histogram lanes, the base
tables) were ruled out by arithmetic: each saves tens of nanoseconds per file or per block, far below what the
benchmark can see.

## Takeaways

- **On a 64-bit CPU a narrower number is not a faster one.** In registers it changes nothing (R), and in memory it
  only helps when the smaller footprint crosses a cache boundary that matters. Twice it made the code slower, because
  narrow fields cost extra loads or extra work to check.
- **Packing is what pays.** Fields that are read together and updated together belong in one word. One load, and a
  shift instead of a chain of moves, beat a smaller table every time we could tell the two apart.
- **Separate the two effects before believing either.** D's 9% looked like a cache win until the padded controls
  showed it wasn't.
- **Look for compares that a little arithmetic can replace.** The FSE threshold, the sort's two-field comparison
  and the bit-writer's guard each cost a compare and a select on a hot path, and each turned out to be arithmetic
  in disguise.
