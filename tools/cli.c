// Command-line front end: encode, decode and inspect LZFSE streams.
//
//   lzfse-zazzcat encode <input> <output>
//   lzfse-zazzcat decode <input> <output>
//   lzfse-zazzcat inspect <input>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bare_functions.h"
#include "file_io.h"
#include "lzfse_zazzcat.h"
#include "memory_operations.h"

static const char usage[] = "usage: lzfse-zazzcat encode|decode <input> <output>\n"
                            "       lzfse-zazzcat inspect <input>\n";

// Read a little-endian 32-bit integer from `data + offset`.
static uint32_t read_le32(const uint8_t *data, size_t offset) {
    return (uint32_t)data[offset] | (uint32_t)data[offset + 1] << 8 | (uint32_t)data[offset + 2] << 16 |
           (uint32_t)data[offset + 3] << 24;
}

// Read a little-endian 64-bit integer from `data + offset`.
static uint64_t read_le64(const uint8_t *data, size_t offset) {
    return (uint64_t)read_le32(data, offset) | (uint64_t)read_le32(data, offset + 4) << 32;
}

// Print each block's offset, type and sizes. Returns 0, or 1 on a malformed stream.
static int inspect(const uint8_t *src, size_t length) {
    size_t at = 0;
    for (;;) {
        if (length < 4 || at > length - 4) {
            bare_fprintf(stderr, "truncated\n");
            return 1;
        }
        // Each block starts with 4 ASCII bytes: "bvx" and a type character.
        char name[5] = {(char)src[at], (char)src[at + 1], (char)src[at + 2], (char)src[at + 3], 0};
        uint64_t size;
        if (strcmp(name, "bvx$") == 0) {
            bare_printf("%10zu  bvx$  end of stream\n", at);
            return 0;
        } else if (strcmp(name, "bvx2") == 0 && length - at >= 32) {
            uint64_t word0 = read_le64(src, at + 8), word1 = read_le64(src, at + 16);
            uint32_t header = read_le32(src, at + 24);
            uint64_t payload = ((word0 >> 20) & 0xFFFFF) + ((word1 >> 40) & 0xFFFFF);
            bare_printf("%10zu  bvx2  raw %8u  literals %6u  matches %6u  header %u\n", at, read_le32(src, at + 4),
                        (unsigned)(word0 & 0xFFFFF), (unsigned)((word0 >> 40) & 0xFFFFF), header);
            size = header + payload;
        } else if (strcmp(name, "bvx-") == 0 && length - at >= 8) {
            size = 8 + (uint64_t)read_le32(src, at + 4);
            bare_printf("%10zu  bvx-  raw %8u  stored %8llu\n", at, read_le32(src, at + 4), (unsigned long long)size);
        } else if (strcmp(name, "bvxn") == 0 && length - at >= 12) {
            size = 12 + (uint64_t)read_le32(src, at + 8);
            bare_printf("%10zu  bvxn  raw %8u  stored %8llu\n", at, read_le32(src, at + 4), (unsigned long long)size);
        } else if (strcmp(name, "bvx1") == 0 && length - at >= 12) {
            size = 772 + (uint64_t)read_le32(src, at + 8);
            bare_printf("%10zu  bvx1  raw %8u  stored %8llu\n", at, read_le32(src, at + 4), (unsigned long long)size);
        } else {
            bare_fprintf(stderr, "unknown or truncated block %s at %zu\n", name, at);
            return 1;
        }
        if (size > length - at) {
            bare_fprintf(stderr, "truncated block at %zu\n", at);
            return 1;
        }
        at += (size_t)size;
    }
}

// Run one command; returns the process exit code.
int main(int argc, char **argv) {
    lzfse_zazzcat_buffer input = {0}, output = {0};
    int result = 1;
    if (argc == 4 && (strcmp(argv[1], "encode") == 0 || strcmp(argv[1], "decode") == 0)) {
        if (read_file(argv[2], &input) != 0) goto done;
        lzfse_zazzcat_status status = strcmp(argv[1], "encode") == 0
                                          ? lzfse_zazzcat_encode(input.data, input.length, NULL, &output)
                                          : lzfse_zazzcat_decode(input.data, input.length, SIZE_MAX, &output);
        if (status != LZFSE_ZAZZCAT_OK) {
            bare_fprintf(stderr, "%s: %s\n", argv[2], lzfse_zazzcat_status_string(status));
            goto done;
        }
        result = write_file(argv[3], output.data, output.length) == 0 ? 0 : 1;
    } else if (argc == 3 && strcmp(argv[1], "inspect") == 0) {
        if (read_file(argv[2], &input) != 0) goto done;
        result = inspect(input.data, input.length);
    } else {
        bare_fputs(usage, stderr);
    }
done:
    lzfse_zazzcat_buffer_free(&input);
    lzfse_zazzcat_buffer_free(&output);
    return result;
}
