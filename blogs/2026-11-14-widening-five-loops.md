# Five loops that took small steps: two got faster, three stayed as they were

*2026-11-14. lzfse-zazzcat is an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

The question for this session was a simple one to ask: which loops in `src/` move in steps that are too small? A
loop that handles one byte or one table entry per step can often handle 4 or 8 at once, with one wide load or one
vector store. We read all of the roughly 80 loops in the library, compiled the hot ones with `-O3`, and read the
arm64 assembly. That left five candidates. We changed all five and measured each one on its own. Two changes made
it in. The other three were no faster on real data, and one of them was clearly slower.

## Step 1: find the candidates

Most loops were already fine. The match finders' forward compare (`match_length`) steps 8 bytes at a time, the
decoder's copies step 16, and the literal decoder reads 4 literals per refill of its bit reader. Clang already
vectorizes the frequency sums. Loops over tokens can't be widened, because every token depends on bit-stream state
that the previous token left behind. Five loops stood out:

| # | Loop | What the assembly showed |
|---|---|---|
| 1 | The encoder's literal histogram (`encode_block.c`) | One byte per step into one table of counts: load, add, store. On a run of equal bytes, every step waits for the previous step's store to the same counter. |
| 2 | The decoder's FSE table build (`fse.c`) | One state per step, a branch on which of a symbol's two runs of states it is in, and three narrow stores (`strb`, `strb`, `strh`). Not vectorized. About 1,300 states per block. |
| 3 | The start of a short-period match (`decode_token_fast` in `copy.h`) | For distances below 8, eight byte copies in a row, each load waiting for an earlier store. |
| 4 | Extending a match backwards (`match_length_backward` in `copy.h`) | One byte per step. Runs on every match the encoders find. |
| 5 | The LZVN encoder's search (`lzvn.c`) | On a miss it always moves on by one byte. The LZFSE encoder steps further ahead after 64 misses in a row (`skip_shift`); the LZVN encoder, which handles every input under 4 KiB, never does. |

## Step 2: the changes

**1. Four histograms.** `count_literals` loads 4 literals with one `load_u32` and gives each of the 4 byte lanes its
own table of counts. The lanes update different counters, so their steps overlap instead of waiting on each other.
The four tables are added together at the end, which clang vectorizes. The trick is a standard one in fast
entropy coders.

**2. Two runs per symbol, no branch.** A symbol with frequency `f` owns `f` states. The first ones read `k` bits and
their next-state bases rise by `2^k`; the rest read `k - 1` bits and their bases rise by `2^(k-1)`. The old loop
decided, state by state, which run the state was in. The new code works out where the first run ends once per
symbol, then fills each run with a loop that has no branch.

The first version of that loop still was not vectorized. We had built each entry as one integer and stored it with
`copy_bytes`, our `memcpy` wrapper. Clang's `-Rpass-analysis=loop-vectorize` said "instruction cannot be
vectorized", and the same loop in a small test file said "call instruction cannot be vectorized": the vectorizer
gives up on the `memcpy`. Assigning the struct's fields one by one fixed it. A small test file showed
that a whole-struct assignment does not vectorize either. With the fields assigned one at a time, clang writes 4
entries per vector store.

**3. The pattern in a register.** For distances 1 to 7, `decode_token_fast` now loads 8 bytes from the match source,
masks off all but the `distance` bytes of the pattern, and multiplies by a constant with a 1 every `distance` bytes,
for example `0x0001000001000001` for distance 3. The copies of the pattern don't overlap, so the multiply makes no
carries, and one 8-byte store writes the result. Unlike the earlier experiments with this path
([copy32](2026-11-14-trying-copy32.md), [runs of one byte](2026-11-14-filling-runs-of-one-byte.md)), it adds no
branch.

**4. Backwards, 8 bytes at a time.** The same idea as the forward `match_length`: XOR two 8-byte loads. Going
backwards, the byte nearest the match is the highest byte of the word, so `__builtin_clzll` finds the first
difference. A second version compared one byte first, so that the common case, a match that doesn't extend
backwards at all, would cost no more than before.

**5. Skipping in the LZVN encoder.** On a miss, step `1 + (misses >> skip_shift)` ahead, as the LZFSE parse loop
does.

All of them passed the unit tests. We added two reference tests. One checks the decoder tables state by state
against the formula in `fse.h`, including a symbol that owns every state and frequencies that add up to less than
the table. The other checks `match_length_backward` against a byte-by-byte loop for every pair of positions in a
96-byte buffer and eight limits.

## Step 3: measure

Every change was built into its own copy of the benchmark (`make BUILD=... CPU_FLAGS=-mcpu=apple-m1`), with the
changes stacked one on top of another, so each build differs from the one before it by one change. The builds ran
in turn, three rounds each, with `lzfse-bench --runs 7` in 64 KiB and 1 MiB chunks. The inputs were the synthetic
set, 16 MiB from the middle of `libclang.dylib`, 16 MiB of macOS SDK headers and, in a second round, 16 MiB of
system icons converted to BMP, which have long runs and many periods of 4 bytes. As in earlier posts, each speed is
taken relative to Apple's in the same run, and the median of the rounds is compared.

