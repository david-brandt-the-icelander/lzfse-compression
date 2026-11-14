# `clear_bytes`: tuning notes

Shared rules and decisions: [README](README.md).

## Where it is called

| Kind | Sites | Size |
|---|---|---|
| Struct and header zeroing | `format.c`, `decode.c`, `encode.c`, `encode_block.c`, `decmpfs.c` | constant, inlined |
| FSE tables | `fse.c` | at most 1024 entries, once per block |
| Encoder tables, once per call | LZVN hash (`lzvn.c`), small-input long slots (`encode_small.c`) | sized to the input: about 1–2 bytes per input byte, up to 64 KiB and 32 KiB |
| Scratch growth | `parallel.c` | a few entries |

The large-input encoder (`encode.c`) does not clear its tables between calls. It tracks positions so that
entries left over from earlier inputs are recognized as stale.

## Assessment

The per-call clears are the only ones that grow with the input. For a 64 KiB input, clearing at memory speed
should take a few microseconds, compared with over 100 µs to encode the input. That estimate is not measured.
Apple's libc already clears with its fastest method (`dc zva`), so a backend could not gain anything. If the
clears ever matter, the fix is to clear less: tag table entries with a generation the way `encode.c` does.

## Steps

None planned. If a 64 KiB-chunk encode profile shows `_platform_bzero` or `_platform_memset` above the 5%
threshold, plan generation-tagged LZVN and small-input tables as an encoder change. That is not a change to
`memory_operations.h`.
