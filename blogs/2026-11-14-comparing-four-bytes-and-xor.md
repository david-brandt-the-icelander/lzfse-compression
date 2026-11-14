# Comparing four bytes, and where XOR does (not) help

*2026-11-14 · lzfse-zazzcat*

This started with one line in the decmpfs header parser:

```c
if (xattr_length < DECMPFS_HEADER_SIZE || !equal_bytes(xattr, decmpfs_magic, 4)) return LZFSE_ZAZZCAT_CORRUPT;
```

`equal_bytes` is the project's wrapper around `memcmp`, and `decmpfs_magic` is the 4-byte string `"fpmc"`. The
question was what the fastest possible way to compare those four bytes is. That led to three more questions:
whether reading through a cast pointer is faster, whether XOR beats a plain comparison, and whether XOR would
speed up anything else in the code base. The answers were "no change", "no, and it is not valid C", "no" and
"no, and the one candidate measured slower".

All code below was compiled with clang at `-O3` and run on an Apple M1 Pro.

## The fastest 4-byte compare was already there

Before changing anything, the parser was compiled to assembly. The `memcmp` had already disappeared:

```
ldr  w8, [x0]                       ; load 4 bytes from xattr
mov  w9, #0x7066                    ; build the constant 0x636d7066 ("fpmc" read as a little-endian integer)
movk w9, #0x636d, lsl #16
cmp  w8, w9
b.eq ...
```

Clang knows what `memcmp` does. With a constant size of 4 and one argument that is a constant array, it replaces
the call with one 32-bit load and one compare against an immediate. That is the minimum: the bytes have to be
loaded, and they have to be compared.

The line was still rewritten to say this directly:

```c
// Compare the 4 magic bytes as one 32-bit integer: a single unaligned load and one compare against a
// constant (load_u32 of the static const magic folds to an immediate).
if (xattr_length < DECMPFS_HEADER_SIZE || load_u32(xattr) != load_u32(decmpfs_magic)) return LZFSE_ZAZZCAT_CORRUPT;
```

At `-O3` this produces the same three instructions; only the order of the two error-return blocks changed. The
difference shows up when the optimizer does less: at `-O0` the old line called `memcmp` and the new one does not.
The new line no longer depends on the compiler recognising `memcmp`.

`load_u32` (in `src/common.h`) copies 4 bytes into a `uint32_t` through the project's `copy_bytes` wrapper.
Comparing the two loaded integers gives the same answer as comparing the bytes, whatever the byte order, because
both sides are loaded the same way.

## Why not just cast the pointers?

The obvious shortcut is:

```c
*(const uint32_t *)xattr != *(const uint32_t *)decmpfs_magic
```

It compiles to exactly the same `ldr` / `cmp`, so it is not faster. It is also not valid C, for two reasons:

- **Alignment.** `xattr` can point anywhere in a buffer. C does not allow reading a `uint32_t` from an address
  that is not a multiple of 4. Apple Silicon handles such reads in hardware, but the compiler is allowed to
  assume they never happen, and `-fsanitize=alignment` reports them. The project requires the sanitizer build to
  stay clean.
- **Strict aliasing.** The data is `uint8_t` storage. C lets you read any object as bytes, but not read bytes as
  some other type. The optimizer may assume such reads do not happen and reorder or remove them.

Copying the bytes into a local integer with `memcpy` is the standard way around both rules. A constant-size copy
compiles to a single load, so it costs nothing. That is what `load_u32` does.

## Why not XOR?

The next idea was that, since only equality matters, `(a ^ b) != 0` might be cheaper than `a != b`.

Clang treats the two as the same expression. In its intermediate code, both become `icmp eq` before any
instruction is chosen, and both compile to the same `cmp` / `b.ne`.

Even written out in assembly, XOR would not win on ARM64. `cmp` is a subtraction that only sets the condition
flags, and it takes one cycle, the same as `eor` (XOR). ARM64 has no XOR that sets the flags, so the XOR version
needs `eor` followed by `cbnz` (branch if not zero): two instructions, the same as `cmp` and `b.ne`.

The line also has a second condition, `xattr_length < DECMPFS_HEADER_SIZE`. XOR cannot help merge the two:

- XOR detects equality only. It cannot express "less than".
- The order matters for safety. `||` skips the load when the attribute is shorter than 16 bytes. Merging the
  two checks into one branch means doing the load every time, which reads past the end of a short attribute.
  The decoder handles untrusted input and must never read out of bounds.

So the line has two well-predicted branches, which is the minimum for what it checks.

## Where XOR does help, and where it might

XOR is useful in two situations.

**Finding the first differing byte.** `match_length` in `src/copy.h` already does this. It XORs 8 bytes from each
side; if the result is not zero, the lowest set bit marks the first byte that differs, and `__builtin_ctzll`
(count trailing zero bits) divided by 8 gives its position:

```c
uint64_t difference = load_u64(buffer + earlier + n) ^ load_u64(buffer + later + n);
if (difference != 0) return n + (size_t)(__builtin_ctzll(difference) / 8);
```

Every match finder in the encoder spends most of its comparison time in this loop.

