// Collects L/M/D tokens and literals into LZFSE blocks and entropy-codes each block as `bvx2` (or `bvx-`).
use alloc::vec::Vec;

use crate::bits::BitWriter;
use crate::format::*;
use crate::fse::{self, EncEntry};

// Spare bytes after the literal buffer so wide copies and padding never run out of room.
const LIT_SLACK: usize = 32;

// One L/M/D token and, once the block is encoded, its three symbols.
#[derive(Clone, Copy, Default)]
struct Token {
    l: u32,
    m: u32,
    d: u32,
    sym: [u8; 3],
}

// Token and literal buffers for the block being built; reused across blocks and calls (two allocations in all).
pub struct Blocks {
    block_start: usize,
    raw: usize,
    tokens: Vec<Token>,
    // Fixed-size literal buffer with slack for 16-byte copies; `n_lits` bytes are in use.
    lits: Vec<u8>,
    n_lits: usize,
}

impl Blocks {
    // Create empty buffers; `reset` allocates them the first time an LZFSE block is actually built.
    pub fn new() -> Self {
        Blocks {
            block_start: 0,
            raw: 0,
            tokens: Vec::new(),
            lits: Vec::new(),
            n_lits: 0,
        }
    }

    // Start a new input whose first block begins at offset 0 of its source, sizing the buffers for a full block
    // (only the first call allocates).
    pub fn reset(&mut self) {
        self.block_start = 0;
        self.clear();
        self.tokens.reserve(MATCHES_PER_BLOCK);
        if self.lits.len() < LITERALS_PER_BLOCK + LIT_SLACK {
            self.lits.resize(LITERALS_PER_BLOCK + LIT_SLACK, 0);
        }
    }

    // Drop the buffered tokens.
    fn clear(&mut self) {
        self.raw = 0;
        self.tokens.clear();
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
        if self.tokens.len() + 1 > MATCHES_PER_BLOCK || self.n_lits + n + 3 > LITERALS_PER_BLOCK {
            self.flush(src, out);
        }
        self.tokens.push(Token {
            l: n as u32,
            m,
            d,
            sym: [0; 3],
        });
        // Wide copy; any overshoot lands in the slack and is overwritten by the next literals.
        crate::kernels::wild_copy(&mut self.lits, self.n_lits, src, lit, n);
        self.n_lits += n;
        self.raw += n + m as usize;
    }

    // Encode the buffered tokens as one block, falling back to a raw block if that is smaller.
    pub fn flush(&mut self, src: &[u8], out: &mut Vec<u8>) {
        if self.tokens.is_empty() {
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
        while self.n_lits % 4 != 0 {
            self.lits[self.n_lits] = 0;
            self.n_lits += 1;
        }
        let lits = &self.lits[..self.n_lits];
        // Distance 0 means "same as the previous token"; literal-only tokens reuse the previous distance too.
        let mut prev: Option<u32> = None;
        for t in self.tokens.iter_mut() {
            let want = if t.m == 0 { prev.unwrap_or(1) } else { t.d };
            if prev == Some(want) {
                t.d = 0;
            } else {
                t.d = want;
                prev = Some(want);
            }
        }
        let mut h = BlockHeader::empty();
        h.n_raw = self.raw as u32;
        h.n_literals = lits.len() as u32;
        h.n_matches = self.tokens.len() as u32;
        let mut lc = [0u32; LIT_SYMBOLS];
        lits.iter().for_each(|&b| lc[b as usize] += 1);
        let (mut l_c, mut m_c, mut d_c) = ([0u32; L_SYMBOLS], [0u32; M_SYMBOLS], [0u32; D_SYMBOLS]);
        for t in self.tokens.iter_mut() {
            let s = [l_symbol(t.l), m_symbol(t.m), d_symbol(t.d)];
            l_c[s[0]] += 1;
            m_c[s[1]] += 1;
            d_c[s[2]] += 1;
            t.sym = s.map(|v| v as u8);
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
        for t in self.tokens.iter().rev() {
            let [a, b, c] = t.sym.map(|v| v as usize);
            w.push(D_EXTRA[c] as u32, (t.d - D_BASE[c]) as u64);
            fse::encode(&mut ds, &d_enc[c], &mut w);
            w.push(M_EXTRA[b] as u32, (t.m - M_BASE[b]) as u64);
            fse::encode(&mut ms, &m_enc[b], &mut w);
            w.push(L_EXTRA[a] as u32, (t.l - L_BASE[a]) as u64);
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
