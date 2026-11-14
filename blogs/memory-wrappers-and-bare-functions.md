# Wrapping memory, taming ignored results: notes from one session

*A write-up of a working session on the C implementation of LZFSE in this repository.*

The session started as housekeeping and turned into three small design decisions. Each one is a rule now written
into `AGENTS.md`, so any agent or contributor follows it. This post records what we looked at, what we found, and why
we chose what we did.

## 1. One home for memory functions

The C library's memory functions (`memcpy`, `memmove`, `memset`, `memcmp`, `malloc`, `calloc`, `realloc`, `free`)
were already hidden behind wrappers in `src/memory_operations.h`: `copy_bytes`, `move_bytes`, `fill_bytes`,
`clear_bytes`, `compare_bytes`, and `raw_malloc`/`raw_calloc`/`raw_realloc`/`raw_free`. The library itself allocates
through `zz_malloc`/`zz_calloc`/`zz_realloc`, which the allocation tests swap for counting versions.

That file mixed two jobs, so we split it:

- `src/memory_operations.h`: byte-level operations only.
- `src/memory_allocation.h`: the `raw_*` and `zz_*` allocation functions.

Nothing changed in behaviour. The point is that each header has one subject, and the rule "only these two files call
the C library's memory functions" is easy to state and check.

## 2. Could CPU-specific instructions make the wrappers faster?

We looked at a common claim: that knowing the CPU lets you beat `memcpy` with `rep movsb`, AVX2/AVX-512, NEON, or
non-temporal stores. Those techniques are real, but they target large copies. This code base mostly does tiny ones.

What the code actually does:

- The hot copies are 8 or 16 bytes, through `copy8`, `copy16` and `wild_copy`.
- Match copies are short and may overlap.
- On Apple Silicon a constant-size `memcpy` already compiles to a single load and store (or a load-pair and
  store-pair), so there is nothing to beat.

So pasting vector or string-instruction assembly into the wrappers would make the hot paths slower. The places a
CPU-specific path could help are narrow: long non-overlapping `move_bytes` calls, literal and match copies of
32-256 bytes, and large buffer copies at the edges (joining parallel chunks, file I/O).

We wrote the result as a plan, not code (`docs/memory-operations/`), with these decisions:

1. **Measure first.** Profile the real copy-size distribution and the share of time spent copying. If copying is
   under about 5% of runtime, stop.
2. **Compile-time gating, no runtime dispatch in the header.** An indirect call on every small copy would cost more
   than it saves.
3. **Apple Silicon only.** Only arm64 on Apple platforms (NEON) gets specialised backends. Every other CPU uses the
   default libc-backed functions, because x86 backends cannot be benchmarked on the machine we develop on.
4. **Respect the slack contract.** `COPY_SLACK` (32 bytes of spare room after wide copies) may need to grow if
   copies get wider.

## 3. Auditing ignored return values

We asked which calls throw away a return value or out value. A text search is too noisy for this, so we used the
compiler: dump clang's AST for each source file and list every call that is a bare statement and returns something
other than `void`.

The findings, by group:

- **Library:** the pthread calls in `src/parallel.c` (lock, unlock, wait, signal, init, detach, once), and one
  `fse_decode_value` call in `src/stats.c` that is used only to advance state.
- **Public API statuses:** encode and decode calls whose `lzfse_zazzcat_status` is dropped, mostly in the benchmark,
  tests and fuzz targets. The fuzz and corrupt-input test cases do this on purpose. The benchmark is different: a
  failed compression would be timed as if it had worked.
- **`buffer_append` / `buffer_reserve`:** allocation failure unchecked, almost all in tests.
- **C library and POSIX:** `fclose`, `closedir`, `mkdir`, `unlink`, `clock_gettime`, `pclose`, `uname`, and the
  `printf` family.

The scan produced false positives (calls inside `if` conditions, a macro, a `void` function reported with the type of
its surrounding expression) that we filtered by hand.

## 4. The `bare_` rule

Marking each ignored call with `(void)` works, but it scatters a convention across the code base and does not say
whether a discard was a decision or an accident. Instead:

- `src/bare_functions.h` declares a `bare_<name>` wrapper for each function whose result callers drop. It has the
  same arguments, returns nothing, and does the one `(void)` discard inside.
- Callers that ignore a result call the `bare_` version. Nobody writes `(void)call()` elsewhere.
- A helper defined outside `src/` (a test or tool helper) gets its `bare_` version right beside it, because the
  shared header cannot depend on test files.
- A new function whose result callers drop gets its wrapper first.

The `printf` family needed varargs wrappers built on `vprintf`/`vfprintf`, with `format` attributes so the compiler
still checks format strings. The pthread wrappers are compiled out under `LZFSE_ZAZZCAT_NO_THREADS`.

A side benefit: the discards now sit in one file, so adding `warn_unused_result` to the real functions later is a
single, reviewable step.

## 5. Formatter and editor setup

The project already had clang-format, `make format`, `make format-check` and a CI check. We tightened the config
(explicit language, no tabs, right-aligned pointers, one blank line at most, final newline), added `.editorconfig`,
and set VS Code to format C files on save with the repository's `.clang-format`. One setting matters more than it
looks: `SortIncludes: Never`, because include order in this project is deliberate. All `.c` and `.h` files were
reformatted.

### A note on red squiggles

A new header showed a wall of errors in the editor even though the build was clean. The cause was the editor parsing
the header alone, as Objective-C++ (the default for `.h` on macOS), with no include paths, and relative `-I` flags
resolving against the wrong directory. Forcing C (`-xc`), using absolute include paths in `.clangd`, and mapping
`*.h` to C removed the errors. The `.clangd` file holds machine-specific paths, so it stays untracked.

## 6. Landing it

The work was committed on a branch and opened as a pull request. Another change had landed on `main` in the
meantime, so the merge had conflicts in `common.h` and `format.c`: the other change still called `memcpy` and
`memset` directly, which the wrapper rule forbids. We kept the wrapper versions, removed duplicated helper
definitions, re-ran the build, unit tests, allocation tests, sanitizer tests and format check, and then merged.

## Takeaways

- Wrap the C library once, and keep each wrapper header to one subject.
- Do not reach for CPU-specific copy code before measuring; for small, constant-size copies the compiler already
  emits the best instructions.
- Use the compiler's AST to find ignored results; text search cannot tell a used call from a discarded one.
- Make "I meant to ignore this" a named function, not a scattering of `(void)` casts.
- When the editor disagrees with the build, check how the editor is parsing the file before touching the code.
