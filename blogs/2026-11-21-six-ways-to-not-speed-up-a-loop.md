# Six ways not to speed up a loop: out-pointers, `volatile`, and where the code sits

*2026-11-21 · lzfse-zazzcat · an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

This post is the record of one working session. It started as an audit of a C idiom (functions that hand results
back through a pointer) and ended with a finding about something no C declaration controls: where the compiler puts
the hot loop in memory. **Every code change in the session was measured and then reverted.** The library is exactly
as it was. What is worth keeping is the method, the numbers, and the one result that explains the others.

It is written as source material for the series' main articles. The last section lists the posts it could become
and the facts each one needs.

## The one-paragraph version

We inventoried every call that passes a pointer for the callee to fill, found that nearly all of them are already
free (inlined, or one store per call), and tried the few that were not: a NULL-tolerant out-value, a split function,
a different clock call, a flag in place of an out-value, a static variable for an ignored result, and two unusual C
attributes. Six edits were measured. Five touched the LZVN decoder's hot loop and slowed small-file decoding
by 1.6% to 5.4%; the sixth, in the encoder, could not be told from noise. The slowdown did not follow instruction count. A
change that **only moved the functions to 64-byte boundaries**, with identical instructions, cost 2.2% to 2.4%.
That is the evidence that this loop's speed on the small-file benchmark depends on its address, not just its code.

## Part 1: the inventory

The question: find every call that passes a pointer to an out-argument that the callee fills, and say which could
have returned the value normally, or been inlined so that the pointer is not needed.

The inventory was built by reading every `.c` and `.h` file. (No call-graph tool was used, and the `bare_*`
wrappers in `src/bare_functions.h` were counted at their call sites, not at their forwarding bodies.) It sorted
the calls into five groups:

| Group | Examples | Verdict |
|---|---|---|
| Scalar or struct results | `decode_run` (`bytes_read`, `output_end`), `parse_v1_header` and `parse_v2_header` (`header_size`), `read_u32_at`, `read_frequency`, `prepare_large_tables` (`bits_out`), `decode_tokens` (`position_io`), the public `decoded_length` | Mostly inlined already, or one store per call |
| Tables the callee fills | `fse_literal_table`, `fse_value_table`, `fse_normalize`, `fse_encoder_table`, `write_v2_fixed_fields`, `lzvn_decode_block` | Kilobytes written: a pointer is the only option |
| In/out state | `bit_reader`, `bit_writer`, the FSE `uint32_t *state`, `stream_stats` | Mutated in place; already inlined |
| System and libc | `fstat`, `lstat`, `uname`, `clock_gettime`, `pthread_*`, `getxattr`, `fread` | The pointer is part of the ABI |
| Append-style output buffers | `lzfse_zazzcat_encode`, `_decode`, `_lzvn_encode`, `_decmpfs_decompress` | Over 100 call sites; the buffer grows |

### What is already free

Checking the compiled code (`-O3 -mcpu=apple-m1`, looking for surviving `bl` instructions) showed that the
pointer is gone in most of the hot cases:

- Every `ALWAYS_INLINE` helper (`read_u32_at`, `load_u16_x3`, `fse_decode_value_from`, `fse_encode`,
  `bit_reader_init`, the `bit_writer_*` functions) turns `&x` into the caller's own register.
- `read_frequency` and `write_frequency` have external linkage, but the compiler inlined both into
  `parse_v2_header` and `write_v2_header`. The out-of-line copies exist only for unit tests.
- `header_at` and `prepare_large_tables` were inlined into their single callers and left no symbol.

### What a real call costs

A real call with an out-pointer costs one store and one load on the stack. For `parse_v1_header` and
`parse_v2_header` that happens once per block. `decode_tokens` is `NOINLINE`, runs once per block, and already keeps
`position` in a local, writing `*position_io` once at the end. This is the pattern the
[parameters as locals](2026-11-14-parameters-as-locals.md) post describes.

Two arguments against "just return it":

- **AArch64 returns structs of 16 bytes or less in `x0:x1`; anything larger goes through memory.** `decode_run`
  has two outputs plus a status. That is 24 bytes, so it would be returned through memory anyway.
- **The library's convention is that every function returns a status.** A `{status, length}` struct for
  `decoded_length` would fit in two registers and save about one store and one load per call. That is measurable
  only on tiny inputs, and it would break the convention.

