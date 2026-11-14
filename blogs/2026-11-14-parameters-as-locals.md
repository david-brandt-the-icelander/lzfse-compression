# Letting parameters do the work of locals

*2026-11-14 · lzfse-zazzcat*

In C, a function's parameters are its own local copies of the arguments. A function may change them freely and the
caller never sees it. So when a function copies a parameter into a new local and never reads the parameter again,
the local is an extra name for the same value. Sometimes that second name helps the reader. Sometimes it is just
one more variable to track. This session searched the code base for both kinds, and for parameters that are never
used at all.

## Finding candidates

Two searches, one for each question.

**Unused parameters.** The compiler already knows which parameters are never read. Every `.c` file in `src`,
`tools`, `tests`, `bench` and `fuzz` was compiled with `clang -Wall -Wextra -Wunused-parameter -fsyntax-only`, and
nothing was reported. A parameter can also be hidden from that warning with a `(void)name;` cast, so the code was
searched for that pattern too. There was none. The code base has no unused parameters.

**Parameters copied into locals.** No compiler warning covers this, so a short script listed every local whose
initial value is a parameter (`size_t x = param;`), or is computed from a parameter that the function never reads
again (`uint8_t *out = output + position;`). That gave about a hundred lines, almost all noise: calls that merely
pass parameters on, conditions, struct fields. Three were real candidates.

## The three changes

### `emit_token` in `src/lzvn.c`

The LZVN encoder writes a token as up to three literals packed into the match opcode, or as a separate literal
opcode followed by the match. The function started like this:

```c
size_t inline_count = literal_count;
if (literal_count > 3) {
    emit_literals(out, literals, literal_count, readable);
    inline_count = 0;
}
...
size_t l = inline_count;
```

After the `if`, `literal_count` was never read again. `inline_count` and `l` were two more names for the same
number, and `l` went against the project's rule on descriptive names. Now the function sets `literal_count = 0`
when the literals go out separately and uses `literal_count` throughout. A one-line comment says that from that
point on it counts only the literals that ride along in the match opcode.

### `write_frequency` in `src/format.c`

```c
uint32_t write_frequency(uint16_t value, uint32_t *bit_count) {
    uint32_t v = value;
```

The copy existed only to widen the value. In C, arithmetic on a `uint16_t` first converts it to `int`, which is
signed. Doing the arithmetic in `uint32_t` keeps it unsigned. Declaring the parameter as `uint32_t` does the same
without the copy. Every caller passes a 16-bit frequency, which converts to `uint32_t` without any change in value,
so the callers stay as they were. The comment in `format.h` says why the parameter is wider than a frequency.

### `decode_token_fast` in `src/copy.h`

This is the decoders' hot path for one token. It checked the bounds and then did:

```c
const uint8_t *literal_source = literals + literal_position;
uint8_t *out = output + position;
```

and never touched `literals` or `output` again. It now moves the parameters themselves:

```c
literals += literal_position;
output += position;
```

Both parameters are `restrict` pointers. `restrict` is a promise that the two buffers do not overlap. Moving a
`restrict` pointer forward keeps that promise, because the moved pointer still points into the same buffer.

## What was left alone

Most of the copies the script found are there for a reason:

- **`void *` to `uint8_t *`.** Public functions such as `lzfse_zazzcat_encoder_encode` take `const void *`, and C
  does not allow pointer arithmetic on `void *`. The typed local (`const uint8_t *source = source_bytes;`) is needed.
  The match-finder callbacks (`large_finder *f = finder;`) take `void *` for the same kind of reason: one function
  pointer type has to fit every finder.
- **Copies made for speed.** `decode_tokens` reads `*position_io` into a local so the position stays in a register
  instead of being written back to memory after every token. `encode_task` copies `*buffer` onto its own thread's
  stack so threads do not keep taking the same cache line from each other (called "false sharing").
- **One cast instead of four.** `frequency_tables_of` makes one non-`const` copy of its header pointer, rather than
  casting each of the four table pointers.
- **Names that would mislead.** `match_length` could turn its `end` parameter into the remaining length, and
  `encode_task` could turn `first_chunk` into a byte offset. Either change would leave the parameter with a name
  that no longer describes what it holds.

## Checking it

AGENTS.md lets a speed change land only with a measurement. These changes are not meant to change speed, so the
check was stronger than a benchmark: the library was built before and after, and the disassembly of all 12 object
files was compared. It is identical, so the compiler produces exactly the same machine code for the old and new
source.

`make all test test-alloc test-sanitize` passed, including the libcompression and kernel compatibility tests on
macOS.

## Takeaway

Before reaching for a new local, check whether the parameter is still needed. If it is not, and the parameter's
name still describes the value, update the parameter itself and add a short comment where its meaning narrows. Keep
the local when it changes the type, keeps the original value, or keeps a name accurate. Comparing the disassembly
before and after is a quick way to show that such a cleanup changes nothing at run time.
