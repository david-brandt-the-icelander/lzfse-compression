# Replacing a file without losing it: metadata, and files that only claim to be compressed

*2026-11-16. lzfse-zazzcat is an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

The last post ended with the reason this library exists: `free-disk-space`'s `compress` command, which stopped
working on macOS 27 because `ditto --hfsCompression` quietly writes uncompressed copies. That's now done:
`compress` makes every compressed copy itself, through the library's new `lzfse_zazzcat_decmpfs_apply`. This post
covers what came after: what compressing a file in place does to everything about the file that isn't its contents,
and what to do about files that say they are compressed when they aren't.

## How `compress` replaces a file

`compress` never writes into the original. For each file it:

1. clones the file into a temporary file next to it, with APFS `clonefile`;
2. compresses the original's contents with the library and puts the result on the clone: the compressed chunks in
   the `com.apple.ResourceFork` attribute, a header in `com.apple.decmpfs`, an empty data fork and the
   `UF_COMPRESSED` flag;
3. checks the clone against the original;
4. renames the clone over the original, which is atomic.

If any check fails, the temporary file is deleted and the original is untouched. So the question "does
`compress` keep a file's metadata?" is really "does step 3 check it?"

## What was checked, and what wasn't

Before this change, step 3 checked:

- the compressed flag and the space saved;
- the size;
- the mode bits, owner and group;
- the extended attributes' names (through `/usr/bin/xattr`);
- the contents, by SHA-256 read through the kernel;
- that the original hadn't changed meanwhile.

That left three gaps:

- **Attribute values.** A copy with the right attribute names but different values would have passed.
- **The ACL.** macOS files can carry an access control list on top of the mode bits. Nothing compared it.
- **The creation time.** The access and modification times were set back explicitly, but the birth time wasn't
  set or checked.

Some things change by design and can't be kept: the replacement is a new file, so its inode number differs.
Files with the setuid or setgid bit, or owned by someone else, already fail the mode and owner check safely,
because a clone made by an ordinary user can't keep those.

## Closing the gaps

A new module reads the metadata directly, with no subprocess:

- every extended attribute and its value (`listxattr` and `getxattr`), leaving out the two compression
  attributes, which are supposed to differ;
- the ACL as text (`acl_get_link_np` and `acl_to_text`);
- the birth time, which is set with `setattrlist` after the other times and then read back.

A copy whose values, ACL or creation time differ is rejected, with a note saying which one.

## What the macOS test found

The container these changes were written in runs Linux, which has none of these macOS calls. So a macOS test
gives a file an ACL, a tagged attribute and an old creation time, compresses it for real, and checks that all
three survive. It also checks that a copy which loses the ACL, or changes the attribute's value, is rejected.
The macOS CI run was the first time any of this code ran at all.

It found a real bug on the first try. The success case failed with "ACL differs": the ACL `clonefile` gives the
clone doesn't read back as the same text as the original's. Every file with an ACL would have been rejected.
Safely, since nothing was replaced, but such files would never have been compressed. `compress` now copies the
original's ACL onto the clone explicitly (`acl_set_fd_np`) before compressing it, and the test passes.

The lesson isn't new, but it keeps being true: a check that was never run against the real system is a guess.
Here the guess was "the clone carries the ACL". The test turned it into a fact, and the fact was different.

## Files that only claim to be compressed

A file counts as compressed when it has the `UF_COMPRESSED` flag. The kernel then ignores the data fork and serves
the contents from the compression attributes. `free-disk-space` used to skip any file with that flag: it was done.
But the flag is a claim, and three things can make it false:

| Case | What the new `repair` command does |
|---|---|
| The scan's state file records the file as compressed, but it has no flag (it was replaced or decompressed since) | Resets it to pending, so the next `compress` handles it |
| The flag is set, but the `com.apple.decmpfs` header is missing or invalid, says the wrong size, or the file can't be read | Reports it and records it as `broken`. Nothing readable is left, so it has to come back from a backup. Nothing is deleted |
| The flag is set and the file reads fine, but it stores as much as its contents | Rewrites it and compresses it for real |

`repair` is a dry run unless it gets `--apply`.

The third case needed a different copy step. Cloning a compressed file gives a compressed clone, and putting new
compressed data onto a file that is already compressed is not something to rely on. So `repair` writes a plain
copy: it reads the contents through the kernel, then copies the mode, group, attributes, ACL and the other flags
onto the copy. Then it compresses the copy with the library, and the copy goes through exactly the checks `compress`
uses before it replaces anything.

"Stores as much as its contents" is measured from the compression attributes themselves (the header plus the
resource fork), not from the allocated size. That keeps the decision independent of how APFS counts blocks for
compressed files.

## Building broken files on purpose

To test `repair` on macOS, the test has to make files the library would never write. It builds them with the same
four steps the library uses (set the attributes, empty the data fork, set the flag), but with chosen contents:

- **A file that saves nothing:** an LZVN resource fork in which every 64 KiB chunk is stored raw, behind the one-byte
  marker LZVN uses for raw chunks. It stores slightly more than the original.
- **A broken file:** a valid header and the flag, but no resource fork.
- **A falsely recorded file:** a plain file that the state file says is done.

macOS CI found two more things here:

- **The kernel hides the compression attributes.** On a file with the compressed flag, `getxattr` returns
  `com.apple.decmpfs` only when called with `XATTR_SHOWCOMPRESSION`. Without it, `repair` would have reported every
  compressed file on the disk as broken. The read now passes that flag.
- **Something else opened the new file.** `compress` skips files another process has open, using `lsof`. On the CI
  machine, a freshly written file was open in some other process, most likely Spotlight indexing it, so the
  rewrite was skipped. The other tests replace `lsof` with a stand-in; this one now does too. On a real disk the
  skip is the right behaviour, and the file is simply handled on a later run.

Once those were fixed, the kernel read the raw-chunk file correctly, `repair` classified it as saving nothing and
compressed it for real with its contents unchanged. Reading the file without a resource fork failed with
"Attribute not found", and `repair` recorded it as broken.

## A test that failed under load

The same pull request carried one unrelated fix. A test from another session's work, in the same repository,
failed intermittently in CI. Its stand-in HTTP server replied after a single `read` and closed the connection.
When the request hadn't fully arrived by then, closing with unread data made the kernel send a reset, which could
reach the client before the reply. Under CPU load the old server failed 5 runs in 40; reading the whole request
before replying brought that to none.

## What's still open

- **An end-to-end run on the owner's Mac.** CI runs on throwaway machines with small, made-up files. A sample
  `compress` run and a `repair` run on a real disk are the real test.
- **Other BSD flags.** Flags like "hidden" are copied, but not compared before the swap.

## What we learned

- **Ask what a replacement keeps, not only what it contains.** The contents were checked byte for byte from the
  start; three kinds of metadata weren't checked at all.
- **Test against the real system before trusting a guess about it.** Three things here behaved differently than
  expected: a clone's ACL, hidden compression attributes and a file opened by someone else. CI on a real Mac
  found all three before any user file was touched.
- **Treat a flag as a claim.** A compressed flag says what a file should be. `repair` checks what the file is.
