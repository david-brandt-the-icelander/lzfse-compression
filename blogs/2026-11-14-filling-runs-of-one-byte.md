# Faster non-zero memset? We tried seven ways, and none of them won

*2026-11-14. lzfse-zazzcat is an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

We started from a list of standard tricks for speeding up `memset` with a value other than zero:

| Technique | Claimed benefit | When |
|---|---|---|
| `__builtin_memset_inline` | High | Small fills of a constant size |
| NEON repeated vector stores | Variable | Medium and large fills |
| `memset_pattern16` | Depends on the workload | Repeated multi-byte patterns |
| Unrolled 64- or 128-bit stores | Moderate | Fixed-size buffers |
| Aligned buffers | Small to moderate | Large repeated fills |
| Parallel memset | Depends on the workload | Very large buffers |

The question was whether any of these would speed up this code base. The short answer is no. Four of them don't
apply here at all. The other two describe the same idea, a byte copied across a wide register and stored many times.
We built seven versions of that idea, and every one decoded more slowly or no faster than the code we already had.
Two side findings were more interesting than the main result. `__builtin_memset_inline` in a loop compiled into a
call to `memset`. And a four-line branch that runs on 0.5% of tokens cost 6–10% of decoding speed.

## Step 1: find out who fills memory with a non-zero byte

As in the [memcpy audit](2026-11-14-auditing-a-memcpy-tuning-plan.md), we listed the call sites first. All of the
project's `memset` calls go through `fill_bytes` and `clear_bytes` in `src/memory_operations.h`. `clear_bytes` always
fills with zero. `fill_bytes` has one caller in the library, in the decoder's slow path (`decode_token_slow` in
`src/decode.c`):

```c
if (distance == 1) {
    // A run of one byte (zeros, padding): memset speed.
    fill_bytes(output + match_start, output[match_start - 1], length);
} else {
    copy_match_exact(output, match_start, distance, length);
}
```

A match at distance 1 repeats the previous byte, so it describes a run of one byte value. That is the only non-zero
fill there is. It can reach the decoder on two paths:

- **Runs of up to 64 bytes** take the fast path, `decode_token_fast` in `src/copy.h`. For any distance below 8 the
  fast path writes the first 8 bytes one at a time, then copies 8 bytes per step from 8 bytes back. Each step reads
  what the step before it has just written, so the steps cannot overlap. That looked like the weak spot.
- **Longer runs**, and runs near the end of the buffer, go to the slow path above and to Apple's `memset`.

That ruled out four techniques straight away:

- **`__builtin_memset_inline`** requires a size known when compiling. Run lengths are only known at run time. (We
  still tried it inside the fast path, with a constant step of 16 bytes. See step 3.)
- **`memset_pattern16`** exists only on Apple's platforms. It handles periods of 2, 4, 8 and 16 bytes but not 3, 5, 6
  or 7. Long runs with a short period already go through `copy_match_exact`, which copies them in a few large pieces.
- **Aligning buffers** isn't possible, because the caller owns the output buffer. Apple's `memset` aligns its own
  stores anyway, and unaligned stores are cheap on Apple Silicon.
- **A parallel memset** doesn't fit, because a single match is at most a few KiB. The decoder already runs streams in
  parallel (`src/parallel.c`).

## Step 2: count the runs

A faster fill only matters if fills are common. We built a throwaway copy of the source with counters in both paths
and decoded three 16 MiB inputs: a slice from the middle of `libclang.dylib`, macOS SDK headers, and system icons
converted to uncompressed 32-bit BMP. The icons are there on purpose: transparent areas are long runs of one byte,
and flat colours are runs with a period of 4 bytes.

| | clang.bin | headers.h | icons.bmp |
|---|---:|---:|---:|
| Distance-1 matches on the fast path | 0.5% of tokens, avg 17.5 B | 0.6%, avg 14.3 B | 0.2%, avg 20.1 B |
| Distance 2–7 on the fast path | 0.9% | 0.1% | **7.3%**, nearly all distance 2 or 4 |
| Distance-1 runs sent to `fill_bytes` | 874, avg 321 B | 113, avg 74 B | **3,793, avg 1,354 B** |

Short runs of one byte are rare everywhere. Long runs are rare too, except in the icons, where `fill_bytes` writes
about 5 MB of the 16 MiB, nearly a third of the output. Those already run at Apple's `memset` speed. The table also
showed something the list of tricks had missed: in the icons, runs with a period of 4 bytes are much more common than
runs of one byte.

