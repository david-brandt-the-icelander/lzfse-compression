// Benchmark harness: compares lzfse-zazzcat with Apple's libcompression in one process and prints a Markdown
// report.
//
//   lzfse-bench [--runs N] [--chunk BYTES]... [--synthetic] [--stats] [FILE]...
//
// Every encoded output is decoded back (and on macOS also by Apple's decoder) before its timing counts. With
// --stats it prints, instead of timings, what each encoder chose: token counts and distributions read from the
// compressed streams. That output holds only aggregate numbers, never the input's content.
#define _DEFAULT_SOURCE // Linux: popen and clock_gettime in <stdio.h>/<time.h> under -std=c11
#define _DARWIN_C_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#include "file_io.h"
#include "lzfse_zazzcat.h"

#ifdef __APPLE__
#include <compression.h>
#endif

// --- Inputs ---

// One input to benchmark.
typedef struct input {
    char name[256];
    lzfse_zazzcat_buffer data;
} input;

// The xorshift generator the synthetic corpus uses (the same sequence as the Rust harness, so the data matches).
static uint32_t random_state = 0x2545F491u;
static uint32_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

// Append bytes to a buffer, exiting if memory runs out.
static void append(lzfse_zazzcat_buffer *buffer, const void *bytes, size_t count) {
    if (buffer->capacity - buffer->length < count) {
        size_t capacity = buffer->capacity * 2 > buffer->length + count ? buffer->capacity * 2 : buffer->length + count;
        buffer->data = realloc(buffer->data, capacity);
        if (buffer->data == NULL) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, bytes, count);
    buffer->length += count;
}

// The built-in corpus (no downloads needed): text, JSON lines with base64 attachments, binary-like code, noise
// and zeros. Stores the inputs in `inputs` and returns how many there are.
static size_t synthetic_inputs(input *inputs) {
    static const char *vocabulary[] = {"the ",  "quick ", "brown ", "fox ", "jumps ", "over ", "a ",
                                       "lazy ", "dog",    ".\n",    ", ",   "and ",   "of "};
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t eight_mib = (size_t)8 << 20;
    input *text = &inputs[0], *jsonl = &inputs[1], *noise = &inputs[3], *binary = &inputs[2], *zeros = &inputs[4];
    memset(inputs, 0, 5 * sizeof *inputs);
    snprintf(text->name, sizeof text->name, "text");
    while (text->data.length < eight_mib) {
        const char *w = vocabulary[next_random() % 13];
        append(&text->data, w, strlen(w));
    }
    text->data.length = eight_mib;
    snprintf(jsonl->name, sizeof jsonl->name, "jsonl+base64");
    while (jsonl->data.length < eight_mib) {
        uint32_t kind = next_random() % 4;
        char line[96];
        int n = snprintf(line, sizeof line, "{\"type\":\"message\",\"id\":%u,\"role\":\"assistant\",\"text\":\"",
                         next_random());
        append(&jsonl->data, line, (size_t)n);
        if (kind == 0) {
            const char *image = "\",\"image\":\"data:image/png;base64,";
            append(&jsonl->data, image, strlen(image));
            for (int i = 0; i < 40000; i++) append(&jsonl->data, &alphabet[next_random() & 63], 1);
        } else {
            for (int i = 0; i < 300; i++) append(&jsonl->data, vocabulary[next_random() % 13], 1);
        }
        append(&jsonl->data, "\"}\n", 3);
    }
    snprintf(noise->name, sizeof noise->name, "noise");
    for (size_t i = 0; i < ((size_t)4 << 20); i++) {
        uint8_t b = (uint8_t)next_random();
        append(&noise->data, &b, 1);
    }
    snprintf(binary->name, sizeof binary->name, "binary");
    while (binary->data.length < eight_mib) {
        uint32_t op = next_random();
        uint8_t instruction[10] = {0x48, 0x89, (uint8_t)op, 0xE8, (uint8_t)(op >> 8), (uint8_t)(op >> 16), 0, 0, 0, 0};
        append(&binary->data, instruction, 10);
    }
    snprintf(zeros->name, sizeof zeros->name, "zeros");
    zeros->data.data = calloc(eight_mib, 1);
    zeros->data.length = zeros->data.capacity = eight_mib;
    return 5;
}

