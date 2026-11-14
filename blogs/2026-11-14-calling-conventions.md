# Should we choose calling conventions by hand?

*14 November 2026. lzfse-zazzcat: an LZFSE implementation in C that aims to beat Apple's `libcompression` on
Apple Silicon.*

We asked a simple question: are we passing the right compiler options for calling conventions? And should some
functions name their calling convention directly, perhaps through a `#define` to cope with platform differences?

The short answer: for almost every function in the library, the compiler's default is right and we should leave
it alone. We found one exception. Changing the calling convention of one rarely called function made decoding
8–13% faster. While looking, we also found a hot loop that called the C library for every token. Fixing that made
small-input decoding more than 50% faster.

## The usual advice

The general advice is sound. With GCC or Clang at `-O2` or `-O3`, you rarely need to pick a calling convention.
The compiler already uses the platform's standard convention, places arguments in registers and inlines small
functions. Inlining removes a call entirely, which is worth more than any calling convention. The compiler cannot
change the convention of a function that other code can see, because separately compiled callers must be able to
call it. So the advice is to build with `-O3` (perhaps with link-time optimisation and profile-guided optimisation),
and to try calling-convention attributes only after profiling finds a hot call that cannot be inlined. The one
attribute it singles out is Clang's `preserve_most`, for small, frequently called functions that stay out of line.

## What the library did

Nothing: no calling-convention attributes and no related compiler options. The `Makefile` passes `-O3`, an
optional `CPU_FLAGS` (for example `-mcpu=apple-m1`) and no link-time optimisation. `src/common.h` defines only
inlining and branch hints:

```c
#define ALWAYS_INLINE static inline __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))
#define COLD __attribute__((cold, noinline))
#define LIKELY(condition)   __builtin_expect(!!(condition), 1)
#define UNLIKELY(condition) __builtin_expect(!!(condition), 0)
```

Almost every helper in the hot paths is `ALWAYS_INLINE`. A calling convention can only matter where a real call
is left in the machine code, so the useful question was not "which convention?" but "which calls survive?"

## Finding the calls that survive

We built the library with `-O3 -mcpu=apple-m1` and listed every branch-and-link instruction (`bl`, `blr`) in the
object files. Because the objects are not yet linked, we read the relocations to name each callee. Then we
checked whether each call sits inside a loop.

| Where | Calls inside the loop | How often they run |
|---|---|---|
| LZFSE decoder token loop (`decode_tokens`) | `decode_token_slow` | rarely: only tokens the fast path declines |
| LZFSE decoder, set-up | `__memcpy_chk`, stack-protector check | once per block |
| Encoder parse loops | `memcpy` (24 sites), `block_writer_flush` (24 sites) | `memcpy` only within 16 bytes of the input's end; flush once per block |
| LZVN decoder (`decode_run`) | `memcpy`, `memmove` | **every token** |

Most of these are harmless. The encoder's `memcpy` calls are in the loop body but come from an exact-copy fallback
that runs only at the very end of the input. Calls between source files happen once per block, so link-time
optimisation has little to gain. That leaves two calls worth a closer look: the decoder's slow path, and the LZVN
decoder's library calls.

## The slow path that slowed the fast path

`decode_token_slow` handles tokens the fast path declines: near the end of a buffer, or with an invalid distance.
It is marked `COLD`, so the compiler moves it out of the way, and a single call to it is cheap. But the call
still shapes the whole loop.

Under the standard 64-bit Arm calling convention, a called function may overwrite the "caller-saved" registers.
Any value the loop needs after the call must either live in one of the ten callee-saved registers (`x19`–`x28`) or
be spilled to the stack. The decoder's loop keeps a lot of state live: positions, limits, the bit reader and three
FSE decoder states. In the generated code, the fast path (the path taken by almost every token) did one spill
and four reloads per token.

The compiler was already doing the obvious thing well. The saves and restores needed around the call itself
were confined to the cold block. The per-token spills on the fast path came from register pressure across the
whole loop, which the call's register clobbers make worse.

`preserve_most` changes the contract: the callee, not the caller, saves almost all registers. According to
Clang's documentation, on 64-bit Arm it preserves every general-purpose register except `x0`–`x8` and
`x16`–`x18`. The register allocator then has many more registers that survive the call, and the loop needs fewer
spills. That is the right trade for a function that is rarely called: it pays a little more on each call, and the
loop pays less on every token.

With `preserve_most` on `decode_token_slow`:

- the fast path does 3 stack reloads per token instead of 1 spill and 4 reloads;
- the cold block does 4 stack accesses instead of 10.

### Making it portable

`preserve_most` is not portable, so it needs a macro:

- GCC does not have the attribute.
- Clang accepts it for 64-bit Arm and x86-64.
- Clang rejects it for 32-bit Arm with "calling convention is not supported for this target". The project builds
  with `-Werror`, so that warning breaks the build.

Checking `__has_attribute(preserve_most)` alone is therefore not enough, because the compiler can know the
attribute and still reject it for the target. The macro tests the compiler and the processor family:

