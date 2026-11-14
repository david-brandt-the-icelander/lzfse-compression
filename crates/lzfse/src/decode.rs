// LZFSE stream decoder: walks the blocks of a stream and writes their output into one buffer.
use alloc::vec;
use alloc::vec::Vec;

use crate::bits::BitReader;
use crate::format::*;
use crate::fse::{self, LitEntry, ValueEntry};
use crate::kernels::{copy_match_cold, token_wild, SLACK};
use crate::{lzvn, Error};

/// A reusable decoder: its tables and literal buffer carry over between calls, which helps when decoding many
/// small streams (such as 64 KiB chunks).
pub struct Decoder {
    s: alloc::boxed::Box<Scratch>,
}

impl Default for Decoder {
    // Same as `Decoder::new`.
    fn default() -> Self {
        Decoder::new()
    }
}

impl Decoder {
    /// Create a decoder; buffers are allocated on first use.
    pub fn new() -> Self {
        Decoder {
            s: alloc::boxed::Box::new(Scratch::new()),
        }
    }

    /// Decode a stream into `dst` and return the number of bytes written. Bytes of `dst` past that length may
    /// be overwritten.
    pub fn decode_into(&mut self, src: &[u8], dst: &mut [u8]) -> Result<usize, Error> {
        decode_with(src, dst, &mut self.s)
    }
}

// Reusable per-block decoding tables and literal buffer.
struct Scratch {
    lit: [LitEntry; LIT_STATES as usize],
    l: [ValueEntry; L_STATES as usize],
    m: [ValueEntry; M_STATES as usize],
    d: [ValueEntry; D_STATES as usize],
    // Grows to the largest block seen (at most 40,000 literals plus slack) and is then reused.
    literals: Vec<u8>,
}

impl Scratch {
    // Tables are fixed-size arrays (about 7 KB); only the literal buffer is allocated, on first use.
    fn new() -> Self {
        Scratch {
            lit: [LitEntry::default(); LIT_STATES as usize],
            l: [ValueEntry::default(); L_STATES as usize],
            m: [ValueEntry::default(); M_STATES as usize],
            d: [ValueEntry::default(); D_STATES as usize],
            literals: Vec::new(),
        }
    }
}

/// Return the decoded size of a stream by reading only its block headers.
pub fn decoded_len(src: &[u8]) -> Result<usize, Error> {
    let mut total = 0usize;
    let mut at = 0usize;
    loop {
        let (raw, next) = match read_u32(src, at)? {
            MAGIC_EOS => return Ok(total),
            MAGIC_RAW => {
                let n = read_u32(src, at + 4)? as usize;
                (n, at + RAW_HEADER_SIZE + n)
            }
            MAGIC_LZVN => {
                let n = read_u32(src, at + 4)? as usize;
                let payload = read_u32(src, at + 8)? as usize;
                (n, at + LZVN_HEADER_SIZE + payload)
            }
            MAGIC_V1 | MAGIC_V2 => {
                let (h, size) = header_at(src, at)?;
                (
                    h.n_raw as usize,
                    at + size + h.n_lit_payload as usize + h.n_lmd_payload as usize,
                )
            }
            _ => return Err(Error::Corrupt),
        };
        total = total.checked_add(raw).ok_or(Error::Corrupt)?;
        at = next;
        if at > src.len() {
            return Err(Error::Truncated);
        }
    }
}

// Parse the v1 or v2 header that starts at `at`.
fn header_at(src: &[u8], at: usize) -> Result<(BlockHeader, usize), Error> {
    let rest = &src[at..];
    if read_u32(src, at)? == MAGIC_V1 {
        parse_v1(rest)
    } else {
        parse_v2(rest)
    }
}

/// Decode a whole stream, refusing to produce more than `limit` bytes.
pub fn decode(src: &[u8], limit: usize) -> Result<Vec<u8>, Error> {
    let len = decoded_len(src)?;
    if len > limit {
        return Err(Error::OutputTooSmall);
    }
    let mut out = vec![0u8; len];
    let n = decode_into(src, &mut out).map_err(|e| {
        if e == Error::OutputTooSmall {
            Error::Corrupt
        } else {
            e
        }
    })?;
    if n != len {
        return Err(Error::Corrupt);
    }
    Ok(out)
}

/// Decode a stream into `dst` and return the number of bytes written.
///
/// Like Apple's decoder, this may overwrite bytes of `dst` past the returned length (it uses wide copies when
/// there is room); their contents are unspecified.
pub fn decode_into(src: &[u8], dst: &mut [u8]) -> Result<usize, Error> {
    decode_with(src, dst, &mut Scratch::new())
}