## Step 3: seven variants

All of them change only the `distance < 8` branch of `decode_token_fast`. Variants B, C and F also let distance-1
runs longer than 64 bytes stay on the fast path. That is the "NEON repeated stores" loop, done inline instead of by
calling `memset`.

| Variant | What it does |
|---|---|
| A | Distance 1: fill a 16-byte local array with `fill_bytes(run, value, 16)`, then `copy16` it per step |
| B | A, plus runs over 64 bytes stay on the fast path |
| C | B, with `__builtin_memset_inline(out + done, value, 16)` per step |
| D | Periods 1, 2 and 4, through the local array |
| E | Distance 1, pattern kept in a register: `value * 0x0101010101010101`, two `store_u64` per step |
| F | E, plus runs over 64 bytes stay on the fast path |
| G | Periods 1, 2 and 4, pattern kept in a register |

Every variant passed the unit tests, including the test that compares the fast path with a byte-by-byte reference
for distances 1 to 39. For B, C and F we changed the test to expect distance 1 to succeed above 64 bytes.

Variant E's inner loop is as small as a fill loop can be: one 16-byte `stp` of a register pair per step.

```
mov  x17, #0x0101010101010101
mul  x16, x16, x17          ; the byte, copied into all 8 bytes of x16
loop:
stp  x16, x16, [x17, #-8]   ; 16 bytes per step
add  x8, x8, #16
add  x17, x17, #16
cmp  x8, x10
b.lo loop
```

### `__builtin_memset_inline` turned back into `memset`

Variant C was there to test the claim that `__builtin_memset_inline` "guarantees no external function call, even
when the value isn't known at compile time". Each builtin does compile without a call. But the loop around it fills
a contiguous range with the same byte, and Apple clang 21's loop-idiom pass recognised that and replaced the whole
loop with one `bl _memset`, inside the hot loop, saving five registers to the stack around the call:

```
ldrb  w1, [x9]              ; the byte
add   x2, x8, #16           ; the length, rounded up to a whole step
str   x24, [sp, #40]        ; 8-byte Folded Spill
stp   x6, x5, [sp, #16]     ; 16-byte Folded Spill
stp   x14, x15, [sp]        ; 16-byte Folded Spill
bl    _memset
ldp   x14, x15, [sp]        ; 16-byte Folded Reload
```

The guarantee covers each call to the builtin, not a loop of them. Variant A had a smaller version of the same
problem. The compiler kept the 16-byte array on the stack and reloaded it in every step, so variants E to G build the
pattern in a register instead.

## Step 4: measure

Each build was compiled with `make CPU_FLAGS=-mcpu=apple-m1` and run with `lzfse-bench --runs 11` on an M1 Pro. The
inputs were the three files above plus the benchmark's synthetic set (text, JSON lines with base64, binary-like data,
noise and all zeros), in 64 KiB and 1 MiB chunks. The eight builds ran in turn, three rounds each. The table shows
single-threaded decoding speed relative to Apple's decoder in the same run (median of three rounds), as a change
from the baseline.

| Input | Chunk | Baseline vs Apple | A | B | C | D | E | F | G |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| text | 1 MiB | 1.236 | −10.1% | −10.4% | −10.8% | −1.1% | −10.3% | −10.6% | −1.2% |
| text | 64 KiB | 1.186 | −10.4% | −10.5% | −10.6% | −1.1% | −10.2% | −10.4% | −1.9% |
| binary | 1 MiB | 1.027 | −8.2% | −8.1% | −8.2% | −1.3% | −7.6% | −8.0% | −1.5% |
| binary | 64 KiB | 1.072 | −7.4% | −7.6% | −7.7% | −1.0% | −7.0% | −7.4% | −0.9% |
| clang.bin | 1 MiB | 0.947 | −6.1% | −6.2% | −6.7% | +0.1% | −6.4% | −6.0% | −0.4% |
| clang.bin | 64 KiB | 0.957 | −5.6% | −5.8% | −6.0% | +0.1% | −5.7% | −5.8% | −1.0% |
| headers.h | 1 MiB | 0.959 | −7.5% | −7.8% | −8.0% | −0.7% | −7.2% | −7.9% | −0.8% |
| headers.h | 64 KiB | 0.964 | −7.0% | −7.1% | −7.5% | −0.7% | −7.0% | −6.8% | −1.2% |
| icons.bmp | 1 MiB | 0.941 | −7.6% | −7.8% | −7.8% | +0.5% | −7.4% | −7.8% | +1.1% |
| icons.bmp | 64 KiB | 0.949 | −6.5% | −7.3% | −6.9% | +0.7% | −6.9% | −6.7% | +0.6% |
| zeros | 1 MiB | 1.193 | −1.6% | −28.2% | −0.3% | −0.3% | −1.2% | −24.7% | −1.6% |
| zeros | 64 KiB | 1.400 | +0.8% | −21.5% | +0.3% | +1.1% | +1.3% | −20.5% | +0.7% |
| jsonl+base64 | both | ≈1.06 | ±0.3% | ±0.3% | ±0.3% | ±0.1% | ±0.2% | ±0.3% | ±0.0% |

