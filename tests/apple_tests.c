// Compatibility with Apple's libcompression and the macOS kernel (macOS only): both directions of the stream
// format and of raw LZVN, the compression ratio, the system's own compressed files, and files we compress
// ourselves read back through the kernel. On other systems run_apple_tests does nothing.
//
// unit_tests.c includes this file (so both share the test framework's failure count), not the Makefile.
#ifndef __APPLE__

void run_apple_tests(void);

// Nothing to test without libcompression.
void run_apple_tests(void) {}

#else

#include <compression.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "file_io.h"
#include "test_support.h"

void run_apple_tests(void);

// libcompression's algorithm number for raw LZVN (not in the public header).
#define APPLE_LZVN ((compression_algorithm)0x900)

// Compress with Apple's encoder into a new buffer.
static lzfse_zazzcat_buffer apple_encode(const uint8_t *data, size_t n, compression_algorithm algorithm) {
    size_t capacity = n * 2 + 4096;
    lzfse_zazzcat_buffer out = {malloc(capacity), 0, capacity};
    out.length = compression_encode_buffer(out.data, capacity, data, n, NULL, algorithm);
    CHECK(out.length > 0 || n == 0, "Apple's encoder failed on %zu bytes", n);
    return out;
}

// Decompress with Apple's decoder; returns true when it produces exactly `data`.
static int apple_decodes_to(const lzfse_zazzcat_buffer *stream, const uint8_t *data, size_t n,
                            compression_algorithm algorithm) {
    uint8_t *out = malloc(n + 1);
    size_t got = compression_decode_buffer(out, n + 1, stream->data, stream->length, NULL, algorithm);
    int same = got == n && (n == 0 || memcmp(out, data, n) == 0);
    free(out);
    return same;
}

// One named test input.
typedef struct sample {
    char name[32];
    lzfse_zazzcat_buffer data;
} sample;

// Inputs covering text, binary, noise, runs and every size threshold. Returns the number of samples.
static size_t make_corpus(sample *samples) {
    size_t count = 0;
    static const char *vocabulary[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over ", "a ", "lazy ", "dog.\n"};
    lzfse_zazzcat_buffer random = noise(400000, 12345), words_all = {0};
    for (size_t i = 0; i < random.length; i++) {
        const char *w = vocabulary[random.data[i] % 9];
        buffer_append(&words_all, w, strlen(w));
    }
    snprintf(samples[count].name, 32, "empty");
    samples[count++].data = copy_of("", 0);
    snprintf(samples[count].name, 32, "tiny");
    samples[count++].data = copy_of("abc", 3);
    snprintf(samples[count].name, 32, "zeros");
    samples[count].data = noise(1 << 20, 1);
    memset(samples[count++].data.data, 0, 1 << 20);
    size_t sizes[] = {7, 8, 100, 4095, 4096, 4097, 65536, 1000000};
    for (int i = 0; i < 8; i++) {
        snprintf(samples[count].name, 32, "words %zu", sizes[i]);
        samples[count++].data = copy_of(words_all.data, min_size(sizes[i], words_all.length));
    }
    snprintf(samples[count].name, 32, "noise");
    samples[count++].data = noise(200000, 777);
    snprintf(samples[count].name, 32, "base64");
    lzfse_zazzcat_buffer b64 = noise(300000, 4242);
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < b64.length; i++) b64.data[i] = (uint8_t)alphabet[b64.data[i] & 63];
    samples[count++].data = b64;
    // This project's own C sources, as a real-world text sample.
    snprintf(samples[count].name, 32, "sources");
    lzfse_zazzcat_buffer sources = {0}, file = {0};
    DIR *dir = opendir("src");
    struct dirent *entry;
    while (dir != NULL && (entry = readdir(dir)) != NULL) {
        char path[512];
        snprintf(path, sizeof path, "src/%s", entry->d_name);
        if (entry->d_name[0] != '.' && read_file(path, &file) == 0) buffer_append(&sources, file.data, file.length);
    }
    if (dir != NULL) closedir(dir);
    lzfse_zazzcat_buffer_free(&file);
    samples[count++].data = sources;
    lzfse_zazzcat_buffer_free(&random);
    lzfse_zazzcat_buffer_free(&words_all);
    return count;
}

// Release the corpus.
static void free_corpus(sample *samples, size_t count) {
    for (size_t i = 0; i < count; i++) lzfse_zazzcat_buffer_free(&samples[i].data);
}

