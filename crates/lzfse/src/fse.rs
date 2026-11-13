// Finite-state entropy (tANS) tables: normalising counts, encoding symbols and decoding states.
//
// Each used symbol owns a contiguous range of `f` decoder states, laid out in symbol order. With `N` states in
// total, `k` is chosen so that N <= f * 2^k < 2N. The first `j0 = 2N / 2^k - f` states of a symbol's range read
// `k` bits and the rest read `k - 1`; the next state is a per-state base plus the bits read.

use crate::bits::{BitReader, BitWriter};
use crate::Error;

// Bits `k` such that n <= f << k < 2n, for 0 < f <= n with n a power of two.
#[inline]
fn shift_for(f: u32, n: u32) -> u32 {
    f.leading_zeros() - n.leading_zeros()
}

// Scale symbol counts (at most 256 symbols, as in every LZFSE alphabet) to frequencies summing to exactly `n`,
// keeping every used symbol at least 1.
pub fn normalize(counts: &[u32], n: u32, out: &mut [u16]) {
    let total: u64 = counts.iter().map(|&c| c as u64).sum();
    out.iter_mut().for_each(|f| *f = 0);
    if total == 0 {
        return;
    }
    // Largest-remainder rounding: floor every share, then hand the leftover states to the biggest remainders.
    let mut assigned = 0u32;
    // At most 256 symbols, so the remainders live on the stack.
    let mut store = [(0u64, 0usize); 256];
    let mut used = 0;
    for (i, &c) in counts.iter().enumerate() {
        if c == 0 {
            continue;
        }
        let scaled = c as u64 * n as u64;
        let floor = ((scaled / total) as u32).max(1);
        out[i] = floor as u16;
        assigned += floor;
        store[used] = (scaled % total, i);
        used += 1;
    }
    let remainders = &mut store[..used];
    if assigned < n {
        remainders.sort_unstable_by(|a, b| b.0.cmp(&a.0).then(a.1.cmp(&b.1)));
        let mut left = n - assigned;
        while left > 0 {
            for &(_, i) in remainders.iter() {
                if left == 0 {
                    break;
                }
                out[i] += 1;
                left -= 1;
            }
        }
    }
    // Rounding small symbols up to 1 can overshoot; take the excess from the largest frequencies.
    let mut excess = assigned.saturating_sub(n);
    while excess > 0 {
        let big = (0..out.len())
            .max_by_key(|&i| (out[i], usize::MAX - i))
            .unwrap_or(0);
        if out[big] <= 1 {
            break;
        }
        let take = excess.min((out[big] as u32 - 1) / 2).max(1);
        out[big] -= take as u16;
        excess -= take;
    }
}

// How to encode one symbol from any state.
#[derive(Clone, Copy, Default)]
pub struct EncEntry {
    s0: u32,
    k: u32,
    delta_hi: i32,
    delta_lo: i32,
}

// Build the encoder entries for frequencies summing to `n`.
pub fn encoder_table(freq: &[u16], n: u32, table: &mut [EncEntry]) {
    let mut offset = 0u32;
    for (&f, slot) in freq.iter().zip(table.iter_mut()) {
        let f = f as u32;
        if f == 0 {
            *slot = EncEntry::default();
            continue;
        }
        let k = shift_for(f, n);
        // States x >= s0 read k bits on decode; smaller states read k - 1 (only possible when k >= 1).
        let s0 = (f << k) - n;
        // The bases can be negative; adding the state's remaining high bits always lands in [offset, offset + f).
        let delta_hi = (offset + (n >> k)) as i32 - f as i32;
        let delta_lo = if k == 0 {
            delta_hi
        } else {
            (offset + (n >> (k - 1))) as i32 - f as i32
        };
        *slot = EncEntry {
            s0,
            k,
            delta_hi,
            delta_lo,
        };
        offset += f;
    }
}

// Encode `symbol` from `state`, writing the low bits of the state and moving to the symbol's decoder state.
#[inline(always)]
pub fn encode(state: &mut u32, e: &EncEntry, w: &mut BitWriter) {
    let s = *state;
    let (nb, delta) = if s >= e.s0 {
        (e.k, e.delta_hi)
    } else {
        (e.k - 1, e.delta_lo)
    };
    w.push(nb, (s & ((1u32 << nb) - 1)) as u64);
    *state = (delta + (s >> nb) as i32) as u32;
}

// Decoder entry for one literal state: bits to read, symbol, next-state base.
#[derive(Clone, Copy, Default)]
pub struct LitEntry {
    pub k: u8,
    pub symbol: u8,
    pub base: u16,
}

// Decoder entry for one L, M or D state: total bits (state + value), value bits, next-state base, value base.
#[derive(Clone, Copy, Default)]
pub struct ValueEntry {
    pub total_bits: u8,
    pub value_bits: u8,
    pub base: u16,
    pub vbase: u32,
}

// Call `emit(state_index, symbol, k, base)` for every decoder state; fails if the frequencies exceed `n`.
fn for_each_state(
    freq: &[u16],
    n: u32,
    mut emit: impl FnMut(usize, usize, u32, u32),
) -> Result<(), Error> {
    let mut offset = 0u32;
    for (sym, &f) in freq.iter().enumerate() {
        let f = f as u32;
        if f == 0 {
            continue;
        }
        if offset + f > n {
            return Err(Error::Corrupt);
        }
        let k = shift_for(f, n);
        let j0 = ((2 * n) >> k) - f;
        for j in 0..f {
            let (bits, base) = if j < j0 {
                (k, ((f + j) << k) - n)
            } else {
                (k - 1, (j - j0) << (k - 1))
            };
            emit((offset + j) as usize, sym, bits, base);
        }
        offset += f;
    }
    Ok(())
}

