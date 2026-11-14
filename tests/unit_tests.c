// Unit and whole-stream tests: format tables and headers, bit streams, FSE, LZVN, the copy kernels, the encoder's
// paths, parallel chunks, decmpfs, statistics, round trips and hostile input. On macOS this also runs the Apple
// interoperability tests (apple_tests.c, included below).
#define _DARWIN_C_SOURCE // macOS: chflags, st_flags and extended attributes, used by the Apple tests
#include "bare_functions.h"
#include "bits.h"
#include "copy.h"
#include "format.h"
#include "fse.h"
#include "lzvn.h"
#include "test_support.h"

// --- Helpers ---

// Compress `data` with the default options into a new buffer (the test fails if encoding fails).
static lzfse_zazzcat_buffer encode_buffer(const void *data, size_t n) {
    lzfse_zazzcat_buffer out = {0};
    lzfse_zazzcat_status status = lzfse_zazzcat_encode(data, n, NULL, &out);
    CHECK(status == LZFSE_ZAZZCAT_OK, "encode failed: %s", lzfse_zazzcat_status_string(status));
    return out;
}

// Decode a whole stream with a limit; returns the status and fills `out`.
static lzfse_zazzcat_status decode_buffer(const void *stream, size_t n, size_t limit, lzfse_zazzcat_buffer *out) {
    out->length = 0;
    return lzfse_zazzcat_decode(stream, n, limit, out);
}

// decode_buffer for callers that ignore the status (see src/bare_functions.h).
static void bare_decode_buffer(const void *stream, size_t n, size_t limit, lzfse_zazzcat_buffer *out) {
    (void)decode_buffer(stream, n, limit, out);
}

// Encode, decode in every way, check the results and return the stream (caller frees it).
static lzfse_zazzcat_buffer round_trip(const void *data, size_t n) {
    lzfse_zazzcat_buffer encoded = encode_buffer(data, n), decoded = {0};
    size_t length = 0;
    CHECK(lzfse_zazzcat_decoded_length(encoded.data, encoded.length, &length) == LZFSE_ZAZZCAT_OK && length == n,
          "decoded length %zu, wanted %zu", length, n);
    CHECK(decode_buffer(encoded.data, encoded.length, n, &decoded) == LZFSE_ZAZZCAT_OK &&
              same_bytes(decoded.data, decoded.length, data, n),
          "decode of %zu bytes", n);
    uint8_t *into = raw_malloc(n + 10);
    size_t written = 0;
    CHECK(lzfse_zazzcat_decode_into(encoded.data, encoded.length, into, n + 10, &written) == LZFSE_ZAZZCAT_OK &&
              same_bytes(into, written, data, n),
          "decode_into of %zu bytes", n);
    raw_free(into);
    lzfse_zazzcat_buffer_free(&decoded);
    return encoded;
}

// The magic of a stream's first block.
static uint32_t first_magic(const lzfse_zazzcat_buffer *stream) {
    return stream->length >= 4 ? load_u32(stream->data) : 0;
}

// --- Format ---

// The symbol whose base is the largest one not above `value` (a slow reference for the fast symbol functions).
static uint32_t symbol_by_search(uint32_t value, const uint32_t *base, size_t count) {
    size_t s = 0;
    while (s + 1 < count && base[s + 1] <= value) s++;
    return (uint32_t)s;
}

static void test_fast_symbols_match_search(void) {
    for (uint32_t v = 0; v <= MAX_LITERAL_LENGTH; v++) {
        CHECK(literal_length_symbol(v) == symbol_by_search(v, literal_length_base, LITERAL_LENGTH_SYMBOLS), "%u", v);
    }
    for (uint32_t v = 0; v <= MAX_MATCH_LENGTH; v++) {
        CHECK(match_length_symbol(v) == symbol_by_search(v, match_length_base, MATCH_LENGTH_SYMBOLS), "%u", v);
    }
    for (uint32_t v = 0; v <= MAX_DISTANCE; v++) {
        uint32_t want = symbol_by_search(v, distance_base, DISTANCE_SYMBOLS);
        if (distance_symbol(v) != want) {
            CHECK(0, "distance %u", v);
            break;
        }
    }
}

static void test_frequency_codes_round_trip(void) {
    for (uint32_t v = 0; v <= 1047; v++) {
        uint32_t count, count_back;
        uint32_t bits = write_frequency((uint16_t)v, &count);
        CHECK(bits < (1u << count), "value %u", v);
        // Garbage above the code must not matter.
        uint16_t back = read_frequency(bits | (0xABC00000u & ~((1u << count) - 1)), &count_back);
        CHECK(back == v && count_back == count, "value %u", v);
    }
}

static void test_symbol_boundaries(void) {
    CHECK(MAX_DISTANCE == distance_base[63] + (1u << distance_extra_bits[63]) - 1, "max distance");
    CHECK(MAX_LITERAL_LENGTH == literal_length_base[19] + (1u << literal_length_extra_bits[19]) - 1, "max L");
    CHECK(MAX_MATCH_LENGTH == match_length_base[19] + (1u << match_length_extra_bits[19]) - 1, "max M");
    CHECK(literal_length_symbol(15) == 15 && literal_length_symbol(16) == 16 && literal_length_symbol(19) == 16,
          "literal length symbols");
    CHECK(distance_symbol(MAX_DISTANCE) == 63, "top distance symbol");
}

// A header with distinctive values in every field.
static block_header sample_header(void) {
    block_header h;
    clear_bytes(&h, sizeof h);
    h.decoded_size = 123456;
    h.literal_count = 40000;
    h.token_count = 9999;
    h.literal_payload_size = 30001;
    h.token_payload_size = 70002;
    h.literal_padding_bits = -3;
    h.literal_states[0] = 1;
    h.literal_states[1] = 1023;
    h.literal_states[2] = 512;
    h.literal_states[3] = 7;
    h.token_padding_bits = -7;
    h.literal_length_state = 63;
    h.match_length_state = 1;
    h.distance_state = 255;
    h.literal_length_frequencies[0] = 60;
    h.literal_length_frequencies[19] = 4;
    h.match_length_frequencies[5] = 64;
    h.distance_frequencies[63] = 200;
    h.distance_frequencies[1] = 56;
    uint32_t sum = 0;
    for (int i = 0; i < LITERAL_SYMBOLS; i++) sum += h.literal_frequencies[i] = (uint16_t)(i % 5);
    h.literal_frequencies[3] = (uint16_t)(1024 - sum + 3);
    return h;
}