// Every stream Apple writes, we decode; every stream we write, Apple decodes.
static void test_lzfse_both_directions(void) {
    sample samples[20];
    size_t count = make_corpus(samples);
    for (size_t i = 0; i < count; i++) {
        const lzfse_zazzcat_buffer *d = &samples[i].data;
        // Apple's encoder reports failure (0 bytes) for empty input, so that direction starts at 1 byte.
        if (d->length > 0) {
            lzfse_zazzcat_buffer theirs = apple_encode(d->data, d->length, COMPRESSION_LZFSE), ours = {0};
            CHECK(lzfse_zazzcat_decode(theirs.data, theirs.length, d->length, &ours) == LZFSE_ZAZZCAT_OK &&
                      same_bytes(ours.data, ours.length, d->data, d->length),
                  "decode Apple's %s", samples[i].name);
            lzfse_zazzcat_buffer_free(&theirs);
            lzfse_zazzcat_buffer_free(&ours);
        }
        lzfse_zazzcat_buffer ours = {0};
        lzfse_zazzcat_encode(d->data, d->length, NULL, &ours);
        CHECK(apple_decodes_to(&ours, d->data, d->length, COMPRESSION_LZFSE), "Apple decodes our %s", samples[i].name);
        lzfse_zazzcat_buffer_free(&ours);
    }
    free_corpus(samples, count);
}

// Raw LZVN streams interoperate both ways.
static void test_lzvn_both_directions(void) {
    sample samples[20];
    size_t count = make_corpus(samples);
    for (size_t i = 0; i < count; i++) {
        const lzfse_zazzcat_buffer *d = &samples[i].data;
        if (d->length < 8 || d->length > 300000) continue;
        lzfse_zazzcat_buffer theirs = apple_encode(d->data, d->length, APPLE_LZVN), ours = {0};
        uint8_t *out = malloc(d->length);
        size_t written = 0;
        CHECK(lzfse_zazzcat_lzvn_decode(theirs.data, theirs.length, out, d->length, &written) == LZFSE_ZAZZCAT_OK &&
                  same_bytes(out, written, d->data, d->length),
              "decode Apple's LZVN %s", samples[i].name);
        lzfse_zazzcat_lzvn_encode(d->data, d->length, &ours);
        CHECK(apple_decodes_to(&ours, d->data, d->length, APPLE_LZVN), "Apple decodes our LZVN %s", samples[i].name);
        free(out);
        lzfse_zazzcat_buffer_free(&theirs);
        lzfse_zazzcat_buffer_free(&ours);
    }
    free_corpus(samples, count);
}

// Session-log-like JSON lines: short messages, re-read documents and re-sent earlier context (long-range
// repeats), optionally with large base64 attachments, like the Claude Code logs free-disk-space compresses.
static lzfse_zazzcat_buffer session_like(size_t n, int with_attachments) {
    test_random r = {0x9E3779B9u};
    static const char *vocabulary[] = {"the",  "quick", "brown", "fox", "jumps", "over", "a",
                                       "lazy", "dog",   "and",   "of",  "to",    "in",   "is"};
    // Messages so far (for re-sending), each a buffer; 30 longer "documents" that get re-read.
    lzfse_zazzcat_buffer documents[30], *messages = NULL, out = {0};
    size_t message_count = 0, message_capacity = 0;
    for (int d = 0; d < 30; d++) {
        documents[d] = (lzfse_zazzcat_buffer){0};
        uint32_t k = 200 + test_random_next(&r) % 1800;
        for (uint32_t w = 0; w < k; w++) {
            if (w) buffer_append(&documents[d], " ", 1);
            const char *word = vocabulary[test_random_next(&r) % 14];
            buffer_append(&documents[d], word, strlen(word));
        }
    }
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    while (out.length < n) {
        uint32_t kind = test_random_next(&r) % 100;
        lzfse_zazzcat_buffer m = {0};
        int is_attachment = 0;
        if (kind < 45) {
            uint32_t count = 20 + test_random_next(&r) % 180;
            for (uint32_t w = 0; w < count; w++) {
                if (w) buffer_append(&m, " ", 1);
                const char *word = vocabulary[test_random_next(&r) % 14];
                buffer_append(&m, word, strlen(word));
            }
        } else if (kind < 70) {
            const lzfse_zazzcat_buffer *doc = &documents[test_random_next(&r) % 30];
            buffer_append(&m, doc->data, doc->length);
        } else if (kind < 95 || !with_attachments) {
            size_t back = min_size(1 + test_random_next(&r) % 10, message_count);
            for (size_t i = message_count - back; i < message_count; i++) {
                if (i > message_count - back) buffer_append(&m, " ", 1);
                buffer_append(&m, messages[i].data, messages[i].length);
            }
        } else {
            size_t length = 20000 + test_random_next(&r) % 130000;
            buffer_append(&m, "data:image/png;base64,", 22);
            buffer_reserve(&m, length);
            for (size_t i = 0; i < length; i++) m.data[m.length++] = (uint8_t)alphabet[test_random_next(&r) & 63];
            is_attachment = 1;
        }
        char prefix[96];
        uint32_t a = test_random_next(&r), b = test_random_next(&r);
        int prefix_length =
            snprintf(prefix, sizeof prefix, "{\"type\":\"message\",\"uuid\":\"%08x%08x\",\"content\":\"", a, b);
        buffer_append(&out, prefix, (size_t)prefix_length);
        buffer_append(&out, m.data, m.length);
        buffer_append(&out, "\"}\n", 3);
        if (message_count == message_capacity) {
            message_capacity = message_capacity ? message_capacity * 2 : 64;
            messages = realloc(messages, message_capacity * sizeof *messages);
        }
        if (is_attachment) {
            lzfse_zazzcat_buffer_free(&m);
            m = copy_of("image", 5);
        }
        messages[message_count++] = m;
    }
    out.length = n;
    for (size_t i = 0; i < message_count; i++) lzfse_zazzcat_buffer_free(&messages[i]);
    free(messages);
    for (int d = 0; d < 30; d++) lzfse_zazzcat_buffer_free(&documents[d]);
    return out;
}