// Build the literal decoder table; unused states decode to symbol 0 and stay in state 0.
pub fn literal_table(freq: &[u16], n: u32, table: &mut [LitEntry]) -> Result<(), Error> {
    table.fill(LitEntry::default());
    for_each_state(freq, n, |i, sym, k, base| {
        table[i] = LitEntry {
            k: k as u8,
            symbol: sym as u8,
            base: base as u16,
        }
    })
}

// Build an L, M or D decoder table from frequencies and the per-symbol extra bits and bases.
pub fn value_table(
    freq: &[u16],
    n: u32,
    extra: &[u8],
    vbase: &[u32],
    table: &mut [ValueEntry],
) -> Result<(), Error> {
    table.fill(ValueEntry::default());
    for_each_state(freq, n, |i, sym, k, base| {
        table[i] = ValueEntry {
            total_bits: k as u8 + extra[sym],
            value_bits: extra[sym],
            base: base as u16,
            vbase: vbase[sym],
        }
    })
}

// Decode one literal, updating the state; the mask is a no-op for valid states and removes the bounds check.
#[inline(always)]
pub fn decode_literal<const N: usize>(
    state: &mut u32,
    table: &[LitEntry; N],
    r: &mut BitReader,
) -> u8 {
    let e = table[*state as usize & (N - 1)];
    *state = e.base as u32 + r.pull(e.k as u32);
    e.symbol
}

// Decode one L, M or D value, updating the state; the mask only removes the bounds check.
#[inline(always)]
pub fn decode_value<const N: usize>(
    state: &mut u32,
    table: &[ValueEntry; N],
    r: &mut BitReader,
) -> u32 {
    let e = table[*state as usize & (N - 1)];
    let bits = r.pull(e.total_bits as u32);
    *state = e.base as u32 + (bits >> e.value_bits);
    e.vbase + (bits & ((1u32 << e.value_bits) - 1))
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec::Vec;

    // Normalised frequencies always sum to the state count and keep used symbols alive.
    #[test]
    fn normalize_sums_exactly() {
        let cases: [&[u32]; 6] = [
            &[1],
            &[1, 0, 0, 1_000_000],
            &[5; 256],
            &[1; 256],
            &[0, 0, 0],
            &[
                7, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 90000,
            ],
        ];
        for counts in cases {
            for n in [64u32, 256, 1024] {
                if counts.iter().filter(|&&c| c > 0).count() > n as usize {
                    continue;
                }
                let mut out = alloc::vec![0u16; counts.len()];
                normalize(counts, n, &mut out);
                let sum: u32 = out.iter().map(|&f| f as u32).sum();
                if counts.iter().all(|&c| c == 0) {
                    assert_eq!(sum, 0);
                    continue;
                }
                assert_eq!(sum, n, "{counts:?} {n}");
                for (c, f) in counts.iter().zip(&out) {
                    assert_eq!(*c > 0, *f > 0);
                }
            }
        }
    }

    // Symbols encoded in reverse decode in forward order for skewed and flat distributions.
    #[test]
    fn literals_round_trip() {
        for (n, symbols) in [(1024u32, 256usize), (64, 20), (256, 64)] {
            let data: Vec<u8> = (0..5000u32)
                .map(|i| ((i * i + i / 7) % symbols as u32) as u8 % (1 + (i % 11) as u8))
                .collect();
            let mut counts = alloc::vec![0u32; symbols];
            data.iter().for_each(|&b| counts[b as usize] += 1);
            let mut freq = alloc::vec![0u16; symbols];
            normalize(&counts, n, &mut freq);
            let mut enc = [EncEntry::default(); 256];
            encoder_table(&freq, n, &mut enc);
            let mut w = BitWriter::new();
            let mut out = alloc::vec![0u8; 8];
            let mut state = 0u32;
            for &b in data.iter().rev() {
                encode(&mut state, &enc[b as usize], &mut w);
                w.flush(&mut out);
            }
            let bits = w.finish(&mut out);
            let mut fixed = [LitEntry::default(); 1024];
            literal_table(&freq, n, &mut fixed).unwrap();
            let fixed = &fixed;
            let mut r = BitReader::new(&out, bits).unwrap();
            for &b in &data {
                r.refill().unwrap();
                assert_eq!(decode_literal(&mut state, fixed, &mut r), b);
            }
            assert_eq!(state, 0, "decoder ends where the encoder started");
        }
    }

    // A single symbol owning every state needs no bits at all.
    #[test]
    fn single_symbol_uses_no_bits() {
        let mut freq = [0u16; 4];
        freq[2] = 64;
        let mut enc = [EncEntry::default(); 4];
        encoder_table(&freq, 64, &mut enc);
        let mut w = BitWriter::new();
        let mut state = 0u32;
        for _ in 0..100 {
            encode(&mut state, &enc[2], &mut w);
        }
        let mut out = Vec::new();
        assert_eq!(w.finish(&mut out), 0);
        assert!(out.is_empty());
    }

    // Frequencies that add up to more than the state count are rejected.
    #[test]
    fn oversized_tables_are_rejected() {
        assert!(literal_table(&[1000, 25], 1024, &mut [LitEntry::default(); 1024]).is_err());
        assert!(value_table(
            &[60, 5],
            64,
            &[0, 0],
            &[0, 1],
            &mut [ValueEntry::default(); 64]
        )
        .is_err());
    }
}