### The short list of candidates

Four small changes looked plausible:

1. `lzfse_zazzcat_lzvn_decode` passes a dead `size_t used` to `decode_run`, which writes it for nothing.
2. `prepare_large_tables` computes a hash-bit count that depends only on `length` and `options`. It could be a
   separate function that returns `uint32_t`.
3. `bench/bench.c` calls `bare_clock_gettime(CLOCK_MONOTONIC, &t)`. On macOS `clock_gettime_nsec_np` returns a plain
   `uint64_t`.
4. `piece` in the benchmark could return a `{data, length}` struct.

Items 1 to 3 were implemented. Item 4 was a readability change and was not tried.

## Part 2: was the code the same?

The first rule the user set was: if the three changes (1 to 3) generate the same code, keep them; if not, abort.
That was a misreading, and the user corrected it: *if the code differs, benchmark it and keep the change only if the
benchmark improves.* That is also what `AGENTS.md` requires of any speed change.

Comparing assembly (`clang -O3 -std=c11 -mcpu=apple-m1 -S`, with labels and debug lines stripped):

| Change | Same code? | What differed |
|---|---|---|
| `decode_run` accepts NULL `bytes_read` (`src/lzvn.c`) | No | The compiler moved the `decode_opcode_slow` call block out of the opcode loop and renumbered the labels |
| `large_hash_bits` split (`src/encode.c`) | Nearly | Same instruction count; one `ldr` scheduled earlier and `x8`/`x9` swapped |
| `clock_gettime_nsec_np` (`bench/bench.c`) | Not compared | It changes the benchmark's own timer, not library code |

Dropping one stack slot and adding one branch was enough to rearrange the hot LZVN loop. That is the first sign of
what Part 5 is about.

## Part 3: the benchmark method

Every result in this post uses the same method, so the numbers can be compared:

- **Build both trees with identical flags.** Baseline from `git archive HEAD` into a scratch directory, new from the
  working tree: `make CPU_FLAGS=-mcpu=apple-m1 BUILD=...`. Before the last two rounds the baseline was rebuilt from
  scratch with the same flags, since its earlier flags were no longer certain.
- **Interleave, never run all of one build then all of the other.** Each round runs base, then new (in the final
  round: base, A, B). Rounds were 8 to 10 pairs of `build/.../lzfse-bench --small-files --runs 15`.
- **Compare medians and count pair wins.** Each row reports the median of the rounds and in how many rounds the new
  build was faster. A row that is slower in 0 of 10 pairs is not noise.
- **Compare only within a round set.** The baseline's 512 B to 4 KiB decode row read 898, 904.5 and 880.5 thousand
  files/s in different sets. Never compare absolute numbers across sets.
- **Run the checks first:** `make format-check` and `make all test test-alloc test-sanitize`.

The metric that matters is the decode throughput for files of 512 B to 4 KiB. That size range uses LZVN, and
the opcode loop in `decode_run` dominates it. Overall decode files/s hides it, because larger files use LZFSE
and move by less than 0.3%.

## Part 4: the results

### The two library edits from Part 2

- **NULL-tolerant `bytes_read`:** decode of 512 B to 4 KiB fell from about 898 to 873 thousand files/s (-2.9%), slower in
  7 of 8 pairs. Overall decode was within noise (-0.15%). The benchmark never executed the NULL call site (it
  decodes through `lzvn_decode_block`, which still passes a real pointer), so the check inside `decode_run` and the
  layout it caused are the only things that could have mattered.
- **`large_hash_bits` split:** the synthetic encode benchmark's 15 rows ranged from -1.2% to +1.4% with no consistent
  direction. The run-to-run noise is about 1%.

### Ignored out-values: static, thread-local, `volatile`

The user then asked about functions that ignore an out-value: what a `static` variable would cost against a stack
value, which are in hot loops, and whether `volatile` would help since nobody reads the value.

Only one such case exists in `src/`: the dead `used` in `lzfse_zazzcat_lzvn_decode`. The benchmark's `decode_share_main`
has an ignored `written`, and the test and fuzz harnesses have a few more. A 512-byte file takes about 260 ns to
decode, so `lzvn_decode` is hot only if a caller calls it for every small file.

