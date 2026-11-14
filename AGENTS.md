# AGENTS.md

LZFSE (Apple's compression format) implemented from scratch in C (C11), aiming to beat Apple's `libcompression` on Apple Silicon. Plan and format notes: `docs/DESIGN.md`.

## Rules
- Never copy or translate Apple's reference code (`lzfse/lzfse`, BSD-3-Clause). Consulting it for format facts and studying its encoder's design is allowed (owner's decision, 2026-11-14); say so in the commit message.
- Write for a reader who is new to the code base and has not used C for a while: descriptive identifiers (`literal_length`, not `l`), and short comments that say what something is for and explain non-obvious C idioms (unaligned loads through `memcpy`, `static inline`, wrap-around tricks).
- Every function, struct, enum and macro gets one short comment; public API comments live in `include/lzfse_zazzcat.h`.
- Public names start with `lzfse_zazzcat_` (macros `LZFSE_ZAZZCAT_`); internal names are plain and live in `src/` headers.
- Decoding handles untrusted input: return a status, never crash, never read or write out of bounds. Wide copies check their room once per token (`decode_token_fast`); anything rare goes to a cold, exact path. The sanitizer build (`make test-sanitize`) and the fuzz targets must stay clean.
- The library allocates only through `zz_malloc`/`zz_calloc`/`zz_realloc` (see `src/common.h`), so the allocation tests can count it; reused encoders and decoders must not allocate.
- A speed change lands only with a measurement showing the gain (the benchmark, or callgrind instruction counts for small changes) and with the round-trip and compatibility tests passing.
- Never commit `build/`, corpora or benchmark inputs.

## Commands
- Build: `make` (library `build/liblzfse_zazzcat.a`, CLI `build/lzfse-zazzcat`, benchmark `build/lzfse-bench`).
- Before committing: `make format && make all test test-alloc test-sanitize` (on macOS `make test` also runs the libcompression and kernel tests).
- No-threads build: `make DEFINES=-DLZFSE_ZAZZCAT_NO_THREADS`.
- CLI: `build/lzfse-zazzcat encode|decode <in> <out>`, `build/lzfse-zazzcat inspect <in>`.
- Benchmarks: `build/lzfse-bench --help` (compares against `libcompression` on macOS; `--stats` compares the encoders' choices). Tune with `make CPU_FLAGS=-mcpu=apple-m1`.
- Fuzzing (clang with libFuzzer): `make fuzz FUZZ_SECONDS=60`.
