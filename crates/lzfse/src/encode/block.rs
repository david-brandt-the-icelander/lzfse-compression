// Collects L/M/D tokens and literals into LZFSE blocks and entropy-codes each block as `bvx2` (or `bvx-`).
use alloc::vec::Vec;

use crate::bits::BitWriter;
use crate::format::*;
use crate::fse;

// Token buffer for the block being built, plus the encoded stream so far.
pub struct Blocks<'a> {
    src: &'a [u8],
    block_start: usize,
    raw: usize,
    l: Vec<u32>,
    m: Vec<u32>,
    d: Vec<u32>,
    lits: Vec<u8>,
    pub out: Vec<u8>,
}

impl<'a> Blocks<'a> {
    // Start collecting tokens for `src` from offset `start`, appending blocks to `out`.
    pub fn new(src: &'a [u8], start: usize, out: Vec<u8>) -> Self {
        Blocks {
            src,
            block_start: start,
            raw: 0,
            l: Vec::with_capacity(MATCHES_PER_BLOCK),
            m: Vec::with_capacity(MATCHES_PER_BLOCK),
            d: Vec::with_capacity(MATCHES_PER_BLOCK),
            lits: Vec::with_capacity(LITERALS_PER_BLOCK),
            out,
        }
    }

    // Add literals followed by a match of `m` bytes at distance `d` (m = 0 for trailing literals), splitting
    // values that exceed the format's limits.
    pub fn push(&mut self, mut lits: &[u8], mut m: u32, d: u32) {
        while lits.len() > MAX_L as usize {
            self.token(&lits[..MAX_L as usize], 0, 0);
            lits = &lits[MAX_L as usize..];
        }
        while m > MAX_M {
            self.token(lits, MAX_M, d);
            lits = &[];
            m -= MAX_M;
        }
        if !lits.is_empty() || m > 0 {
            self.token(lits, m, d);
        }
    }

    // Append one in-range token, closing the block first if it is full.
    fn token(&mut self, lits: &[u8], m: u32, d: u32) {
        if self.l.len() + 1 > MATCHES_PER_BLOCK
            || self.lits.len() + lits.len() + 3 > LITERALS_PER_BLOCK
        {
            self.flush();
        }
        self.l.push(lits.len() as u32);
        self.m.push(m);
        self.d.push(d);
        self.lits.extend_from_slice(lits);
        self.raw += lits.len() + m as usize;
    }

    // Encode the buffered tokens as one block, falling back to a raw block if that is smaller.
    pub fn flush(&mut self) {
        if self.l.is_empty() {
            return;
        }
        let mark = self.out.len();
        encode_block(
            &self.l,
            &self.m,
            &self.d,
            &mut self.lits,
            self.raw as u32,
            &mut self.out,
        );
        if self.out.len() - mark >= self.raw + RAW_HEADER_SIZE {
            self.out.truncate(mark);
            self.out.extend_from_slice(&MAGIC_RAW.to_le_bytes());
            self.out.extend_from_slice(&(self.raw as u32).to_le_bytes());
            self.out
                .extend_from_slice(&self.src[self.block_start..self.block_start + self.raw]);
        }
        self.block_start += self.raw;
        self.raw = 0;
        self.l.clear();
        self.m.clear();
        self.d.clear();
        self.lits.clear();
    }
}