**Merging several equality checks into one branch.** `((a ^ b) | (c ^ d)) == 0` tests two equalities with one
branch instead of two. Compilers already do this for `memcmp(p, q, 16) == 0`.

A review of `src/` looked for both patterns.

### Paired equality checks: ARM64 already merges them

There are several conditions such as:

```c
if (distance == previous_distance && inline_count == 0)                                    // lzvn.c
bool lzfse = type == LZFSE_ZAZZCAT_DECMPFS_LZFSE_INLINE || type == LZFSE_ZAZZCAT_DECMPFS_LZFSE_FORK;  // decmpfs.c
if (bucket.values[k] != four || candidate >= position) continue;                           // encode_small.c
```

ARM64 has a conditional compare instruction, `ccmp`. It performs the second comparison only if the first one
came out a given way, and otherwise sets the flags to a chosen value. Two conditions then need one branch,
without XOR. Clang uses `ccmp` 30 times across `lzvn.c`, `encode_small.c`, `encode.c`, `decode.c` and
`decmpfs.c`. Several of these conditions also mix in `<` or `>=`, which XOR cannot express.

Checks that guard a load, such as `distance <= position && load_u64(src + position - distance) == eight`, cannot
be merged for the same reason as the decmpfs line: the first check is what makes the load safe.

The decoder's hot loop compares positions and lengths against limits, not data against data, so XOR has nothing
to do there.

### The one candidate: extending matches backward

After a match is found, the encoder tries to extend it backward over the literals before it.
`match_length_backward` does this one byte at a time:

```c
while (n < limit && n < earlier && buffer[earlier - 1 - n] == buffer[later - 1 - n]) n++;
```

This is the same job `match_length` does going forward, so the same trick applies in mirror image. Load the 8
bytes just before each position and XOR them. On a little-endian CPU the byte nearest the match is the most
significant, so `__builtin_clzll` (count leading zero bits) divided by 8 gives the number of equal bytes:

```c
if (limit > earlier) limit = earlier;
size_t n = 0;
while (n + 8 <= limit) {
    uint64_t difference = load_u64(buffer + earlier - 8 - n) ^ load_u64(buffer + later - 8 - n);
    if (difference != 0) return n + (size_t)(__builtin_clzll(difference) / 8);
    n += 8;
}
while (n < limit && buffer[earlier - 1 - n] == buffer[later - 1 - n]) n++;
return n;
```

Whether this pays off depends on how far matches usually extend backward. A counter was added to a scratch copy
of the encoder and run over the benchmark's built-in corpus: 28.7 million calls.

| Bytes extended | 0 | 1 | 2–8 | 9 or more |
|---|---:|---:|---:|---:|
| Share of calls | 68% | 29% | 3% | 0.1% |

The mean is 0.46 bytes. The byte loop almost always stops on its first or second comparison. The word version
has to clamp `limit` first and then do two 8-byte loads, an XOR, a test and a count, so it does more work in the
common case and saves work only in the rare long one.

It was built and benchmarked anyway: `lzfse-bench --runs 7`, two rounds, alternating between the two binaries.
The compressed sizes were identical, which confirms the new version finds the same matches. Encode throughput in
MB/s, one number per round:

| Input | Byte loop | XOR, 8 bytes at a time |
|---|---:|---:|
| binary, 64 KiB chunks | 145 / 144 | 138 / 136 |
| binary, 1 MiB chunks | 148 / 142 | 138 / 141 |
| binary, whole | 151 / 148 | 142 / 141 |
| text, 64 KiB chunks | 205 / 204 | 195 / 198 |
| zeros, 64 KiB chunks | 8542 / 8830 | 6820 / 7145 |

The other inputs (text in larger pieces, JSON lines with base64, noise, zeros in larger pieces) were within
noise. The XOR version was about 5% slower on binary-like data and up to 20% slower on zeros in 64 KiB chunks. It
was not adopted.

### Not worth trying

`match_length` ends with a byte loop for the last few bytes. It only runs when a match reaches the last 8 bytes
of the whole input, which happens about once per input.

## What changed in the code

Only the decmpfs line, from `!equal_bytes(xattr, decmpfs_magic, 4)` to
`load_u32(xattr) != load_u32(decmpfs_magic)`. It generates the same code at `-O3` and no longer relies on the
compiler recognising `memcmp`. Formatting, the unit tests, the comparisons with Apple's `libcompression` and the
kernel's decmpfs reader, and the sanitizer build all pass.

## Takeaways

- **Read the assembly first.** The "fastest possible" compare was already in the binary. Thirty seconds with
  `cc -S` answered a question that otherwise invites guessing.
- **Clever source and plain source often compile the same.** A pointer cast, `memcpy` into an integer, and
  `(a ^ b) != 0` all became the same instructions here. When they do, choose the one that is valid C and easy to
  read.
- **Know what the target already does.** XOR's classic use, merging two equality branches into one, is something
  ARM64 does by itself with `ccmp`.
- **Measure the input, not just the code.** The word-at-a-time backward extension looks like an obvious win next
  to the word-at-a-time forward one. A count of how far matches actually extend backward (usually zero bytes) explained
  why it was slower before any timing was needed.
