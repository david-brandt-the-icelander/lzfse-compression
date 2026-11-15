# The binary decode margin: a simpler refill, and a mask idea that only helped x86

*2026-11-15. lzfse-zazzcat is an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

Binary files were the last input where Apple's decoder had recently beaten ours. On the owner's M1 Pro, one run had
us 1–5% behind on binaries and the next 2–7% ahead. Single runs vary by about 5%, so neither result settles it, and
the honest goal was a margin that noise can't erase. This post covers where the binary decoder spends its time, two
changes that cut it on every architecture, and one change that looked good on x86 and was dropped because of what
it did on arm64.

The x86 numbers come from a 4-vCPU cloud VM with Clang 18; the arm64 numbers come from reading the assembly Clang
generates with `-mcpu=apple-m1`. The real gain on the M1 is still to be measured.

## Where the time goes

Binaries decode differently from text. Our synthetic binary input, x86-like instruction records, compresses into
many short matches: 842 thousand tokens and 1.79 million literals in 8 MiB at 64 KiB chunks. Most match lengths are
8–15 bytes and most distances 256–4095 bytes. Each token carries three entropy-coded values (literal length, match
length and distance), so the per-token work is a large share of the total.

Counting instructions in the decoder alone on x86:

| Part | Instructions | Share |
|---|---:|---:|
| Token loop | 132 M | 75% |
| Literal decoding | 42 M | 24% |
| Block headers and FSE tables | about 2 M | 1% |

That's about 156 instructions per token and 24 per literal on x86. The arm64 code is leaner, because the M1 has
twice as many registers: about 94 instructions per token on the common path, but with two values spilled to the
stack and one reloaded on every token.

Splitting the arm64 token loop by job:

| Job | Instructions per token |
|---|---:|
| Refilling the bit reader | 13 |
| Decoding three FSE values | about 44 |
| Bounds checks | 16 |
| Copying literals and the match | about 13 |
| Loop bookkeeping | 5 |

## A refill that just reloads

LZFSE reads its entropy-coded streams backwards, from the end of each stream towards the start. Our bit reader keeps
up to 64 bits in an accumulator; its low `bit_count` bits are the ones still unread. When fewer than 56 remain, a
refill steps the position back by as many whole bytes as fit and brings them in.

The old refill did that by hand: load 8 bytes from the new position, mask them down to just the new bytes, shift
the old accumulator up to make room, and merge the two with an OR. That's several instructions for what amounts to
a reload.

It is a reload. The accumulator's valid bits always equal the bytes at the reader's position, exactly as a
little-endian 8-byte load from there would give them. That holds after the reader starts and after every refill,
and reading bits only lowers the count, never touches the bytes. So after stepping back, a single load from the new
position produces the new bytes in the low bits and the old valid bits right above them, which is what the
shift-and-merge built. The bits above the valid ones differ: they're bytes the reader has already consumed, rather
than leftovers. Reading never looks at them, because it masks every value down to the bits it asked for.

One case needs care. A stream whose last byte is fully used starts with only 7 bytes in the accumulator, and a load
at that position would read one byte past the end of the stream. The refill already returns early when no byte is
needed, and in exactly that state none is, so the reload can never happen there. The comment in `bits.h` now says
so, because this is the kind of reasoning nobody should have to reconstruct.

On arm64 the refill drops from 13 instructions to 9, for both tokens and literals.

## Checks that could never fail

Before copying a token, `decode_token_fast` makes one combined bounds check, so the copies themselves can run without
any. Two of its comparisons asked whether the literal length and the match length were at most 65,536, to make
sure the position sums below couldn't overflow.

They can't fail. The lengths come out of LZFSE's FSE decoding as a base value from a constant table plus a fixed
number of extra bits, also from a constant table. Whatever a corrupt stream contains, a literal length can't
exceed 315 and a match length can't exceed 2,359. The LZVN decoder, which shares this routine, caps both at 271.
So the two comparisons and their branches went, and a comment states the bounds and why the remaining sums can't
overflow.

That's four instructions per token. It also freed the registers the comparisons needed, and with them the stack
spills: the arm64 loop no longer saves two values and reloads one on every token. The common path is now about
84 instructions per token, down from 94.

## Masks in the table: good on x86, not on arm64

Each FSE value decode builds two bit masks, `(1 << n) - 1`, from widths it has just loaded, at two instructions per
mask. The literal decoder builds one per literal. Since the tables are rebuilt only once per block, storing the
finished masks in each table entry looked free: the decoder would load a mask instead of building it.

On x86 it worked. Decoding took 6% fewer instructions on text, 8.5% on binaries and 14% on JSON.

On arm64 it didn't. Value entries grew from 8 to 16 bytes and literal entries from 4 to 8. The token loop keeps the
next token's three entries in registers, which is how it overlaps their loads with the current token's copies, and
three 16-byte entries need six registers instead of three. The extra registers cost as much in new stack spills as
the masks saved. In the literal loop a load simply replaced a shift, so the count didn't change. And Clang's
vectorizer saw three 16-byte loads and moved them into vector registers. The resulting code shuffled fields between
vector and integer registers, and such moves can take several cycles each on the M1. With fewer x86 instructions and no
arm64 gain, the change was dropped. The M1 is the target, so its assembly decides.

## Checking that nothing changed

The bit reader sits under every entropy-coded byte, and the decoder reads untrusted files, so the change needed
more than the unit tests. We compiled the old and new library each into its own shared library, loaded both into
one test program, and compared their status, decoded length and every output byte on 1,086,000 inputs:

- valid streams of up to 1 MB from five inputs, with output space that was roomy, exact and too small;
- 80,000 of those streams with bytes replaced, bits flipped or their ends cut off;
- a million random byte strings, half of them starting with a valid block marker.

They agreed on every one. The tests also passed under AddressSanitizer and UndefinedBehaviorSanitizer, the four
fuzz targets ran without findings, and the differential test was repeated after merging a day of other sessions'
work into the branch.

## Result

| Decode instructions, x86, 64 KiB chunks | Before | After | Change |
|---|---:|---:|---:|
| Binary | 175.1 M | 162.4 M | −7.3% |
| Text | 131.4 M | 120.7 M | −8.1% |
| JSON + base64 | 196.0 M | 187.0 M | −4.6% |

On arm64 the token loop's common path went from about 94 instructions to 84 with no stack traffic, and the literal
loop saves 4 instructions per four literals. Timings on the x86 VM swing by up to 80% between runs, so they aren't
quoted. The M1 benchmark will say how much of this shows up in MB/s.

## What we learned

- **Look for a simpler statement of what the code already does.** The refill was a reload all along; noticing the
  invariant removed the work.
- **Check that every check can fail.** A comparison against a bound the format makes impossible costs instructions
  and, here, registers too.
- **Judge an optimization on the target's assembly.** Precomputed masks cut 6–14% of x86 instructions and nothing on
  arm64, because wider data costs registers that the M1 code was already short of.
