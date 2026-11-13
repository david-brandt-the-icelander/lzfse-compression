// LZFSE stream decoder: walks the blocks of a stream and writes their output into one buffer.
use alloc::vec;
use alloc::vec::Vec;

use crate::bits::BitReader;
use crate::format::*;
use crate::fse::{self, LitEntry, ValueEntry};
use crate::kernels::copy_match;
use crate::{lzvn, Error};

// Reusable per-block decoding tables and literal buffer.
struct Scratch {
    lit: Vec<LitEntry>,
    l: Vec<ValueEntry>,
    m: Vec<ValueEntry>,
    d: Vec<ValueEntry>,
    literals: Vec<u8>,
}

impl Scratch {
    // Allocate empty tables; they grow on first use.
    fn new() -> Self {
        Scratch {
            lit: Vec::new(),
            l: Vec::new(),
            m: Vec::new(),
            d: Vec::new(),
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
pub fn decode_into(src: &[u8], dst: &mut [u8]) -> Result<usize, Error> {
    let mut scratch = Scratch::new();
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
                decode_literals(&h, &src[..lmd_start], &mut scratch)?;
                pos = decode_tokens(&h, &src[lmd_start..lmd_end], &mut scratch, dst, pos)?;
                at = lmd_end;
            }
            _ => return Err(Error::Corrupt),
        }
    }
}

// Build the literal table and decode the block's literals (a multiple of 4) into the scratch buffer.
fn decode_literals(h: &BlockHeader, src: &[u8], s: &mut Scratch) -> Result<(), Error> {
    fse::literal_table(&h.lit_freq, LIT_STATES, &mut s.lit)?;
    let count = (h.n_literals as usize + 3) & !3;
    s.literals.clear();
    s.literals.resize(count, 0);
    let mut r = BitReader::new(src, h.literal_bits)?;
    let mut st = h.literal_state.map(|v| v as u32);
    for group in s.literals.chunks_exact_mut(4) {
        r.refill()?;
        for (byte, state) in group.iter_mut().zip(st.iter_mut()) {
            *byte = fse::decode_literal(state, &s.lit, &mut r);
        }
    }
    Ok(())
}

// Decode the block's L, M, D tokens and execute them into `dst` from `pos`; return the new output position.
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
    let mut r = BitReader::new(lmd, h.lmd_bits)?;
    let (mut ls, mut ms, mut ds) = (h.l_state as u32, h.m_state as u32, h.d_state as u32);
    let mut lit = 0usize;
    let mut d_prev: Option<usize> = None;
    for _ in 0..h.n_matches {
        r.refill()?;
        let l = fse::decode_value(&mut ls, &s.l, &mut r) as usize;
        let m = fse::decode_value(&mut ms, &s.m, &mut r) as usize;
        let d_new = fse::decode_value(&mut ds, &s.d, &mut r) as usize;
        let d = if d_new != 0 {
            d_new
        } else {
            d_prev.ok_or(Error::Corrupt)?
        };
        d_prev = Some(d);
        let lits = s.literals.get(lit..lit + l).ok_or(Error::Corrupt)?;
        if d > pos + l {
            return Err(Error::Corrupt);
        }
        if pos + l + m > dst.len() {
            return Err(Error::OutputTooSmall);
        }
        dst[pos..pos + l].copy_from_slice(lits);
        lit += l;
        pos += l;
        copy_match(dst, pos, d, m);
        pos += m;
    }
    Ok(pos)
}
