# Finding the memcpy calls the compiler left behind

*2026-11-14. lzfse-zazzcat is an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

This session started from a familiar checklist of ways to make memory copies faster:

| Technique | Best use case |
|---|---|
| Compile-time constant size | Small copies, especially 8–128 bytes |
| Force inline memcpy | Frequent small copies |
| Avoid copying altogether | Large buffers, hot paths |
| Align source and destination | Repeated large or fixed-size copies |
| SIMD/NEON intrinsics | Specialized fixed-size copying |
| Copy in larger blocks | Custom bulk-copy routines |
| Non-temporal stores | Very large, one-way streaming copies |
| Multithreaded copying | Very large buffers, several MB or more |

The question was whether any of these apply to this code base. Most of the list had been covered already: the
[memcpy audit](2026-11-14-auditing-a-memcpy-tuning-plan.md) showed that the large-copy techniques have nothing
to work on here, and [the copy32 experiment](2026-11-14-trying-copy32.md) measured "copy in larger blocks" and
found it slower. That left the first two rows: copies whose size the compiler cannot see, which become calls into
the C library.

## Asking the object files

Every copy in the library goes through `copy_bytes` and its siblings in `src/memory_operations.h`. They are
`static inline`, so they disappear when the library is compiled. A copy with a constant size becomes plain load
and store instructions. A copy whose size is only known at run time becomes a call to libc's `memcpy`.

That gives a direct way to find the copies that matter for this part of the list: look for the calls that are
still there. In an object file, every call to an outside function carries a relocation, a note telling the linker
to fill in the address. Listing those relocations by the function they sit in takes one command:

```sh
objdump -d -r --no-show-raw-insn build/obj/lzvn.o |
  awk '/^[0-9a-f]+ <.*>:$/ {fn=$2} /ARM64_RELOC_BRANCH26.*_(memcpy|memmove|memset|bzero)$/ {print fn, $NF}' |
  sort | uniq -c
```

Run over every object file, it found these:

| Function | Calls | What they copy | Path |
|---|---:|---|---|
| `encode_small`, `lzfse_zazzcat_encoder_encode` | 29, 9 | literals within 16 bytes of the input's end (the inlined `block_writer_token`) | rare |
| `decode_token_slow` (`decode.c`), `decode_run` (`lzvn.c`) | 4, 3 | tokens the fast decoder declined | rare |
| `decode_literals`, `decode_tokens` | 2 | `bit_reader_init`'s 7 or 8 bytes (as `__memcpy_chk`) | once per block |
| `decode_with`, `decmpfs.c`, `parallel.c`, `encode_block.c` | a few | whole uncompressed blocks and chunks | large copies |
| `lzvn_encode_with_table` | 3 | every token's literals | **per token** |

All but the last are either rare or already large enough for libc's `memcpy` to be the right choice. The last one
is different. The LZVN encoder handles every input shorter than 4 KiB, which includes small files compressed by
APFS. It called `memcpy` with a variable size for the literals of every token it wrote.

## A dead end first

The slow decoder paths looked like a candidate too, because the benchmark shows our decoder well behind Apple's at
2 KiB chunks (641 MB/s against 1,018 MB/s on text). Small streams have proportionally more tokens near their end,
and those tokens skip the fast path and call `memcpy` and `memmove`. We replaced those calls with an inline byte
loop for copies of up to 32 bytes and measured. The result was between −3% and +3% for each input, which is
noise. Whatever makes small streams slow to decode, it is not these copies, so the change was dropped.

## The change

`emit_token` writes one LZVN match opcode. Up to three literal bytes ride along with the match, and the code
copied them like this:

```c
if (inline_count > 0) buffer_append_reserved(out, literals, inline_count);
```

`inline_count` is 1, 2 or 3, so the compiler emitted a call to `memcpy`, which then has to branch on a size of
1, 2 or 3. The new code always copies four bytes and keeps the ones it needs:

```c
if (inline_count > 0) {
    store_u32(out->data + out->length, load_u32(literals));
    out->length += inline_count;
}
```

`load_u32` and `store_u32` are constant-size copies, so this is one load and one store. Two things make it safe:

- **Reading four bytes stays inside the input.** The literals are followed by the match, which is at least
  three bytes long, so there are at least four readable bytes from the first literal.
