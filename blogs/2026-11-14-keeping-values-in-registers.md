# Keeping values in registers, without the `register` keyword

*2026-11-14 · lzfse-zazzcat*

The question was simple: are there hot loops in this code base where we should tell the compiler to keep a value in
a register? C has a keyword for exactly that, `register`. It turned out to be the wrong tool. But the question
behind it was a good one, and the compiler can answer it directly. This post covers how we asked, the five places
it pointed to, and the one fix that made things slower.

## Why `register` does nothing

`register` is a hint from the 1970s, when compilers did little more than translate line by line. Today's compilers
decide for themselves which values live in registers, and they do it better than a hand-written hint. At `-O1` and
above, Clang and GCC ignore `register` when choosing registers. Its only remaining effect is that you may not take
the variable's address (`&x` is an error). Adding it anywhere in this library would compile to the same machine
code.

So the useful question is a different one: **where does the compiler keep a value in memory when we expected it in
a register, and why?**

## Why a value ends up in memory

This loop looks as if `out->length` could stay in a register:

```c
for (...) {
    out->data[out->length] = byte;   // a one-byte store
    out->length += 1;
}
```

It can't. The store goes through a `uint8_t *`, and C allows a byte store to change *any* object, including `*out`
itself. The compiler cannot prove that `out->data` does not point at `out`, so after every byte it must write
`length` back to memory and load `data` and `length` again. A value read through a pointer is reloaded after each
store that might overwrite it. A plain local variable whose address never escapes cannot be overwritten that way,
so the compiler keeps it in a register.

The cure is the pattern this code base already uses in `decode_tokens`: copy the value into a local before the
loop, work on the local, and write it back once at the end.

## Asking the compiler

Clang reports these cases if asked. Two remark flags did all the searching:

- `-Rpass-missed=licm` reports each load that the compiler wanted to move out of a loop, because it reads the same
  address every time, but could not, because something in the loop might overwrite that address. The message is
  "failed to move load with loop-invariant address because the loop may invalidate its value". That is exactly the
  "value behind a pointer" problem.
- `-Rpass-missed=regalloc` reports, per loop, how many values the register allocator had to spill to the stack and
  load back.

```sh
clang -std=c11 -O3 -Iinclude -Isrc -c src/encode_block.c -o /dev/null -Rpass-missed=licm
```

**The decoder was clean.** `decode_literals` and `decode_tokens` produced no LICM remarks; earlier work (locals
instead of reads through `h` and `d`, the out-of-line `decode_token_slow`) had already done this job. Everything
the compiler flagged was in the encoders and in the LZVN code.

## The five places

### 1. The parse loop: options and block-writer fields

`parse` in `src/encode_internal.h` runs once for every input position. It read `options->skip_shift` on every miss
and `options->lazy_below` on every match. The finder's hash tables hold `uint32_t` values, and `skip_shift` is a
`uint32_t` too. C's type rules say stores of one type cannot change an object of an unrelated type, but these two
types are the same, so the compiler had to assume each table store might change the option. Both now live in
locals at the top of `parse`.

`block_writer_token`, called once per token, read each `block_writer` field, copied the literals (byte stores), and
then read some fields again, which forced fresh loads. It now reads every field once into a local before the copy.

### 2. The FSE bit writer

The block writer's two entropy-coding loops (four literals per step, then one token per step) end every step with
`bit_writer_flush`, which stored 8 bytes at `output->data + output->length` and then advanced `output->length`. That
is the byte-store problem again, so every step went through memory: store, then reload `data` and `length` for the
next step.

The writer now carries its own cursor:

```c
typedef struct bit_writer {
    uint64_t accumulator;
    uint32_t bit_count;
    uint8_t *next;   // where the next whole byte goes
} bit_writer;
```

`bit_writer_start` sets `next` from the buffer, `bit_writer_flush` advances it, and `bit_writer_finish` writes the
buffer's `length` once. The writer is a local variable whose address never escapes (every function is inlined),
so all three fields stay in registers.

### 3. The LZVN encoder's output

`emit_literals`, `emit_repeat` and `emit_token` wrote opcodes with `buffer_push_*_reserved(out, ...)`, and each push
reloaded `out->data` and `out->length`. They now take a `uint8_t *at` cursor and return the advanced one.
`lzvn_encode_with_table` sets `out->length` once at the end.

### 4. The LZVN decoder's careful path

When `decode_token_fast` declines an opcode (near the end of a buffer, or invalid), `decode_run` copied it with
`copy_bytes` and `copy_match_exact`, inline in the loop. Those are real calls to `memcpy`. A call may overwrite
the registers it does not promise to preserve, so any value the loop needs after the call has to sit in one of
the few registers a call preserves, or on the stack. The register allocator reported 6 reloads in that loop.

