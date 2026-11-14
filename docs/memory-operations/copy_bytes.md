# `copy_bytes`: tuning notes

Shared rules and decisions: [README](README.md).

## Where it is called

| Kind | Sites | Size |
|---|---|---|
| Unaligned integer access | `load_u*`/`store_u*` (`common.h`), `copy8`/`copy16` (`copy.h`) | constant 2–16 B, inlined |
| Short exact copies | `bit_reader_init` (`bits.h`), decoder and LZVN literals (`decode.c` slow path, `lzvn.c`), encoder literals near the input end (`encode_internal.h`) | 0 to a few hundred B |
| Large copies | `gather` in `parallel.c` (joining task outputs), `push_uncompressed` (`encode.c`), uncompressed blocks in `decode.c`, raw decmpfs chunks (`decmpfs.c`, ≤ 64 KiB) | KiB to MiB |

## Assessment

- **Constant sizes:** these are already single loads and stores, and a backend could only make them worse. They
  must keep calling `memcpy`.
- **Short variable sizes:** these sit on paths that are cold or rare (the fast decoder handles nearly every
  token with `copy16` loops). They are unlikely to show up in a profile.
- **Large copies:** on macOS, `memcpy` is libsystem's `_platform_memmove`, which Apple tunes for its own cores.
  A hand-written NEON loop is unlikely to beat it. The noise rows in
  [the M1 Pro benchmark](../benchmarks/2026-11-14-owner-m1-pro-c.md) already decode uncompressed blocks at about
  34 GB/s, which is a single `copy_bytes`.

## Steps

1. Profile the parallel encoder and decoder on inputs of 64 MiB or more. Measure the share of time spent in
   `gather` and in uncompressed-block copies.
2. If `gather` matters, avoiding the copy (letting tasks write into the final buffer) is a bigger lever than
   copying faster. Plan that separately.
3. Try a NEON or non-temporal (`stnp`) path only if a micro-benchmark shows libc losing at some size. The effect
   of `stnp` on Apple cores is unverified.

Expected outcome: no change.
