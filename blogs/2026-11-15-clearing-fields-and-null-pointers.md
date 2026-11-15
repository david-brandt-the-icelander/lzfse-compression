# Clearing struct fields: the compiler merges the stores, and null is all zero bits

*2026-11-15 · lzfse-zazzcat · branch `null-pointer-test`. lzfse-zazzcat is an LZFSE compressor written from
scratch in C, with the goal of beating Apple's `libcompression` on Apple Silicon.*

A common pattern in the code base frees a pointer and then zeroes the struct that held it, one field at a time:

```c
zz_free(offsets->data);
offsets->data = NULL;
offsets->length = 0;
offsets->capacity = 0;
```

The question was whether places like this would be faster with one `clear_bytes` call (the project's `memset`
wrapper) or some other single clear. They would not. Along the way the audit raised a question C programmers
rarely ask: is a null pointer really all zero bits?

## Where fields are cleared together

A search for two or more consecutive field assignments of `0`, `NULL` or `false` found 11 places, in three
kinds:

| Kind | Places | Example |
|---|---|---|
| Every field of the struct | 3 | `lzfse_zazzcat_buffer_free`, `lzfse_zazzcat_offsets_free`, `small_finder_tables_free` |
| Some fields, not contiguous or not all of the struct | 5 | `block_writer_free` clears `tokens` and `literals`, which have `token_count` between them |
| Deliberately left uncleared | 3 | The decoder's 7 KB of FSE tables, and the encoder's `block_header` frequency tables |

The third kind is left over from an earlier change that dropped clears nothing reads. Clearing the whole struct
there would bring back a multi-kilobyte `memset`.

## The compiler already merges the stores

We compiled the three ways of writing the clear with clang `-O3` for arm64. Each one gives the same
instructions:

| Style | 24-byte `lzfse_zazzcat_buffer` | 32-byte `small_finder_tables` |
|---|---|---|
| One assignment per field | `stp xzr, xzr` + `str xzr` | `stp q0, q0` |
| `clear_bytes(b, sizeof *b)` | same | same |
| `*b = (type){0}` | same | same |

clang merges stores of zero to neighbouring fields into paired 64-bit stores (`stp xzr, xzr`), or into a pair
of 128-bit stores from a zeroed vector register. A `memset` of known small size is expanded the same way. Even
partial clears whose fields are not contiguous come out as three or four plain stores. All of these sites are
cold anyway: free functions, an out-of-memory path, and resets that run once per block.

**Verdict: no speed to gain.** What is worth having is maintainability. Every field list repeats the struct's
layout somewhere far from its definition, so a field added later can be missed. The plan is a small
always-inline helper for each clear, defined right beside the struct it clears: one that clears the whole
struct, and one per partial clear (`clean_foo_entirely` and `clean_foo_data_and_length`, say). Whoever changes the
struct then sees every clear that depends on it. That work will happen in a separate session.

## Is a null pointer all zero bits?

`clear_bytes` writes zero bytes, and so does `zz_calloc`. Code that zeroes a struct this way and then tests a
pointer field against `NULL` assumes that the bytes of a null pointer are all zero. The library already makes
this assumption: `lzfse_zazzcat_encoder_create` uses `zz_calloc` precisely so that every table pointer starts
out `NULL`.

The C standard does not promise it. A null pointer constant is written `0` or `NULL` in source code, but the
bits the compiler stores for it are up to the implementation. The comp.lang.c FAQ (question 5.17) lists old
machines that used something else, such as the Prime 50 series. Nothing built in the last few decades does.
Every engineer on this project would reasonably take zero bits to mean null.

The obvious place to state the assumption is a compile-time check:

```c
_Static_assert((uintptr_t)(void *)0 == 0, "null is all zero bits");
```

This doesn't work, for two reasons:

- **It isn't standard C11.** Converting a pointer to an integer is not allowed in an integer constant
  expression. clang accepts the line under the project's normal flags only as a GNU extension, and with
  `-pedantic` it fails: "folding it to a constant is a GNU extension".
- **It checks the wrong thing.** It tests the value that null converts to as an integer, not the bits stored
  in a pointer variable. On a machine where those differ, the check could pass while the assumption fails.

C11 has no way to look at an object's bytes in a constant expression, so the check is a unit test instead. It
runs first in `make test`, under the sanitizers too, and checks both directions:

```c
static void test_null_pointer_is_all_zero_bits(void) {
    void *null_pointer = NULL;
    unsigned char null_bytes[sizeof null_pointer];
    copy_bytes(null_bytes, &null_pointer, sizeof null_pointer);
    unsigned char zero_bytes[sizeof null_pointer] = {0};
    CHECK(equal_bytes(null_bytes, zero_bytes, sizeof null_bytes), "NULL is not stored as all zero bits");

    void *zeroed_pointer = &null_pointer;
    clear_bytes(&zeroed_pointer, sizeof zeroed_pointer);
    CHECK(zeroed_pointer == NULL, "a pointer cleared to zero bytes is not NULL");
}
```

## What the project supports

The test is a backstop, not a portability effort. lzfse-zazzcat targets Apple Silicon: the M1 and later, on
macOS. Linux is supported only so that the code can be built and tested in cloud sessions, which run on Linux.
No other platform is supported. The code may rely on anything both platforms share, and a null pointer stored
as zero bits is one of those things. `AGENTS.md` now says so, and names the test.

## One rule dropped

`AGENTS.md` used to ask that the code be written for a reader who is new to the code base and has not used C
for a while. That stopped being realistic long ago: the hot paths are full of wide unaligned stores,
wild copies and register-pressure workarounds that no beginner follows. The rule could also be read as
a reason to prefer a plainer but slower form. Performance comes first here, so the rule is gone. The rule that
every function, struct, enum and macro gets one short comment stays.
