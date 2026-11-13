// LZFSE stream constants, value tables and block header parsing and writing.
use alloc::vec::Vec;

use crate::Error;

pub const MAGIC_EOS: u32 = u32::from_le_bytes(*b"bvx$");
pub const MAGIC_RAW: u32 = u32::from_le_bytes(*b"bvx-");
pub const MAGIC_V1: u32 = u32::from_le_bytes(*b"bvx1");
pub const MAGIC_V2: u32 = u32::from_le_bytes(*b"bvx2");
pub const MAGIC_LZVN: u32 = u32::from_le_bytes(*b"bvxn");

pub const L_SYMBOLS: usize = 20;
pub const M_SYMBOLS: usize = 20;
pub const D_SYMBOLS: usize = 64;
pub const LIT_SYMBOLS: usize = 256;
pub const L_STATES: u32 = 64;
pub const M_STATES: u32 = 64;
pub const D_STATES: u32 = 256;
pub const LIT_STATES: u32 = 1024;

pub const MATCHES_PER_BLOCK: usize = 10_000;
pub const LITERALS_PER_BLOCK: usize = 4 * MATCHES_PER_BLOCK;
pub const MAX_L: u32 = 315;
pub const MAX_M: u32 = 2359;
pub const MAX_D: u32 = 262_139;

pub const V1_HEADER_SIZE: usize = 772;
pub const V2_FIXED_SIZE: usize = 32;
pub const RAW_HEADER_SIZE: usize = 8;
pub const LZVN_HEADER_SIZE: usize = 12;

pub const L_EXTRA: [u8; L_SYMBOLS] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 5, 8];
pub const L_BASE: [u32; L_SYMBOLS] = [
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 28, 60,
];
pub const M_EXTRA: [u8; M_SYMBOLS] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 5, 8, 11];
pub const M_BASE: [u32; M_SYMBOLS] = [
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 24, 56, 312,
];
pub const D_EXTRA: [u8; D_SYMBOLS] = [
    0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7,
    8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14,
    14, 14, 15, 15, 15, 15,
];
pub const D_BASE: [u32; D_SYMBOLS] = [
    0, 1, 2, 3, 4, 6, 8, 10, 12, 16, 20, 24, 28, 36, 44, 52, 60, 76, 92, 108, 124, 156, 188, 220,
    252, 316, 380, 444, 508, 636, 764, 892, 1020, 1276, 1532, 1788, 2044, 2556, 3068, 3580, 4092,
    5116, 6140, 7164, 8188, 10236, 12284, 14332, 16380, 20476, 24572, 28668, 32764, 40956, 49148,
    57340, 65532, 81916, 98300, 114684, 131068, 163836, 196604, 229372,
];

// Return the symbol whose base is the largest one not above the value.
pub fn symbol_for(value: u32, base: &[u32]) -> usize {
    base.partition_point(|&b| b <= value) - 1
}

// Read a little-endian u32 at an offset, or report truncation.
pub fn read_u32(src: &[u8], at: usize) -> Result<u32, Error> {
    let bytes = src.get(at..at + 4).ok_or(Error::Truncated)?;
    Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

// Read a little-endian u16 at an offset that the caller has bounds-checked.
fn read_u16(src: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([src[at], src[at + 1]])
}

/// Decoded fields of an LZFSE block header (both v1 and v2 map to this).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BlockHeader {
    pub n_raw: u32,
    pub n_literals: u32,
    pub n_matches: u32,
    pub n_lit_payload: u32,
    pub n_lmd_payload: u32,
    pub literal_bits: i32,
    pub literal_state: [u16; 4],
    pub lmd_bits: i32,
    pub l_state: u16,
    pub m_state: u16,
    pub d_state: u16,
    pub l_freq: [u16; L_SYMBOLS],
    pub m_freq: [u16; M_SYMBOLS],
    pub d_freq: [u16; D_SYMBOLS],
    pub lit_freq: [u16; LIT_SYMBOLS],
}

impl BlockHeader {
    // Return an all-zero header to fill in.
    pub fn empty() -> Self {
        BlockHeader {
            n_raw: 0,
            n_literals: 0,
            n_matches: 0,
            n_lit_payload: 0,
            n_lmd_payload: 0,
            literal_bits: 0,
            literal_state: [0; 4],
            lmd_bits: 0,
            l_state: 0,
            m_state: 0,
            d_state: 0,
            l_freq: [0; L_SYMBOLS],
            m_freq: [0; M_SYMBOLS],
            d_freq: [0; D_SYMBOLS],
            lit_freq: [0; LIT_SYMBOLS],
        }
    }

