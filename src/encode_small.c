// Match finder for inputs of at most 64 KiB (every APFS chunk and most files): 16-bit positions in tables small
// enough to stay in the L1 cache, cleared for each input, and no distance limit to check because every distance
// is below MAX_DISTANCE.
#include "encode_internal.h"

#define SMALL_BUCKET_WAYS 4

// One bucket of the 4-byte table: the most recent positions whose 4 bytes hash here (newest first), and those
// 4 bytes.
typedef struct small_bucket {
    uint16_t positions[SMALL_BUCKET_WAYS];
    uint32_t values[SMALL_BUCKET_WAYS];
} small_bucket;

// The finder's state for one input. The 4-byte table has 2^bits buckets and the 8-byte table 2^(bits+1) slots.
typedef struct small_finder {
    const uint8_t *source;
    size_t source_length;
    small_bucket *buckets;
    uint16_t *long_slots;
    size_t good_match;
} small_finder;

// Hash 4 bytes into one of 2^bits buckets.
ALWAYS_INLINE size_t small_bucket_index(uint32_t four_bytes, uint32_t bits) {
    return (size_t)((four_bytes * 0x9E3779B1u) >> (32 - bits));
}

// Hash 8 bytes into one of 2^(bits+1) long-table slots.
ALWAYS_INLINE size_t small_long_index(uint64_t eight_bytes, uint32_t bits) {
    return (size_t)((eight_bytes * 0x9E3779B97F4A7C15ull) >> (63 - bits));
}

// Record `position` in its bucket of the 4-byte table, evicting the oldest entry.
ALWAYS_INLINE void small_insert_short(small_finder *f, size_t position, uint32_t four_bytes, uint32_t bits) {
    small_bucket *b = &f->buckets[small_bucket_index(four_bytes, bits)];
    for (int k = SMALL_BUCKET_WAYS - 1; k > 0; k--) {
        b->positions[k] = b->positions[k - 1];
        b->values[k] = b->values[k - 1];
    }
    b->positions[0] = (uint16_t)position;
    b->values[0] = four_bytes;
}

// Record `position` in both tables.
ALWAYS_INLINE void small_insert(small_finder *f, size_t position, uint32_t bits) {
    uint64_t eight = load_u64(f->source + position);
    small_insert_short(f, position, (uint32_t)eight, bits);
    f->long_slots[small_long_index(eight, bits)] = (uint16_t)position;
}

// Record `position` in the long table only.
ALWAYS_INLINE void small_insert_long(small_finder *f, size_t position, uint32_t bits) {
    f->long_slots[small_long_index(load_u64(f->source + position), bits)] = (uint16_t)position;
}

// Find the best match at `position` among the previous distance, the long table and the bucket; then record
// `position`. Every table entry is a genuine earlier position (empty ones hold position 0 and its real bytes),
// so a candidate only needs to lie before `position`.
ALWAYS_INLINE match small_find(small_finder *f, size_t position, size_t literal_start, size_t previous_distance,
                               uint32_t bits) {
    const uint8_t *src = f->source;
    size_t n = f->source_length;
    uint64_t eight = load_u64(src + position);
    uint32_t four = (uint32_t)eight;
    small_bucket bucket = f->buckets[small_bucket_index(four, bits)];
    size_t slot = small_long_index(eight, bits);
    size_t long_candidate = f->long_slots[slot];
    f->long_slots[slot] = (uint16_t)position;
    match best = {position, 0, 0};
    if (previous_distance != 0 && previous_distance <= position &&
        load_u32(src + position - previous_distance) == four) {
        best.length = 4 + match_length(src, position - previous_distance + 4, position + 4, n);
        best.distance = previous_distance;
    }
    // Slot 0 at position 0 (or a slot pointing at `position` itself) gives distance 0 and is skipped.
    if (best.length < f->good_match && long_candidate < position && load_u64(src + long_candidate) == eight) {
        size_t length = 8 + match_length(src, long_candidate + 8, position + 8, n);
        if (length > best.length) best = (match){position, length, position - long_candidate};
    }
    for (int k = 0; k < SMALL_BUCKET_WAYS; k++) {
        if (best.length >= f->good_match) break;
        size_t candidate = bucket.positions[k];
        if (bucket.values[k] != four || candidate >= position) continue;
        size_t length = 4 + match_length(src, candidate + 4, position + 4, n);
        if (length > best.length) best = (match){position, length, position - candidate};
    }
    small_insert_short(f, position, four, bits);
    if (best.length < 4) return (match){position, 0, 0};
    size_t back = match_length_backward(src, position - best.distance, position, position - literal_start);
    best.start -= back;
    best.length += back;
    return best;
}