// --- Contenders ---

typedef enum contender_kind { APPLE, OURS, OURS_PARALLEL } contender_kind;

// One contender on one input and chunk size: its encoded streams and what it needs to time encoding and decoding.
typedef struct contender {
    const char *name;
    contender_kind kind;
    const input *in;
    size_t chunk;
    size_t piece_count;
    size_t threads;
    // The encoded chunks (one stream each) and their total size.
    lzfse_zazzcat_buffer *streams;
    size_t packed_bytes;
    // Reused state: our encoder and decoder, Apple's scratch buffers, an output buffer, and the parallel
    // encoder's arena and offsets.
    lzfse_zazzcat_encoder *encoder;
    lzfse_zazzcat_decoder *decoder;
    void *apple_encode_scratch;
    void *apple_decode_scratch;
    uint8_t *output;
    size_t output_capacity;
    lzfse_zazzcat_buffer arena;
    lzfse_zazzcat_offsets offsets;
} contender;

// Start and length of piece `i` of the contender's input.
static const uint8_t *piece(const contender *c, size_t i, size_t *length) {
    size_t at = i * c->chunk;
    *length = c->in->data.length - at < c->chunk ? c->in->data.length - at : c->chunk;
    return c->in->data.data + at;
}

// Encode piece `i` into `out` (replacing its contents) with the contender's encoder.
static void encode_piece(contender *c, size_t i, lzfse_zazzcat_buffer *out) {
    size_t length;
    const uint8_t *data = piece(c, i, &length);
    out->length = 0;
#ifdef __APPLE__
    if (c->kind == APPLE) {
        size_t capacity = length + length / 2 + 1024;
        if (out->capacity < capacity) {
            out->data = realloc(out->data, capacity);
            out->capacity = capacity;
        }
        out->length =
            compression_encode_buffer(out->data, capacity, data, length, c->apple_encode_scratch, COMPRESSION_LZFSE);
        return;
    }
#endif
    lzfse_zazzcat_encoder_encode(c->encoder, data, length, out);
}

// Decode stream `s` into the contender's output buffer with its decoder; returns the decoded length.
static size_t decode_stream(contender *c, const lzfse_zazzcat_buffer *s) {
#ifdef __APPLE__
    if (c->kind == APPLE) {
        return compression_decode_buffer(c->output, c->output_capacity, s->data, s->length, c->apple_decode_scratch,
                                         COMPRESSION_LZFSE);
    }
#endif
    size_t written = 0;
    lzfse_zazzcat_decoder_decode(c->decoder, s->data, s->length, c->output, c->output_capacity, &written);
    return written;
}

// Exit with a message when a correctness check fails.
static void require(bool ok, const char *what, const contender *c, size_t i) {
    if (!ok) {
        fprintf(stderr, "%s failed: %s, %s, chunk %zu\n", what, c->name, c->in->name, i);
        exit(1);
    }
}