    // Check the limits a decoder relies on: counts, initial states, bit counts and table sums.
    pub fn validate(&self) -> Result<(), Error> {
        let sum = |f: &[u16]| f.iter().map(|&v| v as u32).sum::<u32>();
        let ok = self.n_literals as usize <= LITERALS_PER_BLOCK
            && self.n_matches as usize <= MATCHES_PER_BLOCK
            && self.literal_state.iter().all(|&s| (s as u32) < LIT_STATES)
            && (self.l_state as u32) < L_STATES
            && (self.m_state as u32) < M_STATES
            && (self.d_state as u32) < D_STATES
            && (-7..=0).contains(&self.literal_bits)
            && (-7..=0).contains(&self.lmd_bits)
            && sum(&self.l_freq) <= L_STATES
            && sum(&self.m_freq) <= M_STATES
            && sum(&self.d_freq) <= D_STATES
            && sum(&self.lit_freq) <= LIT_STATES;
        if ok {
            Ok(())
        } else {
            Err(Error::Corrupt)
        }
    }

    // Visit the four frequency tables in stream order.
    fn freqs_mut(&mut self) -> impl Iterator<Item = &mut u16> {
        self.l_freq
            .iter_mut()
            .chain(self.m_freq.iter_mut())
            .chain(self.d_freq.iter_mut())
            .chain(self.lit_freq.iter_mut())
    }

    // Visit the four frequency tables in stream order.
    fn freqs(&self) -> impl Iterator<Item = u16> + '_ {
        self.l_freq
            .iter()
            .chain(self.m_freq.iter())
            .chain(self.d_freq.iter())
            .chain(self.lit_freq.iter())
            .copied()
    }
}

// Parse a v1 header (plain little-endian fields) and return it with its size.
pub fn parse_v1(src: &[u8]) -> Result<(BlockHeader, usize), Error> {
    let s = src.get(..V1_HEADER_SIZE).ok_or(Error::Truncated)?;
    let u = |at| read_u32(s, at).unwrap_or(0);
    let mut h = BlockHeader::empty();
    h.n_raw = u(4);
    let n_payload = u(8);
    h.n_literals = u(12);
    h.n_matches = u(16);
    h.n_lit_payload = u(20);
    h.n_lmd_payload = u(24);
    h.literal_bits = u(28) as i32;
    for (i, st) in h.literal_state.iter_mut().enumerate() {
        *st = read_u16(s, 32 + 2 * i);
    }
    h.lmd_bits = u(40) as i32;
    h.l_state = read_u16(s, 44);
    h.m_state = read_u16(s, 46);
    h.d_state = read_u16(s, 48);
    for (i, f) in h.freqs_mut().enumerate() {
        *f = read_u16(s, 50 + 2 * i);
    }
    if h.n_lit_payload.checked_add(h.n_lmd_payload) != Some(n_payload) {
        return Err(Error::Corrupt);
    }
    Ok((h, V1_HEADER_SIZE))
}

// Return `width` bits of a packed word starting at bit `at`.
fn field(word: u64, at: u32, width: u32) -> u32 {
    ((word >> at) & ((1u64 << width) - 1)) as u32
}

// Decode one frequency value from the low bits of `bits`, returning (value, bit count).
fn read_freq(bits: u32) -> (u16, u32) {
    match bits & 0b11 {
        0b00 => (0, 2),
        0b10 => (1, 2),
        _ => match bits & 0b111 {
            0b001 => (2, 3),
            0b101 => (3, 3),
            _ => match bits & 0b1111 {
                0b0111 => (8 + ((bits >> 4) & 0xF) as u16, 8),
                0b1111 => (24 + ((bits >> 4) & 0x3FF) as u16, 14),
                _ => (4 + ((bits >> 3) & 0b11) as u16, 5),
            },
        },
    }
}

// Encode one frequency value as (bits, bit count), the inverse of `read_freq`.
fn write_freq(value: u16) -> (u32, u32) {
    let v = value as u32;
    match v {
        0 => (0b00, 2),
        1 => (0b10, 2),
        2 => (0b001, 3),
        3 => (0b101, 3),
        4..=7 => (0b011 | ((v - 4) << 3), 5),
        8..=23 => (0b0111 | ((v - 8) << 4), 8),
        _ => (0b1111 | ((v - 24) << 4), 14),
    }
}

