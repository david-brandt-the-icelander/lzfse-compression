# Reviewing every switch and if/else ladder

*2026-11-14. lzfse-zazzcat is an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

The question was simple: are there `switch` statements or `if`/`else if` ladders in the library that a faster
pattern should replace? Lookup tables, dense `switch`es that compile to jump tables, branchless selects, testing
the hot case first, bit tests and profile-guided optimization are all standard advice. But each one only pays where
the code runs often enough to notice. So the review had two steps: find every place that branches on one value,
then measure only the ones on a hot path.

Exactly one place mattered, the LZVN opcode dispatch. A table and a `switch` there made LZVN decoding 7–12% faster.
The same change had already landed in parallel (see *Many small files: the allocator didn't matter, the LZVN
decoder did*), so this post is mostly about the review itself. It also covers a second, independent check of that
change, and one measurement mistake worth knowing about.

All numbers come from an M1 Pro running macOS 27 with Apple clang 21 at `-O3`. Apple's reference code was not
consulted.

## The survey

The whole repository has four `switch` statements and about 25 `else if` branches, spread across seven library
files. Sorted by how often they run:

| Where | How often it runs | Verdict |
|---|---|---|
| LZVN decoder: which kind of opcode is this byte? | Once per opcode | **The only candidate** |
| `copy.h`: distance ≥ 16, ≥ 8, or shorter | Once per decoded match | A range test, not a value lookup; the short-distance case already uses a table |
| LZVN encoder: `emit_token` picks an opcode layout | Once per match found | Match finding costs far more |
| `encode_small.c`: `switch (bits)` | Once per call | Already the right pattern (below) |
| Block magic number in `decode.c`, `stats.c`, `decmpfs.c` | Once per block | Too rare to matter |
| `common.c`: status → string | When a caller asks | Clang already turns it into a lookup table |
| Two `switch`es in `bench.c` | Once per file, in the benchmark | Not library code |

The `switch (bits)` in `encode_small.c` deserves a note, because it looks like something a table of function
pointers could replace. Each case calls `parse` with match-finder functions built for one table size, and the
compiler builds a separate copy of `parse` for each case with that size fixed. A table of function pointers would
make every call indirect and stop that specialization. A `switch` that runs once per call is the cheap part.

## What clang made of the ladder

The old decoder tested the opcode byte against each range in turn: end of stream, the two no-ops, long literals,
short literals, long and short previous-distance matches, then a helper that compared the byte with five reserved
values and two reserved ranges, then medium distance, then the low three bits. The most common opcodes, matches with
a small distance, came last.

To see the dispatch, we compiled `decode_run` to assembly and counted the branch instructions:

| `decode_run` on arm64 | Instructions | Conditional branches | Indirect `br` |
|---|---:|---:|---:|
| `if`/`else` ladder | 322 | 52 | 0 |
| Kind table + `switch` (pull request #20) | 279 | 31 | 1 |
| The same, with the exact-copy path moved out of line (pull request #23) | 225 | 26 | 1 |

Clang didn't turn the ladder into anything clever. It kept a tree of comparisons, and the common small-distance
opcodes walked about a dozen of them, each depending on the opcode byte, so the branch predictor has to guess every
one. With a 256-entry table mapping each byte to one of 11 kinds, the `switch` has dense case values from 0 to 10,
and clang compiled it into a jump table (`adrp` of the table, one `br`). It did the same at `-O2`, `-O3` and
`-O3 -mcpu=apple-m1`.

The earlier post saw arm64 clang compile the same `switch` into a short tree of three or four comparisons instead.
Whether clang uses a jump table or a comparison tree is the compiler's choice and changes between versions. Check
the assembly from the compiler you ship with rather than assuming either.

## Testing the hot case first

One standard suggestion is to test the most common case before the general dispatch. We tried exactly that: if the
kind is small distance and two bytes are available, decode it directly, marked `__builtin_expect(..., 1)`, and only
otherwise go through the `switch`. Measured against the ladder in the same run:

| Input (4095-byte pieces) | Plain `switch` | Hot case first, then `switch` |
|---|---:|---:|
| Repository source and docs | +8.7% | +6.0% |
| `/usr/share/dict/words` | +12.0% | +8.6% |
| `/bin/zsh` | +6.4% | +5.6% |

It lost on every input. Small-distance opcodes are common but not common enough: when the test fails, the jump
table still has to be consulted, and the extra test costs more than the cases it saves. We kept the plain `switch`.

The other suggestions don't fit this decoder. Bit tests and range checks are what the table replaces. A branchless
select can't choose between eleven different layouts. We didn't try profile-guided optimization, because the table
gives the compiler nothing left to guess about the dispatch, and it would add a profiling step to the build.

## Checking the change that had already landed

The review ran on a checkout from before the earlier pull request was merged, so we wrote our own table-and-switch
decoder before finding that `main` already had one. The two came out nearly identical: the same eleven kinds, the
same cases, and literal-only opcodes using match length 0 instead of a "no match" marker value. That gave us a
second, independent check of the merged decoder.

A small test program built 49 to 610 LZVN streams per input from 4095-byte pieces of three inputs (source text, a
word list and a program binary). For each input it then:

- decoded every stream with the old ladder and the new decoder and compared both with the original bytes;
- decoded 2 million damaged copies with both decoders: one to four bytes changed, a quarter of them cut short, a
  quarter given an output buffer that is too small. Status, decoded length and every output byte had to agree;
- timed each decoder, best of 7 runs, over about 2 GB of output each.

All 6 million damaged streams agreed. The speed:

| Decode, MB/s of output | Old ladder | Pull request #20 | Gain |
|---|---:|---:|---:|
| Repository source and docs | 1057 | 1150 | 8.9% |
| `/usr/share/dict/words` | 773 | 864 | 11.7% |
| `/bin/zsh` | 1151 | 1228 | 6.6% |

These are smaller than the earlier post's 29%, which counted files per second across 512-byte to 4 KiB files in
the benchmark. Smaller files have more opcodes per byte and more setup per byte. Our test only times the opcode loop
on full 4 KiB pieces. The merged decoder also ran 1.5–2% faster than our own version on all three inputs. We didn't
chase the difference.

These timings are of the decoder as pull request #20 merged it. Pull request #23 later moved the decoder's rare
exact-copy path into a separate cold function. That leaves the dispatch alone, and its own benchmark showed
small-file decoding a few percent faster again.

## The measurement mistake

Our first timing of `main`'s decoder showed only a 3–8% gain over the ladder. The baseline was the library from the
working tree, built while it had uncommitted changes to `copy.h`, the header that holds the shared copy routines.
`main`'s decoder was compiled against `main`'s `copy.h`. So the comparison also measured a difference in the copy
code, not just in the dispatch. Rebuilding both sides from `main`'s headers, with only `lzvn.c` different, gave the
6.6–11.7% above.

## What we learned

- **Survey first, then measure only what runs per byte or per token.** Of four `switch` statements and about 25
  `else if`s, one ran per opcode; nothing else could have shown up in a benchmark.
- **Map values to small dense kinds, then `switch`.** The table removes the range tests, and the dense cases let
  the compiler use a jump table.
- **Read the assembly.** It showed the ladder was a 52-branch tree, and it showed that what the compiler does with a
  `switch` changes between compilers.
- **Hot-case-first only pays when the input is lopsided.** Here it lost to the plain `switch` on every input.
- **Keep function-pointer tables out of specializing dispatch.** A `switch` that picks a specialized copy of a
  function costs nothing next to the indirect calls a table would add.
- **Build both sides of a comparison from the same tree.** One uncommitted header made a 7–12% gain look like 3–8%.