- **Writing up to three extra bytes stays inside the output.** The encoder reserves 2 bytes of output per input
  byte, plus 64. Each opcode with its literals takes at most twice the input it covers, so the output written so
  far never passes twice the input consumed, and the extra bytes land inside the reservation. The next opcode, or
  at least the 8-byte end-of-stream marker, then overwrites them.

Longer literal runs go through `emit_literals`, in pieces of up to 271 bytes. Those now use `wild_copy`, the same
16-bytes-per-step copy the LZFSE encoder uses for its literals, whenever 16 bytes beyond the piece can still be
read. Near the end of the input they keep the exact `memcpy`. To know where the input ends, the two functions now
take a `readable` count.

The encoded bytes are exactly the same as before. Only the way they are copied changed.

## Measuring it

We built the baseline and the change with `make CPU_FLAGS=-mcpu=apple-m1` on an M1 Pro and ran the benchmark
five times each, alternating the two builds. Chunks of 1, 2 and 3 KiB go through the LZVN encoder; 64 KiB chunks
go through the LZFSE encoder and serve as a control. Encode speeds, as the median over the five rounds:

| Input | 1 KiB | 2 KiB | 3 KiB | 64 KiB (control) |
|---|---:|---:|---:|---:|
| text | +5.9% | +5.0% | +4.8% | −1.0% |
| binary | +3.6% | +3.0% | +3.9% | −0.7% |
| jsonl+base64 | −2.8% | 0.0% | −0.9% | −1.8% |
| noise | −1.6% | +0.8% | −0.3% | +0.2% |

For text and binary, the change's slowest round beat the baseline's fastest round at every small size. Those are
the inputs where the LZVN encoder finds matches with literals. Noise and jsonl+base64 find almost no matches, so
they are stored raw and barely run the changed code; their differences stay within the run-to-run spread. The
full table is in [the benchmark record](../docs/benchmarks/2026-11-14-owner-m1-pro-lzvn-literals.md).

One row looked alarming at first: decoding small jsonl+base64 chunks seemed up to 23% slower, although no decoder
code changed. Those chunks are stored raw, so decoding one is a single `memcpy` of a few KiB at around 17 GB/s.
Its timing jumped between two levels from round to round in both builds. To rule out a real effect, we compared
the decoder functions in the two binaries: they disassemble to identical instructions at identical addresses.
Noise at that speed is large, and the disassembly is a cheap way to tell noise from a real change.

The round-trip, allocation and sanitizer test suites pass. The fuzz targets could not be built on this Mac (the
libFuzzer runtime fails to link with both Apple's clang and Homebrew's), so they still need a run elsewhere.

## The rest of the list

- **Force inline:** already the case. The wrappers are `static inline`, and the hot copies (`copy8`, `copy16`,
  the `load_*`/`store_*` helpers) are marked always-inline with constant sizes.
- **Avoid copying altogether:** the decoders already write straight into the caller's buffer. The one avoidable
  large copy is the parallel encoder joining its per-task outputs (`gather` in `src/parallel.c`). We estimate it
  at under 1% of the time on typical data and about 5% on incompressible data, without having measured it, so it
  waits for a profile.
- **SIMD, alignment, non-temporal stores, multithreaded copying:** these are for large copies. The few large
  copies here already go to Apple's tuned `memcpy`, which decodes stored blocks at 34–41 GB/s on an M1 Pro. The
  benchmark's 10-thread decode of incompressible data is slower than one thread, which suggests that splitting
  copies across threads would not help either.
- **Copy in larger blocks:** measured in [the copy32 experiment](2026-11-14-trying-copy32.md); it lost 3–17%.

## Takeaways

- **The object file lists the copies the compiler could not simplify.** Constant-size copies vanish into loads
  and stores. The calls to `memcpy` that remain are exactly the variable-size copies, and the function each one
  sits in tells you whether it is on a hot path.
- **A small variable-size copy can often become a larger constant-size one.** If there is room to read and write
  a few bytes too many, copying a fixed 4 or 16 bytes is cheaper than asking libc to handle 1 to 3.
- **Write the safety argument down.** Copying past the end of the literals is safe only because of two facts
  elsewhere in the encoder: a match always follows, and the output reservation covers twice the input. The
  comments in the code now say so.
- **Not every libc call is a problem.** The decoder's slow path also calls `memcpy`, and replacing those calls
  made no difference. Measure each one.