// Parse a v2 header (packed fields plus compressed frequency tables) and return it with its size.
pub fn parse_v2(src: &[u8]) -> Result<(BlockHeader, usize), Error> {
    let fixed = src.get(..V2_FIXED_SIZE).ok_or(Error::Truncated)?;
    let word = |i: usize| {
        let mut b = [0u8; 8];
        b.copy_from_slice(&fixed[8 + 8 * i..16 + 8 * i]);
        u64::from_le_bytes(b)
    };
    let (w0, w1, w2) = (word(0), word(1), word(2));
    let size = field(w2, 0, 32) as usize;
    if size < V2_FIXED_SIZE {
        return Err(Error::Corrupt);
    }
    let tables = src.get(V2_FIXED_SIZE..size).ok_or(Error::Truncated)?;
    let mut h = BlockHeader::empty();
    h.n_raw = read_u32(fixed, 4)?;
    h.n_literals = field(w0, 0, 20);
    h.n_lit_payload = field(w0, 20, 20);
    h.n_matches = field(w0, 40, 20);
    h.literal_bits = field(w0, 60, 3) as i32 - 7;
    for (i, st) in h.literal_state.iter_mut().enumerate() {
        *st = field(w1, 10 * i as u32, 10) as u16;
    }
    h.n_lmd_payload = field(w1, 40, 20);
    h.lmd_bits = field(w1, 60, 3) as i32 - 7;
    h.l_state = field(w2, 32, 10) as u16;
    h.m_state = field(w2, 42, 10) as u16;
    h.d_state = field(w2, 52, 10) as u16;
    if tables.is_empty() {
        return Ok((h, size));
    }
    let (mut accum, mut nbits, mut next) = (0u32, 0u32, 0usize);
    for f in h.freqs_mut() {
        while next < tables.len() && nbits + 8 <= 32 {
            accum |= (tables[next] as u32) << nbits;
            nbits += 8;
            next += 1;
        }
        let (value, n) = read_freq(accum);
        if n > nbits {
            return Err(Error::Corrupt);
        }
        *f = value;
        accum >>= n;
        nbits -= n;
    }
    if nbits >= 8 || next != tables.len() {
        return Err(Error::Corrupt);
    }
    Ok((h, size))
}

// Return `value` as a field of `width` bits at bit `at`, which the caller keeps in range.
fn put(value: u32, at: u32, width: u32) -> u64 {
    debug_assert!(width == 32 || value < (1 << width));
    (value as u64) << at
}

// Append a v2 header for `h` to `out`.
pub fn write_v2(h: &BlockHeader, out: &mut Vec<u8>) {
    let mut tables = Vec::with_capacity(720);
    let (mut accum, mut nbits) = (0u32, 0u32);
    for f in h.freqs() {
        let (bits, n) = write_freq(f);
        accum |= bits << nbits;
        nbits += n;
        while nbits >= 8 {
            tables.push(accum as u8);
            accum >>= 8;
            nbits -= 8;
        }
    }
    if nbits > 0 {
        tables.push(accum as u8);
    }
    let size = (V2_FIXED_SIZE + tables.len()) as u32;
    let w0 = put(h.n_literals, 0, 20)
        | put(h.n_lit_payload, 20, 20)
        | put(h.n_matches, 40, 20)
        | put((h.literal_bits + 7) as u32, 60, 3);
    let w1 = put(h.literal_state[0] as u32, 0, 10)
        | put(h.literal_state[1] as u32, 10, 10)
        | put(h.literal_state[2] as u32, 20, 10)
        | put(h.literal_state[3] as u32, 30, 10)
        | put(h.n_lmd_payload, 40, 20)
        | put((h.lmd_bits + 7) as u32, 60, 3);
    let w2 = put(size, 0, 32)
        | put(h.l_state as u32, 32, 10)
        | put(h.m_state as u32, 42, 10)
        | put(h.d_state as u32, 52, 10);
    out.extend_from_slice(&MAGIC_V2.to_le_bytes());
    out.extend_from_slice(&h.n_raw.to_le_bytes());
    for w in [w0, w1, w2] {
        out.extend_from_slice(&w.to_le_bytes());
    }
    out.extend_from_slice(&tables);
}

