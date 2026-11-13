# AGENTS.md

LZFSE (Apple's compression format) implemented from scratch in Rust, aiming to beat Apple's `libcompression` on Apple Silicon. Plan and format notes: `docs/DESIGN.md`.

## Rules
- Clean room: never copy or translate Apple's reference code (`lzfse/lzfse`). Consult it only for format facts, and say so in the commit message.
- `crates/lzfse` (package `lzfse-zazzcat`) is `no_std` + `alloc`; anything needing threads or the OS goes behind the default `std` feature.
- Public items get a short rustdoc comment; every private fn, struct and enum gets exactly one plain-sentence `//` comment.
- Decoding handles untrusted input: return `Error`, never panic, never index out of bounds. Keep `unsafe` out of the crate except in `kernels`, each block with a `// SAFETY:` comment and a safe reference kernel it is tested against.
- A speed change lands only with a benchmark showing the gain and the round-trip and compatibility tests passing.
- Never commit `target/`, corpora or benchmark inputs.

## Commands
- Before committing: `cargo fmt --all && cargo clippy --workspace --all-targets -- -D warnings && cargo test --workspace`
- `no_std` check: `cargo build -p lzfse-zazzcat --no-default-features`
- CLI: `cargo run --release -p lzfse-cli -- encode|decode|inspect <in> <out>`
- Benchmarks: `cargo run --release -p lzfse-bench -- --help` (compares against `libcompression` on macOS).
- Fuzzing (nightly): `cd fuzz && cargo +nightly fuzz run decode`
