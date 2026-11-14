# Merging repeated stores: which ones mattered, and which only looked better

*2026-11-14 · lzfse-zazzcat · [PR #7](https://github.com/david-brandt-the-icelander/lzfse-compression/pull/7)*

This started with three lines in the encoder:

```c
store_u32(output->data + mark, MAGIC_LZVN);
store_u32(output->data + mark + 4, (uint32_t)length);
store_u32(output->data + mark + 8, (uint32_t)payload);
```

The question was whether lines like these, the same operation repeated once per line, should become one call:

```c
store_u32_x3(output->data + mark, MAGIC_LZVN, (uint32_t)length, (uint32_t)payload);
```

and where else the code base does the same thing. It turned out that only one group of these runs could plausibly
make the code faster, and it was not the one in the example. Below are the search, the reason the groups differ,
and the measurements, which were less impressive than the generated code suggested.

## Finding the runs

A short script grouped consecutive lines that start by calling the same function. A grep over every
`load_*`, `store_*` and `buffer_push_*` call site then caught runs the script missed, such as
`h->field = load_u32(...)` assignments. The search found about 40 runs, which fell into three groups.

**Leave as they are.** These looked repetitive but had a reason to be separate calls:

- three `fse_decode_value_from` calls in the decoder's hot loop, which all read from the same bit reader and so
  must run in order;
- four `fse_encode` calls on four separate states, an interleaving written out by hand on purpose;
- one `fse_normalize` or `fse_encoder_table` call per FSE table, which could become a loop but would not get faster;
- `free` runs, thread setup, and test code.

**Cold header code.** The LZVN, raw and LZFSE v1/v2 block headers and the decmpfs header each wrote two to five
adjacent fields one call at a time. This code runs once per block or once per stream.

**Byte pushes in the LZVN encoder.** `emit_token`, `emit_literals` and `emit_repeat` write each 2- or 3-byte
opcode one byte at a time:

```c
buffer_push_reserved(out, (uint8_t)(0xA0 | (l << 3) | ((first - 3) >> 2)));
buffer_push_reserved(out, (uint8_t)word);
buffer_push_reserved(out, (uint8_t)(word >> 8));
```

These run for every token in every small input that LZVN encodes.

## Why the original example could not get faster

`store_u32` is an always-inlined `memcpy` with a constant size of 4. The compiler turns that into one `str`
instruction, and it can already pair neighbouring stores into an `stp` when it wants to. A `store_u32_x3`
built on a 12-byte `memcpy` compiles to roughly the same thing. In addition, that line runs once per stream. Merging it
makes the code shorter and makes the header layout easier to see, and that is all it does.

## Why the byte pushes could

This is the helper the LZVN encoder called:

```c
ALWAYS_INLINE void buffer_push_reserved(lzfse_zazzcat_buffer *buffer, uint8_t byte) {
    buffer->data[buffer->length++] = byte;
}
```

C allows a store through a `uint8_t *` (a character type) to modify any object in memory. That includes `buffer->data`
and `buffer->length`. After `buffer->data[...] = byte`, the compiler cannot prove that the buffer struct is
unchanged, so the next push has to load `data` and `length` from memory again and then store `length` again. Three
pushes in a row repeat that work three times.

The fix reads both fields once, writes the bytes through a local pointer, and stores the new length once:

```c
ALWAYS_INLINE void buffer_push_3_reserved(lzfse_zazzcat_buffer *buffer, uint8_t first, uint8_t second, uint8_t third) {
    size_t length = buffer->length;
    uint8_t *at = buffer->data + length;
    at[0] = first;
    at[1] = second;
    at[2] = third;
    buffer->length = length + 3;
}
```

`at` and `length` are locals whose address is never taken, so a byte store cannot change them. The compiler may also merge
the three byte stores into fewer, wider ones. The call site becomes one statement:

```c
buffer_push_3_reserved(out, (uint8_t)(0xA0 | (l << 3) | ((first - 3) >> 2)), (uint8_t)word,
                       (uint8_t)(word >> 8));
```

## What changed in the code

- `buffer_push_2_reserved` and `buffer_push_3_reserved` for the five byte-push runs in `src/lzvn.c`.
- `store_u16_x3`, `store_u32_x2`, `store_u32_x3`, `store_u64_x3` and `load_u16_x3` for the headers. Each one fills a small
  array and copies it with a single `memcpy`. On a little-endian CPU, which the project requires and checks at
  compile time, an array of integers has the same bytes as the integers stored one after another.
- `buffer_push_u32_x2_reserved` and `buffer_push_u64_reserved`, replacing a hand-written
  `store_u64(...); length += 8;` in the decmpfs header.
- In the v1 header writer, the literal padding field moved into the existing `fields[]` loop instead of being
  written by a separate call after it.

One run was left alone on purpose: the `load_u32` calls in `parse_v1_header`. They fill fields with different
names, one goes into a local variable and one needs a sign cast, so a merged version had more lines than the original.

## The measurements

Both versions were built from scratch with identical flags (`-mcpu=apple-m1`) on an M1 Pro. The only difference
between them was the LZVN byte pushes.

The generated code for `lzvn_encode_with_table`, the function that `emit_token` is inlined into:

| | Instructions | Loads | Stores |
|---|---:|---:|---:|
| Before | 452 | 63 | 58 |
| After | 437 | 58 | 52 |

Encode throughput from `lzfse-bench --synthetic --chunk 1024 --chunk 4000`, in four rounds of five runs each,
alternating between the two binaries (MB/s):

| Input | 1 KiB chunks | 3 KiB chunks |
|---|---:|---:|
| zeros | 1281 → 1318 (+2.9%) | 1296 → 1330 (+2.7%) |
| text | 264 → 267 (+1.1%) | 302 → 306 (+1.2%) |
| binary | 298 → 300 (+0.7%) | 320 → 319 (−0.4%) |
| jsonl+base64 | 245 → 241 (−1.5%) | 321 → 320 (−0.3%) |
| noise | 248 → 240 (−2.9%) | 332 → 331 (−0.5%) |

Individual rounds varied by about ±3%. Only zeros improved in every round, and it is the input where the
encoder spends the largest share of its time emitting match opcodes. Noise has almost no matches, so it barely
runs the changed code, and its −2.9% shows how large the noise is.

The project's rule is that a speed change lands only with a measurement that shows the gain. This one shows a
gain only on zeros. The code really does less work, with five fewer loads and six fewer stores in the encoder loop,
but on everything except zeros the gain is too small to separate from noise. The change was merged as a cleanup
whose effect on speed is small, and the commit message says so.

## Getting it merged

When the commit was made, the working tree also held a larger uncommitted refactor that moved the buffer helpers
into their own header and replaced `memcpy` with project wrappers. To commit only these changes, the edits were
ported onto a branch made from `main` in a separate worktree, with the helpers placed where `main` keeps them. The
full test suite passed there, including the comparisons with Apple's `libcompression` and the kernel's decmpfs reader.

One mistake: the PR was merged while CI was still pending, because the repository has no branch protection and
the merge was allowed immediately. All six checks (tests on macOS and Ubuntu, fuzzing, lint, the benchmark and
the reference check) passed afterwards, but the order was wrong. Waiting for the checks before merging would have
cost nothing.

## Takeaways

- **Repetition alone does not make code slow.** A constant-size `memcpy` is already a single instruction, so merging
  several of them into one call mostly makes the code easier to read.
- **Look at the byte stores.** Writing through `uint8_t *` (or `char *`) may modify anything in memory, including the
  struct that holds the pointer and length, so the compiler reloads them after each write. Keeping the pointer
  and length in local variables avoids that.
- **Look at the generated code before timing.** An instruction count is exact and takes seconds, while a timing difference
  of a few percent was smaller than the noise between rounds here.
- **Report results that are mostly noise as they are.** "Five fewer loads, speed change within noise except on
  zeros" is a useful finding. Calling it a speed-up would not be accurate.
