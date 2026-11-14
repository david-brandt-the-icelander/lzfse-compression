# CPU tuning of `src/memory_operations.h`

Status: proposed, nothing implemented. One document per wrapper:

| Wrapper | Library call sites | Hot? | Plan |
|---|---|---|---|
| [`copy_bytes`](copy_bytes.md) | constant-size loads/stores, literal copies, large block and chunk copies | constant sizes yes; large copies at the edges | measure the large copies; probably leave to libc |
| [`move_bytes`](move_bytes.md) | `copy_match_exact` only | cold path | switch the call to `copy_bytes`; no backend |
| [`fill_bytes`](fill_bytes.md) | decoder slow path, one-byte runs | rare | none |
| [`clear_bytes`](clear_bytes.md) | struct and header zeroing, per-call table resets | small share | none; clear less, not faster |
| [`compare_bytes`](compare_bytes.md) | one 4-byte magic check | no | none |

## Scope

Large-copy advice (REP MOVSB, AVX-512, non-temporal stores) does not fit this codebase. Its hot copies are 8 or
16 bytes, and with a constant size `copy_bytes` already compiles to one `ldr`/`str` or `ldp`/`stp`. The decoder's
32–256 B literal and match copies are `copy16` loops in `decode_token_fast` (`src/copy.h`) and never reach these
wrappers. Widening those loops is a `copy.h` change and is not covered here.

## Decisions

- Only Apple Silicon (`__aarch64__ && __APPLE__`) gets CPU-specific backends. Every other target keeps the libc
  calls, because x86 backends (AVX2, AVX-512, ERMS) cannot be measured on the owner's M1 Pro.
- The wrapper signatures stay. A call with a constant `count` keeps going to libc so the compiler inlines it.
- Nothing is dispatched at runtime in the header, because that would put an indirect call on every small copy.
  If dispatch is ever needed, it goes in a `.c` file on the large-copy path only.
- Intrinsics and inline assembly live only in `memory_operations.h`.

## Rules that apply to every wrapper

1. **Measure first.** Profile `build/lzfse-bench` with Instruments (`xcrun xctrace record --template 'Time
   Profiler' --launch -- build/lzfse-bench ...`). The wrappers are `static inline`, so they never show up by name.
   Look for libc's `_platform_memmove` / `_platform_memset` / `_platform_bzero` (variable sizes), and attribute
   them to callers. If a wrapper's share is under about 5% of runtime, stop there and record the number.
2. **Micro-benchmark before writing a backend.** Add a `bench/` program that compares the candidate with libc for
   the sizes the profile shows, cache-hot and cache-cold, aligned and unaligned, with the results consumed so
   the copies cannot be optimized away.
3. **Verify.** Run `make format && make all test test-alloc test-sanitize` and the fuzz targets, plus unit tests
   for the changed wrapper: every size from 0 to 1024 at every alignment from 0 to 15, and overlapping cases
   where the wrapper allows them.
4. **Land only a measured win** (AGENTS.md). Record the before and after numbers in `docs/benchmarks/`. If a
   backend lands, list it and its feature macro in the header comment.
