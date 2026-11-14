# Skipping a 4 KiB clear made the encoder slower

*2026-11-14. lzfse-zazzcat is an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

[The last post on zeroing](2026-11-14-zeroing-memory.md) ended with one idea still worth trying: rather than
clearing a hash table before each input, tag its entries so that old ones look stale. The large-input match
finder already works this way. This time we went through every remaining clear in the library, picked the one
that looked most worth tagging, which is the LZVN encoder's hash table, and tagged it. Encoding got 2–12% slower in
a focused test, and up to 36% slower in the full benchmark. We tried three versions, and none of them won.
Three smaller clean-ups from the same audit were kept.

All numbers come from an M1 Pro running macOS 27.

## The audit

The library zeroes memory in about two dozen places. The previous round had already removed nine clears that
nothing needed. Each one stays in the source as a commented-out line, with a note explaining why removing it is
safe. We checked those notes again, and they hold. The rest fell into three groups:

| Group | Examples | Verdict |
|---|---|---|
| Zero means something | encoder and thread-pool pointers that start NULL, symbol counts, the 8 zero bytes that open a token stream, a v2 header with no frequency tables | keep |
| Zero keeps the encoder correct or repeatable | large-finder tables, 64 KiB-path buckets and long slots, FSE table tails that a hostile stream can reach | keep |
| Zero only marks "empty" | the LZVN encoder's hash table | try tagging |
| Zero that nothing reads | v1 header padding, a decoder's tables, two bit readers | remove |

Freeing memory never zeroes anything. `zz_free` is plain `free`.

The 64 KiB path is worth a word. Its long-slot table is safe without a clear, because every candidate is checked
against the real input bytes. But leftover slots would make the output depend on the previous input. A test,
`test_reused_encoder_matches_fresh`, requires an encoder that has been used before to give the same bytes as a new
one. So the clear stays.

## The LZVN table and its tag

Inputs under 4 KiB are encoded as one LZVN block. The encoder hashes the 4 bytes at each position into a table
with roughly one slot per 4 input bytes, between 256 and 16,384 slots. Each slot holds `position + 1`, so a cleared
slot (0) means empty. The table is reused across calls and cleared at the start of each one: 1 KiB for a 256-byte
input, 4 KiB for a 4,000-byte one.

Generation tagging removes the clear. Slots store `position_base + position`, and each call moves
`position_base` past everything stored so far. A slot below the base is then left over from an earlier input, and
the distance check rejects it like any other bad candidate. The table needs clearing only when it is new or grows,
or when the 32-bit base would overflow:

```c
uint64_t next_base = (uint64_t)table->position_base + n + 1;
if (table->position_base == 0 || next_base > UINT32_MAX) {
    clear_bytes(slots, table->capacity * sizeof *slots);
    table->position_base = 1;
    next_base = (uint64_t)n + 2;
}
uint32_t base = table->position_base;
```

Leftover slots are rejected, so the output is the same as with the clear. We checked that byte for byte: 1,581
inputs of up to 70 KB, through a reused encoder and the one-shot LZVN encoder, gave the same hash of every output
byte before and after.

## The result

The full benchmark (`lzfse-bench --synthetic --runs 11`, three rounds, alternating old and new builds; the table
shows the median of the three rounds) showed encode speed in MB/s. Decode speed did not change:

| Input | 256 B chunks | 1 KiB chunks | 4,000 B chunks |
|---|---|---|---|
| text | 246 → 213 (−13%) | 286 → 252 (−12%) | 309 → 299 (−3%) |
| JSON lines + base64 | 229 → 202 (−12%) | 248 → 220 (−11%) | 327 → 299 (−9%) |
| binary-like | 351 → 226 (−36%) | 316 → 210 (−34%) | 342 → 236 (−31%) |
| noise | 228 → 202 (−11%) | 249 → 224 (−10%) | 331 → 308 (−7%) |
| zeros | 1241 → 1187 (−4%) | 1323 → 1235 (−7%) | 1332 → 1249 (−6%) |

It was slower on every row.

## Three tries to fix it

The first version computed the distance with a wrapping 32-bit subtraction and checked it in three parts:
`distance != 0 && distance <= i && distance <= MAX_LZVN_DISTANCE`. The old code only needed `candidate > 0`
followed by a range check. So we tried two cheaper checks:

- **A** tests `candidate >= base` first, then does a single range check on `distance - 1`. In unsigned arithmetic
  that check also rejects a distance of 0.
- **B** turns the slot back into a position in this input with one wrapping subtraction,
  `earlier = (uint32_t)(candidate - base)`. Old and cleared slots wrap to a value past `i`, so the distance wraps
  too and fails the same single range check. For a live candidate this matches the old code instruction for
  instruction. The difference is in the empty case.

We also built a **control** that used A's loop but cleared the table on every call anyway, to see whether skipping
the clear was what hurt.

