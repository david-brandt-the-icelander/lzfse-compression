# Letting allocations stay too large

*2026-11-15 · lzfse-zazzcat*

`realloc` can make an allocation smaller as well as larger. We asked whether any of the library's `zz_realloc`
calls ever shrink one. If some did, we wanted to know whether the shrink was needed or whether the memory could
simply stay too large. The short answer is that none of them shrink, and all of them already let memory stay too
large on purpose. The more useful finding came from asking the opposite question: is there memory the library holds
on to that it ought to give back? There is one place, and it is a big one.

## Every call site grows, and only grows

The library allocates only through `zz_malloc`, `zz_calloc` and `zz_realloc` (see `src/memory_allocation.h`).
There are six places in the library that call `zz_realloc`:

| Call site | Grows when | New size | Largest it gets |
|---|---|---|---|
| `buffer_reserve` (`src/buffer.c`) | the free room is less than `extra` | the larger of twice the old capacity and `length + extra` (at least 64) | the caller's output |
| `offsets_reserve` (`src/parallel.c`) | the capacity is less than the requested count | the requested count | one `size_t` per chunk, plus one |
| `scratch_reserve`, task buffers (`src/parallel.c`) | there are fewer buffers than tasks | one buffer per task | 4 per thread |
| `scratch_reserve`, chunk ends (`src/parallel.c`) | there are fewer entries than chunks | one entry per chunk | one `size_t` per chunk |
| `decode_literals` (`src/decode.c`) | the buffer is smaller than this block's literals plus slack | the literal count rounded up to 4, plus `COPY_SLACK` | 40,032 bytes (`LITERALS_PER_BLOCK` + 32) |
| `encode_small` (`src/encode_small.c`), two tables | either table is smaller than this input needs | the needed count | 192 KiB of buckets, 32 KiB of long-match slots |
| `lzvn_encode_with_table` (`src/lzvn.c`) | the table is smaller than this input needs | the needed count | 2^14 entries, 64 KiB |

Each call sits behind a "capacity is too small" check, and each new size is at least the old capacity, so no call
can shrink. A shrink could still happen if some code lowered a capacity field while keeping the old pointer, so we
listed every assignment to a capacity field in `src/`. There are three kinds: right after a successful grow; set to
0 in the `*_free` functions, which free the pointer at the same moment; and set to 0 when a new decoder is created.
None of them lowers a capacity while the old pointer stays.

The same holds outside the library. The `raw_realloc` calls in `tools/file_io.h`, `bench/bench.c` and
`tests/apple_tests.c` all grow only.

## Why too large is the design

When a smaller input follows a large one, the code keeps the large allocation and uses only the front of it:

- `lzvn_encode_with_table` clears `entries` slots, not the whole capacity, and its hash function's shift is
  computed for `entries`, so it never indexes past them.
- `encode_small` fills `bucket_count` buckets and `long_count` slots and ignores the rest.
- `decode_literals` says it in a comment: "Only grow the buffer: every literal up to `count` is overwritten below
  and the slack is never output."

That is what the `AGENTS.md` rule "reused encoders and decoders must not allocate" needs. If any of these shrank,
a reused encoder that saw a large input and then a small one would allocate again on the next large one. The
small-files benchmark counts exactly that kind of churn.

For the per-object allocations the cost of staying too large is small and has an owner. The tables and the literal
buffer have fixed caps (the last column above), and they belong to an encoder or decoder that the caller frees with
`lzfse_zazzcat_encoder_free` or `lzfse_zazzcat_decoder_free`. The output buffer and offsets belong to the caller
too, and `lzfse_zazzcat_buffer_free` and `lzfse_zazzcat_offsets_free` release them.

So the answer to the question we started with is: none of the reallocations shrink, and none should.

## The one that has no owner

`lzfse_zazzcat_encode_chunks` is different. Its scratch arrays, `pool.scratch` in the threaded build and
`serial_scratch` in the `LZFSE_ZAZZCAT_NO_THREADS` build, are process-wide statics. Nothing ever frees them, and
the public API has no call that would.

The chunk-end array is small, 8 bytes per chunk. The task buffers are not. Before encoding, each task reserves
its worst-case output:

```c
// Room to reserve for encoding `length` bytes as `chunks` streams ...
static size_t task_bound(size_t length, size_t chunks) {
    return length + length / 4 + chunks * 4096;
}
```

Added up over all tasks, that is 1.25 times the input plus 4 KiB per chunk. After `gather` copies the streams into
the caller's `output`, the task buffers keep that whole capacity until the process exits.

We measured this with a small program linked against `build/liblzfse_zazzcat.a` from the default `make` build, on
an M1 Pro running macOS 27 with 10 pool threads. It encodes pseudo-random text in 64 KiB chunks, frees everything
the caller owns (the output, the offsets and the source), and then reads the heap's bytes in use with
`malloc_zone_statistics`:

| Input | Compressed output | Heap the library still holds | Expected from `task_bound` |
|---|---|---|---|
| 64 MiB | 35.1 MiB | 88.6 MiB | 84 MiB + 4.6 MiB of per-thread encoders |
| 512 MiB | 280.5 MiB | 676.8 MiB | 672 MiB + 4.6 MiB of per-thread encoders |

A later 8 MiB call left the number unchanged. The buffers are reused, never trimmed.

So one call on a 512 MiB file leaves the process holding more heap than the file itself, for the rest of its life.
A caller can't tell this from the API, which documents only that "repeated calls allocate nothing" once things
have warmed up.

Two details make the picture less bad and one makes it worse:

- **Not all of it is resident.** The heap counts allocated bytes. Large allocations come straight from the
  operating system as untouched pages, and the encoder writes only about as far as each task's compressed output
  gets. Most of the 25% headroom and the 4 KiB per chunk are probably never touched, so the resident cost is
  likely closer to the compressed size than to 677 MiB. We did not measure resident memory: the process
  footprint counter did not fall even when we freed the 512 MiB source, so it can't separate the library's pages
  from the allocator's cache.
- **Not all of it is lost.** A program that compresses many files of similar size gets exactly the reuse the
  design wants.
- **A slightly larger call can double it.** The task buffers grow through `buffer_reserve`, which grows to at least
  twice the old capacity. If the next call needs a little more than the last one, each task buffer jumps to twice
  its size, up to about 2.5 times that input.

## A smaller observation: `realloc` copies dead bytes

When `realloc` cannot grow a block in place, it copies the old contents to the new block. For most of these call
sites the old contents are already dead:

| Call site | Old contents needed? |
|---|---|
| `buffer_reserve` for the caller's output | yes |
| task buffers array | yes: each entry is a live buffer |
| task buffer data | no: `encode_task` sets `length` to 0 before reserving, but `realloc` copies the whole old block anyway |
| chunk ends, offsets | no: rewritten from the start |
| decoder literals | no: every literal is overwritten |
| `encode_small` tables, LZVN table | no: refilled or cleared right after |

Freeing and allocating again (`zz_free` then `zz_malloc`) would skip the copy. These grows are rare, since each
happens only when an input is larger than any before it, so we make no speed claim and changed nothing. It would
matter most for the task buffers, where the copy can be hundreds of MiB.

## What we took away

- **Check the guard, then check who else writes the capacity.** A "grow only" check stops shrinking only if
  nothing lowers the capacity field behind its back. Listing every assignment settled that in a minute.
- **Too large is fine when someone owns it.** The encoder's and decoder's buffers have fixed caps and are freed
  along with their object. Shrinking them would only bring back the allocations that reuse is meant to avoid.
- **Look for memory nobody can free.** The useful question wasn't "does anything shrink?" but "does anything hold
  memory the caller can't release?" Only the chunk encoder's process-wide pool does, and its holdings grow with
  the largest input ever passed in.

## Open items

- Decide what the chunk encoder should do with its task buffers. The options we see: a public call that releases
  the pool's scratch memory (keeps "warmed-up calls allocate nothing" and gives long-lived processes a way out);
  freeing task buffers after `gather` when they are above a size threshold (costs an allocation per large call);
  or documenting the behaviour in `include/lzfse_zazzcat.h` and leaving it.
- Measure the resident cost of the held task buffers, for example with `vmmap` on a process kept alive after the
  call, to replace the estimate above.
- Consider `zz_free` plus `zz_malloc` in place of `zz_realloc` where the old contents are dead, starting with the
  task buffers, with a measurement of a growing sequence of `encode_chunks` calls.