static void test_headers_round_trip(void) {
    block_header h = sample_header(), back;
    // The parsers set every field but not the struct's padding bytes, which compare_bytes also compares.
    clear_bytes(&back, sizeof back);
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_OK, "sample header is valid");
    lzfse_zazzcat_buffer v2 = {0}, v1 = {0};
    size_t size = write_v2_header(&h, &v2), parsed_size = 0;
    CHECK(size == v2.length, "v2 size");
    CHECK(parse_v2_header(v2.data, v2.length, &back, &parsed_size) == LZFSE_ZAZZCAT_OK && parsed_size == size &&
              compare_bytes(&back, &h, sizeof h) == 0,
          "v2 round trip");
    CHECK(write_v1_header(&h, &v1), "write v1");
    CHECK(parse_v1_header(v1.data, v1.length, &back, &parsed_size) == LZFSE_ZAZZCAT_OK &&
              parsed_size == V1_HEADER_SIZE && compare_bytes(&back, &h, sizeof h) == 0,
          "v1 round trip");
    lzfse_zazzcat_buffer_free(&v2);
    lzfse_zazzcat_buffer_free(&v1);
}

static void test_bad_headers_are_errors(void) {
    block_header h = sample_header(), back;
    lzfse_zazzcat_buffer v2 = {0};
    bare_write_v2_header(&h, &v2);
    size_t size;
    for (size_t cut = 0; cut < v2.length; cut++) {
        CHECK(parse_v2_header(v2.data, cut, &back, &size) != LZFSE_ZAZZCAT_OK, "cut at %zu", cut);
    }
    // A header size below the fixed part, or one byte too long, is corrupt.
    lzfse_zazzcat_buffer bad = copy_of(v2.data, v2.length);
    store_u32(bad.data + 24, 5);
    CHECK(parse_v2_header(bad.data, bad.length, &back, &size) == LZFSE_ZAZZCAT_CORRUPT, "tiny size");
    lzfse_zazzcat_buffer longer = copy_of(v2.data, v2.length);
    bare_buffer_append(&longer, "", 1);
    store_u32(longer.data + 24, (uint32_t)longer.length);
    CHECK(parse_v2_header(longer.data, longer.length, &back, &size) == LZFSE_ZAZZCAT_CORRUPT, "extra byte");
    h.literal_length_frequencies[0] = 65;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "frequencies over the state count");
    lzfse_zazzcat_buffer_free(&v2);
    lzfse_zazzcat_buffer_free(&bad);
    lzfse_zazzcat_buffer_free(&longer);
}

// --- Bit streams ---

static void test_bits_read_back_in_reverse(void) {
    for (uint32_t extra = 0; extra < 16; extra++) {
        // 8 leading zero bytes, as the token stream has, so the reader may load bytes before the stream.
        lzfse_zazzcat_buffer out = {0};
        bare_buffer_reserve(&out, 4096);
        clear_bytes(out.data, 8);
        out.length = 8;
        bit_writer writer = {0, 0};
        uint32_t counts[200];
        uint64_t values[200];
        for (uint32_t i = 0; i < 200; i++) {
            counts[i] = i % 17 + extra % 3;
            values[i] = ((uint64_t)i * 2654435761u) & ((1ull << counts[i]) - 1);
            bit_writer_push(&writer, counts[i], values[i]);
            bit_writer_flush(&writer, &out);
        }
        int32_t padding = bit_writer_finish(&writer, &out);
        CHECK(padding >= -7 && padding <= 0, "padding %d", padding);
        bit_reader reader = {0};
        REQUIRE(bit_reader_init(&reader, out.data, out.length, padding) == LZFSE_ZAZZCAT_OK, "init");
        for (int i = 199; i >= 0; i--) {
            REQUIRE(bit_reader_refill(&reader) == LZFSE_ZAZZCAT_OK, "refill");
            CHECK(bit_reader_pull(&reader, counts[i]) == values[i], "value %d", i);
        }
        lzfse_zazzcat_buffer_free(&out);
    }
}

static void test_bit_reader_rejects_bad_starts(void) {
    uint8_t zeros[8] = {0};
    uint8_t top_bit[8] = {0, 0, 0, 0, 0, 0, 0, 0x80}, below_top[8] = {0, 0, 0, 0, 0, 0, 0, 0x40};
    bit_reader r = {0};
    CHECK(bit_reader_init(&r, zeros, 6, 0) != LZFSE_ZAZZCAT_OK, "6 bytes");
    CHECK(bit_reader_init(&r, zeros, 7, -1) != LZFSE_ZAZZCAT_OK, "7 bytes with padding");
    CHECK(bit_reader_init(&r, top_bit, 8, -1) != LZFSE_ZAZZCAT_OK, "stray bit in the padding");
    CHECK(bit_reader_init(&r, below_top, 8, -1) == LZFSE_ZAZZCAT_OK, "bit just below the padding");
    REQUIRE(bit_reader_init(&r, zeros, 8, -1) == LZFSE_ZAZZCAT_OK, "init");
    bare_bit_reader_pull(&r, 32);
    bare_bit_reader_pull(&r, 31);
    CHECK(bit_reader_refill(&r) != LZFSE_ZAZZCAT_OK, "reading before the start");
}

// --- FSE ---

static void test_normalize_sums_exactly(void) {
    uint32_t one[1] = {1}, skew[4] = {1, 0, 0, 1000000}, flat5[256], flat1[256], none[3] = {0, 0, 0};
    uint32_t mixed[20] = {7, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 90000};
    for (int i = 0; i < 256; i++) flat5[i] = 5, flat1[i] = 1;
    const uint32_t *cases[] = {one, skew, flat5, flat1, none, mixed};
    size_t sizes[] = {1, 4, 256, 256, 3, 20};
    uint32_t state_counts[] = {64, 256, 1024};
    for (int c = 0; c < 6; c++) {
        for (int s = 0; s < 3; s++) {
            size_t used = 0;
            for (size_t i = 0; i < sizes[c]; i++) used += cases[c][i] > 0;
            if (used > state_counts[s]) continue;
            uint16_t frequencies[256];
            fse_normalize(cases[c], sizes[c], state_counts[s], frequencies);
            uint32_t sum = 0;
            for (size_t i = 0; i < sizes[c]; i++) {
                sum += frequencies[i];
                CHECK((cases[c][i] > 0) == (frequencies[i] > 0), "case %d symbol %zu", c, i);
            }
            CHECK(sum == (used == 0 ? 0 : state_counts[s]), "case %d states %u sum %u", c, state_counts[s], sum);
        }
    }
}

