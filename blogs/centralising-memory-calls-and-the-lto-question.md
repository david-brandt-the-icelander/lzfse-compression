# Centralising memory calls, and why LTO did not move the headers

*Notes from a working session on the C implementation of LZFSE in this repository.*

This post is a second pass over the ground in `memory-wrappers-and-bare-functions.md`: what was asked, what we found,
and what we decided.

## What was asked

1. Move `memcpy`, `memset` and similar calls behind one header, and forbid direct calls everywhere else.
2. Do the same for the `malloc`/`free` family.
3. Add `clear_bytes` for zero-fills and use it wherever memory is zeroed.
4. Move the `lzfse_zazzcat_buffer` implementation into its own `.h`/`.c`.
5. Build with link-time optimisation (LTO) and move header implementations into `.c` files where LTO makes that safe.
6. Remove LTO again.

## What we did

- `src/memory_operations.h` became the only file that includes the C library's memory functions. Wrappers:
  `copy_bytes`, `move_bytes`, `fill_bytes`, `clear_bytes`, `compare_bytes`, `raw_malloc`, `raw_calloc`,
  `raw_realloc`, `raw_free`. The library allocates through `zz_*`, which the allocation tests replace with counting
  versions; tests, tools, the benchmark and fuzz targets use `raw_*` so they never disturb those counts.
- Every `fill_bytes(x, 0, n)` became `clear_bytes(x, n)`. The two non-zero fills (`decode.c`, one unit test) stay.
- The buffer code moved to `src/buffer.h` and `src/buffer.c`; `common.c` keeps only the status strings.
- `AGENTS.md` gained a rule: no direct calls to those functions anywhere, wrappers first.
- A grep of `src`, `tests`, `bench`, `tools` and `fuzz` found no remaining direct calls, and
  `make all test test-alloc test-sanitize` passed.

## Findings

**Every function left in the headers is a hot-path `ALWAYS_INLINE` helper.** The bit reader and writer, the copy
helpers, the FSE step functions and the symbol lookups are inline for speed. `always_inline` needs the body visible
in the same translation unit; LTO can inline across files but does not guarantee it. The project rule is that a speed
change needs a measurement, and none was taken, so nothing moved. The cold candidates are `bit_writer_finish`,
`read_u32_at` and `div_ceil`; whether the hot ones can move without a loss is a question for the benchmark on an LTO
build.

**LTO was added and then removed.** `-flto` built and passed every test, but with no benchmark there was no reason to
keep it as the default. It is back to `CFLAGS ?= -O3`. The reason was caution about unmeasured speed, not distrust of
LTO itself.

**A hung command cost about ten minutes.** One shell command included a stray `cat >> /dev/null`, which waits for
standard input forever, so the first full test run never finished. A status monitor that matched its own process with
`pgrep -f` could not end either. Lesson: wait on a specific PID, and never leave a bare `cat` in a scripted command.

**Parallel work collided.** Another session landed the same refactor (plus a split into `memory_allocation.h`) on
`main` while this one was editing, so by commit time there was nothing left to commit. Check `git log origin/main`
before a large mechanical refactor.

## Result

The rule stands in `AGENTS.md`; the code is on `main`; LTO is off; moving header helpers to `.c` files waits for a
benchmark.
