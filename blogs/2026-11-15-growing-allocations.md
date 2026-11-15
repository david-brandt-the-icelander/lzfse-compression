# Growing allocations: doubling is free, and one realloc was not

*2026-11-15 · lzfse-zazzcat · branch `one-shot-lzvn-reserve`. lzfse-zazzcat is an LZFSE compressor written from
scratch in C, with the goal of beating Apple's `libcompression` on Apple Silicon.*

The question: for each `realloc` that grows an allocation, would it save CPU time to grow it in bigger steps, or
to start it bigger? The worry is the usual one. A buffer that starts tiny and doubles five times before it
settles pays for five calls and four copies, when one allocation of the final size would have done. The second
question came up after the first answer: if starting bigger leaves memory unused, can that unused memory cost CPU
time through memory pressure?

Short answers: only one call site was worth changing, and unused capacity costs nothing we could measure.

## Watching every realloc

Rather than reason about each call site, we logged them. A test program built the library with
`LZFSE_ZAZZCAT_COUNT_ALLOCATIONS` (the hook the allocation tests use) and recorded, for every `zz_realloc`, the
two calling functions, the old block size (`malloc_size`) and the new size, and whether the block moved. It ran
every entry point: one-shot encode and decode, a fresh encoder and decoder, LZVN, chunked encoding and decmpfs.
The inputs ranged from 300 bytes to 2.5 MB: source code, the 2.5 MB `words` list, the `zsh` binary, 1 MB of random
bytes and 1 MB of zeros.

Most call sites behave well:

| Call site | What the log showed |
|---|---|
| Decoder literal buffer (`decode_literals`) | At most two growths per stream, the second usually in place |
| LZVN hash table, small-input tables, chunk scratch arrays, offsets | Sized exactly for the input, grown only while warming up, then kept |
| `lzfse_zazzcat_decode` output | One allocation of the exact decoded size |

Three patterns stood out:

1. **A reused encoder writing into an empty buffer doubles all the way up.** For `words` the output buffer went
   88 KB → 176 KB → 353 KB → 706 KB → 1.4 MB, five reallocations. For random bytes it was six, four of which moved
   the block. This is exactly the "starts tiny and doubles" case.
2. **The one-shot encoder overshoots on incompressible input.** It reserves half the input, expecting the output
   to be smaller than that. Random data needs slightly more than the whole input, so one doubling is just short
   and a second one follows: 1 MB of random input ends in a 2 MB buffer, after two moves.
3. **Small one-shot encodes always reallocate once, straight away.** `lzfse_zazzcat_encode` reserved half the
   input. Inputs under 4096 bytes go to the LZVN encoder, which reserves its worst case of twice the input plus
   64 bytes. So every small one-shot encode allocated a buffer, then immediately moved it to a block four times
   larger before writing a single byte.

## Does the doubling cost anything?

We timed a warmed-up encoder writing into a fresh output buffer, which was freed after each call. The only
difference between the rows is how much was reserved first. Medians of 41 runs on an M1 Pro, in microseconds:

| Starting reservation | `words` 2.5 MB | `zsh` 2.0 MB | random 1 MB | source 382 KB | source 32 KB |
|---|---:|---:|---:|---:|---:|
| Empty (doubles up) | 23,574 | 17,624 | 2,140 | 3,246 | 184 |
| Half the input | 23,539 | 17,704 | 2,157 | 3,267 | 183 |
| Half the input + 64 KiB | 23,549 | 17,677 | 2,148 | 3,251 | 182 |
| Exact final size | 23,509 | 17,685 | 2,169 | 3,271 | 184 |
| A reused buffer | 23,514 | 17,695 | 2,146 | 3,283 | 182 |

Every row is within 1% of the others, and no row wins consistently. Even six reallocations and four moves of
up to 800 KB don't show. macOS serves allocations this large straight from the virtual memory system, so growing
one often extends it in place, and moving one is cheap next to compressing the data. Patterns 1 and 2 cost nothing,
so we left them alone.

## Does unused capacity cost anything?

Pattern 2 leaves a buffer twice the size it needs, and the LZVN encoder routinely holds two to five times its
output. CPU time is what we care about, but memory pressure can turn into CPU time: page faults, and in the worst
case swapping. So we measured what the unused capacity actually costs.

The test encoded the same input 40 times and kept all 40 results alive, as a program compressing a batch of
files would. It recorded how much the process's physical footprint grew and how many page faults each call took,
then ran again with every result shrunk to its exact length with `realloc`:

| Path and input | Written | Capacity | Footprint growth | Faults per call | 16 KB pages written per call |
|---|---:|---:|---:|---:|---:|
| One-shot, random 1 MB | 42 MB | 84 MB | 45 MB | 68 | 64 |
| … shrunk to fit | 42 MB | 42 MB | 52 MB | 80 | 64 |
| Reused encoder, `zsh` | 39 MB | 80 MB | 42 MB | 64 | 60 |
| … shrunk to fit | 39 MB | 39 MB | 51 MB | 78 | 60 |
| LZVN, `words` | 42 MB | 200 MB | 43 MB | 65 | 64 |
| … shrunk to fit | 42 MB | 42 MB | 43 MB | 65 | 64 |

Physical memory follows the bytes written, not the capacity. macOS gives a page real memory only when something
first writes to it, so the unused tail of a buffer is reserved address space and nothing more. It is never
faulted in, never resident and never swapped. Even LZVN's 4.8× capacity costs nothing. The page faults, which are
where memory turns into CPU time, track the pages written almost one for one.

We ran all 12 combinations of path (one-shot, reused encoder, LZVN) and input (random, `zsh`, `words`, the
382 KB source file). Shrinking to fit made no difference in six, made things worse in four (up to a third more
page faults and a larger footprint, the first two pairs above) and helped in two. Where it helped, the footprint
had grown 3–6 MB more than the bytes written across the 40 results: one-shot encodes of the source file, and the
reused encoder on `words`. Both of those runs grow or free large blocks on every call, so the extra is most
likely freed memory the allocator kept, not the unused tail of the results. We haven't chased it further. On
balance a shrink step would add a call and gain nothing.

Unused capacity would matter only where address space itself is limited, such as under a virtual memory limit or
on Linux with overcommit turned off. The project targets Apple Silicon on macOS, so neither applies.

**Verdict: leave the capacity alone.**

## Pattern 3: the one worth fixing

Pattern 3 is different from the others because the first reservation is never right: the LZVN encoder always
asks for more, and the buffer moves before anything is written. The fix makes the one-shot path ask for the LZVN
reservation in the first place. The bound now lives in one function, so the two places can't drift apart:

```c
// src/lzvn.h
static inline size_t lzvn_encode_bound(size_t source_length) {
    return 2 * source_length + 64;
}

// src/encode.c, lzfse_zazzcat_encode
size_t initial =
    length < LZFSE_ZAZZCAT_LZVN_THRESHOLD ? LZVN_HEADER_SIZE + lzvn_encode_bound(length) + 4 : length / 2 + 64;
```

The 12-byte LZVN block header and the 4-byte end-of-stream marker are included, so the whole small-input path
fits in the first allocation. Inputs of 4096 bytes and up keep the half-the-input reservation, which the
measurements above show is as good as anything else.

This uses no extra memory: the LZVN encoder took the same amount a moment later anyway. Small one-shot encodes now
make 3 allocations instead of 4, and the allocation test shows a 1000-byte encode at 3 allocations and 3272 bytes.

We timed it with the same test program linked against the library before and after the change. Each call
encodes into a fresh output buffer and frees it. Best of 7 rounds, averaged over two runs, in microseconds:

| Input | Before | After | Change |
|---|---:|---:|---:|
| 300 B | 0.707 | 0.639 | −10% |
| 1,259 B | 2.22 | 2.10 | −6% |
| 3,000 B | 5.06 | 4.96 | −2% |
| 4,095 B | 6.89 | 6.83 | within noise |
| 6,000 B (path not changed) | 28.80 | 28.82 | none |

That's about 40–70 ns saved per call, which matters most where the call itself is shortest. The repository's
`lzfse-bench --small-files` table doesn't move, as expected: it reuses output buffers, so the one-shot reservation
only happens on its first call.

## A loose end in chunked encoding

The log showed one more growth, which we did not change. Chunked encoding reserves each task's buffer with
`task_bound`: the input, plus a quarter, plus 4096 bytes per chunk, or 86 KB for one 64 KiB chunk. While a block
is being written, the block writer temporarily reserves 10 bits per literal plus 8 bytes per token, which can
reach about 130 KB for one block. So the first chunked call sometimes grows a task buffer once. Those buffers are
kept between calls, so steady state never pays this, but `task_bound`'s comment, which mentions only the literals,
undercounts what the block writer asks for. It is worth tidying the next time someone works in `parallel.c`.
