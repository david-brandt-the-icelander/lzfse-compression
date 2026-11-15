# One store per opcode: fewer instructions, the same speed, and two wide stores that lost

*2026-11-15 · lzfse-zazzcat · branch `wide-opcode-stores`. lzfse-zazzcat is an LZFSE compressor written from
scratch in C, with the goal of beating Apple's `libcompression` on Apple Silicon.*

This started with four lines in the LZVN encoder, which write a 3-byte match opcode:

```c
size_t word = (distance << 2) | ((first - 3) & 3);
at[0] = (uint8_t)(0xA0 | (literal_count << 3) | ((first - 3) >> 2));
at[1] = (uint8_t)word;
at[2] = (uint8_t)(word >> 8);
```

The question was whether it would be better to build the whole value in a register and write it with one 4-byte
store, something like `*(int32_t *)&at[0] = b`. We made that change, then searched the code base for the same
pattern and tried it in three more places. It landed in one place, and there it removed up to 1.7% of the
encoder's instructions without making it any faster. In two other places the wide version was slower, so we
reverted it.

## Not with a pointer cast

`*(int32_t *)&at[0] = b` is undefined behaviour in C, for two reasons:

- **Strict aliasing.** A `uint8_t *` (a character pointer) may point at any object, but the reverse is not
  allowed: bytes may not be accessed through an `int32_t` lvalue. The optimizer is entitled to assume this never
  happens, and it may reorder or drop the store.
- **Alignment.** `at` is a cursor into the output and is rarely a multiple of 4. Apple Silicon handles unaligned
  stores, but the compiler is still allowed to assume an `int32_t *` is aligned.

The project already has the legal version. `store_u32` in `src/common.h` is a `memcpy` of a constant 4 bytes,
which the compiler turns into a single unaligned `str` instruction:

```c
// Store a 32-bit little-endian integer.
ALWAYS_INLINE void store_u32(uint8_t *at, uint32_t value) {
    copy_bytes(at, &value, sizeof value);
}
```

It writes the integer in the machine's own byte order. The project only builds on little-endian machines, which
`src/common.h` checks at compile time, so the low byte of the value lands in `at[0]`.

## What the compiler did with the byte stores

An [earlier post](2026-11-14-merging-adjacent-stores.md) said the compiler "may also merge the three byte stores
into fewer, wider ones". At `-O2` and `-O3`, Apple Silicon clang does not. The branch compiles to three separate
byte stores:

```asm
strb w12, [x8]        ; at[0]
strb w11, [x8, #1]    ; at[1]
...
strb w11, [x8, #2]    ; at[2], shared with the large-distance branch
```

The compiler can't use one 4-byte store here, because that would write a byte the C code never asked it to
write. Doing so is only safe if you know something the compiler can't see: the buffer has room for the extra
byte, and the next output will overwrite it.

## Finding the same pattern elsewhere

A grep over every tracked `.c` and `.h` file looked for two shapes: values stored one byte at a time
(`at[1] = (uint8_t)...`, `*p++ = (uint8_t)...`, `buffer_push_reserved`), and values assembled from single-byte
loads (`data[i + 1] << 8`). The hits fell into four groups:

1. **LZVN encoder opcodes** (`src/lzvn.c`, the hot path): five sites that write 2- or 3-byte opcodes byte by byte.
2. **The LZFSE frequency-table header** (`src/format.c`, once per block): the writer pushes one byte at a time, and
   the reader assembles its accumulator one byte at a time. The reader parses untrusted input and must stop exactly
   at the end, so we tried only the writer.
3. **A struct field** (`src/encode_block.c`): three FSE symbols per token, stored as three bytes and loaded again as
   three bytes.
4. **Other hits:** test data generators; a byte-by-byte `read_le32` in the CLI's `inspect` command; and two
   helpers, `buffer_push_2_reserved` and `buffer_push_3_reserved`, that no code calls any more.

## Group 1: the LZVN opcodes

