// Collects L/M/D tokens and literals into LZFSE blocks and entropy-codes each block as `bvx2` (or `bvx-`).
use alloc::vec::Vec;

use crate::bits::BitWriter;
use crate::format::*;
use crate::fse::{self, EncEntry};

// Spare bytes after the literal buffer so wide copies and padding never run out of room.
const LIT_SLACK: usize = 32;

// Token buffers for the block being built; reused across blocks and calls.
pub struct Blocks {
    block_start: usize,
    raw: usize,
    l: Vec<u32>,
    m: Vec<u32>,
    d: Vec<u32>,
    // Fixed-size literal buffer with slack for 16-byte copies; `n_lits` bytes are in use.
    lits: Vec<u8>,
    n_lits: usize,
    syms: Vec<[u8; 3]>,
}

impl Blocks {
    // Create empty buffers; `reset` allocates them the first time an LZFSE block is actually built.
    pub fn new() -> Self {
        Blocks {
            block_start: 0,
            raw: 0,
            l: Vec::new(),
            m: Vec::new(),
            d: Vec::new(),
            lits: Vec::new(),
            n_lits: 0,
            syms: Vec::new(),
        }
    }

    // Start a new input whose first block begins at offset 0 of its source, sizing the buffers for a full block
    // (only the first call allocates).
    pub fn reset(&mut self) {
        self.block_start = 0;
        self.clear();
        self.l.reserve(MATCHES_PER_BLOCK);
        self.m.reserve(MATCHES_PER_BLOCK);
        self.d.reserve(MATCHES_PER_BLOCK);
        self.syms.reserve(MATCHES_PER_BLOCK);
        if self.lits.len() < LITERALS_PER_BLOCK + LIT_SLACK {
            self.lits.resize(LITERALS_PER_BLOCK + LIT_SLACK, 0);
        }
    }

    // Drop the buffered tokens.
    fn clear(&mut self) {
        self.raw = 0;
        self.l.clear();
        self.m.clear();
        self.d.clear();
        self.syms.clear();
        self.n_lits = 0;
    }

    // Add the literals `src[lit..lit_end]` followed by a match of `m` bytes at distance `d` (m = 0 for trailing
    // literals), splitting values that exceed the format's limits; full blocks are written to `out`.
    pub fn push(
        &mut self,
        src: &[u8],
        mut lit: usize,
        lit_end: usize,
        mut m: u32,
        d: u32,
        out: &mut Vec<u8>,
    ) {
        while lit_end - lit > MAX_L as usize {
            self.token(src, lit, MAX_L as usize, 0, 0, out);
            lit += MAX_L as usize;
        }
        while m > MAX_M {
            self.token(src, lit, lit_end - lit, MAX_M, d, out);
            lit = lit_end;
            m -= MAX_M;
        }
        if lit < lit_end || m > 0 {
            self.token(src, lit, lit_end - lit, m, d, out);
        }
    }

    // Append one in-range token, closing the block first if it is full.
    #[inline(always)]
    fn token(&mut self, src: &[u8], lit: usize, n: usize, m: u32, d: u32, out: &mut Vec<u8>) {
        if self.l.len() + 1 > MATCHES_PER_BLOCK || self.n_lits + n + 3 > LITERALS_PER_BLOCK {
            self.flush(src, out);
        }
        self.l.push(n as u32);
        self.m.push(m);
        self.d.push(d);
        // Wide copy; any overshoot lands in the slack and is overwritten by the next literals.
        crate::kernels::wild_copy(&mut self.lits, self.n_lits, src, lit, n);
        self.n_lits += n;
        self.raw += n + m as usize;
    }

    // Encode the buffered tokens as one block, falling back to a raw block if that is smaller.
    pub fn flush(&mut self, src: &[u8], out: &mut Vec<u8>) {
        if self.l.is_empty() {
            return;
        }
        let mark = out.len();
        self.encode_block(out);
        if out.len() - mark >= self.raw + RAW_HEADER_SIZE {
            out.truncate(mark);
            out.extend_from_slice(&MAGIC_RAW.to_le_bytes());
            out.extend_from_slice(&(self.raw as u32).to_le_bytes());
            out.extend_from_slice(&src[self.block_start..self.block_start + self.raw]);
        }
        self.block_start += self.raw;
        self.clear();
    }