Five variants of `lzvn.c`, measured as ns per `lzvn_decode` call (best of 3 runs, each the best of 9, on an M1 Pro):

| Variant | 512 B | 1 KiB | 4000 B | 512 B, 4 threads | 1 KiB, 4 threads |
|---|---:|---:|---:|---:|---:|
| stack local (now) | 267 | 731 | 3400 | 244 | 739 |
| `static` | 261 | 725 | 3400 | 239 | 723 |
| `_Thread_local` | 250 | 710 | 3321 | 244 | 695 |
| `volatile` stack | 264 | 772 | 3374 | 250 | 740 |
| `static volatile` | 262 | 767 | 3356 | 245 | 717 |

- **The code did shrink.** A stack local whose address escapes forces a frame in the wrapper (`sub sp`, `stp`,
  `add x29`, `bl`, `ldp`, `add sp`, `ret`). A static lets the compiler emit `adrp; add; b _decode_run`, a frameless
  tail call. That saves about five instructions, roughly 1 ns.
- **The timing did not follow.** Differences are 1% to 3% with no consistent direction. `_Thread_local` is the
  fastest, even though on macOS it goes through the `tlv_get_addr` thunk on every access. That is the sign that the
  spread is noise or layout.
- **`decode_run`'s machine code is byte-identical in the stack, static and volatile variants.** Only the wrapper
  changed. That is why these variants did not shift the loop.
- **`volatile` does not help.** Nothing reads the value, so there is no consistency to relax. It also doesn't make
  a data race defined in C11. If a shared static were ever wanted, a relaxed `_Atomic` store is the correct tool,
  and on arm64 it compiles to a plain `str`.
- **Why not a `static` anyway?** The user pointed out that the racy value is ignored, so a race on it is harmless in
  practice. That is true, but it is still formally undefined behaviour in C11, ThreadSanitizer would flag it, and the
  gain is about 1 ns. The rule held: no measured gain, no change.

### Removing the out-value with an `exact` flag

`decode_run` has two callers, both in `src/lzvn.c`. `lzfse_zazzcat_lzvn_decode` ignores `bytes_read`.
`lzvn_decode_block` (called from `src/decode.c`) uses it: `used != payload_length` enforces that the end-of-stream
opcode sits exactly at the payload's last byte. So the out-value cannot simply be deleted, but the check can move
into `decode_run`:

```c
case END_OF_STREAM:
    if (available < 8) return LZFSE_ZAZZCAT_TRUNCATED;
    if (exact && (available != 8 || position != output_capacity)) return LZFSE_ZAZZCAT_CORRUPT;
    *output_end = position;
    return LZFSE_ZAZZCAT_OK;
```

`lzvn_decode_block` becomes a single call and loses both post-call checks. This passed all tests and sanitizers,
and was measured in 10 interleaved pairs:

| Row | HEAD | exact flag | Change | Pairs where new was faster |
|---|---:|---:|---:|---:|
| Decode files/s, all | 25091 | 25042 | -0.20% | 4/10 |
| Decode 512 B to 4 KiB (k files/s) | 904.5 | 890.4 | -1.56% | 0/10 |
| Decode 4 to 32 KiB | 61.3 | 61.2 | -0.16% | 4/10 |
| Decode 32 to 256 KiB | 9.4 | 9.4 | -0.53% | 2/10 |
| Encode files/s, all | 5470 | 5451 | -0.35% | 2/10 |

The code is simpler and the small decode is 1.6% slower, in every pair. It was not kept. The generated code for this
variant was not inspected.

## Part 5: the "strange declarations" review