// Our compression ratio stays within 2% of Apple's on compressible inputs, at 64 KiB chunks, 1 MiB chunks and
// whole inputs.
static void test_ratio_close_to_apple(void) {
    sample samples[22];
    size_t count = make_corpus(samples);
    snprintf(samples[count].name, 32, "session log");
    samples[count++].data = session_like(4 << 20, 0);
    snprintf(samples[count].name, 32, "session+b64");
    samples[count++].data = session_like(4 << 20, 1);
    double worst = -100;
    for (size_t i = 0; i < count; i++) {
        const lzfse_zazzcat_buffer *d = &samples[i].data;
        if (d->length < 65536 || strcmp(samples[i].name, "noise") == 0) continue;
        size_t chunk_sizes[3] = {65536, 1 << 20, d->length};
        for (int c = 0; c < 3; c++) {
            size_t chunk = min_size(chunk_sizes[c], d->length), theirs = 0, ours = 0;
            for (size_t at = 0; at < d->length; at += chunk) {
                size_t piece = min_size(chunk, d->length - at);
                lzfse_zazzcat_buffer a = apple_encode(d->data + at, piece, COMPRESSION_LZFSE), b = {0};
                lzfse_zazzcat_encode(d->data + at, piece, NULL, &b);
                theirs += a.length;
                ours += b.length;
                lzfse_zazzcat_buffer_free(&a);
                lzfse_zazzcat_buffer_free(&b);
            }
            double difference = 100.0 * ((double)ours / (double)theirs - 1.0);
            if (difference > worst) worst = difference;
            printf("    %14s %9zu: apple %9zu  ours %9zu  (%+.2f%%)\n", samples[i].name, chunk, theirs, ours,
                   difference);
        }
    }
    CHECK(worst <= 2.0, "worst ratio gap %+.2f%% (see the table above)", worst);
    free_corpus(samples, count);
}

// Read one extended attribute, including the hidden compression ones; returns 0 when it exists.
static int read_xattr(const char *path, const char *name, lzfse_zazzcat_buffer *out) {
    out->length = 0;
    ssize_t size = getxattr(path, name, NULL, 0, 0, XATTR_SHOWCOMPRESSION);
    if (size < 0) return -1;
    buffer_reserve(out, (size_t)size + 1);
    ssize_t got = getxattr(path, name, out->data, (size_t)size, 0, XATTR_SHOWCOMPRESSION);
    if (got < 0) return -1;
    out->length = (size_t)got;
    return 0;
}