The three rounds of each build agreed closely on these inputs. For example, text at 1 MiB gave 1.236, 1.235 and
1.241 for the baseline and 1.109, 1.114 and 1.101 for E. The differences are not noise. (We left the synthetic noise
input out of the table. It is stored uncompressed, so its decoding is a plain copy, and its ratio jumps around.)

Three results:

1. **Keeping long runs on the fast path loses badly** (B, F: 20–28% slower on zeros). Apple's `memset` beats our
   simple 16-byte store loop on runs of hundreds of bytes. It probably uses wider stores, and for zero it can clear a
   whole cache line at a time with `DC ZVA`, which plain stores cannot. Sending those runs to the cold path was
   already the right call.
2. **A `distance == 1` branch costs 6–10% everywhere** (A, C, E). That includes text and binaries, where only one
   token in two hundred takes the branch. The work inside the branch can't explain that.
3. **Periods 1, 2 and 4 together** (D, G) cost about 1% on most inputs and gain about 1% on the icons, the input
   built to favour them. Not a win.

## Why does a rarely taken branch cost 7%? We don't know yet

The compiled code doesn't explain it. Compared with the baseline loop (315 instructions, 13 reloads), E adds 18
instructions and one reload. G adds 32 instructions and three reloads. Yet G is the faster of the two by 6–9%. The
amount of work points the wrong way. So the loss is more likely a layout effect than a cost of the branch itself.
Possible causes include where the loop's hot blocks land relative to fetch and alignment boundaries, or two hot
branches that end up sharing a predictor entry. We didn't profile it, so this is a hypothesis.

It is the second time today that a small, rarely taken change to `decode_token_fast` has cost far more than it should
([the copy32 experiment](2026-11-14-trying-copy32.md) was the first). The decoder's hot loop is sensitive to its own
shape, so any change there needs a benchmark, however harmless it looks.

## Outcome

No code changed. `fill_bytes` stays a plain `memset` on the cold path, and the fast path keeps its byte-by-byte start
for short periods. For the list of tricks:

| Technique | Verdict here |
|---|---|
| `__builtin_memset_inline` | No constant-size fills to use it on. In a loop it compiled into a `memset` call. |
| NEON repeated vector stores | Slower than Apple's `memset` for long runs. Inline for short runs: 6–10% slower overall. |
| `memset_pattern16` | Apple-only, and doesn't cover periods 3, 5, 6 and 7. Not tried. |
| Unrolled 64/128-bit stores | Same as the NEON idea (variants E–G). No gain. |
| Aligned buffers | The caller owns the output buffer. Not applicable. |
| Parallel memset | Fills are a few KiB at most. Not applicable. |

One idea is untested. The LZVN decoder's slow path has no `distance == 1` case and copies long runs in growing pieces
through `copy_match_exact` (about 9 calls for its longest match of about 271 bytes). A `fill_bytes` case there would
be off the hot loop, so it can't disturb its layout. LZVN only handles small inputs, though, so the gain would be
small.

## Takeaways

- **Count before you optimise.** The counters showed that short runs of one byte are 0.5% of tokens. That capped the
  possible gain before any variant was written.
- **Read the assembly, not the promise.** `__builtin_memset_inline` kept its word for each call, and the optimiser
  still produced a `memset` call from the loop around it.
- **The C library's `memset` is hard to beat for long fills.** It has had years of tuning, and for zero it can use
  instructions that ordinary stores can't.
- **In a tight decode loop, the cost of a change is not the cost of the code it adds.** A branch taken once in two
  hundred tokens cost 7%, and a bigger change cost 1%. Only a benchmark could tell them apart.
