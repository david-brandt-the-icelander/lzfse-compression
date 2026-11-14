# `compare_bytes`: tuning notes

Shared rules and decisions: [README](README.md).

## Where it is called

Its one library caller is the 4-byte magic check in `decmpfs.c`. All other callers are tests, the benchmark's
correctness gate and the fuzz targets. The encoders find matches with `match_length` in `src/copy.h`, which XORs
8 bytes at a time and does not call `compare_bytes`.

## Assessment

It is not on any hot path, so no work is planned.

If match finding ever needs a faster compare, change `match_length` (for example, 16-byte NEON compares). That
is a `copy.h` change.
