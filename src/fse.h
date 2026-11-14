// Finite-state entropy (FSE, a table-driven form of asymmetric numeral systems) for LZFSE's four alphabets.
//
// A table of N states (a power of two) is split among the symbols in proportion to their normalized frequencies:
// each used symbol owns a contiguous range of `f` states, in symbol order. Decoding a symbol from state S reads
// some bits and moves to a new state; encoding runs the same steps in reverse. With k chosen so that
// N <= f * 2^k < 2N, the first j0 = 2N / 2^k - f states of a symbol's range read k bits and the rest read k - 1.
#ifndef LZFSE_ZAZZCAT_FSE_H
#define LZFSE_ZAZZCAT_FSE_H

#include "bits.h"
#include "common.h"

// Scale symbol counts (at most 256 symbols) to frequencies that sum to exactly `state_count`, keeping every used
// symbol at least 1.
void fse_normalize(const uint32_t *counts, size_t symbol_count, uint32_t state_count, uint16_t *frequencies);

// How to encode one symbol from any state.
typedef struct fse_encoder_entry {
    // States at or above `threshold` write `bits` bits; smaller states write `bits - 1`.
    uint32_t threshold;
    uint32_t bits;
    // Added to the state's remaining high bits to give the next state (may be negative).
    int32_t delta_high;
    int32_t delta_low;
} fse_encoder_entry;

// Build the encoder entries for `symbol_count` frequencies that sum to `state_count`.
void fse_encoder_table(const uint16_t *frequencies, size_t symbol_count, uint32_t state_count,
                       fse_encoder_entry *table);

// Encode a symbol: write the low bits of `*state` and move to the symbol's state.
ALWAYS_INLINE void fse_encode(uint32_t *state, const fse_encoder_entry *entry, bit_writer *writer) {
    uint32_t s = *state;
    bool high = s >= entry->threshold;
    uint32_t bits = high ? entry->bits : entry->bits - 1;
    int32_t delta = high ? entry->delta_high : entry->delta_low;
    bit_writer_push(writer, bits, s & ((1u << bits) - 1));
    *state = (uint32_t)(delta + (int32_t)(s >> bits));
}

// Decoder entry for one literal state: bits to read, the symbol, and the base of the next state.
typedef struct fse_literal_entry {
    uint8_t bits;
    uint8_t symbol;
    uint16_t next_state_base;
} fse_literal_entry;

// Decoder entry for one literal-length, match-length or distance state. The value's extra bits are read together
// with the state bits: `total_bits` in all, the low `value_bits` of which belong to the value.
typedef struct fse_value_entry {
    uint8_t total_bits;
    uint8_t value_bits;
    uint16_t next_state_base;
    uint32_t value_base;
} fse_value_entry;

// Build the literal decoder table (LITERAL_STATES entries); unused states decode to symbol 0 and stay in state 0.
// Fails if the frequencies add up to more than `state_count`.
lzfse_zazzcat_status fse_literal_table(const uint16_t *frequencies, uint32_t state_count, fse_literal_entry *table);

// Build a value decoder table from frequencies and the per-symbol extra bits and value bases.
lzfse_zazzcat_status fse_value_table(const uint16_t *frequencies, size_t symbol_count, uint32_t state_count,
                                     const uint8_t *extra_bits, const uint32_t *value_base, fse_value_entry *table);

// Decode one literal and update the state. `state_mask` is the table size minus one: masking keeps the index in
// range even for a corrupt state.
ALWAYS_INLINE uint8_t fse_decode_literal(uint32_t *state, const fse_literal_entry *table, uint32_t state_mask,
                                         bit_reader *reader) {
    fse_literal_entry entry = table[*state & state_mask];
    *state = entry.next_state_base + bit_reader_pull(reader, entry.bits);
    return entry.symbol;
}

// Decode one literal length, match length or distance and update the state.
ALWAYS_INLINE uint32_t fse_decode_value(uint32_t *state, const fse_value_entry *table, uint32_t state_mask,
                                        bit_reader *reader) {
    fse_value_entry entry = table[*state & state_mask];
    uint32_t bits = bit_reader_pull(reader, entry.total_bits);
    *state = entry.next_state_base + (bits >> entry.value_bits);
    return entry.value_base + (bits & ((1u << entry.value_bits) - 1));
}

// Decode one value from an entry the caller already looked up, and store the next state in `*state`. Lets a loop
// look up the next entry early (see decode_tokens).
ALWAYS_INLINE uint32_t fse_decode_value_from(fse_value_entry entry, uint32_t *state, bit_reader *reader) {
    uint32_t bits = bit_reader_pull(reader, entry.total_bits);
    *state = entry.next_state_base + (bits >> entry.value_bits);
    return entry.value_base + (bits & ((1u << entry.value_bits) - 1));
}

#endif