static void test_fse_literals_round_trip(void) {
    uint32_t state_counts[] = {1024, 64, 256};
    size_t symbol_counts[] = {256, 20, 64};
    for (int c = 0; c < 3; c++) {
        uint32_t n = state_counts[c];
        uint8_t data[5000];
        for (uint32_t i = 0; i < 5000; i++) {
            data[i] = (uint8_t)(((i * i + i / 7) % symbol_counts[c]) % (1 + i % 11));
        }
        uint32_t counts[256] = {0};
        for (int i = 0; i < 5000; i++) counts[data[i]]++;
        uint16_t frequencies[256] = {0};
        fse_normalize(counts, symbol_counts[c], n, frequencies);
        fse_encoder_entry encoder[256];
        fse_encoder_table(frequencies, symbol_counts[c], n, encoder);
        lzfse_zazzcat_buffer out = {0};
        bare_buffer_reserve(&out, 16000);
        clear_bytes(out.data, 8);
        out.length = 8;
        bit_writer writer = {0, 0};
        uint32_t state = 0;
        for (int i = 4999; i >= 0; i--) {
            fse_encode(&state, &encoder[data[i]], &writer);
            bit_writer_flush(&writer, &out);
        }
        int32_t padding = bit_writer_finish(&writer, &out);
        // Pad the frequencies to a full literal table so the literal decoder can read them.
        uint16_t literal_frequencies[256] = {0};
        copy_bytes(literal_frequencies, frequencies, symbol_counts[c] * sizeof frequencies[0]);
        fse_literal_entry *table = raw_calloc(1024, sizeof *table);
        REQUIRE(fse_literal_table(literal_frequencies, n, table) == LZFSE_ZAZZCAT_OK, "table");
        bit_reader reader = {0};
        REQUIRE(bit_reader_init(&reader, out.data, out.length, padding) == LZFSE_ZAZZCAT_OK, "init");
        for (int i = 0; i < 5000; i++) {
            REQUIRE(bit_reader_refill(&reader) == LZFSE_ZAZZCAT_OK, "refill");
            uint8_t symbol = fse_decode_literal(&state, table, n - 1, &reader);
            if (symbol != data[i]) {
                CHECK(0, "states %u symbol %d", n, i);
                break;
            }
        }
        CHECK(state == 0, "the decoder ends where the encoder started");
        raw_free(table);
        lzfse_zazzcat_buffer_free(&out);
    }
}

static void test_single_symbol_uses_no_bits(void) {
    uint16_t frequencies[4] = {0, 0, 64, 0};
    fse_encoder_entry encoder[4];
    fse_encoder_table(frequencies, 4, 64, encoder);
    lzfse_zazzcat_buffer out = {0};
    bare_buffer_reserve(&out, 64);
    bit_writer writer = {0, 0};
    uint32_t state = 0;
    for (int i = 0; i < 100; i++) fse_encode(&state, &encoder[2], &writer);
    CHECK(bit_writer_finish(&writer, &out) == 0 && out.length == 0, "no bits written");
    lzfse_zazzcat_buffer_free(&out);
}

static void test_oversized_tables_are_rejected(void) {
    uint16_t literals[256] = {1000, 25};
    fse_literal_entry literal_table[1024];
    CHECK(fse_literal_table(literals, 1024, literal_table) != LZFSE_ZAZZCAT_OK, "literal table");
    uint16_t values[2] = {60, 5};
    uint8_t extra[2] = {0, 0};
    uint32_t base[2] = {0, 1};
    fse_value_entry value_table[64];
    CHECK(fse_value_table(values, 2, 64, extra, base, value_table) != LZFSE_ZAZZCAT_OK, "value table");
}

// --- LZVN ---

// LZVN-encode and decode `data`, check the result, and return the payload size.
static size_t lzvn_round_trip(const uint8_t *data, size_t n) {
    lzfse_zazzcat_buffer encoded = {0};
    CHECK(lzfse_zazzcat_lzvn_encode(data, n, &encoded) == LZFSE_ZAZZCAT_OK, "encode");
    uint8_t *decoded = raw_malloc(n + 1);
    CHECK(lzvn_decode_block(encoded.data, encoded.length, decoded, 0, n) == LZFSE_ZAZZCAT_OK &&
              (n == 0 || compare_bytes(decoded, data, n) == 0),
          "LZVN round trip of %zu bytes", n);
    size_t size = encoded.length;
    raw_free(decoded);
    lzfse_zazzcat_buffer_free(&encoded);
    return size;
}

// lzvn_round_trip for callers that ignore the payload size (see src/bare_functions.h).
static void bare_lzvn_round_trip(const uint8_t *data, size_t n) {
    (void)lzvn_round_trip(data, n);
}

static void test_lzvn_round_trips(void) {
    bare_lzvn_round_trip((const uint8_t *)"", 0);
    bare_lzvn_round_trip((const uint8_t *)"a", 1);
    bare_lzvn_round_trip((const uint8_t *)"abc", 3);
    bare_lzvn_round_trip((const uint8_t *)"hello hello hello hello world", 29);
    const char *line = "hello compressible text line\n";
    size_t line_length = strlen(line), text_length = line_length * 4000;
    uint8_t *text = raw_malloc(text_length);
    for (size_t i = 0; i < 4000; i++) copy_bytes(text + i * line_length, line, line_length);
    CHECK(lzvn_round_trip(text, text_length) < 2000, "repetitive text compresses");
    raw_free(text);
    uint8_t *run = raw_malloc(100000);
    fill_bytes(run, 7, 100000);
    bare_lzvn_round_trip(run, 100000);
    raw_free(run);
    // Far distances: pieces of the start repeated after 70,000 noise bytes.
    size_t far_length = 70000 + 2990 + 2000;
    uint8_t *far = raw_malloc(far_length);
    for (uint32_t i = 0; i < 70000; i++) far[i] = (uint8_t)((i * 2654435761u) >> 13);
    copy_bytes(far + 70000, far + 10, 2990);
    copy_bytes(far + 72990, far + 60000, 2000);
    bare_lzvn_round_trip(far, far_length);
    raw_free(far);
    uint8_t mixed[20000];
    for (uint32_t i = 0; i < 20000; i++) mixed[i] = i % 97 < 50 ? (uint8_t)(i % 7) : (uint8_t)(i * 31 % 251);
    bare_lzvn_round_trip(mixed, sizeof mixed);
}