// Append a v1 header for `h` to `out` (Apple's encoder never writes v1; we use it to test the decoder).
#[cfg(test)]
pub fn write_v1(h: &BlockHeader, out: &mut Vec<u8>) {
    let start = out.len();
    for v in [
        MAGIC_V1,
        h.n_raw,
        h.n_lit_payload + h.n_lmd_payload,
        h.n_literals,
        h.n_matches,
        h.n_lit_payload,
        h.n_lmd_payload,
    ] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    out.extend_from_slice(&h.literal_bits.to_le_bytes());
    for s in h.literal_state {
        out.extend_from_slice(&s.to_le_bytes());
    }
    out.extend_from_slice(&h.lmd_bits.to_le_bytes());
    for s in [h.l_state, h.m_state, h.d_state] {
        out.extend_from_slice(&s.to_le_bytes());
    }
    for f in h.freqs() {
        out.extend_from_slice(&f.to_le_bytes());
    }
    out.resize(start + V1_HEADER_SIZE, 0);
}

#[cfg(test)]
mod tests {
    use super::*;

    // Every frequency value round-trips through the variable-length code.
    #[test]
    fn freq_code_round_trips() {
        for v in 0..=1047u16 {
            let (bits, n) = write_freq(v);
            assert!(bits < (1 << n));
            assert_eq!(
                read_freq(bits | (0xABC0_0000 & !((1 << n) - 1))),
                (v, n),
                "value {v}"
            );
        }
    }

    // The symbol lookup picks the right base for boundary values.
    #[test]
    fn symbol_lookup_boundaries() {
        assert_eq!(symbol_for(0, &L_BASE), 0);
        assert_eq!(symbol_for(15, &L_BASE), 15);
        assert_eq!(symbol_for(16, &L_BASE), 16);
        assert_eq!(symbol_for(19, &L_BASE), 16);
        assert_eq!(symbol_for(MAX_L, &L_BASE), 19);
        assert_eq!(symbol_for(MAX_M, &M_BASE), 19);
        assert_eq!(symbol_for(MAX_D, &D_BASE), 63);
        assert_eq!(MAX_D, D_BASE[63] + (1 << D_EXTRA[63]) - 1);
        assert_eq!(MAX_L, L_BASE[19] + (1 << L_EXTRA[19]) - 1);
        assert_eq!(MAX_M, M_BASE[19] + (1 << M_EXTRA[19]) - 1);
    }

    // Build a header with distinctive values in every field.
    fn sample() -> BlockHeader {
        let mut h = BlockHeader::empty();
        h.n_raw = 123_456;
        h.n_literals = 40_000;
        h.n_matches = 9_999;
        h.n_lit_payload = 30_001;
        h.n_lmd_payload = 70_002;
        h.literal_bits = -3;
        h.literal_state = [1, 1023, 512, 7];
        h.lmd_bits = -7;
        h.l_state = 63;
        h.m_state = 1;
        h.d_state = 255;
        h.l_freq[0] = 60;
        h.l_freq[19] = 4;
        h.m_freq[5] = 64;
        h.d_freq[63] = 200;
        h.d_freq[1] = 56;
        for (i, f) in h.lit_freq.iter_mut().enumerate() {
            *f = (i % 5) as u16;
        }
        h.lit_freq[3] = 1024 - h.lit_freq.iter().map(|&v| v as u32).sum::<u32>() as u16 + 3;
        h
    }

    // Both header versions round-trip every field.
    #[test]
    fn headers_round_trip() {
        let h = sample();
        h.validate().unwrap();
        let mut v2 = Vec::new();
        write_v2(&h, &mut v2);
        assert_eq!(parse_v2(&v2).unwrap(), (h.clone(), v2.len()));
        let mut v1 = Vec::new();
        write_v1(&h, &mut v1);
        assert_eq!(parse_v1(&v1).unwrap(), (h, V1_HEADER_SIZE));
    }

    // Truncated or inconsistent headers are rejected without panicking.
    #[test]
    fn bad_headers_are_errors() {
        let mut v2 = Vec::new();
        write_v2(&sample(), &mut v2);
        for cut in 0..v2.len() {
            assert!(parse_v2(&v2[..cut]).is_err());
        }
        let mut tiny = v2.clone();
        tiny[8 + 16..8 + 20].copy_from_slice(&5u32.to_le_bytes());
        assert_eq!(parse_v2(&tiny), Err(Error::Corrupt));
        let mut long = v2.clone();
        long.push(0);
        let size = long.len() as u32;
        long[8 + 16..8 + 20].copy_from_slice(&size.to_le_bytes());
        assert_eq!(parse_v2(&long), Err(Error::Corrupt));
        let mut bad = sample();
        bad.l_freq[0] = 65;
        assert_eq!(bad.validate(), Err(Error::Corrupt));
    }
}