The user then asked for each unusual C declaration (such as `volatile`) to be reviewed for opportunities likely to
justify a benchmark. The review read every hot loop and the existing uses of `restrict`, `ALWAYS_INLINE`, `NOINLINE`,
`COLD`, `PRESERVE_REGISTERS` (Clang's `preserve_most`) and `LIKELY`/`UNLIKELY`. The code base already had most of the
useful ones, and the earlier [keeping values in registers](2026-11-14-keeping-values-in-registers.md) post had done
the same job for `register`.

| Declaration | Verdict | Evidence |
|---|---|---|
| `restrict` on `parse`, `block_writer_token` | Skip | Tried in a scratch copy: loop-invariant-load remarks (353 and 568) and instruction counts did not change. The remaining reloads of `block_writer` fields are blocked by the real `block_writer_flush` call, not by aliasing |
| `register` | No effect | Already shown in the registers post |
| `volatile`, `_Atomic`, `_Thread_local` | Skip | Measured above |
| `const`, `pure`, `nonnull`, `noescape`, `malloc`, `hot`, `flatten` | Skip | The hot code is `ALWAYS_INLINE`, so there is nothing for them to act on |
| `ALWAYS_INLINE` on `emit_token`, `emit_repeat` | Already done | No `bl _emit_*` in the LZVN encoder's assembly |
| `UNLIKELY` on error returns in `decode_run` | Weak | The compiler already lays them out of line: every `TRUNCATED` and `CORRUPT` branch goes forward to a shared exit. Only one `UNLIKELY` exists in the whole code base (`bits.h:103`) |
| `__builtin_prefetch` for the encoder's next bucket | Weak | About 4 extra instructions on a path of roughly 60 per position, and the tables fit in L2 |
| `alignas(32)` on `hash_bucket` | Weak | Only the default 14-bit table is certainly page-aligned. A fix needs an aligned allocator |
| Computed goto or `musttail` dispatch | Rewrite | `decode_run` dispatches through one shared `br x1`. Per-handler dispatch could help prediction, but M1's predictor already uses history |
| `__builtin_unreachable()` on the switch `default` | **Benchmark** | See below |
| `aligned(64)` on the decoder functions | **Benchmark** | See below |

### The one concrete finding

`decode_run` reads each opcode's kind from `opcode_kinds`, a `static const uint8_t` table whose entries are all in
0 to 10. The compiler cannot know that, so every opcode pays `cmp w15, #9; b.hi` before the jump table. Making
`UNDEFINED` an explicit `case` and the `default` unreachable removes that compare. In the assembly:

```text
before:  ldrb w15,[x24,x17]   cmp w15,#9   b.hi default   ...   br x1
after:   ldrb w15,[x24,x17]   mov w0,#-2   ...                    br x2   (no range check)
```

The compare and branch are gone and one `mov` is added. That is about one instruction fewer per opcode out of
roughly 25 to 30.

## Part 6: the result that explains the rest

Two variants were measured against a clean baseline, three-way interleaved, 10 rounds:

- **A:** `__attribute__((aligned(64)))` on `decode_run`, `decode_opcode_slow`, `decode_tokens`, `decode_literals`
  and `decode_token_slow` (the assembly confirms `.p2align 6`). No instruction changes.
- **B:** A plus the unreachable `default`.

The unreachable `default` on its own was also measured in an earlier round against HEAD (10 pairs of the same
method): 512 B to 4 KiB decode fell 2.6% to 2.8%, slower in 10 of 10 pairs, other rows within noise.

Decode in thousands of files per second (medians of 10 rounds):

| Row | Base | A | A change | A faster | B | B change | B faster |
|---|---:|---:|---:|---:|---:|---:|---:|
| 512 B to 4 KiB, reused | 904.5 | 882.5 | -2.44% | 0/10 | 861.5 | -4.75% | 0/10 |
| 512 B to 4 KiB, one-shot | 904.5 | 884.5 | -2.22% | 0/10 | 856.0 | -5.37% | 0/10 |
| 4 to 32 KiB, reused | 61.5 | 61.2 | -0.49% | 3/10 | 61.2 | -0.33% | 2/10 |
| 32 to 256 KiB, reused | 9.3 | 9.4 | +1.08% | 4/10 | 9.4 | +0.54% | 4/10 |
| All sizes, reused (files/s) | 25044 | 25080 | +0.15% | 6/10 | 24985 | -0.24% | 5/10 |

Encode rows are within noise for both variants.

### How to read it

- **Variant A changed nothing but addresses, and it cost 2.2% to 2.4% on the one row that exercises this loop.**
  Every earlier edit to `decode_run` also moved that row by 1.5% to 3%, in the same direction.
- **Variant B lost more than A.** So the unreachable `default` costs something beyond layout in this configuration.
  It saves an instruction and still loses. The compare it removes was perfectly predicted and fused with its
  branch, so it was close to free.
- **What is not shown.** The result does not say that the current layout is lucky, or that no alignment would win.
  It says only that 64-byte alignment is worse. An alignment sweep (none, 16, 32, 64, 128) on `decode_run` alone would
  show how wide the swing is. It has not been run. The link from layout to the 1.5% to 3% regressions is an
  inference: nothing in the generated code except addresses and block order differed, and a pure address change
  reproduces the size of the effect.
- **The practical consequence.** Hand-tuning the instruction count of `decode_run` is not productive without a way to
  control its layout. Each experiment is dominated by its address shift. Passing `AGENTS.md`'s rule needs a gain on
  the 512 B to 4 KiB row, and nothing tried here supplied one.

## Takeaways

- **A pointer parameter usually costs nothing already.** The compiler removes it when the callee inlines, and the
  rest cost one stack store per call. Audit with `-O3` assembly, not by reading signatures.
- **An escaping stack local gives the caller a frame; a static gives a frameless tail call.** That saved about five
  instructions (1 ns) and made no measurable difference.
- **`volatile` is the wrong tool for an ignored value, and a data race is undefined behaviour even when nobody reads
  it.** The right tool, if ever needed, is a relaxed `_Atomic` store.
- **A smaller instruction count can lose.** The edits in this session removed instructions and lost every time on the
  small-file decode row, by 1.6% to 5.4%.
- **Code placement is a variable.** A function-alignment attribute that changes no instruction cost 2.2% to 2.4%.
  Compare the same row, interleaved, with pair wins, and be suspicious of any single-digit change to a loop this
  small.
- **Keep the rule.** A speed change needs a measured gain. Six were rejected here, and the tree stayed at HEAD.

## Notes for the series author

### Posts this could become

1. **"Does the out-pointer cost anything?"** Parts 1 and 4's static/volatile section. Needs: the five-group
   inventory, the frame-versus-tail-call assembly, the variant table, and the C11 point on data races. It stands alone
   and is the most reusable.
2. **"The check that moved the loop."** Parts 2 and 4 (NULL check, exact flag). Needs: the two code snippets, the
   assembly observation (the `decode_opcode_slow` block moved out of the loop), the two benchmark tables, and the
   statement that the benchmark never executed the NULL path.
3. **"What `register`, `restrict` and friends can still do."** Part 5. Needs: the verdict table and the
   `restrict`-on-`parse` negative result. Link the earlier registers post.
4. **"When the address is the optimisation."** Part 6. Needs: variant A's table and the careful reading of it. This
   is the most surprising result and probably the best post. It should end with the open question: an alignment
   sweep.

### Reproducing the method

```sh
# baseline: HEAD in its own directory
git archive HEAD | tar -x -C "$SCRATCH/basetree"
(cd "$SCRATCH/basetree" && make CPU_FLAGS=-mcpu=apple-m1 BUILD=build build/lzfse-bench)
# candidate: the working tree
make CPU_FLAGS=-mcpu=apple-m1 BUILD=build/new build/new/lzfse-bench
# ten interleaved rounds
for i in 1 2 3 4 5 6 7 8 9 10; do
  "$SCRATCH/basetree/build/lzfse-bench" --small-files --runs 15 > base_$i.txt
  build/new/lzfse-bench --small-files --runs 15 > new_$i.txt
done
```

Parse the `lzfse-zazzcat` rows of the two tables, take medians per row and count rounds where the new value is higher.

### Caveats to carry into any post

- **Single machine.** Everything ran on one Apple M1 Pro under macOS 27. Layout effects are CPU- and
  linker-specific. Do not state the percentages as general.
- **Noise.** The earlier registers post puts the wall-clock benchmark's noise at about 1% to 3% between rounds,
  and some values looked quantized. Rows slower in 10 of 10 pairs are the ones to trust.
- **Not examined.** The assembly of the exact-flag variant, which loop block moved in variant A, an alignment sweep,
  and any non-Apple CPU.
- **Baselines differ between sets** (880.5, 898 and 904.5 thousand files/s for the same row), so always quote
  deltas, not absolutes.
- **Do not claim** that `__builtin_unreachable()` or alignment attributes "make code slower" in general. The claim
  supported here is narrower: on this loop, on this machine, on this benchmark, they lost.
- **Status:** all experiments were reverted. The tree and `HEAD` are identical, and nothing from this session is
  committed. Files such as `src/lzvn.c` and `src/encode.c` may be cited at their current line numbers, but those
  shift with any later edit.
