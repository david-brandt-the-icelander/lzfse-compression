// LZVN decoder and encoder.
//
// An LZVN stream is a sequence of opcodes. Each opcode carries some literal bytes (copied from the stream) and/or
// a match (bytes copied from earlier output), in one of several compact layouts chosen by the opcode byte.
#include "lzvn.h"

#include "copy.h"

#define END_OF_STREAM_OPCODE 0x06
#define MAX_LZVN_DISTANCE    0xFFFF
#define MAX_HASH_BITS        14

// Opcodes that are reserved (undefined) and must be rejected.
static bool is_undefined_opcode(uint8_t op) {
    return op == 0x1E || op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || (op >= 0x70 && op <= 0x7F) ||
           (op >= 0xD0 && op <= 0xDF);
}

// What one opcode asks for. `distance` is NO_MATCH for literal-only opcodes and REUSE_DISTANCE for "the previous
// distance".
typedef struct lzvn_opcode {
    size_t opcode_length;
    size_t literal_length;
    size_t match_length;
    size_t distance;
} lzvn_opcode;

#define NO_MATCH       SIZE_MAX
#define REUSE_DISTANCE (SIZE_MAX - 1)

// Run opcodes from `source` into `output` starting at `position` until end-of-stream. Store the number of source
// bytes used and the output end position.
static lzfse_zazzcat_status decode_run(const uint8_t *source, size_t source_length, uint8_t *output,
                                       size_t output_capacity, size_t position, size_t *bytes_read,
                                       size_t *output_end) {
    size_t at = 0;
    size_t previous_distance = 0;
    for (;;) {
        if (at >= source_length) return LZFSE_ZAZZCAT_TRUNCATED;
        uint8_t op = source[at];
        size_t available = source_length - at;
        lzvn_opcode o = {1, 0, 0, NO_MATCH};
        if (op == END_OF_STREAM_OPCODE) {
            // End of stream: the opcode plus 7 zero bytes.
            if (available < 8) return LZFSE_ZAZZCAT_TRUNCATED;
            *bytes_read = at + 8;
            *output_end = position;
            return LZFSE_ZAZZCAT_OK;
        } else if (op == 0x0E || op == 0x16) {
            // No-op.
        } else if (op == 0xE0) {
            // Literals: 16 + the next byte.
            if (available < 2) return LZFSE_ZAZZCAT_TRUNCATED;
            o = (lzvn_opcode){2, 16 + (size_t)source[at + 1], 0, NO_MATCH};
        } else if (op >= 0xE1 && op <= 0xEF) {
            // Literals: 1 to 15.
            o = (lzvn_opcode){1, op & 0x0F, 0, NO_MATCH};
        } else if (op == 0xF0) {
            // Match at the previous distance: 16 + the next byte.
            if (available < 2) return LZFSE_ZAZZCAT_TRUNCATED;
            o = (lzvn_opcode){2, 0, 16 + (size_t)source[at + 1], REUSE_DISTANCE};
        } else if (op >= 0xF1) {
            // Match at the previous distance: 1 to 15.
            o = (lzvn_opcode){1, 0, op & 0x0F, REUSE_DISTANCE};
        } else if (is_undefined_opcode(op)) {
            return LZFSE_ZAZZCAT_CORRUPT;
        } else if (op >= 0xA0 && op <= 0xBF) {
            // Medium distance: 0-3 literals, match length 3-34, 14-bit distance.
            if (available < 3) return LZFSE_ZAZZCAT_TRUNCATED;
            size_t word = load_u16(source + at + 1);
            o = (lzvn_opcode){3, (op >> 3) & 3, ((((size_t)op & 7) << 2) | (word & 3)) + 3, word >> 2};
        } else {
            // Small and large distance, or previous distance: 0-3 literals, match length 3-10.
            size_t literals = op >> 6, match = ((op >> 3) & 7) + 3;
            size_t low_bits = op & 7;
            if (low_bits == 7) {
                if (available < 3) return LZFSE_ZAZZCAT_TRUNCATED;
                o = (lzvn_opcode){3, literals, match, load_u16(source + at + 1)};
            } else if (low_bits == 6) {
                o = (lzvn_opcode){1, literals, match, REUSE_DISTANCE};
            } else {
                if (available < 2) return LZFSE_ZAZZCAT_TRUNCATED;
                o = (lzvn_opcode){2, literals, match, (low_bits << 8) | source[at + 1]};
            }
        }
        // Every opcode but end-of-stream needs at least one more byte after its literals.
        if (available <= o.opcode_length + o.literal_length) return LZFSE_ZAZZCAT_TRUNCATED;
        at += o.opcode_length;
        if (o.literal_length > 0) {
            if (o.literal_length > output_capacity - position) return LZFSE_ZAZZCAT_OUTPUT_TOO_SMALL;
            memcpy(output + position, source + at, o.literal_length);
            at += o.literal_length;
            position += o.literal_length;
        }
        if (o.distance != NO_MATCH) {
            size_t distance = o.distance == REUSE_DISTANCE ? previous_distance : o.distance;
            if (distance == 0 || distance > position) return LZFSE_ZAZZCAT_CORRUPT;
            if (o.match_length > output_capacity - position) return LZFSE_ZAZZCAT_OUTPUT_TOO_SMALL;
            copy_match_exact(output, position, distance, o.match_length);
            position += o.match_length;
            previous_distance = distance;
        }
    }
}

