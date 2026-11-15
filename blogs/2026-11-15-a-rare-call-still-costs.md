# A `memcpy` that rarely runs still costs

*2026-11-15 · lzfse-zazzcat*

The question started with one function, the LZVN encoder's `emit_literals`: could it copy its literals in batches
of four bytes, with a short section at the end for the remaining one to three? The answer turned out to be "no,
but something close to it", and following it removed 39 `memcpy` calls from the encoders' hot loops. This post
covers why batches of four were the wrong size, why a call that almost never runs can still slow a loop down, and
the measurement, which came out larger than the explanation.

## The function

`emit_literals` writes literal-only LZVN opcodes: an opcode byte (or two), then up to 271 bytes copied straight
from the input. The copy already had two paths:

```c
if (n + 16 <= readable) {
    wild_copy(at, literals, n);   // 16 bytes per step, may write up to 15 bytes too many
} else {
    copy_bytes(at, literals, n);  // exact: memcpy
}
```

`wild_copy` already works in batches, of 16 bytes rather than 4, and its overshoot lands in output space that the
next opcode overwrites. It may also *read* up to 15 bytes past the literals, which is fine except near the end of
the input, where those bytes do not exist. That is what the second path is for, and it runs about once per input.

So batches of four would have made the common path four times slower. The interesting part was the exact path.

## Why a rare call matters

`copy_bytes` with a size known only at run time compiles to a call to the C library's `memcpy`. A call has a
price beyond its own instructions. Under the Arm64 calling convention, the called function may overwrite about
half of the registers (the *caller-saved* ones), so any value that must survive the call has to sit in one of the
other half or on the stack. A loop with more live values than the safe registers hold has to put some on the
stack.

The compiler does not save them only on the path that calls. In `lzvn_encode_with_table`, before this change, the
disassembly showed this around every literal run of four or more bytes:

```text
stp  x6, x0, [sp, #0x40]     ; save 5 loop values before the literal loop
stp  x17, x5, [sp, #0x50]
str  x24, [sp, #0x38]
...                          ; the copy: wild_copy, or the rare bl memcpy
ldr  x25, [sp, #0x28]        ; load 7 values back after it
ldr  w2, [sp, #0x24]
...
```

The saves and loads ran on every such token, whether or not `memcpy` did. The loop paid for the call it almost
never made.

[An earlier post](2026-11-14-constant-size-lzvn-literals.md) listed the same pattern in the LZFSE encoders (29
calls in `encode_small`, 9 in the main encoder, all from the inlined `block_writer_token`) and set them aside as
rare. Rare was true; harmless was the assumption worth checking.

## Copying exactly, without a call

The new helper, `copy_exact_inline` in `src/copy.h`, copies an exact length using only constant-size loads and
stores, which compile to single instructions:

```c
if (length >= 16) {
    for (size_t done = 0; done + 16 < length; done += 16) copy16(destination + done, source + done);
    copy16(destination + length - 16, source + length - 16);
} else if (length >= 8) {
    copy8(destination, source);
    copy8(destination + length - 8, source + length - 8);
} else if (length >= 4) {
    store_u32(destination, load_u32(source));
    store_u32(destination + length - 4, load_u32(source + length - 4));
} else if (length > 0) {
    destination[0] = source[0];
    destination[length / 2] = source[length / 2];
    destination[length - 1] = source[length - 1];
}
```

This is the "batches plus remainder" idea from the original question, with the remainder handled by overlapping
instead of a second loop. For 20 bytes: one 16-byte copy of bytes 0–15, then one of bytes 4–19. Bytes 4–15 are
written twice with the same values, which is harmless because the source and destination are different buffers.
For 1 to 3 bytes, the three single-byte copies hit positions 0, `length / 2` and `length - 1`, which together
cover every byte. Nothing outside `[0, length)` is read or written, so it is safe at the very end of the input.

A new unit test copies every length from 0 to 100 from a heap block of exactly that size. The sanitizer build
reports any read past it, and the test checks that no byte outside the destination range changed.

## The compiler pushed back once

The first attempt replaced the call and would have made the encoder slower: the disassembly showed a new
`bl _emit_literals`. With the copy inlined, `emit_literals` had grown past the compiler's inlining threshold, so
the compiler compiled it as a separate function and called it for every literal run. Trading a rare call for a
frequent one would have defeated the point. Marking `emit_literals` `ALWAYS_INLINE` fixed it. After that, the
parse loop in `lzvn_encode_with_table` does not touch the stack at all: the function's 30 stack loads and stores
dropped to the 12 of its prologue and epilogue.

## The alternative: keep the call, move it out of the way

The decoders already handle rare work with a `COLD PRESERVE_REGISTERS` function: `preserve_most` tells the
compiler that the callee saves almost every register itself, so the caller should not have to. We measured that
too, wrapping the `memcpy` in such a helper. The LZVN gain was similar, but the loop still saved and reloaded
values around the call, and in the LZFSE encoders the same attribute had already made things slower when tried on
`block_writer_flush` ([keeping values in registers](2026-11-14-keeping-values-in-registers.md)). The inline copy
won on both counts.

## Measuring

Five alternating rounds of the benchmark on an M1 Pro, baseline and change built from the same `main`. One method
change: earlier notes divided our speed by Apple's in the same run to cancel drift, but in these runs Apple's
encoder jumped between two speeds (text at 64 KiB: 104 or about 128 MB/s) while ours stayed within 1–2%. Dividing
would have copied Apple's jumps into our numbers, so the comparison uses our raw MB/s.

| Encoder | Chunks | text | binary |
|---|---|---:|---:|
| LZVN | 1–3 KiB | +1.8% to +2.3% | +3.3% to +4.5% |
| small-input LZFSE | 16 KiB | +0.5% | +0.7% |
| small-input LZFSE | 64 KiB | **+14.1%** | +2.9% |
| main LZFSE | 1 MiB | −0.4% | 0.0% |

The LZVN rows match the explanation: the saves and loads around every literal run are gone, and the gain is a few
percent. jsonl+base64 and noise at 1–3 KiB gained 2–3% as well. They end up stored raw, but the encoder still runs
over them first, and their whole input is one long literal run.

The 64 KiB text row does not match it as neatly. It gained 14% in every round, 226–227 MB/s against 195–200,
yet `encode_small`'s stack traffic went *up* slightly (300 → 326 loads and stores), and its code grew by about
3 KB. Removing 29 call sites changes how the loop's blocks are laid out and how its branches line up, and some of
the gain probably comes from that rather than from the call itself. A gain from layout is real, but fragile: an
unrelated change later can move it back. The benchmark note records the numbers so that a future regression in
that row has something to be compared against.

Compressed output is byte-for-byte identical; only how the bytes get copied changed.

## What else has this shape

The same search, for loops that work byte by byte where a wider step would do, found a few more candidates,
none taken yet:

- `match_length_backward` in `src/copy.h` compares one byte at a time, on every match. It could compare 8 bytes
  at a time, but most calls stop at the first byte, so it needs measuring first.
- The decoder's first 8 bytes of a short repeating match (`decode_token_fast`, period under 8) are copied one byte
  at a time, each possibly depending on the store just before it. Building the 8-byte pattern in a register
  would remove that chain.
- The byte tail of `match_length` and the zero padding in `encode_block` have the shape but run once per input or
  block, which is too rarely to matter.

`emit_token` already does the four-byte trick for its one to three literals, which is where this question began:
copy four, keep what you need.
