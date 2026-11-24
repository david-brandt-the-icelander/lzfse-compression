# `.zazzle/zizd/`: lzfse-compression's zizd pipeline

The steps zazzle's builders run for this repository, written against zazzle's step-pipeline spec
(`src/macos/zazzle-pipeline/pipeline-spec.md`, sections 1–4). One pipeline, `default`, so each step is a folder
`.zazzle/zizd/STEP/` with a `Dockerfile` and a `run.sh`. The pipeline only checks: there is nothing to deploy, so it has
no deploy phase. The pipeline file itself lives in the orchestrator (`continuous_integration_and_delivery`,
`.zazzle/zizd/pipelines/david-brandt-the-icelander/lzfse-compression/default.json`); the copy below is what goes there.

| Step | What it does (CI job in `.github/workflows/ci.yml`) | Leaves in `/zazzle/output` |
|---|---|---|
| `lint` | `make format-check`; warnings-as-errors builds with gcc, clang and `-DLZFSE_ZAZZCAT_NO_THREADS`; `shellcheck scripts/*.sh` (`lint`) | `lzfse-zazzcat-linux-ARCH.tar.gz`: the gcc build's `lib/liblzfse_zazzcat.a`, `include/lzfse_zazzcat.h`, `bin/lzfse-zazzcat`, `bin/lzfse-bench` and `BUILD` (commit and tool versions) |
| `test` | `make test`, `make test-alloc`, `make test-sanitize CC=gcc` (`test`, its Linux leg) | nothing |
| `reference` | `scripts/compat-reference.sh` between the `lint` step's CLI and Apple's reference `lzfse`, built into the image at a pinned commit (`reference`) | nothing |
| `fuzz` | `make fuzz FUZZ_SECONDS=30`: each of the four libFuzzer targets for **30 seconds**, about two minutes of fuzzing after a one-minute build (`fuzz`, which runs 60 seconds each). On a crash the log shows a hex dump of the input, since a failed step's output is not uploaded. | nothing |

All four images share one toolchain layer: `ubuntu:noble-20261110` with apt pinned to the snapshot `20261115T000000Z`
(gcc 13.3.0, clang, clang-format and libclang-rt 18.1.3, shellcheck 0.9.0, the compilers of GitHub's `ubuntu-24.04`
runners). The `reference` image adds `lzfse/lzfse` at `e634ca58b4821d9f3d560cdc6df5dec02ffc93fd` (CI takes its head).
Every build tree is in the container's `/tmp`.

Every `FROM` names its image by tag and by digest (the multi-arch index), so a rebuilt image starts from the same
bytes on any builder.

## The pipeline file

```json
{
  "id": "david-brandt-the-icelander/lzfse-compression:default",
  "repos": ["https://github.com/david-brandt-the-icelander/lzfse-compression.git"],
  "paths": ["**", "!blogs/**", "!docs/**", "!*.md"],
  "steps": [
    { "name": "lint", "path": ".zazzle/zizd/lint" },
    { "name": "test", "path": ".zazzle/zizd/test" },
    { "name": "reference", "path": ".zazzle/zizd/reference" },
    { "name": "fuzz", "path": ".zazzle/zizd/fuzz" }
  ],
  "supersede": true
}
```

No `deploys` step and no `environments`: nothing here is released. `paths` leaves out the blog posts, the design notes and
the Markdown at the root, which no step reads.

No step sets `platform`, so they run on the builder's own architecture (tried on linux/arm64; GitHub's runners are
x86_64). The tarball's name says which.

## Config keys

None. The repository is public and no step publishes anything. The builder's own `GITHUB_TOKEN` and z3 keys
(`shared.json`) are zazzle's and reach no step. Nothing goes in `deploy_keys`.

## What is not here yet

- **macOS:** the `test` job's `macos-latest` leg (which adds the Apple interoperability tests: libcompression both ways,
  ratio, system files and kernel reads) and `.github/workflows/bench.yml` (the benchmark against libcompression on Apple
  Silicon) need macOS VM steps (Tart), which come later.
- **Branch coverage** (`scripts/branch_coverage.sh`) is not in CI today, so it is not a step.

## Trying it by hand

Each step was run with the spec's "Trying a step by hand" recipe (`--cpus 2 --memory 4g --pids-limit 4096`, the host's
uid, the repository read-only), each step's output copied into the later steps' `/zazzle/input/artifacts/STEP/`.

## What goes at cutover

Not yet removed: `.github/workflows/ci.yml` (its `lint`, `reference` and `fuzz` jobs and the Ubuntu leg of `test` are
these steps). `.github/workflows/bench.yml` and the macOS leg of `test` stay until macOS VM steps exist. There is no
legacy runner for this repository in `continuous_integration_and_delivery`.