// Prepare a contender: encode every piece once and check that the streams decode correctly (with both our and,
// on macOS, Apple's decoder).
static void contender_init(contender *c, const char *name, contender_kind kind, const input *in, size_t chunk,
                           size_t threads) {
    memset(c, 0, sizeof *c);
    c->name = name;
    c->kind = kind;
    c->in = in;
    c->chunk = chunk;
    c->threads = threads;
    c->piece_count = (in->data.length + chunk - 1) / chunk;
    c->streams = calloc(c->piece_count, sizeof *c->streams);
    c->encoder = lzfse_zazzcat_encoder_create(NULL);
    c->decoder = lzfse_zazzcat_decoder_create();
    c->output_capacity = chunk + 64;
    c->output = malloc(c->output_capacity);
#ifdef __APPLE__
    c->apple_encode_scratch = malloc(compression_encode_scratch_buffer_size(COMPRESSION_LZFSE) + 1);
    c->apple_decode_scratch = malloc(compression_decode_scratch_buffer_size(COMPRESSION_LZFSE) + 1);
#endif
    for (size_t i = 0; i < c->piece_count; i++) {
        encode_piece(c, i, &c->streams[i]);
        c->packed_bytes += c->streams[i].length;
        size_t length, written = 0;
        const uint8_t *data = piece(c, i, &length);
        lzfse_zazzcat_status status = lzfse_zazzcat_decode_into(c->streams[i].data, c->streams[i].length, c->output,
                                                                c->output_capacity, &written);
        require(status == LZFSE_ZAZZCAT_OK && written == length && memcmp(c->output, data, length) == 0, "our decoder",
                c, i);
#ifdef __APPLE__
        written = compression_decode_buffer(c->output, c->output_capacity, c->streams[i].data, c->streams[i].length,
                                            NULL, COMPRESSION_LZFSE);
        require(written == length && memcmp(c->output, data, length) == 0, "Apple's decoder", c, i);
#endif
    }
}

// Release a contender's state.
static void contender_free(contender *c) {
    for (size_t i = 0; i < c->piece_count; i++) lzfse_zazzcat_buffer_free(&c->streams[i]);
    free(c->streams);
    lzfse_zazzcat_encoder_destroy(c->encoder);
    lzfse_zazzcat_decoder_destroy(c->decoder);
    free(c->apple_encode_scratch);
    free(c->apple_decode_scratch);
    free(c->output);
    lzfse_zazzcat_buffer_free(&c->arena);
    lzfse_zazzcat_offsets_free(&c->offsets);
}

// Arguments of one parallel-decoding thread: decode every `step`-th stream starting at `first`.
typedef struct decode_share {
    const contender *c;
    size_t first;
    size_t step;
} decode_share;

// Thread body for parallel decoding, with its own decoder and output buffer.
static void *decode_share_main(void *argument) {
    const decode_share *share = argument;
    lzfse_zazzcat_decoder *decoder = lzfse_zazzcat_decoder_create();
    uint8_t *out = malloc(share->c->output_capacity);
    for (size_t i = share->first; i < share->c->piece_count; i += share->step) {
        size_t written;
        lzfse_zazzcat_decoder_decode(decoder, share->c->streams[i].data, share->c->streams[i].length, out,
                                     share->c->output_capacity, &written);
    }
    free(out);
    lzfse_zazzcat_decoder_destroy(decoder);
    return NULL;
}

// Encode the whole input once (the timed operation).
static void run_encode(contender *c) {
    if (c->kind == OURS_PARALLEL) {
        // The allocation-free contiguous form, as a decmpfs writer uses it.
        c->arena.length = 0;
        lzfse_zazzcat_encode_chunks(c->in->data.data, c->in->data.length, c->chunk, NULL, &c->arena, &c->offsets);
        return;
    }
    for (size_t i = 0; i < c->piece_count; i++) encode_piece(c, i, &c->streams[i]);
}

// Decode every stream once (the timed operation).
static void run_decode(contender *c) {
    if (c->kind == OURS_PARALLEL) {
        pthread_t threads[256];
        decode_share shares[256];
        size_t count = c->threads < 256 ? c->threads : 256;
        for (size_t t = 0; t < count; t++) {
            shares[t] = (decode_share){c, t, count};
            pthread_create(&threads[t], NULL, decode_share_main, &shares[t]);
        }
        for (size_t t = 0; t < count; t++) pthread_join(threads[t], NULL);
        return;
    }
    for (size_t i = 0; i < c->piece_count; i++) decode_stream(c, &c->streams[i]);
}

// --- Timing and the report ---