// Collect up to `max` compressed regular files under `directory` (not following links) into `paths`.
static void find_compressed_files(const char *directory, size_t max, char (*paths)[512], size_t *count) {
    DIR *dir = opendir(directory);
    if (dir == NULL) return;
    struct dirent *entry;
    while (*count < max && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", directory, entry->d_name);
        struct stat info;
        if (lstat(path, &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            find_compressed_files(path, max, paths, count);
        } else if (S_ISREG(info.st_mode) && (info.st_flags & UF_COMPRESSED)) {
            snprintf(paths[(*count)++], 512, "%s", path);
        }
    }
    closedir(dir);
}

// Files macOS itself compressed decode with our decmpfs parser to the same bytes the kernel returns.
static void test_system_compressed_files(void) {
    size_t max = 16000, count = 0;
    char(*paths)[512] = malloc(max * 512);
    const char *directories[] = {"/usr/bin", "/usr/libexec", "/System/Library/CoreServices",
                                 "/System/Library/Frameworks"};
    for (int i = 0; i < 4; i++) find_compressed_files(directories[i], count + 4000, paths, &count);
    size_t by_type[16] = {0}, checked = 0;
    lzfse_zazzcat_buffer attribute = {0}, fork = {0}, want = {0}, got = {0};
    for (size_t i = 0; i < count; i++) {
        uint32_t type;
        uint64_t size;
        if (read_xattr(paths[i], "com.apple.decmpfs", &attribute) != 0 ||
            lzfse_zazzcat_decmpfs_parse_header(attribute.data, attribute.length, &type, &size) != LZFSE_ZAZZCAT_OK) {
            continue;
        }
        by_type[type < 16 ? type : 15]++;
        if (!(type == 7 || type == 8 || type == 11 || type == 12) || size > 50000000) continue;
        if (read_xattr(paths[i], "com.apple.ResourceFork", &fork) != 0) fork.length = 0;
        if (read_file(paths[i], &want) != 0) continue;
        got.length = 0;
        CHECK(lzfse_zazzcat_decmpfs_decompress(attribute.data, attribute.length, fork.data, fork.length, UINT64_MAX,
                                               &got) == LZFSE_ZAZZCAT_OK &&
                  same_bytes(got.data, got.length, want.data, want.length),
              "%s (type %u)", paths[i], type);
        checked++;
    }
    printf("    compressed files by type:");
    for (int t = 0; t < 16; t++) {
        if (by_type[t]) printf(" %d: %zu", t, by_type[t]);
    }
    printf("; verified %zu\n", checked);
    CHECK(checked > 0, "no LZFSE/LZVN-compressed system files found to check");
    lzfse_zazzcat_buffer *buffers[] = {&attribute, &fork, &want, &got};
    for (int i = 0; i < 4; i++) lzfse_zazzcat_buffer_free(buffers[i]);
    free(paths);
}

// Files we compress (inline and resource-fork layouts) read back through the kernel unchanged.
static void test_kernel_reads_our_files(void) {
    char directory[256];
    snprintf(directory, sizeof directory, "/tmp/lzfse-zazzcat-%d", (int)getpid());
    mkdir(directory, 0700);
    sample samples[20];
    size_t count = make_corpus(samples);
    lzfse_zazzcat_buffer back = {0};
    for (size_t i = 0; i < count; i++) {
        const lzfse_zazzcat_buffer *d = &samples[i].data;
        if (d->length == 0) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/sample%zu", directory, i);
        write_file(path, "", 0);
        lzfse_zazzcat_decmpfs c;
        REQUIRE(lzfse_zazzcat_decmpfs_compress(d->data, d->length, &c) == LZFSE_ZAZZCAT_OK, "compress");
        if (c.fork.length > 0) {
            CHECK(setxattr(path, "com.apple.ResourceFork", c.fork.data, c.fork.length, 0, 0) == 0, "setxattr fork: %s",
                  strerror(errno));
        }
        CHECK(setxattr(path, "com.apple.decmpfs", c.xattr.data, c.xattr.length, 0, 0) == 0, "setxattr: %s",
              strerror(errno));
        CHECK(chflags(path, UF_COMPRESSED) == 0, "chflags: %s", strerror(errno));
        struct stat info;
        lstat(path, &info);
        CHECK(info.st_flags & UF_COMPRESSED, "%s: the kernel refused the compressed flag", samples[i].name);
        CHECK((size_t)info.st_size == d->length, "%s: size", samples[i].name);
        CHECK(read_file(path, &back) == 0 && same_bytes(back.data, back.length, d->data, d->length),
              "%s: contents through the kernel", samples[i].name);
        printf("    %14s: %zu bytes stored as %zu (%lld blocks on disk)\n", samples[i].name, d->length,
               c.xattr.length + c.fork.length, (long long)info.st_blocks);
        lzfse_zazzcat_decmpfs_free(&c);
        unlink(path);
    }
    rmdir(directory);
    lzfse_zazzcat_buffer_free(&back);
    free_corpus(samples, count);
}

void run_apple_tests(void) {
    RUN_TEST(test_lzfse_both_directions);
    RUN_TEST(test_lzvn_both_directions);
    RUN_TEST(test_ratio_close_to_apple);
    RUN_TEST(test_system_compressed_files);
    RUN_TEST(test_kernel_reads_our_files);
}

#endif
