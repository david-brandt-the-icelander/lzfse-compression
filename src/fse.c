// FSE table construction: normalizing counts, and building encoder and decoder tables.
#include "fse.h"

#include "format.h"

// The k for which state_count <= frequency * 2^k < 2 * state_count (state_count is a power of two and
// 0 < frequency <= state_count): the difference of their leading-zero counts.
static uint32_t shift_for(uint32_t frequency, uint32_t state_count) {
    return (uint32_t)(__builtin_clz(frequency) - __builtin_clz(state_count));
}

// One used symbol's leftover share, for distributing the states that rounding down left over.
typedef struct remainder_entry {
    uint64_t remainder;
    size_t symbol;
} remainder_entry;

// qsort comparison: larger remainders first, then lower symbols first (so the result is deterministic).
static int compare_remainders(const void *a, const void *b) {
    const remainder_entry *x = a, *y = b;
    if (x->remainder != y->remainder) return x->remainder > y->remainder ? -1 : 1;
    return x->symbol < y->symbol ? -1 : (x->symbol > y->symbol);
}

void fse_normalize(const uint32_t *counts, size_t symbol_count, uint32_t state_count, uint16_t *frequencies) {
    uint64_t total = 0;
    for (size_t i = 0; i < symbol_count; i++) total += counts[i];
    // Not cleared up front: the loop below writes every entry (0 for unused symbols), and an empty input gets
    // its zeros here, so no entry is left unset.
    // memset(frequencies, 0, symbol_count * sizeof *frequencies);
    if (total == 0) {
        for (size_t i = 0; i < symbol_count; i++) frequencies[i] = 0;
        return;
    }
    // Largest-remainder rounding: give every used symbol the floor of its share (at least 1), then hand the
    // leftover states to the largest remainders.
    remainder_entry remainders[256];
    size_t used = 0;
    uint32_t assigned = 0;
    for (size_t i = 0; i < symbol_count; i++) {
        if (counts[i] == 0) {
            frequencies[i] = 0;
            continue;
        }
        uint64_t scaled = (uint64_t)counts[i] * state_count;
        uint32_t share = (uint32_t)(scaled / total);
        if (share < 1) share = 1;
        frequencies[i] = (uint16_t)share;
        assigned += share;
        remainders[used].remainder = scaled % total;
        remainders[used].symbol = i;
        used++;
    }
    if (assigned < state_count) {
        qsort(remainders, used, sizeof remainders[0], compare_remainders);
        uint32_t left = state_count - assigned;
        while (left > 0) {
            for (size_t r = 0; r < used && left > 0; r++, left--) frequencies[remainders[r].symbol]++;
        }
    }
    // Rounding small symbols up to 1 can overshoot; take the excess from the largest frequencies.
    uint32_t excess = assigned > state_count ? assigned - state_count : 0;
    while (excess > 0) {
        size_t largest = 0;
        for (size_t i = 1; i < symbol_count; i++) {
            if (frequencies[i] > frequencies[largest]) largest = i;
        }
        if (frequencies[largest] <= 1) break;
        uint32_t take = (frequencies[largest] - 1u) / 2;
        if (take > excess) take = excess;
        if (take < 1) take = 1;
        frequencies[largest] = (uint16_t)(frequencies[largest] - take);
        excess -= take;
    }
}

void fse_encoder_table(const uint16_t *frequencies, size_t symbol_count, uint32_t state_count,
                       fse_encoder_entry *table) {
    uint32_t offset = 0;
    for (size_t s = 0; s < symbol_count; s++) {
        uint32_t f = frequencies[s];
        if (f == 0) {
            // Not cleared: a symbol with frequency 0 never occurs in the block, so the encoder never looks up
            // its entry.
            // memset(&table[s], 0, sizeof table[s]);
            continue;
        }
        uint32_t k = shift_for(f, state_count);
        // States x >= threshold read k bits when decoded; smaller ones read k - 1 (only possible when k >= 1).
        table[s].threshold = (f << k) - state_count;
        table[s].bits = k;
        // The deltas can be negative; adding a state's remaining high bits always lands in [offset, offset + f).
        table[s].delta_high = (int32_t)(offset + (state_count >> k)) - (int32_t)f;
        table[s].delta_low = k == 0 ? table[s].delta_high : (int32_t)(offset + (state_count >> (k - 1))) - (int32_t)f;
        offset += f;
    }
}