// Decode a stream into `dst` using `scratch` for tables and literals.
fn decode_with(src: &[u8], dst: &mut [u8], scratch: &mut Scratch) -> Result<usize, Error> {
    let (mut at, mut pos) = (0usize, 0usize);
    loop {
        match read_u32(src, at)? {
            MAGIC_EOS => return Ok(pos),
            MAGIC_RAW => {
                let n = read_u32(src, at + 4)? as usize;
                let body = src
                    .get(at + RAW_HEADER_SIZE..at + RAW_HEADER_SIZE + n)
                    .ok_or(Error::Truncated)?;
                dst.get_mut(pos..pos + n)
                    .ok_or(Error::OutputTooSmall)?
                    .copy_from_slice(body);
                pos += n;
                at += RAW_HEADER_SIZE + n;
            }
            MAGIC_LZVN => {
                let n = read_u32(src, at + 4)? as usize;
                let size = read_u32(src, at + 8)? as usize;
                let start = at + LZVN_HEADER_SIZE;
                let payload = src.get(start..start + size).ok_or(Error::Truncated)?;
                let end = pos
                    .checked_add(n)
                    .filter(|&e| e <= dst.len())
                    .ok_or(Error::OutputTooSmall)?;
                lzvn::decode_block(payload, dst, pos, end)?;
                pos = end;
                at = start + size;
            }
            MAGIC_V1 | MAGIC_V2 => {
                let (h, size) = header_at(src, at)?;
                h.validate()?;
                let lit_start = at + size;
                let lmd_start = lit_start + h.n_lit_payload as usize;
                let lmd_end = lmd_start + h.n_lmd_payload as usize;
                if lmd_end > src.len() {
                    return Err(Error::Truncated);
                }
                // The literal stream may read back into earlier bytes of the input, as Apple's decoder does.
                decode_literals(&h, &src[..lmd_start], scratch)?;
                pos = decode_tokens(&h, &src[lmd_start..lmd_end], scratch, dst, pos)?;
                at = lmd_end;
            }
            _ => return Err(Error::Corrupt),
        }
    }
}

// Build the literal table and decode the block's literals (a multiple of 4) into the scratch buffer.
#[inline(never)]
fn decode_literals(h: &BlockHeader, src: &[u8], s: &mut Scratch) -> Result<(), Error> {
    fse::literal_table(&h.lit_freq, LIT_STATES, &mut s.lit)?;
    let count = (h.n_literals as usize + 3) & !3;
    // Only grow the buffer: every literal up to `count` is overwritten below and the slack is never read as output.
    if s.literals.len() < count + SLACK {
        s.literals.resize(count + SLACK, 0);
    }
    let mut r = BitReader::new(src, h.literal_bits)?;
    let table = &s.lit;
    let [mut s0, mut s1, mut s2, mut s3] = h.literal_state.map(|v| v as u32);
    for group in s.literals[..count].chunks_exact_mut(4) {
        r.refill()?;
        group[0] = fse::decode_literal(&mut s0, table, &mut r);
        group[1] = fse::decode_literal(&mut s1, table, &mut r);
        group[2] = fse::decode_literal(&mut s2, table, &mut r);
        group[3] = fse::decode_literal(&mut s3, table, &mut r);
    }
    Ok(())
}

// Decode the block's L, M, D tokens and execute them into `dst` from `pos`; return the new output position.
#[inline(never)]
fn decode_tokens(
    h: &BlockHeader,
    lmd: &[u8],
    s: &mut Scratch,
    dst: &mut [u8],
    mut pos: usize,
) -> Result<usize, Error> {
    fse::value_table(&h.l_freq, L_STATES, &L_EXTRA, &L_BASE, &mut s.l)?;
    fse::value_table(&h.m_freq, M_STATES, &M_EXTRA, &M_BASE, &mut s.m)?;
    fse::value_table(&h.d_freq, D_STATES, &D_EXTRA, &D_BASE, &mut s.d)?;
    let (lt, mt, dt) = (&s.l, &s.m, &s.d);
    let mut r = BitReader::new(lmd, h.lmd_bits)?;
    let (mut ls, mut ms, mut ds) = (h.l_state as u32, h.m_state as u32, h.d_state as u32);
    let mut lit = 0usize;
    let n_lits = (h.n_literals as usize + 3) & !3;
    // 0 until the first token sets it, so a block starting with "repeat the previous distance" fails below.
    let mut d_prev = 0usize;
    for _ in 0..h.n_matches {
        r.refill()?;
        let l = fse::decode_value(&mut ls, lt, &mut r) as usize;
        let m = fse::decode_value(&mut ms, mt, &mut r) as usize;
        let d_new = fse::decode_value(&mut ds, dt, &mut r) as usize;
        let d = if d_new != 0 { d_new } else { d_prev };
        d_prev = d;
        if lit + l > n_lits {
            return Err(Error::Corrupt);
        }
        // Fast path: one bounds check and wide copies that may scribble into the slack past the token, which later
        // tokens overwrite. Everything else (buffer ends, bad distances, long short-period runs) goes out of line.
        if !token_wild(dst, pos, &s.literals, lit, l, d, m) {
            token_slow(dst, pos, &s.literals[lit..lit + l], d, m)?;
        }
        lit += l;
        pos += l + m;
    }
    Ok(pos)
}

// Execute a token the fast kernel declined: check its distance, then copy exactly (out of line: it is rarely
// taken, and keeping its calls out of the hot loop keeps the decoder's state in registers).
#[cold]
#[inline(never)]
fn token_slow(dst: &mut [u8], pos: usize, lits: &[u8], d: usize, m: usize) -> Result<(), Error> {
    let end = pos + lits.len();
    if d == 0 || d > end {
        return Err(Error::Corrupt);
    }
    if end + m > dst.len() {
        return Err(Error::OutputTooSmall);
    }
    dst[pos..end].copy_from_slice(lits);
    if d == 1 {
        // A run of one byte (zeros, padding): memset speed.
        let b = dst[end - 1];
        dst[end..end + m].fill(b);
    } else {
        copy_match_cold(dst, end, d, m);
    }
    Ok(())
}