```c
// Calling convention for a rarely called function that a hot loop calls: the callee saves almost every register,
// so the loop keeps its state in registers across the call. Only Clang on 64-bit Arm and x86-64 supports it (GCC
// lacks it, and Clang rejects it on other targets), so elsewhere this expands to nothing.
#if defined(__clang__) && (defined(__aarch64__) || defined(__x86_64__))
#define PRESERVE_REGISTERS __attribute__((preserve_most))
#else
#define PRESERVE_REGISTERS
#endif
```

`decode_token_slow` is now declared `static COLD PRESERVE_REGISTERS`. We use the macro nowhere else, because no
other call in a hot loop runs often enough to matter.

## The LZVN decoder's per-token library calls

LZVN is Apple's byte-oriented LZ77 format. The encoder uses it for inputs under 4096 bytes, and macOS uses it for
some transparently compressed files (decmpfs). Our LZVN decoder copied each token's literals with `copy_bytes` and
each match with `copy_match_exact`. Both wrap the C library's `memcpy` and `memmove`, with a length known only at
run time. That is a real function call, with a size dispatch inside it, for every token.

A calling convention cannot help here. `memcpy` belongs to the C library, and we cannot change its convention.
The fix is to stop calling it. The main decoder already has `decode_token_fast`: one combined bounds check per
token, then 16-byte (or, for short repeat distances, 8-byte) copies that may write a little past the end into
spare room that later output overwrites. It makes no calls. The LZVN decoder now runs each token through the same
routine, using the LZVN stream itself as the source of literals. If a token is too close to the end of either
buffer, or has an invalid distance, the decoder falls back to the old exact copies.

This changes one documented behaviour. Like the main LZFSE decoder, `lzfse_zazzcat_lzvn_decode` may now
overwrite bytes after the decoded length (never beyond the capacity the caller passes). The public header says so.

## Measurements

Single-thread decode throughput in MB/s on an Apple M1 Pro, using `lzfse-bench --synthetic --runs 11` built with
`-mcpu=apple-m1`. Each figure is the median of three runs per build, alternating the two builds.

| Input | Before | After | Change |
|---|---:|---:|---:|
| text, whole | 1474 | 1659 | +13% |
| text, 64 KiB chunks | 1382 | 1541 | +12% |
| binary, whole | 1077 | 1170 | +9% |
| binary, 64 KiB chunks | 1028 | 1109 | +8% |
| jsonl+base64, whole | 727 | 732 | flat |
| text, 4000-byte chunks | 417 | 659 | +58% |
| binary, 4000-byte chunks | 710 | 1082 | +52% |

The rows for whole inputs and 64 KiB chunks measure the `preserve_most` change, because only inputs under 4096
bytes are encoded as LZVN. The 4000-byte rows measure the LZVN change. Encoding speed did not change, and neither
did the near-incompressible inputs (whose blocks are stored raw).

There is still room to improve small inputs. At 4000-byte chunks Apple's decoder reaches 1210 MB/s on text and
1373 MB/s on binary, against our 659 and 1082.

## What we did not change

- **Link-time optimisation.** The calls between source files run once per block, so there is little for it to
  gain. It would also put compiler-specific bitcode inside the static library, so anyone linking it would need a
  compatible compiler and linker.
- **Profile-guided optimisation.** It might improve branch layout, but `LIKELY`, `UNLIKELY` and `COLD` already
  mark the known cases. It also needs a training run and stored profiles in the build. We may try it later, to
  chase the last few percent.
- **The stack protector.** Apple's Clang enables it by default. It costs a few instructions per block, not per
  token, and the decoder reads untrusted input. It stays.

## Verification

- `make format-check`, `make test`, `make test-alloc` and `make test-sanitize` (AddressSanitizer and
  UndefinedBehaviorSanitizer) pass on macOS.
- All four fuzz targets ran for 90 seconds each, about 39 million inputs in total, under AddressSanitizer and
  UndefinedBehaviorSanitizer, with no findings. One toolchain snag: Homebrew clang 21.1.2's AddressSanitizer hangs
  at start-up on this macOS, even for an empty fuzz target. We built the targets with Apple's clang
  (`-fsanitize=fuzzer-no-link,address,undefined`) and linked Homebrew's libFuzzer runtime and libc++.
- One caveat: continuous integration currently fails to build anything on Linux, because of an unrelated problem
  already on `main`. So the GCC build, where `PRESERVE_REGISTERS` expands to nothing, has not yet been checked
  by CI.

The change landed in pull request #11.

## Takeaways

1. Start with inlining. When the hot helpers are inlined, most calling-convention questions disappear.
2. To find where a convention could matter, list the calls left in the compiled code and check which ones sit
   inside loops.
3. A rarely taken call inside a hot loop can still cost every iteration, through the registers it takes away.
   `preserve_most` moves that cost onto the rare call.
4. A non-portable attribute needs a macro, and the macro must test the processor family as well as the
   compiler: `__has_attribute` can report an attribute that the target then rejects.
5. A call to the C library with a run-time length is not a calling-convention problem. Remove the call.