The full benchmark takes minutes, so we timed these with a small harness instead: one reused encoder on 4 MiB of
binary-like data and 4 MiB of text, at 256, 1,024 and 4,000-byte chunks, taking the best of 15 runs. On Apple
Silicon, `/usr/bin/time -l` also reports instructions retired and cycles for the whole run. All five builds came
from the same sources and produced the same output.

| Build | Encode speed vs old | Instructions | Cycles |
|---|---|---|---|
| Old: clear every call | — | 12.97 G | 4.01 G |
| First version | −7% to −12% | +6.4% | +9.4% |
| A: test `candidate >= base` first | −2% to −11% | +6.6% | +6.9% |
| B: one wrapping subtraction | −2% to −8% | +5.6% | +5.4% |
| Control: A's loop, cleared every call | −3% to −9% | +3.5% | +6.1% |

The harness shows smaller losses than the full benchmark, especially on the binary-like input. We didn't
look into why. Both point the same way.

## Why the clear wins

The control row is the key result. It clears the table on every call, just as the old code did, and it is still
slower. So skipping the clear was never what slowed things down. The new check was, and it runs at every input
position.

Compare what each version pays per lookup. A cleared table gives the old code a free sentinel: an empty slot is 0,
and one `cbz` instruction skips it. With tags, an old slot is a non-zero number that has to go through
the arithmetic before the range check can reject it. In version B's compiled loop, an empty or old slot takes six
instructions to reject, where the old loop took two. The harness confirms it: every tagged build retires 3.5–6.6%
more instructions, which is 1.2–2.3 extra instructions per input byte.

Now compare the budgets. In this test the old encoder runs about 34 instructions per input byte, and a 4,000-byte
input takes about 12 µs. Clearing its 4 KiB table takes about 0.05 µs: Apple's `memset` zeroes 64 bytes per
`dc zva` instruction, so the whole clear is well under a hundred instructions. That is about 0.4% of the call. To
come out ahead, a tag check would have to cost less than about one instruction per *seven* input bytes. The
cheapest version cost more than one instruction per byte.

The large-input finder is the opposite case. Its tables are 768 KiB by default and can reach 192 MiB, while a
single input may be far smaller. There, clearing per call would cost more than the encoding, and tags are clearly
the right tool. What decides it is how many table bytes a call would have to clear for each input byte it then
looks up. For the LZVN table that is about one byte, and a clear of that size costs less than any per-lookup
check.

## What this means for the 64 KiB path

The last post's open item was to try tags on the 64 KiB encoder path, where the per-call setup (a 192 KiB bucket
fill and a 32 KiB clear) costs 1–3% of encoding a chunk of real data. It would also need 16-bit positions widened
to 32 bits. We haven't run that experiment. That path gives tags a bigger budget than LZVN's 0.4%, but 1–3% is
still less than the 3.5–6.6% in extra instructions that the tag checks cost here. Unless its lookups turn out to
absorb a longer check much better, the experiment looks likely to lose too.

## What was kept

The tagged LZVN table was reverted. The project only accepts speed changes that come with a measured gain, and
this one measured a loss. The other three findings from the audit were kept. None of them is meant as a speedup:

- **The v1 header writer** cleared all 772 bytes and then wrote 770 of them. The last two are struct padding. It
  now stores a 2-byte zero there instead. Only the tests write v1 headers.
- **`lzfse_zazzcat_decoder_create`** used `calloc` on a struct of about 7 KB. Only the literal-buffer pointer and
  capacity need a starting value, because the FSE tables are rebuilt for every block before anything reads them.
  The one-shot decoder already relied on that. It now uses `malloc` and sets those two fields. This happens once
  per decoder.
- **Two bit readers in the decoder and one in the stream statistics** were declared `= {0}`, although
  `bit_reader_init` sets every field whenever it succeeds and the callers return when it fails. The compiled code
  is identical with and without the initialiser. We checked by disassembling both, which gave the same 951 and
  386 lines. So that change is only about not suggesting the zero matters.

The unit, allocation, sanitizer, libcompression and kernel tests all pass.

## Takeaways

- **A cleared table gives you a free "empty" test.** Zero is the one value a CPU can test without a comparison.
  Generation tags give that up at every lookup to save one clear per call. That trade only pays when the clear is
  large compared with the number of lookups.
- **Work out the break-even point before writing the code.** The clear was 0.4% of the call, which left a budget
  of about 0.14 instructions per byte for the replacement. Knowing that number up front would have predicted the
  result.
- **A control build separates causes that a single comparison can't.** Putting the clear back on top of the new
  loop showed that the loss came from the check, not from skipping the clear.
- **When a branch costs more than it should, count instructions as well as time.** Cycle counts tell you
  something is slower. Instruction counts tell you whether the code does more work or the hardware is handling the
  same work less well.