The copies moved to `decode_opcode_slow`, marked `COLD PRESERVE_REGISTERS` just as `decode_token_slow` is in
`decode.c` (see [the calling-conventions post](2026-11-14-calling-conventions.md)). `COLD` keeps it out of line and
tells the compiler the call is rare. `PRESERVE_REGISTERS` (Clang's `preserve_most`) makes the callee save almost
every register itself, so the loop does not have to. Afterwards the loop had no reloads at all. The error checks
run in the same order as before, so corrupt streams return the same status.

### 5. Clearing the small finder's buckets

`encode_small` fills its bucket table before every input of up to 64 KiB:

```c
for (size_t i = 0; i < bucket_count; i++) tables->buckets[i] = first;
```

Each bucket store might, as far as the compiler knows, change `tables->buckets`, so the pointer was loaded again
for every bucket (up to 8192 of them). A local pointer fixes it.

## The fix that made things slower

The register allocator's report for the parse loop was the worst in the library: in the large-input loop, 44 values
spilled and 134 reloads. One suspect was the call to `block_writer_flush`, which closes a block about once every
few thousand tokens. It sits inside the loop, so it looked like a textbook case for the same `COLD
PRESERVE_REGISTERS` treatment as the decoders' slow paths.

It made the encoder slower. We measured each attribute separately, in instructions and cycles relative to the
unchanged code (each figure is the median of 5 runs; an input's chunk size picks the path):

| Input | No attribute | `PRESERVE_REGISTERS` only | `COLD` only |
|---|---:|---:|---:|
| binary, whole (large finder) | −0.5% / +0.1% | +1.0% / +0.7% | +0.5% / +0.7% |
| noise, whole | −2.6% / +0.6% | −2.5% / 0.0% | −0.8% / +1.9% |
| binary, 64 KiB chunks (small finder) | −2.3% / −2.4% | −3.0% / −2.0% | −0.8% / +0.8% |
| noise, 64 KiB chunks | −3.6% / −0.1% | −3.9% / 0.0% | −1.7% / +3.4% |

With both attributes, an earlier run measured +1.5% instructions and +2.0% cycles on the whole binary input. `COLD`
alone is the worst for cycles; `PRESERVE_REGISTERS` alone wins some instructions on the small finder but loses on
the large one. Overall the plain call is best, so `block_writer_flush` stays a plain call, and a comment beside its
declaration says why.

We have not fully explained this. One clue: with `COLD`, the register allocator reported fewer spills in the
large-input loop (41 instead of 44) but a much higher *weighted* cost (reloads 3,916 instead of 1,284). The weight
reflects how often the compiler expects each block of code to run. Telling it that one branch is cold seems to
have moved spill code into blocks that run on every position. The lesson is the same either way: an attribute that
helped one loop is not automatically right for another, and only measuring can tell.

## Measurements

AGENTS.md asks for a measurement with every speed change. The library's wall-clock benchmark is too noisy for
changes of a few percent: two back-to-back rounds disagreed by up to 3%, and the LZFSE decoder, whose code did not
change, moved by ±2.5%. So the main measure is **instructions retired**, which macOS reports through `/usr/bin/time
-l` and which barely varies between runs. A small driver linked against the library encoded or decoded a file
repeatedly in chunks of a chosen size. That picks the path: chunks below 4 KiB use LZVN, 64 KiB chunks use the
small finder, and whole files over 64 KiB use the large finder. Inputs were a dictionary word list (text), system
binaries (binary) and random bytes (noise). Each figure is the median of 5 runs against the code before the change:

| Path | Fixes | Input | Instructions | Cycles |
|---|---|---|---:|---:|
| LZVN decode, 3 KB chunks | 4 | text | −6.7% | −1.3% |
| | | binary | −5.6% | −2.3% |
| LZVN encode, 3 KB chunks | 3 | text | −1.0% | −0.2% |
| | | binary | −0.8% | −1.0% |
| Encode, 64 KiB chunks | 1, 2, 5 | text | −2.1% | −0.8% |
| | | binary | −2.3% | −2.2% |
| | | noise | −3.7% | −0.9% |
| Encode, whole file | 1, 2 | text | −0.5% | +0.5% |
| | | binary | −0.5% | −0.4% |
| | | noise | −2.7% | +0.2% |
| Control: LZFSE decode, 64 KiB (unchanged code) | none | text | −0.5% | 0.0% |

The control row sets the noise floor: about half a percent of instructions, and about a percent of cycles. Without
the bit-writer change (fix 2) the 64 KiB encoder saved only 1.3–1.5% of instructions, so that change carries about
a third of the encoder's gain.

The effect is clearest where the benchmark's own small-files table can see it. Decoding the 199 files of 512 bytes
to 4 KiB (all LZVN) rose from 829–840 to 858–880 thousand files per second, in both rounds. The other rows of that
table, and the synthetic table, did not move beyond their noise.

The gains are modest, as expected. Reloading a value from the L1 cache costs a few cycles, and an out-of-order core
like the M1 often does other work in the meantime. The LZVN decoder gained most because its fix also took two
`memcpy` calls out of the loop, not just a few loads.

## What is left

The LICM report still flags the `block_writer` fields in `block_writer_token`: one load each per token instead of
two. Removing them would mean keeping the block writer's counts in locals across the whole parse loop and writing
them back around each flush. That is a larger restructuring, and at one load per field per token the likely gain
is small, so it is left for now.

## Checking it

`make all test test-alloc test-sanitize` passed, including the libcompression and kernel compatibility tests on
macOS. The unit tests for the bit writer were updated for `bit_writer_start` and the cursor-based flush. They still
round-trip 200 values of mixed widths, and 5000 FSE symbols, through the bit reader.

## Takeaways

- `register` is a no-op. To keep a value in a register, keep it in a local variable whose address does not escape,
  especially across byte stores (`uint8_t *`, `copy_bytes`), which may change any memory.
- `clang -Rpass-missed=licm` lists every load that stays in a loop because a store might change it. It found every
  case in this post in seconds.
- Moving a rarely called function's calls out of a hot loop (`COLD PRESERVE_REGISTERS`) helped both decoders. On
  the encoder's parse loop it hurt. Measure each one.