static void test_lzvn_bad_streams_are_errors(void) {
    const char *phrase = "abcabcabcabcabcabc xyz xyz xyz";
    size_t phrase_length = strlen(phrase);
    uint8_t text[30 * 20];
    for (int i = 0; i < 20; i++) copy_bytes(text + i * phrase_length, phrase, phrase_length);
    lzfse_zazzcat_buffer encoded = {0};
    bare_lzfse_zazzcat_lzvn_encode(text, 20 * phrase_length, &encoded);
    uint8_t out[1000];
    size_t written;
    for (size_t cut = 0; cut < encoded.length; cut++) {
        CHECK(lzfse_zazzcat_lzvn_decode(encoded.data, cut, out, sizeof out, &written) != LZFSE_ZAZZCAT_OK, "cut at %zu",
              cut);
    }
    uint8_t reserved_ops[3] = {0x1E, 0x70, 0xD5};
    for (int i = 0; i < 3; i++) {
        uint8_t stream[9] = {reserved_ops[i], 0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(lzfse_zazzcat_lzvn_decode(stream, 9, out, sizeof out, &written) == LZFSE_ZAZZCAT_CORRUPT, "op %x",
              reserved_ops[i]);
    }
    uint8_t no_previous[9] = {0xF1, 6, 0, 0, 0, 0, 0, 0, 0};
    CHECK(lzfse_zazzcat_lzvn_decode(no_previous, 9, out, sizeof out, &written) == LZFSE_ZAZZCAT_CORRUPT,
          "repeat with no previous distance");
    uint8_t zero_distance[12] = {0xE1, 'a', 0x00, 2, 6, 0, 0, 0, 0, 0, 0, 0};
    CHECK(lzfse_zazzcat_lzvn_decode(zero_distance, 12, out, sizeof out, &written) == LZFSE_ZAZZCAT_CORRUPT,
          "distance 0");
    uint8_t too_long[12] = {0xE3, 1, 2, 3, 6, 0, 0, 0, 0, 0, 0, 0};
    uint8_t small[2];
    CHECK(lzfse_zazzcat_lzvn_decode(too_long, 12, small, 2, &written) == LZFSE_ZAZZCAT_OUTPUT_TOO_SMALL,
          "output too small");
    lzfse_zazzcat_buffer_free(&encoded);
}

// --- Copy kernels ---

static void test_match_lengths(void) {
    const uint8_t *buffer = (const uint8_t *)"abcdefghabcdefghabcdXfghabcdefgh";
    CHECK(match_length(buffer, 0, 8, 32) == 12, "forward from 8");
    CHECK(match_length(buffer, 0, 24, 32) == 8, "forward to the end");
    CHECK(match_length_backward(buffer, 8, 16, 100) == 8, "backward");
    CHECK(match_length_backward(buffer, 8, 16, 3) == 3, "backward limit");
}

static void test_copy_match_exact(void) {
    for (size_t d = 1; d < 20; d++) {
        for (size_t m = 0; m < 70; m++) {
            uint8_t a[180] = {0}, b[180] = {0};
            for (int i = 0; i < 100; i++) a[i] = b[i] = (uint8_t)i;
            copy_match_exact(a, 100, d, m);
            for (size_t i = 0; i < m; i++) b[100 + i] = b[100 + i - d];
            CHECK(compare_bytes(a, b, sizeof a) == 0, "d=%zu m=%zu", d, m);
        }
    }
}

static void test_wild_copy(void) {
    uint8_t source[120];
    for (int i = 0; i < 120; i++) source[i] = (uint8_t)i;
    for (size_t n = 0; n < 90; n++) {
        uint8_t destination[120] = {0};
        wild_copy(destination + 3, source + 5, n);
        CHECK(compare_bytes(destination + 3, source + 5, n) == 0, "n=%zu", n);
        // Nothing beyond the 15-byte slack is touched (steps of 16 from the start, the first always runs).
        size_t touched = n == 0 ? 16 : (n + 15) / 16 * 16;
        for (size_t i = 3 + touched; i < 120; i++) CHECK(destination[i] == 0, "n=%zu wrote at %zu", n, i);
    }
}

static void test_decode_token_fast_matches_reference(void) {
    uint8_t literals[120];
    for (int i = 0; i < 120; i++) literals[i] = (uint8_t)(100 + i);
    size_t literal_lengths[] = {0, 1, 7, 16, 17, 33};
    size_t match_lengths[] = {0, 1, 3, 8, 9, 16, 31, 64, 65, 90};
    for (size_t d = 1; d < 40; d++) {
        for (int li = 0; li < 6; li++) {
            for (int mi = 0; mi < 10; mi++) {
                size_t l = literal_lengths[li], m = match_lengths[mi];
                uint8_t a[360] = {0}, before[360], want[360];
                for (int i = 0; i < 60; i++) a[i] = (uint8_t)i;
                copy_bytes(before, a, sizeof a);
                bool done = decode_token_fast(a, sizeof a, 60, literals, sizeof literals, 5, l, d, m);
                if (d < 8 && m > 64) {
                    CHECK(!done && compare_bytes(a, before, sizeof a) == 0, "d=%zu l=%zu m=%zu should decline", d, l,
                          m);
                    continue;
                }
                CHECK(done, "d=%zu l=%zu m=%zu", d, l, m);
                copy_bytes(want, before, sizeof want);
                copy_bytes(want + 60, literals + 5, l);
                for (size_t i = 0; i < m; i++) want[60 + l + i] = want[60 + l + i - d];
                size_t end = 60 + l + m;
                CHECK(compare_bytes(a, want, end) == 0, "d=%zu l=%zu m=%zu", d, l, m);
                CHECK(compare_bytes(a + end + 16, before + end + 16, sizeof a - end - 16) == 0,
                      "wrote past the slack: d=%zu l=%zu m=%zu", d, l, m);
            }
        }
    }
    // Declines: distance 0 or beyond the start, too little room in the output or in the literals.
    uint8_t out[100] = {0}, zeros[100] = {0};
    CHECK(!decode_token_fast(out, 100, 10, literals, 120, 0, 2, 0, 4), "distance 0");
    CHECK(!decode_token_fast(out, 100, 10, literals, 120, 0, 2, 13, 4), "distance before the start");
    CHECK(!decode_token_fast(out, 100, 60, literals, 120, 0, 2, 5, 10), "output too short");
    CHECK(!decode_token_fast(out, 100, 10, literals, 120, 110, 2, 5, 4), "literals too short");
    CHECK(compare_bytes(out, zeros, sizeof out) == 0, "nothing written when declining");
}

// --- Whole streams ---

static void test_sizes_and_block_types(void) {
    lzfse_zazzcat_buffer stream = round_trip("", 0);
    CHECK(stream.length == 4 && first_magic(&stream) == MAGIC_END_OF_STREAM, "empty stream");
    lzfse_zazzcat_buffer_free(&stream);
    stream = round_trip("1234567", 7);
    CHECK(first_magic(&stream) == MAGIC_UNCOMPRESSED, "7 bytes stored raw");
    lzfse_zazzcat_buffer_free(&stream);
    lzfse_zazzcat_buffer text = words(4096), random = noise(100000, 2);
    stream = round_trip(text.data, 4095);
    CHECK(first_magic(&stream) == MAGIC_LZVN, "4095 bytes of text as LZVN");
    lzfse_zazzcat_buffer_free(&stream);
    stream = round_trip(random.data, 4095);
    CHECK(first_magic(&stream) == MAGIC_UNCOMPRESSED, "4095 noise bytes stored raw");
    lzfse_zazzcat_buffer_free(&stream);
    stream = round_trip(text.data, 4096);
    CHECK(first_magic(&stream) == MAGIC_LZFSE_V2, "4096 bytes of text as LZFSE");
    lzfse_zazzcat_buffer_free(&stream);
    stream = round_trip(random.data, 100000);
    CHECK(first_magic(&stream) == MAGIC_UNCOMPRESSED, "noise stored raw");
    lzfse_zazzcat_buffer_free(&stream);
    size_t sizes[] = {8, 9, 100, 4097, 65536, 65537, 200003};
    for (int i = 0; i < 7; i++) {
        lzfse_zazzcat_buffer w = words(sizes[i]), r = noise(sizes[i], (uint32_t)sizes[i]);
        lzfse_zazzcat_buffer a = round_trip(w.data, w.length), b = round_trip(r.data, r.length);
        lzfse_zazzcat_buffer_free(&w);
        lzfse_zazzcat_buffer_free(&r);
        lzfse_zazzcat_buffer_free(&a);
        lzfse_zazzcat_buffer_free(&b);
    }
    lzfse_zazzcat_buffer_free(&text);
    lzfse_zazzcat_buffer_free(&random);
}

static void test_compressible_inputs(void) {
    uint8_t *zeros = raw_calloc(1000000, 1);
    lzfse_zazzcat_buffer stream = round_trip(zeros, 1000000);
    CHECK(stream.length < 2000, "a million zeros in %zu bytes", stream.length);
    lzfse_zazzcat_buffer_free(&stream);
    raw_free(zeros);
    lzfse_zazzcat_buffer text = words(500000);
    stream = round_trip(text.data, text.length);
    CHECK(stream.length < text.length / 3, "%zu of %zu", stream.length, text.length);
    lzfse_zazzcat_buffer_free(&stream);
    lzfse_zazzcat_buffer_free(&text);
    const char *line = "hello compressible text line\n";
    size_t line_length = strlen(line);
    uint8_t *repeated = raw_malloc(line_length * 40000);
    for (int i = 0; i < 40000; i++) copy_bytes(repeated + i * line_length, line, line_length);
    stream = round_trip(repeated, line_length * 40000);
    CHECK(stream.length < 5000, "%zu", stream.length);
    lzfse_zazzcat_buffer_free(&stream);
    raw_free(repeated);
}

static void test_block_limits(void) {
    // Many short matches fill blocks to their token limit.
    lzfse_zazzcat_buffer r = noise(400000, 7);
    lzfse_zazzcat_buffer data = {0};
    for (int i = 0; i < 60000; i++) {
        bare_buffer_append(&data, r.data + i * 5, 3);
        bare_buffer_append(&data, "abcd", 4);
    }
    lzfse_zazzcat_buffer stream = round_trip(data.data, data.length);
    lzfse_zazzcat_buffer_free(&stream);
    // Mostly literals fill blocks to their literal limit.
    lzfse_zazzcat_buffer heavy = noise(300000, 9);
    for (size_t i = 0; i + 100 < heavy.length; i += 1000) move_bytes(heavy.data + i + 500, heavy.data + i, 8);
    stream = round_trip(heavy.data, heavy.length);
    lzfse_zazzcat_buffer_free(&stream);
    lzfse_zazzcat_buffer_free(&r);
    lzfse_zazzcat_buffer_free(&data);
    lzfse_zazzcat_buffer_free(&heavy);
}

static void test_far_matches(void) {
    lzfse_zazzcat_buffer data = noise(300000, 3);
    bare_buffer_reserve(&data, 30000);
    size_t length = data.length;
    // A copy from exactly the maximum distance back, then one from the start.
    copy_bytes(data.data + length, data.data + length - MAX_DISTANCE, 5000);
    data.length += 5000;
    copy_bytes(data.data + data.length, data.data + 10, 19990);
    data.length += 19990;
    lzfse_zazzcat_buffer stream = round_trip(data.data, data.length);
    CHECK(stream.length < data.length, "far repeats are found");
    lzfse_zazzcat_buffer_free(&stream);
    lzfse_zazzcat_buffer_free(&data);
}

static void test_options_round_trip(void) {
    lzfse_zazzcat_buffer data = words(150000);
    uint32_t hash_bits[] = {10, 14, 16};
    size_t lazy[] = {0, 8, 64};
    uint32_t skip[] = {0, 2, 6};
    for (int h = 0; h < 3; h++) {
        for (int l = 0; l < 3; l++) {
            for (int s = 0; s < 3; s++) {
                lzfse_zazzcat_options options = {hash_bits[h], 24, lazy[l], skip[s]};
                lzfse_zazzcat_buffer stream = {0}, decoded = {0};
                CHECK(lzfse_zazzcat_encode(data.data, data.length, &options, &stream) == LZFSE_ZAZZCAT_OK, "encode");
                CHECK(decode_buffer(stream.data, stream.length, data.length, &decoded) == LZFSE_ZAZZCAT_OK &&
                          same_bytes(decoded.data, decoded.length, data.data, data.length),
                      "hash %u lazy %zu skip %u", hash_bits[h], lazy[l], skip[s]);
                lzfse_zazzcat_buffer_free(&stream);
                lzfse_zazzcat_buffer_free(&decoded);
            }
        }
    }
    lzfse_zazzcat_buffer_free(&data);
}

static void test_v1_headers_decode(void) {
    lzfse_zazzcat_buffer data = words(50000), stream = encode_buffer(data.data, data.length);
    REQUIRE(first_magic(&stream) == MAGIC_LZFSE_V2, "one LZFSE block");
    block_header h;
    size_t size;
    REQUIRE(parse_v2_header(stream.data, stream.length, &h, &size) == LZFSE_ZAZZCAT_OK, "parse");
    size_t payload = h.literal_payload_size + h.token_payload_size;
    REQUIRE(size + payload == stream.length - 4, "one block then the end marker");
    // Rewrite the block with a v1 header and the same payload.
    lzfse_zazzcat_buffer v1 = {0}, decoded = {0};
    bare_write_v1_header(&h, &v1);
    bare_buffer_append(&v1, stream.data + size, payload);
    uint8_t end[4];
    store_u32(end, MAGIC_END_OF_STREAM);
    bare_buffer_append(&v1, end, 4);
    CHECK(decode_buffer(v1.data, v1.length, data.length, &decoded) == LZFSE_ZAZZCAT_OK &&
              same_bytes(decoded.data, decoded.length, data.data, data.length),
          "v1 decode");
    lzfse_zazzcat_buffer_free(&data);
    lzfse_zazzcat_buffer_free(&stream);
    lzfse_zazzcat_buffer_free(&v1);
    lzfse_zazzcat_buffer_free(&decoded);
}

static void test_multi_block_streams(void) {
    lzfse_zazzcat_buffer text = words(10000), tail = noise(10, 4);
    lzfse_zazzcat_buffer a = encode_buffer(text.data, text.length), b = encode_buffer(tail.data, tail.length);
    // The first stream without its end marker, then the second: one stream of both.
    lzfse_zazzcat_buffer joined = copy_of(a.data, a.length - 4), want = copy_of(text.data, text.length);
    bare_buffer_append(&joined, b.data, b.length);
    bare_buffer_append(&want, tail.data, tail.length);
    lzfse_zazzcat_buffer decoded = {0};
    CHECK(decode_buffer(joined.data, joined.length, want.length, &decoded) == LZFSE_ZAZZCAT_OK &&
              same_bytes(decoded.data, decoded.length, want.data, want.length),
          "joined streams");
    lzfse_zazzcat_buffer *all[] = {&text, &tail, &a, &b, &joined, &want, &decoded};
    for (int i = 0; i < 7; i++) lzfse_zazzcat_buffer_free(all[i]);
}

static void test_hostile_input(void) {
    lzfse_zazzcat_buffer data = words(20000), stream = encode_buffer(data.data, data.length), out = {0};
    uint8_t *buffer = raw_malloc(data.length);
    size_t written;
    for (size_t cut = 0; cut < stream.length; cut += 37) {
        CHECK(decode_buffer(stream.data, cut, SIZE_MAX, &out) != LZFSE_ZAZZCAT_OK, "cut at %zu", cut);
    }
    // Bit flips and random bytes must give an error or some output, never a crash (the sanitizer build checks
    // that no access goes out of bounds).
    for (size_t i = 0; i < stream.length; i += 7) {
        uint8_t flips[3] = {0x01, 0x10, 0x80};
        for (int f = 0; f < 3; f++) {
            lzfse_zazzcat_buffer bad = copy_of(stream.data, stream.length);
            bad.data[i] ^= flips[f];
            bare_decode_buffer(bad.data, bad.length, 1 << 20, &out);
            bare_lzfse_zazzcat_decode_into(bad.data, bad.length, buffer, data.length, &written);
            lzfse_zazzcat_buffer_free(&bad);
        }
    }
    uint32_t magics[4] = {MAGIC_LZFSE_V2, MAGIC_LZVN, MAGIC_LZFSE_V1, MAGIC_UNCOMPRESSED};
    for (uint32_t seed = 0; seed < 200; seed++) {
        lzfse_zazzcat_buffer junk = noise(64 + seed * 3, seed);
        store_u32(junk.data, magics[seed % 4]);
        bare_decode_buffer(junk.data, junk.length, 1 << 20, &out);
        bare_lzfse_zazzcat_decode_into(junk.data, junk.length, buffer, data.length, &written);
        lzfse_zazzcat_buffer_free(&junk);
    }
    CHECK(decode_buffer(stream.data, stream.length, data.length - 1, &out) == LZFSE_ZAZZCAT_OUTPUT_TOO_SMALL, "limit");
    uint8_t small[100];
    CHECK(lzfse_zazzcat_decode_into(stream.data, stream.length, small, sizeof small, &written) ==
              LZFSE_ZAZZCAT_OUTPUT_TOO_SMALL,
          "small buffer");
    CHECK(decode_buffer("bvx?", 4, 10, &out) == LZFSE_ZAZZCAT_CORRUPT, "unknown magic");
    CHECK(decode_buffer("bv", 2, 10, &out) == LZFSE_ZAZZCAT_TRUNCATED, "truncated magic");
    raw_free(buffer);
    lzfse_zazzcat_buffer_free(&data);
    lzfse_zazzcat_buffer_free(&stream);
    lzfse_zazzcat_buffer_free(&out);
}

static void test_random_round_trips(void) {
    // Random lengths and contents, with random repetition, as the Rust property tests did.
    test_random r = {2024};
    for (int c = 0; c < 64; c++) {
        size_t n = test_random_next(&r) % 20000;
        lzfse_zazzcat_buffer piece = noise(n, test_random_next(&r));
        // Make some cases compressible by narrowing the byte range.
        if (c % 2) {
            for (size_t i = 0; i < n; i++) piece.data[i] %= 4;
        }
        lzfse_zazzcat_buffer data = copy_of(piece.data, piece.length);
        for (uint32_t k = test_random_next(&r) % 4; k > 0; k--) bare_buffer_append(&data, piece.data, piece.length);
        lzfse_zazzcat_buffer stream = round_trip(data.data, data.length);
        lzfse_zazzcat_buffer_free(&stream);
        lzfse_zazzcat_buffer_free(&data);
        lzfse_zazzcat_buffer_free(&piece);
    }
}

static void test_random_input_never_crashes(void) {
    test_random r = {77};
    uint32_t magics[4] = {MAGIC_LZFSE_V2, MAGIC_LZVN, MAGIC_LZFSE_V1, MAGIC_UNCOMPRESSED};
    lzfse_zazzcat_buffer out = {0};
    for (int c = 0; c < 64; c++) {
        lzfse_zazzcat_buffer junk = noise(test_random_next(&r) % 4000, test_random_next(&r));
        uint32_t pick = test_random_next(&r) % 5;
        if (junk.length >= 4 && pick < 4) store_u32(junk.data, magics[pick]);
        bare_decode_buffer(junk.data, junk.length, 1 << 20, &out);
        lzfse_zazzcat_buffer_free(&junk);
    }
    lzfse_zazzcat_buffer_free(&out);
}

// --- Encoder paths ---

static void test_small_path_boundaries(void) {
    size_t sizes[] = {4096, 8192, 8193, 16384, 16385, 32768, 32769, 65535, 65536, 65537};
    for (int s = 0; s < 10; s++) {
        size_t n = sizes[s], quarter = n / 4;
        uint8_t *data = raw_malloc(n);
        for (uint32_t j = 0; j < n; j++) data[j] = (uint8_t)((j * 2654435761u) >> 13);
        // The last quarter repeats the first, so a match must reach across the whole input.
        copy_bytes(data + n - quarter, data, quarter);
        lzfse_zazzcat_buffer stream = round_trip(data, n);
        CHECK(stream.length < n - quarter + n / 16, "size %zu: far repeat missed", n);
        lzfse_zazzcat_buffer_free(&stream);
        raw_free(data);
    }
}

static void test_reused_encoder_matches_fresh(void) {
    lzfse_zazzcat_encoder *encoder = lzfse_zazzcat_encoder_create(NULL);
    for (uint32_t i = 0; i < 40; i++) {
        size_t n = 5000 + i * 997;
        uint8_t *data = raw_malloc(n);
        for (uint32_t j = 0; j < n; j++) data[j] = (uint8_t)(((j * (i + 3)) % 251) ^ (j / 13));
        lzfse_zazzcat_buffer reused = {0}, fresh = encode_buffer(data, n);
        CHECK(lzfse_zazzcat_encoder_encode(encoder, data, n, &reused) == LZFSE_ZAZZCAT_OK, "encode");
        CHECK(same_bytes(reused.data, reused.length, fresh.data, fresh.length), "input %u", i);
        lzfse_zazzcat_buffer_free(&reused);
        lzfse_zazzcat_buffer_free(&fresh);
        raw_free(data);
    }
    lzfse_zazzcat_encoder_destroy(encoder);
}

static void test_chunks_round_trip_in_order(void) {
    size_t n = 300000;
    uint8_t *data = raw_malloc(n);
    for (uint32_t i = 0; i < n; i++) data[i] = (uint8_t)(((i / 3) % 251) ^ (i % 7));
    lzfse_zazzcat_buffer out = copy_of("prefix", 6), decoded = {0};
    lzfse_zazzcat_offsets offsets = {0};
    REQUIRE(lzfse_zazzcat_encode_chunks(data, n, 65536, NULL, &out, &offsets) == LZFSE_ZAZZCAT_OK, "encode");
    CHECK(offsets.length == 6 && offsets.data[0] == 6, "5 chunks after the prefix");
    for (size_t i = 0; i + 1 < offsets.length; i++) {
        size_t start = offsets.data[i], end = offsets.data[i + 1];
        // Each chunk is its own complete stream, identical to encoding the chunk alone.
        lzfse_zazzcat_buffer alone = encode_buffer(data + i * 65536, min_size(65536, n - i * 65536));
        CHECK(same_bytes(out.data + start, end - start, alone.data, alone.length), "chunk %zu", i);
        lzfse_zazzcat_buffer_free(&alone);
        CHECK(lzfse_zazzcat_decode(out.data + start, end - start, 65536, &decoded) == LZFSE_ZAZZCAT_OK, "decode");
    }
    CHECK(same_bytes(decoded.data, decoded.length, data, n), "chunks decode in order");
    lzfse_zazzcat_buffer empty = {0};
    CHECK(lzfse_zazzcat_encode_chunks(data, 0, 10, NULL, &empty, &offsets) == LZFSE_ZAZZCAT_OK && offsets.length == 1,
          "no chunks for empty input");
    CHECK(lzfse_zazzcat_encode_chunks(data, n, 0, NULL, &empty, &offsets) == LZFSE_ZAZZCAT_INVALID_ARGUMENT,
          "chunk size 0");
    lzfse_zazzcat_buffer_free(&out);
    lzfse_zazzcat_buffer_free(&decoded);
    lzfse_zazzcat_buffer_free(&empty);
    lzfse_zazzcat_offsets_free(&offsets);
    raw_free(data);
}

// --- decmpfs and statistics ---

static void test_decmpfs_layouts_round_trip(void) {
    const char *phrase = "tiny file contents ";
    lzfse_zazzcat_buffer small = {0}, out = {0};
    for (int i = 0; i < 20; i++) bare_buffer_append(&small, phrase, strlen(phrase));
    lzfse_zazzcat_decmpfs c;
    REQUIRE(lzfse_zazzcat_decmpfs_compress(small.data, small.length, &c) == LZFSE_ZAZZCAT_OK, "compress");
    uint32_t type;
    uint64_t size;
    CHECK(lzfse_zazzcat_decmpfs_parse_header(c.xattr.data, c.xattr.length, &type, &size) == LZFSE_ZAZZCAT_OK &&
              type == LZFSE_ZAZZCAT_DECMPFS_LZFSE_INLINE && size == small.length && c.fork.length == 0,
          "small files go inline");
    CHECK(lzfse_zazzcat_decmpfs_decompress(c.xattr.data, c.xattr.length, c.fork.data, c.fork.length, UINT64_MAX,
                                           &out) == LZFSE_ZAZZCAT_OK &&
              same_bytes(out.data, out.length, small.data, small.length),
          "inline round trip");
    lzfse_zazzcat_decmpfs_free(&c);
    size_t n = 200000;
    uint8_t *big = raw_malloc(n);
    for (uint32_t i = 0; i < n; i++) big[i] = (uint8_t)(i % 13 * (i / 1000 % 7));
    REQUIRE(lzfse_zazzcat_decmpfs_compress(big, n, &c) == LZFSE_ZAZZCAT_OK, "compress");
    bare_lzfse_zazzcat_decmpfs_parse_header(c.xattr.data, c.xattr.length, &type, &size);
    CHECK(type == LZFSE_ZAZZCAT_DECMPFS_LZFSE_FORK, "large files go in the resource fork");
    CHECK(load_u32(c.fork.data) == 20, "4 chunks need 5 offsets");
    out.length = 0;
    CHECK(lzfse_zazzcat_decmpfs_decompress(c.xattr.data, c.xattr.length, c.fork.data, c.fork.length, UINT64_MAX,
                                           &out) == LZFSE_ZAZZCAT_OK &&
              same_bytes(out.data, out.length, big, n),
          "fork round trip");
    CHECK(lzfse_zazzcat_decmpfs_decompress(c.xattr.data, c.xattr.length, c.fork.data, c.fork.length, 10, &out) ==
              LZFSE_ZAZZCAT_OUTPUT_TOO_SMALL,
          "limit");
    CHECK(lzfse_zazzcat_decmpfs_decompress(c.xattr.data, c.xattr.length, c.fork.data, 20, UINT64_MAX, &out) !=
              LZFSE_ZAZZCAT_OK,
          "truncated fork");
    lzfse_zazzcat_decmpfs_free(&c);
    lzfse_zazzcat_buffer_free(&small);
    lzfse_zazzcat_buffer_free(&out);
    raw_free(big);
}

static void test_stats_totals_match_the_input(void) {
    const char *phrase = "the quick brown fox jumps over the lazy dog ";
    lzfse_zazzcat_buffer text = {0};
    while (text.length < 300000) bare_buffer_append(&text, phrase, strlen(phrase));
    text.length = 300000;
    lzfse_zazzcat_buffer random = noise(20000, 5);
    struct {
        const uint8_t *data;
        size_t length;
    } cases[] = {
        {text.data, text.length}, {random.data, random.length}, {text.data, 1000}, {(const uint8_t *)"abc", 3}};
    for (int c = 0; c < 4; c++) {
        lzfse_zazzcat_buffer stream = encode_buffer(cases[c].data, cases[c].length);
        lzfse_zazzcat_stream_stats s;
        clear_bytes(&s, sizeof s);
        CHECK(lzfse_zazzcat_stream_stats_add(&s, stream.data, stream.length) == LZFSE_ZAZZCAT_OK, "stats");
        CHECK(s.decoded_bytes == cases[c].length && s.stored_bytes == stream.length, "case %d totals", c);
        CHECK(s.header_bytes + s.literal_bytes + s.match_stream_bytes + s.other_block_bytes + 4 == s.stored_bytes,
              "case %d parts add up", c);
        // In all-LZFSE streams every byte is a literal or matched; each block pads its literals to a multiple of 4.
        if (s.blocks[0] > 0 && s.blocks[1] == 0 && s.blocks[2] == 0) {
            uint64_t matched = 0;
            for (int b = 0; b < LZFSE_ZAZZCAT_LENGTH_BUCKETS; b++) matched += s.bytes_by_length[b];
            uint64_t unpadded = s.decoded_bytes - matched;
            CHECK(s.literals >= unpadded && s.literals < unpadded + 4 * s.blocks[0], "case %d literals", c);
        }
        lzfse_zazzcat_buffer_free(&stream);
    }
    lzfse_zazzcat_buffer_free(&text);
    lzfse_zazzcat_buffer_free(&random);
}

// The Apple interoperability tests (they define run_apple_tests, which does nothing on other systems).
#include "apple_tests.c"

int main(void) {
    RUN_TEST(test_fast_symbols_match_search);
    RUN_TEST(test_frequency_codes_round_trip);
    RUN_TEST(test_symbol_boundaries);
    RUN_TEST(test_headers_round_trip);
    RUN_TEST(test_bad_headers_are_errors);
    RUN_TEST(test_bits_read_back_in_reverse);
    RUN_TEST(test_bit_reader_rejects_bad_starts);
    RUN_TEST(test_normalize_sums_exactly);
    RUN_TEST(test_fse_literals_round_trip);
    RUN_TEST(test_single_symbol_uses_no_bits);
    RUN_TEST(test_oversized_tables_are_rejected);
    RUN_TEST(test_lzvn_round_trips);
    RUN_TEST(test_lzvn_bad_streams_are_errors);
    RUN_TEST(test_match_lengths);
    RUN_TEST(test_copy_match_exact);
    RUN_TEST(test_wild_copy);
    RUN_TEST(test_decode_token_fast_matches_reference);
    RUN_TEST(test_sizes_and_block_types);
    RUN_TEST(test_compressible_inputs);
    RUN_TEST(test_block_limits);
    RUN_TEST(test_far_matches);
    RUN_TEST(test_options_round_trip);
    RUN_TEST(test_v1_headers_decode);
    RUN_TEST(test_multi_block_streams);
    RUN_TEST(test_hostile_input);
    RUN_TEST(test_random_round_trips);
    RUN_TEST(test_random_input_never_crashes);
    RUN_TEST(test_small_path_boundaries);
    RUN_TEST(test_reused_encoder_matches_fresh);
    RUN_TEST(test_chunks_round_trip_in_order);
    RUN_TEST(test_decmpfs_layouts_round_trip);
    RUN_TEST(test_stats_totals_match_the_input);
    run_apple_tests();
    if (test_failures) {
        bare_printf("%d check(s) failed\n", test_failures);
        return 1;
    }
    bare_printf("all tests passed\n");
    return 0;
}