// The parse loop takes the finder as plain functions, so define one set per table size: the size is then a
// compile-time constant inside each, which keeps the hash shifts out of registers (about 8% faster than a
// variable size). They are always inlined into the parse loop, so no call is made per position. DEFINE_SMALL_FINDER(13)
// defines small_find_13, small_insert_13 and small_insert_long_13.
#define DEFINE_SMALL_FINDER(BITS)                                                                                      \
    ALWAYS_INLINE match small_find_##BITS(void *f, size_t position, size_t literal_start, size_t previous_distance) {  \
        return small_find(f, position, literal_start, previous_distance, BITS);                                        \
    }                                                                                                                  \
    ALWAYS_INLINE void small_insert_##BITS(void *f, size_t position) {                                                 \
        small_insert(f, position, BITS);                                                                               \
    }                                                                                                                  \
    ALWAYS_INLINE void small_insert_long_##BITS(void *f, size_t position) {                                            \
        small_insert_long(f, position, BITS);                                                                          \
    }
DEFINE_SMALL_FINDER(10)
DEFINE_SMALL_FINDER(11)
DEFINE_SMALL_FINDER(12)
DEFINE_SMALL_FINDER(13)

void small_finder_tables_free(small_finder_tables *tables) {
    zz_free(tables->buckets);
    zz_free(tables->long_slots);
    tables->buckets = NULL;
    tables->long_slots = NULL;
    tables->bucket_capacity = 0;
    tables->long_capacity = 0;
}

lzfse_zazzcat_status encode_small(const uint8_t *source, size_t length, small_finder_tables *tables,
                                  block_writer *blocks, const lzfse_zazzcat_options *options,
                                  lzfse_zazzcat_buffer *output) {
    // Table size from the input size: log2 of the length rounded up, minus 3, between 10 and 13 bits
    // (13 bits for 64 KiB inputs: a 192 KiB 4-byte table and a 32 KiB long table).
    uint32_t bits = (uint32_t)(64 - __builtin_clzll((unsigned long long)(length - 1)));
    bits = bits >= 3 ? bits - 3 : 0;
    if (bits < 10) bits = 10;
    if (bits > 13) bits = 13;
    size_t bucket_count = (size_t)1 << bits, long_count = (size_t)2 << bits;
    if (tables->bucket_capacity < bucket_count) {
        small_bucket *grown = zz_realloc(tables->buckets, bucket_count * sizeof *grown);
        if (grown == NULL) return LZFSE_ZAZZCAT_OUT_OF_MEMORY;
        tables->buckets = grown;
        tables->bucket_capacity = bucket_count;
    }
    if (tables->long_capacity < long_count) {
        uint16_t *grown = zz_realloc(tables->long_slots, long_count * sizeof *grown);
        if (grown == NULL) return LZFSE_ZAZZCAT_OUT_OF_MEMORY;
        tables->long_slots = grown;
        tables->long_capacity = long_count;
    }
    // Every bucket entry starts as position 0 with its true bytes, so an unused entry is a genuine (if useless)
    // candidate and needs no separate validity check.
    small_bucket first;
    for (int k = 0; k < SMALL_BUCKET_WAYS; k++) {
        first.positions[k] = 0;
        first.values[k] = load_u32(source);
    }
    for (size_t i = 0; i < bucket_count; i++) tables->buckets[i] = first;
    memset(tables->long_slots, 0, long_count * sizeof *tables->long_slots);
    small_finder finder = {source, length, tables->buckets, tables->long_slots, options->good_match};
    switch (bits) {
    case 10:
        parse(source, length, &finder, small_find_10, small_insert_10, small_insert_long_10, blocks, options, output);
        break;
    case 11:
        parse(source, length, &finder, small_find_11, small_insert_11, small_insert_long_11, blocks, options, output);
        break;
    case 12:
        parse(source, length, &finder, small_find_12, small_insert_12, small_insert_long_12, blocks, options, output);
        break;
    default:
        parse(source, length, &finder, small_find_13, small_insert_13, small_insert_long_13, blocks, options, output);
        break;
    }
    return blocks->status;
}