Each multi-byte opcode is now built as one little-endian value. The 2-byte opcodes use `store_u16`. The 3-byte
opcodes use `store_u32` and advance the cursor by 3:

```c
size_t word = (distance << 2) | ((first - 3) & 3);
size_t opcode = 0xA0 | (literal_count << 3) | ((first - 3) >> 2);
store_u32(at, (uint32_t)(opcode | word << 8));
at += 3;
```

The 4th byte is garbage. That is safe for the same reason as the literal copy a few lines further down, which
already writes 4 bytes and keeps 1 to 3. The encoder reserves 2 bytes of output per input byte plus 64. And
something always follows a match opcode: literals, another opcode, or at least the 8-byte end-of-stream trailer.
That later output overwrites the extra byte.

### The measurements

valgrind does not run on this Mac, so instead of callgrind we used the instruction counter on Apple Silicon.
`/usr/bin/time -l` prints "instructions retired" and "cycles elapsed" for a process. Whole-benchmark counts include
Apple's encoder and the thread pool, which made them vary by about 0.2%. So we wrote a 30-line driver that only
calls our encoder, on independent 4000-byte pieces (the LZVN path), 20 times over the input.

The baseline was a clean build of `main`, and every number came from three runs alternating the two builds:

| | Before | After | Change |
|---|---:|---:|---:|
| Instructions in `lzvn_encode_with_table` (objdump) | 433 | 415 | −18 |
| Stores in that function (byte stores) | 46 (20) | 38 (5) | −8 |
| Retired instructions, `/usr/share/dict/words` | 1,552.5 M | 1,526.3 M | **−1.7%** |
| Retired instructions, 8 MB of `/bin` binaries | 4,789.4 M | 4,754.2 M | **−0.7%** |
| Cycles, both inputs | | | within ±0.5% |

On text the encoder runs about 2,100 fewer instructions per 4000-byte piece. It does not get faster: the cycle counts move by less than they vary
between runs. `lzfse-bench --synthetic --chunk 1024 --chunk 4000` agrees. Over five alternating rounds of five
runs each, encode speed changed by −1.8% to +1.6% per input, inside the 1–6% spread between rounds of the same
build.

We did not profile further, but the likely reason is that the M1's wide out-of-order core runs the byte stores
in parallel with the match search, which is the slow part of the loop. Removing instructions that are not on the
critical path removes no time.

The output is byte-identical to `main` on 40 inputs: pieces of 100 to 4095 bytes from text, binaries and source
code, plus the whole files. The project's rule accepts instruction counts as the measurement for a small change,
so this group landed, and the commit message says that the timing did not change.

## Group 2: the frequency-table header

`write_v2_header` packs 360 variable-length codes, one per symbol frequency, and writes each finished byte with
`buffer_push_reserved`. That pushes through the buffer struct, so the compiler reloads `data` and `length` after
every byte. That was the problem the earlier post fixed in the LZVN encoder. The payloads already use a bit writer
(`src/bits.h`) that keeps its cursor in a register and flushes with one 8-byte store. The codes are a stream of the
same kind, forward with the least significant bit first. So the obvious change was to reuse that writer: push
every code and flush after every four (4 × 14 bits + 7 left over = 63 bits, which fits in 64).

Whole-encoder instruction counts on 64 KiB blocks didn't change beyond their ±0.05% run-to-run noise. A header
costs about 350 ns, roughly 0.2% of encoding its block. A microbenchmark that rewrites one real header a million
times showed the new version was slower:

| Header (from a 64 KiB block of) | Instructions per call | Time per call |
|---|---:|---:|
| text, 179 bytes | 6,961 → 7,654 (+10%) | 346 → 401 ns (+16%) |
| binaries, 205 bytes | 7,628 → 8,073 (+6%) | 390–425 → 427–431 ns |

Most of the 360 frequencies are zero or tiny and take 2 or 3 bits, so the old loop wrote only about 180 bytes in
total, with one cheap `strb` per finished byte. The bit writer adds overhead to every code: a test that skips
zero-length pushes, and every four codes a flush with a variable shift and an 8-byte store. That costs more than the
byte stores it replaces. We reverted the change.