// What a decoder state does: read `bits` bits and add them to `next_state_base`.
typedef struct decoder_state {
    uint32_t bits;
    uint32_t next_state_base;
} decoder_state;

// Describe decoder state `j` (counting from 0) of a symbol with frequency `f` in a table of `state_count`.
ALWAYS_INLINE decoder_state decoder_state_for(uint32_t f, uint32_t j, uint32_t state_count) {
    uint32_t k = shift_for(f, state_count);
    uint32_t first_low = ((2 * state_count) >> k) - f; // states before this read k bits, the rest k - 1
    decoder_state d;
    if (j < first_low) {
        d.bits = k;
        d.next_state_base = ((f + j) << k) - state_count;
    } else {
        d.bits = k - 1;
        d.next_state_base = (j - first_low) << (k - 1);
    }
    return d;
}

lzfse_zazzcat_status fse_literal_table(const uint16_t *frequencies, uint32_t state_count, fse_literal_entry *table) {
    // Not cleared up front: the loop below writes every state from 0 up to the sum of the frequencies, and only
    // the states past that sum are cleared at the end. Untrusted streams may have frequencies that sum to less
    // than state_count, and the decoder can still reach those states, so they must hold a defined (zero) entry.
    // A table left half-built by a CORRUPT return is never used.
    // memset(table, 0, state_count * sizeof *table);
    uint32_t offset = 0;
    for (uint32_t symbol = 0; symbol < LITERAL_SYMBOLS; symbol++) {
        uint32_t f = frequencies[symbol];
        if (f == 0) continue;
        if (offset + f > state_count) return LZFSE_ZAZZCAT_CORRUPT;
        for (uint32_t j = 0; j < f; j++) {
            decoder_state d = decoder_state_for(f, j, state_count);
            fse_literal_entry *entry = &table[offset + j];
            entry->bits = (uint8_t)d.bits;
            entry->symbol = (uint8_t)symbol;
            entry->next_state_base = (uint16_t)d.next_state_base;
        }
        offset += f;
    }
    // States the frequencies did not cover (none for well-formed streams, whose frequencies sum to state_count).
    memset(table + offset, 0, (state_count - offset) * sizeof *table);
    return LZFSE_ZAZZCAT_OK;
}

lzfse_zazzcat_status fse_value_table(const uint16_t *frequencies, size_t symbol_count, uint32_t state_count,
                                     const uint8_t *extra_bits, const uint32_t *value_base, fse_value_entry *table) {
    // Not cleared up front, for the same reason as in fse_literal_table: every state up to the sum of the
    // frequencies is written below, and only the states past it are cleared at the end.
    // memset(table, 0, state_count * sizeof *table);
    uint32_t offset = 0;
    for (size_t symbol = 0; symbol < symbol_count; symbol++) {
        uint32_t f = frequencies[symbol];
        if (f == 0) continue;
        if (offset + f > state_count) return LZFSE_ZAZZCAT_CORRUPT;
        for (uint32_t j = 0; j < f; j++) {
            decoder_state d = decoder_state_for(f, j, state_count);
            fse_value_entry *entry = &table[offset + j];
            entry->total_bits = (uint8_t)(d.bits + extra_bits[symbol]);
            entry->value_bits = extra_bits[symbol];
            entry->next_state_base = (uint16_t)d.next_state_base;
            entry->value_base = value_base[symbol];
        }
        offset += f;
    }
    // States the frequencies did not cover (none for well-formed streams, whose frequencies sum to state_count).
    memset(table + offset, 0, (state_count - offset) * sizeof *table);
    return LZFSE_ZAZZCAT_OK;
}