lzfse_zazzcat_status lzvn_decode_block(const uint8_t *payload, size_t payload_length, uint8_t *output, size_t position,
                                       size_t end) {
    size_t used, output_end;
    lzfse_zazzcat_status status = decode_run(payload, payload_length, output, end, position, &used, &output_end);
    if (status != LZFSE_ZAZZCAT_OK) return status;
    if (used != payload_length || output_end != end) return LZFSE_ZAZZCAT_CORRUPT;
    return LZFSE_ZAZZCAT_OK;
}

lzfse_zazzcat_status lzfse_zazzcat_lzvn_decode(const void *source, size_t source_length, void *destination,
                                               size_t destination_capacity, size_t *decoded_length) {
    size_t used;
    return decode_run(source, source_length, destination, destination_capacity, 0, &used, decoded_length);
}

// Largest match length a short opcode (3-bit length) can carry alongside `literal_count` literals: some longer
// combinations would collide with reserved opcodes.
static size_t max_short_match(size_t literal_count) {
    static const size_t limits[4] = {10, 8, 6, 4};
    return limits[literal_count];
}

// Append literal-only opcodes for `count` bytes at `literals`.
static void emit_literals(lzfse_zazzcat_buffer *out, const uint8_t *literals, size_t count) {
    while (count > 0) {
        size_t n = min_size(count, 271);
        if (n >= 16) {
            buffer_push_2_reserved(out, 0xE0, (uint8_t)(n - 16));
        } else {
            buffer_push_reserved(out, (uint8_t)(0xE0 | n));
        }
        buffer_append_reserved(out, literals, n);
        literals += n;
        count -= n;
    }
}

// Append match-only opcodes that repeat the previous distance for `length` bytes.
static void emit_repeat(lzfse_zazzcat_buffer *out, size_t length) {
    while (length > 0) {
        size_t n = min_size(length, 271);
        if (n >= 16) {
            buffer_push_2_reserved(out, 0xF0, (uint8_t)(n - 16));
        } else {
            buffer_push_reserved(out, (uint8_t)(0xF0 | n));
        }
        length -= n;
    }
}

// Append the opcodes for `literal_count` literals followed by a match of `length` (>= 3) bytes at `distance`
// (<= 65535). Up to 3 literals ride along in the match opcode; more go in a literal opcode of their own.
static void emit_token(lzfse_zazzcat_buffer *out, const uint8_t *literals, size_t literal_count, size_t length,
                       size_t distance, size_t previous_distance) {
    size_t inline_count = literal_count;
    if (literal_count > 3) {
        emit_literals(out, literals, literal_count);
        inline_count = 0;
    }
    if (distance == previous_distance && inline_count == 0) {
        emit_repeat(out, length);
        return;
    }
    size_t first;
    size_t l = inline_count;
    if (distance == previous_distance) {
        first = min_size(length, max_short_match(l));
        buffer_push_reserved(out, (uint8_t)((l << 6) | ((first - 3) << 3) | 6));
    } else if (distance < 16384 && (length > max_short_match(l) || distance >= 1536)) {
        first = min_size(length, 34);
        size_t word = (distance << 2) | ((first - 3) & 3);
        buffer_push_3_reserved(out, (uint8_t)(0xA0 | (l << 3) | ((first - 3) >> 2)), (uint8_t)word,
                               (uint8_t)(word >> 8));
    } else if (distance < 1536) {
        first = min_size(length, max_short_match(l));
        buffer_push_2_reserved(out, (uint8_t)((l << 6) | ((first - 3) << 3) | (distance >> 8)), (uint8_t)distance);
    } else {
        first = min_size(length, max_short_match(l));
        buffer_push_3_reserved(out, (uint8_t)((l << 6) | ((first - 3) << 3) | 7), (uint8_t)distance,
                               (uint8_t)(distance >> 8));
    }
    if (inline_count > 0) buffer_append_reserved(out, literals, inline_count);
    emit_repeat(out, length - first);
}

