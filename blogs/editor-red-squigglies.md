# Red squiggles on correct code: missing include paths in the editor

*A short write-up of a debugging session on the C implementation of LZFSE in this repository.*

## The symptom

Opening `src/common.c` in VS Code showed red squiggles on two things:

- `#include "common.h"` on line 2.
- `lzfse_zazzcat_buffer` in the signature of `buffer_reserve` on line 4.

The file tab also showed a "9+" problem count.

## What we found

The code was fine. The Makefile builds everything with `-Iinclude -Isrc` (and `-Itools -Itests` for the tools and
tests). `common.h` includes `lzfse_zazzcat.h`, which lives in `include/` and defines the `lzfse_zazzcat_buffer` type.

The editor's C analyzer does not read the Makefile, so it had no include paths. It could not find
`lzfse_zazzcat.h`. That left the type undefined, and the error then showed up at both the include and the use of the
type. One missing header produced every squiggle in the file.

## The fix

We told the editor what the Makefile already told the compiler. Two small files, so it works with either C extension:

- `.clangd` for clangd: `-Iinclude -Isrc -Itools -Itests -std=gnu11 -pthread`.
- `.vscode/settings.json` for the Microsoft C/C++ extension: the same include paths and the `gnu11` standard.

After a window reload or a language-server restart the squiggles should clear. Only the file for the extension in use
is needed.

## Takeaway

When an editor flags correct code in a project with a custom build, first check whether it knows the include paths.
Keep the editor's include paths in step with the build flags. If the Makefile's `-I` list changes, update both files.
A `compile_commands.json` generated from the build would remove the duplication, but this project has no generator
for one yet.