    // Entropy-code the buffered tokens and append them to `out` as a `bvx2` block.
    fn encode_block(&mut self, out: &mut Vec<u8>) {
        let (l, m) = (&self.l, &self.m);
        while self.n_lits % 4 != 0 {
            self.lits[self.n_lits] = 0;
            self.n_lits += 1;
        }
        let lits = &self.lits[..self.n_lits];
        // Distance 0 means "same as the previous token"; literal-only tokens reuse the previous distance too.
        let mut prev: Option<u32> = None;
        for (dist, &len) in self.d.iter_mut().zip(m) {
            let want = if len == 0 { prev.unwrap_or(1) } else { *dist };
            if prev == Some(want) {
                *dist = 0;
            } else {
                *dist = want;
                prev = Some(want);
            }
        }
        let dv = &self.d;
        let mut h = BlockHeader::empty();
        h.n_raw = self.raw as u32;
        h.n_literals = lits.len() as u32;
        h.n_matches = l.len() as u32;
        let mut lc = [0u32; LIT_SYMBOLS];
        lits.iter().for_each(|&b| lc[b as usize] += 1);
        let (mut l_c, mut m_c, mut d_c) = ([0u32; L_SYMBOLS], [0u32; M_SYMBOLS], [0u32; D_SYMBOLS]);
        self.syms.clear();
        for i in 0..l.len() {
            let s = [l_symbol(l[i]), m_symbol(m[i]), d_symbol(dv[i])];
            l_c[s[0]] += 1;
            m_c[s[1]] += 1;
            d_c[s[2]] += 1;
            self.syms.push(s.map(|v| v as u8));
        }
        fse::normalize(&lc, LIT_STATES, &mut h.lit_freq);
        fse::normalize(&l_c, L_STATES, &mut h.l_freq);
        fse::normalize(&m_c, M_STATES, &mut h.m_freq);
        fse::normalize(&d_c, D_STATES, &mut h.d_freq);
        // Encoder tables are small fixed arrays on the stack.
        let mut lit_enc = [EncEntry::default(); LIT_SYMBOLS];
        let mut l_enc = [EncEntry::default(); L_SYMBOLS];
        let mut m_enc = [EncEntry::default(); M_SYMBOLS];
        let mut d_enc = [EncEntry::default(); D_SYMBOLS];
        fse::encoder_table(&h.lit_freq, LIT_STATES, &mut lit_enc);
        fse::encoder_table(&h.l_freq, L_STATES, &mut l_enc);
        fse::encoder_table(&h.m_freq, M_STATES, &mut m_enc);
        fse::encoder_table(&h.d_freq, D_STATES, &mut d_enc);

        // The header's size depends only on the frequency tables, so reserve it now and fill it in at the end.
        let header_at = out.len();
        write_v2(&h, out);
        let header_size = (out.len() - header_at) as u32;

        // Literals: four interleaved states, encoded last group first so the decoder reads forwards.
        let lit_at = out.len();
        let mut w = BitWriter::new();
        let mut st = [0u32; 4];
        for group in lits.chunks_exact(4).rev() {
            fse::encode(&mut st[3], &lit_enc[group[3] as usize], &mut w);
            fse::encode(&mut st[2], &lit_enc[group[2] as usize], &mut w);
            fse::encode(&mut st[1], &lit_enc[group[1] as usize], &mut w);
            fse::encode(&mut st[0], &lit_enc[group[0] as usize], &mut w);
            w.flush(out);
        }
        h.literal_bits = w.finish(out);
        h.literal_state = st.map(|s| s as u16);
        h.n_lit_payload = (out.len() - lit_at) as u32;

        // Tokens: 8 zero bytes first, then for each token (last first) D, M and L, extra bits before state bits.
        let lmd_at = out.len();
        out.extend_from_slice(&[0u8; 8]);
        let mut w = BitWriter::new();
        let (mut ls, mut ms, mut ds) = (0u32, 0u32, 0u32);
        for i in (0..l.len()).rev() {
            let [a, b, c] = self.syms[i].map(|v| v as usize);
            w.push(D_EXTRA[c] as u32, (dv[i] - D_BASE[c]) as u64);
            fse::encode(&mut ds, &d_enc[c], &mut w);
            w.push(M_EXTRA[b] as u32, (m[i] - M_BASE[b]) as u64);
            fse::encode(&mut ms, &m_enc[b], &mut w);
            w.push(L_EXTRA[a] as u32, (l[i] - L_BASE[a]) as u64);
            fse::encode(&mut ls, &l_enc[a], &mut w);
            w.flush(out);
        }
        h.lmd_bits = w.finish(out);
        h.l_state = ls as u16;
        h.m_state = ms as u16;
        h.d_state = ds as u16;
        h.n_lmd_payload = (out.len() - lmd_at) as u32;
        // Only the fixed fields change (final states and payload sizes); the tables written above stay.
        out[header_at..header_at + V2_FIXED_SIZE].copy_from_slice(&v2_fixed(&h, header_size));
    }
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