// Hash 4 bytes into a table index (multiplicative hashing: the top bits of the product).
ALWAYS_INLINE size_t lzvn_hash(uint32_t four_bytes, uint32_t shift) {
    return (size_t)((four_bytes * 0x9E3779B1u) >> shift);
}

lzfse_zazzcat_status lzvn_encode_with_table(const uint8_t *source, size_t n, lzfse_zazzcat_buffer *out,
                                            lzvn_hash_table *table) {
    // Every input byte costs at most 2 output bytes (a literal plus its share of an opcode), plus the trailer.
    if (!buffer_reserve(out, 2 * n + 64)) return LZFSE_ZAZZCAT_OUT_OF_MEMORY;
    // Small inputs get small tables: about one bucket per 4 input bytes, between 2^8 and 2^14.
    uint32_t bits = n == 0 ? 0 : (uint32_t)(64 - __builtin_clzll((unsigned long long)n));
    bits = bits >= 2 ? bits - 2 : 0;
    if (bits < 8) bits = 8;
    if (bits > MAX_HASH_BITS) bits = MAX_HASH_BITS;
    size_t entries = (size_t)1 << bits;
    if (table->capacity < entries) {
        uint32_t *grown = zz_realloc(table->entries, entries * sizeof *grown);
        if (grown == NULL) return LZFSE_ZAZZCAT_OUT_OF_MEMORY;
        table->entries = grown;
        table->capacity = entries;
    }
    // Entries hold position + 1, so 0 means empty.
    uint32_t *slots = table->entries;
    memset(slots, 0, entries * sizeof *slots);
    uint32_t shift = 32 - bits;
    size_t i = 0, literal_start = 0, previous_distance = 0;
    while (i + 4 <= n) {
        uint32_t four = load_u32(source + i);
        size_t h = lzvn_hash(four, shift);
        size_t candidate = slots[h];
        slots[h] = (uint32_t)(i + 1);
        size_t best_length = 0, best_distance = 0;
        if (previous_distance > 0 && previous_distance <= i) {
            size_t length = match_length(source, i - previous_distance, i, n);
            if (length >= 3) {
                best_length = length;
                best_distance = previous_distance;
            }
        }
        if (candidate > 0) {
            size_t earlier = candidate - 1;
            size_t distance = i - earlier;
            if (distance <= MAX_LZVN_DISTANCE && load_u32(source + earlier) == four) {
                size_t length = 4 + match_length(source, earlier + 4, i + 4, n);
                if (length > best_length + 1) {
                    best_length = length;
                    best_distance = distance;
                }
            }
        }
        if (best_length < 3) {
            i++;
            continue;
        }
        // Extend backwards over pending literals.
        size_t back = match_length_backward(source, i - best_distance, i, i - literal_start);
        size_t start = i - back;
        size_t length = best_length + back;
        emit_token(out, source + literal_start, start - literal_start, length, best_distance, previous_distance);
        previous_distance = best_distance;
        size_t end = start + length;
        for (size_t j = i + 1; j + 4 <= n && j < end; j++)
            slots[lzvn_hash(load_u32(source + j), shift)] = (uint32_t)(j + 1);
        i = end;
        literal_start = end;
    }
    emit_literals(out, source + literal_start, n - literal_start);
    static const uint8_t trailer[8] = {END_OF_STREAM_OPCODE, 0, 0, 0, 0, 0, 0, 0};
    buffer_append_reserved(out, trailer, 8);
    return LZFSE_ZAZZCAT_OK;
}

lzfse_zazzcat_status lzfse_zazzcat_lzvn_encode(const void *source, size_t source_length, lzfse_zazzcat_buffer *output) {
    lzvn_hash_table table = {NULL, 0};
    lzfse_zazzcat_status status = lzvn_encode_with_table(source, source_length, output, &table);
    zz_free(table.entries);
    return status;
}