## Group 3: three symbols in a struct

Each `token` holds three one-byte FSE symbols in `uint8_t symbols[3]`. The struct is 3 × `uint32_t` + 3 bytes, so
it has one byte of padding at the end. Making the array `symbols[4]` costs nothing in size and makes a 4-byte store
legal: one `store_u32` when the symbols are computed, and one `load_u32` when the token stream is written.

Static code size barely moved (648 → 650 instructions). Retired instructions went up by 0.30% on text and 0.17% on
binaries, with cycles unchanged. The read side explains it. `ldrb` loads a byte and zero-extends it in one
instruction, so three byte loads cost 3 instructions. One 4-byte load needs two more instructions to extract the
second and third bytes, and the low byte may need a mask too, so it costs 3 to 4. The write side comes out even:
three `strb`, against two `orr` and one `str`. So the wide version is worse by about one instruction per token. We
reverted it.

## Group 4: cleanup

- `buffer_push_2_reserved` and `buffer_push_3_reserved` were added for the LZVN encoder in PR #7. The encoder later
  moved to a local output cursor and stopped calling them. We deleted them. The comment explaining why hot loops
  avoid `buffer_push_reserved` moved to `buffer_push_reserved` itself.
- The CLI's `inspect` had its own byte-by-byte `read_le32` and `read_le64`. It now uses `load_u32` and `load_u64`
  from `common.h`, like the library, and prints exactly what it did before.

## Mistakes along the way

**A baseline that was not a baseline.** The first byte-identical check compared the new encoder with itself. The
baseline CLI was built after `src/lzvn.c` had been edited, in the same build directory as the baseline
benchmark, so `make` recompiled the library with the edit. A `git stash` and rebuild meant to undo that left
the new object file in place. The benchmark binary timed from that directory was genuine, because it had been
linked before the edit. We found the mistake because we counted `strb`
instructions in every binary we measured, and the "baseline" library had 5 instead of 20. Every comparison was
then rerun against a build of `main` in a separate `git worktree`. A
[previous post](2026-11-14-switches-and-ladders.md) records a similar mistake, a baseline built from a tree with
uncommitted changes. A clean worktree per baseline costs nothing and rules this out.

**The fuzzers did not run locally.** On this machine `clang` is Homebrew LLVM 21, whose libFuzzer expects
Homebrew's libc++. `make fuzz` linked against the system's libc++ and failed with an undefined
`std::__1::__hash_memory`. Adding Homebrew's libc++ to `FUZZ_FLAGS` fixed the link. The first target then spun for
ten hours inside AddressSanitizer's start-up code (`InitializeShadowMemory`, walking the dyld shared cache), before
it ever reached `main`. We noticed only when someone asked whether the work was still going. That runtime does not
work on macOS 27. The code was never reached, so this says nothing about the change. The checks that did run
locally are the sanitizer build of the test suite (`make test-sanitize`, Apple clang with AddressSanitizer and
UndefinedBehaviorSanitizer, round trips through every LZVN opcode form), which passed. The fuzz targets run in CI.
Two lessons: a 60-second job that hasn't reported back after a few minutes is broken, not slow; and a background
job needs a time limit of its own.

## Takeaways

- **Write a wide unaligned store with a constant-size `memcpy`** (`store_u32` here), never a pointer cast.
- **The compiler will not invent the wide store for you.** It may not write bytes the program didn't ask for. If
  your buffer has slack, a wider store is something only you can decide to do.
- **Fewer instructions is not the same as faster.** On an M1, three byte stores that were off the critical path
  cost no time. A −1.7% instruction count is still a real result, but call it that and not a speed-up.
- **Wide is not always cheaper.** A byte load zero-extends for free, and a wide load needs extracting afterwards.
  A loop that writes few bytes gains nothing from a writer built for long streams. Measure each site on its own.
- **Count the instruction you changed in the binary you measured.** It takes a second, and here it caught a
  baseline that wasn't one.
