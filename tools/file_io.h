// Whole-file reading and writing for the command-line tools, the benchmark and the tests.
#ifndef LZFSE_ZAZZCAT_FILE_IO_H
#define LZFSE_ZAZZCAT_FILE_IO_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bare_functions.h"
#include "lzfse_zazzcat.h"
#include "memory_allocation.h"
#include "memory_operations.h"

// Read the whole file at `path` into `out` (replacing its contents). Returns 0, or -1 with an error printed.
static inline int read_file(const char *path, lzfse_zazzcat_buffer *out) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        bare_fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    out->length = 0;
    for (;;) {
        if (out->capacity - out->length < 65536) {
            size_t capacity = out->capacity < 65536 ? 65536 : out->capacity * 2;
            unsigned char *grown = raw_realloc(out->data, capacity);
            if (grown == NULL) {
                bare_fprintf(stderr, "%s: out of memory\n", path);
                bare_fclose(file);
                return -1;
            }
            out->data = grown;
            out->capacity = capacity;
        }
        size_t got = fread(out->data + out->length, 1, out->capacity - out->length, file);
        out->length += got;
        if (got == 0) break;
    }
    int failed = ferror(file);
    bare_fclose(file);
    if (failed) {
        bare_fprintf(stderr, "%s: read error\n", path);
        return -1;
    }
    return 0;
}

// Write `length` bytes to the file at `path`. Returns 0, or -1 with an error printed.
static inline int write_file(const char *path, const void *data, size_t length) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        bare_fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    // fwrite must not be given a NULL pointer, even for 0 bytes.
    size_t written = length == 0 ? 0 : fwrite(data, 1, length, file);
    if (fclose(file) != 0 || written != length) {
        bare_fprintf(stderr, "%s: write error\n", path);
        return -1;
    }
    return 0;
}

// write_file for callers that ignore the result (see src/bare_functions.h).
static inline void bare_write_file(const char *path, const void *data, size_t length) {
    (void)write_file(path, data, length);
}

#endif