// Seconds from a monotonic clock.
static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// qsort comparison for doubles.
static int compare_doubles(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

// Median of `count` timings (the array is sorted in place).
static double median(double *values, size_t count) {
    qsort(values, count, sizeof *values, compare_doubles);
    return values[count / 2];
}

// One row of the report.
typedef struct row {
    const char *contender;
    const char *input;
    char shape[32];
    double ratio;
    double encode_mbs;
    double decode_mbs;
} row;

// Describe a chunk size for the report.
static void describe_shape(size_t chunk, size_t length, char *out, size_t size) {
    if (chunk >= length) {
        snprintf(out, size, "whole");
    } else if (chunk % (1 << 20) == 0) {
        snprintf(out, size, "%zu MiB chunks", chunk >> 20);
    } else {
        snprintf(out, size, "%zu KiB chunks", chunk >> 10);
    }
}

// Time every contender on one input and chunk size, interleaving them run by run (and rotating which goes first)
// so noise from other activity on the machine hits all of them alike. Appends the rows to `rows`.
static size_t measure(const input *in, size_t chunk, size_t runs, size_t threads, row *rows) {
    contender contenders[3];
    size_t count = 0;
#ifdef __APPLE__
    contender_init(&contenders[count++], "apple libcompression", APPLE, in, chunk, 1);
#endif
    contender_init(&contenders[count++], "lzfse-zazzcat", OURS, in, chunk, 1);
    if (threads > 1 && chunk < in->data.length) {
        contender_init(&contenders[count++], "lzfse-zazzcat parallel", OURS_PARALLEL, in, chunk, threads);
    }
    double *encode_times = calloc(runs * count, sizeof(double)), *decode_times = calloc(runs * count, sizeof(double));
    // One untimed warm-up run each.
    for (size_t c = 0; c < count; c++) {
        run_encode(&contenders[c]);
        run_decode(&contenders[c]);
    }
    for (size_t run = 0; run < runs; run++) {
        for (size_t k = 0; k < count; k++) {
            size_t c = (run + k) % count;
            double start = now_seconds();
            run_encode(&contenders[c]);
            encode_times[c * runs + run] = now_seconds() - start;
            start = now_seconds();
            run_decode(&contenders[c]);
            decode_times[c * runs + run] = now_seconds() - start;
        }
    }
    for (size_t c = 0; c < count; c++) {
        row *r = &rows[c];
        r->contender = contenders[c].name;
        r->input = in->name;
        describe_shape(chunk, in->data.length, r->shape, sizeof r->shape);
        r->ratio = (double)contenders[c].packed_bytes / (double)in->data.length;
        r->encode_mbs = (double)in->data.length / median(encode_times + c * runs, runs) / 1e6;
        r->decode_mbs = (double)in->data.length / median(decode_times + c * runs, runs) / 1e6;
        contender_free(&contenders[c]);
    }
    free(encode_times);
    free(decode_times);
    return count;
}

#ifdef __APPLE__
// Print the first line of a command's output after `label`, if the command runs.
static void print_command_line(const char *label, const char *command) {
    FILE *pipe = popen(command, "r");
    if (pipe == NULL) return;
    char line[256] = {0};
    if (fgets(line, sizeof line, pipe) != NULL) {
        line[strcspn(line, "\n")] = 0;
        printf("- %s: %s\n", label, line);
    }
    pclose(pipe);
}
#endif

// Print the machine description and the results as Markdown.
static void report(const row *rows, size_t count, size_t threads) {
    struct utsname system;
    uname(&system);
    printf("## Environment\n\n");
    printf("- OS: %s %s\n", system.sysname, system.machine);
#ifdef __APPLE__
    print_command_line("Chip", "sysctl -n machdep.cpu.brand_string");
    print_command_line("macOS", "sw_vers -productVersion");
#endif
    printf("- Threads for the parallel rows: %zu\n", threads);
    printf("- Throughput is MB/s (10^6 bytes) of uncompressed data: the median of the timed runs, with the "
           "contenders interleaved run by run.\n\n");
    printf("| Input | Shape | Contender | Ratio | Encode MB/s | Decode MB/s |\n");
    printf("|---|---|---|---:|---:|---:|\n");
    for (size_t i = 0; i < count; i++) {
        printf("| %s | %s | %s | %.3f | %.0f | %.0f |\n", rows[i].input, rows[i].shape, rows[i].contender,
               rows[i].ratio, rows[i].encode_mbs, rows[i].decode_mbs);
    }
}

// --- Stats mode ---

// Percentage of `part` in `whole`.
static double percent(uint64_t part, uint64_t whole) {
    return whole == 0 ? 0 : 100.0 * (double)part / (double)whole;
}

// Print one bucket distribution: "label: low-high: count (x% of bytes), ...".
static void print_buckets(const char *label, const uint32_t *limits, size_t count, const uint64_t *matches,
                          const uint64_t *bytes, uint64_t total) {
    printf("- %s: ", label);
    uint32_t low = 0;
    for (size_t i = 0; i < count; i++) {
        if (limits[i] == UINT32_MAX) {
            printf("%u+", low);
        } else {
            printf("%u-%u", low, limits[i] - 1);
        }
        printf(": %llu (%.1f%% of bytes)%s", (unsigned long long)matches[i], percent(bytes[i], total),
               i + 1 < count ? ", " : "\n");
        low = limits[i];
    }
}

// Print one encoder's stream statistics for one input and shape.
static void print_stats(const char *input_name, const char *shape, const char *who,
                        const lzfse_zazzcat_stream_stats *s) {
    uint64_t matches = s->tokens - s->literal_only_tokens;
    printf("### %s, %s: %s\n\n", input_name, shape, who);
    printf("- stored %llu of %llu bytes (ratio %.4f); blocks lzfse/lzvn/raw [%llu, %llu, %llu]\n",
           (unsigned long long)s->stored_bytes, (unsigned long long)s->decoded_bytes,
           s->decoded_bytes ? (double)s->stored_bytes / (double)s->decoded_bytes : 0, (unsigned long long)s->blocks[0],
           (unsigned long long)s->blocks[1], (unsigned long long)s->blocks[2]);
    printf("- bytes: headers %llu, literals %llu, match stream %llu, other blocks %llu\n",
           (unsigned long long)s->header_bytes, (unsigned long long)s->literal_bytes,
           (unsigned long long)s->match_stream_bytes, (unsigned long long)s->other_block_bytes);
    printf("- literals %llu (%.2f bits each); tokens %llu (%.2f bits each); literal-only tokens %llu; repeat "
           "distances %llu (%.1f%% of matches)\n",
           (unsigned long long)s->literals, s->literals ? 8.0 * (double)s->literal_bytes / (double)s->literals : 0,
           (unsigned long long)s->tokens, s->tokens ? 8.0 * (double)s->match_stream_bytes / (double)s->tokens : 0,
           (unsigned long long)s->literal_only_tokens, (unsigned long long)s->repeat_distances,
           percent(s->repeat_distances, matches));
    print_buckets("match lengths", lzfse_zazzcat_length_bucket_limits, LZFSE_ZAZZCAT_LENGTH_BUCKETS,
                  s->matches_by_length, s->bytes_by_length, s->decoded_bytes);
    print_buckets("distances", lzfse_zazzcat_distance_bucket_limits, LZFSE_ZAZZCAT_DISTANCE_BUCKETS,
                  s->matches_by_distance, s->bytes_by_distance, s->decoded_bytes);
    printf("\n");
}

// For every input and shape, encode with each encoder and print what it chose.
static void stats_mode(const input *inputs, size_t input_count, const size_t *chunks, size_t chunk_count) {
    for (size_t i = 0; i < input_count; i++) {
        for (size_t k = 0; k < chunk_count; k++) {
            size_t length = inputs[i].data.length;
            size_t chunk = chunks[k] < length ? chunks[k] : (length > 0 ? length : 1);
            char shape[32];
            describe_shape(chunk, length, shape, sizeof shape);
            lzfse_zazzcat_stream_stats ours, theirs;
            memset(&ours, 0, sizeof ours);
            memset(&theirs, 0, sizeof theirs);
            lzfse_zazzcat_encoder *encoder = lzfse_zazzcat_encoder_create(NULL);
            lzfse_zazzcat_buffer out = {0};
#ifdef __APPLE__
            void *scratch = malloc(compression_encode_scratch_buffer_size(COMPRESSION_LZFSE) + 1);
#endif
            for (size_t at = 0; at < length; at += chunk) {
                size_t n = length - at < chunk ? length - at : chunk;
                out.length = 0;
                lzfse_zazzcat_encoder_encode(encoder, inputs[i].data.data + at, n, &out);
                lzfse_zazzcat_stream_stats_add(&ours, out.data, out.length);
#ifdef __APPLE__
                size_t capacity = n + n / 2 + 1024;
                if (out.capacity < capacity) {
                    out.data = realloc(out.data, capacity);
                    out.capacity = capacity;
                }
                out.length = compression_encode_buffer(out.data, capacity, inputs[i].data.data + at, n, scratch,
                                                       COMPRESSION_LZFSE);
                lzfse_zazzcat_stream_stats_add(&theirs, out.data, out.length);
#endif
            }
#ifdef __APPLE__
            print_stats(inputs[i].name, shape, "apple libcompression", &theirs);
            free(scratch);
#endif
            print_stats(inputs[i].name, shape, "lzfse-zazzcat", &ours);
            lzfse_zazzcat_buffer_free(&out);
            lzfse_zazzcat_encoder_destroy(encoder);
        }
    }
}

// Parse arguments, load the inputs and run every contender on every shape.
int main(int argc, char **argv) {
    size_t runs = 7, chunks[16], chunk_count = 0, input_count = 0;
    bool use_synthetic = false, stats = false;
    input *inputs = calloc((size_t)argc + 5, sizeof *inputs);
    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--runs") == 0 && a + 1 < argc) {
            runs = strtoul(argv[++a], NULL, 10);
            if (runs == 0) runs = 1;
        } else if (strcmp(argv[a], "--chunk") == 0 && a + 1 < argc) {
            if (chunk_count < 16) chunks[chunk_count++] = strtoull(argv[++a], NULL, 10);
        } else if (strcmp(argv[a], "--synthetic") == 0) {
            use_synthetic = true;
        } else if (strcmp(argv[a], "--stats") == 0) {
            stats = true;
        } else if (strcmp(argv[a], "-h") == 0 || strcmp(argv[a], "--help") == 0) {
            printf("usage: lzfse-bench [--runs N] [--chunk BYTES]... [--synthetic] [--stats] [FILE]...\n");
            return 0;
        } else {
            input *in = &inputs[input_count++];
            const char *slash = strrchr(argv[a], '/');
            snprintf(in->name, sizeof in->name, "%s", slash ? slash + 1 : argv[a]);
            if (read_file(argv[a], &in->data) != 0) return 1;
        }
    }
    if (use_synthetic || input_count == 0) input_count += synthetic_inputs(inputs + input_count);
    if (chunk_count == 0) {
        chunks[chunk_count++] = 65536;
        chunks[chunk_count++] = 1 << 20;
        chunks[chunk_count++] = SIZE_MAX;
    }
    if (stats) {
        stats_mode(inputs, input_count, chunks, chunk_count);
        return 0;
    }
    size_t threads = lzfse_zazzcat_parallelism();
    row *rows = calloc(input_count * chunk_count * 3, sizeof *rows);
    size_t row_count = 0;
    for (size_t i = 0; i < input_count; i++) {
        for (size_t k = 0; k < chunk_count; k++) {
            size_t length = inputs[i].data.length;
            size_t chunk = chunks[k] < length ? chunks[k] : (length > 0 ? length : 1);
            row_count += measure(&inputs[i], chunk, runs, threads, rows + row_count);
            fprintf(stderr, "done: %s %s\n", inputs[i].name, rows[row_count - 1].shape);
        }
    }
    report(rows, row_count, threads);
    return 0;
}