// Entropy-code one block of tokens and append it to `out` as a `bvx2` block.
fn encode_block(
    l: &[u32],
    m: &[u32],
    d: &[u32],
    lits: &mut Vec<u8>,
    n_raw: u32,
    out: &mut Vec<u8>,
) {
    while lits.len() % 4 != 0 {
        lits.push(0);
    }
    // Distance 0 means "same as the previous token"; literal-only tokens reuse the previous distance too.
    let mut dv = Vec::with_capacity(d.len());
    let mut prev: Option<u32> = None;
    for (&dist, &len) in d.iter().zip(m) {
        let want = if len == 0 { prev.unwrap_or(1) } else { dist };
        if prev == Some(want) {
            dv.push(0);
        } else {
            dv.push(want);
            prev = Some(want);
        }
    }
    let mut h = BlockHeader::empty();
    h.n_raw = n_raw;
    h.n_literals = lits.len() as u32;
    h.n_matches = l.len() as u32;
    let mut lc = [0u32; LIT_SYMBOLS];
    lits.iter().for_each(|&b| lc[b as usize] += 1);
    let (mut l_sym, mut m_sym, mut d_sym) = (
        Vec::with_capacity(l.len()),
        Vec::with_capacity(l.len()),
        Vec::with_capacity(l.len()),
    );
    let (mut l_c, mut m_c, mut d_c) = ([0u32; L_SYMBOLS], [0u32; M_SYMBOLS], [0u32; D_SYMBOLS]);
    for i in 0..l.len() {
        let (a, b, c) = (
            symbol_for(l[i], &L_BASE),
            symbol_for(m[i], &M_BASE),
            symbol_for(dv[i], &D_BASE),
        );
        l_c[a] += 1;
        m_c[b] += 1;
        d_c[c] += 1;
        l_sym.push(a as u8);
        m_sym.push(b as u8);
        d_sym.push(c as u8);
    }
    fse::normalize(&lc, LIT_STATES, &mut h.lit_freq);
    fse::normalize(&l_c, L_STATES, &mut h.l_freq);
    fse::normalize(&m_c, M_STATES, &mut h.m_freq);
    fse::normalize(&d_c, D_STATES, &mut h.d_freq);
    let lit_enc = fse::encoder_table(&h.lit_freq, LIT_STATES);
    let l_enc = fse::encoder_table(&h.l_freq, L_STATES);
    let m_enc = fse::encoder_table(&h.m_freq, M_STATES);
    let d_enc = fse::encoder_table(&h.d_freq, D_STATES);

    // Literals: four interleaved states, encoded last group first so the decoder reads forwards.
    let mut lit_payload = Vec::with_capacity(lits.len());
    let mut w = BitWriter::new();
    let mut st = [0u32; 4];
    for group in lits.chunks_exact(4).rev() {
        for k in (0..4).rev() {
            fse::encode(&mut st[k], &lit_enc[group[k] as usize], &mut w);
        }
        w.flush(&mut lit_payload);
    }
    h.literal_bits = w.finish(&mut lit_payload);
    h.literal_state = st.map(|s| s as u16);

    // Tokens: 8 zero bytes first, then for each token (last first) D, M and L, extra bits before state bits.
    let mut lmd = Vec::with_capacity(8 + 7 * l.len());
    lmd.extend_from_slice(&[0u8; 8]);
    let mut w = BitWriter::new();
    let (mut ls, mut ms, mut ds) = (0u32, 0u32, 0u32);
    for i in (0..l.len()).rev() {
        let (a, b, c) = (l_sym[i] as usize, m_sym[i] as usize, d_sym[i] as usize);
        w.push(D_EXTRA[c] as u32, (dv[i] - D_BASE[c]) as u64);
        fse::encode(&mut ds, &d_enc[c], &mut w);
        w.push(M_EXTRA[b] as u32, (m[i] - M_BASE[b]) as u64);
        fse::encode(&mut ms, &m_enc[b], &mut w);
        w.push(L_EXTRA[a] as u32, (l[i] - L_BASE[a]) as u64);
        fse::encode(&mut ls, &l_enc[a], &mut w);
        w.flush(&mut lmd);
    }
    h.lmd_bits = w.finish(&mut lmd);
    h.l_state = ls as u16;
    h.m_state = ms as u16;
    h.d_state = ds as u16;
    h.n_lit_payload = lit_payload.len() as u32;
    h.n_lmd_payload = lmd.len() as u32;
    write_v2(&h, out);
    out.extend_from_slice(&lit_payload);
    out.extend_from_slice(&lmd);
}

/// Encode a block as v1 instead of v2 (test-only; Apple's encoder never emits v1).
#[cfg(test)]
pub fn to_v1(stream: &[u8]) -> Vec<u8> {
    let (h, size) = parse_v2(stream).unwrap();
    let mut out = Vec::new();
    write_v1(&h, &mut out);
    out.extend_from_slice(&stream[size..]);
    out
}