Because two builds ran identical code on most inputs (change 5 only affects inputs under 4 KiB), we could read the
noise straight off the table: about ±1.5% on most rows, more on zeros, and far more on the incompressible noise
input, which is only copied. We left noise out.

Changes 1 and 2 are small compared with a whole compression or decompression, so we also timed them on their own,
old against new code, over 40,000-byte pieces of the real inputs:

| Loop | headers | clang binary | icons |
|---|---:|---:|---:|
| Histogram, ns per byte | 0.70 → 0.30 (2.4×) | 0.62 → 0.32 (1.9×) | 2.21 → 0.62 (3.6×) |
| Table build, ns per block | 1,254 → 838 (1.5×) | 1,298 → 867 (1.5×) | 1,302 → 840 (1.6×) |

The icons gain the most from four histograms because they have the longest runs of one byte, which is exactly the
case where one table makes every step wait.

## Results

| Change | End to end | Verdict |
|---|---|---|
| 1. Four histograms | Encoding about +1% on average (from −1.3% to +2.8%, the most on JSON with base64, which has the most literals) | **Kept**: small end to end, but clear on its own |
| 2. Table build in two runs | Decoding +14.6% on all-zero 64 KiB chunks, where building the tables is most of the work; within noise elsewhere | **Kept** |
| 3. Pattern in a register | Within ±1.5% everywhere, including the icons | Dropped |
| 4. Backwards, 8 bytes per step | Encoding 3–7% faster on icons, 2–6% slower on text, binary and files of 4–256 KiB; the version that compares one byte first was no better | Dropped |
| 5. LZVN skipping | Encoding +23% on the benchmark's small files (0.5–4 KiB), but 3–10% *slower* on small files cut from the headers, the binary, the icons and real small SDK headers, with output up to 0.3% larger | Dropped |

### Why 3 didn't help

Short periods are rare. Earlier counts put distances 2–7 at 1% of tokens or fewer on most inputs and 7% on the
icons.
The change removes a few cycles from each of those tokens, and the benchmark can't see a few cycles on 1% of
tokens. What it does show is that this change, unlike the earlier ones in this function, costs nothing. The decoder
loop is sensitive to branches more than to instructions. We still didn't keep it, because the project only takes
speed changes that show a gain.

### Why 4 got slower

Most matches don't extend backwards at all, so the old loop usually stopped after one compare. The wide version
loads 16 bytes before it can stop. But comparing one byte first did not remove the loss, so the extra loads are not
the whole story. `match_length_backward` is inlined into every match finder, and the parse loop is inlined into
four copies of the small-input finder plus the large one. A larger body there can change how the hot loop is laid
out and which values stay in registers. We didn't profile it, so this is a hypothesis. The icons gained because
their matches really do extend backwards a long way.

### Why 5 was faster on one benchmark and slower on real files

The benchmark's small files are cut from its synthetic inputs, and one of them is JSON with long base64 strings,
which have few matches. Skipping helps a lot there. Real small files have matches every few bytes, so the skip
rarely triggers, and yet the loop got slower. The assembly gave one reason: the extra live values (the miss count
and `skip_shift`) made the compiler move more of the loop's state to the stack, with 33 stack accesses in the
function against 28. A version with a constant shift on the length of the pending literals, which adds no live
value, brought the spills back to 28 and was still 5% slower on the headers. Our best guess, not measured: skipped
positions are not added to the hash table, so later searches find fewer matches and try more positions overall.
The slightly larger output points the same way.

The lesson is about the benchmark more than the loop. A synthetic small-files set can reward a heuristic that real
small files punish. The decision here rested on real files.

## Outcome

Two changes landed: the four-lane literal histogram (`count_literals` in `src/encode_block.c`) and the branch-free,
vectorized FSE table build (`fill_literal_run` and `fill_value_run` in `src/fse.c`), together with the two new
reference tests. The table-build test and the backward-match test stay, since they check behaviour that doesn't
depend on which version of the code is in place.

## Takeaways

- **Read the assembly before widening a loop.** Two of the five loops were not what the C suggested. The table
  build compiled to three narrow stores per entry, and the vectorizer refused a loop because of one `memcpy`.
- **Time a small loop on its own as well.** A 2–3× faster histogram shows up as about 1% of encoding time. Without
  the microbenchmark we couldn't have told whether the change worked or the benchmark was just noisy.
- **A wider step only pays if the narrow loop ran for long.** The backward match usually stops after one byte, so 8
  bytes per step had nothing to save on most data.
- **Check a heuristic on real data, not only on the benchmark's own.** LZVN skipping was 23% faster on the
  synthetic small files and 3–10% slower on real ones.
