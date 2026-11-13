// LZVN: Apple's byte-oriented LZ77 format, used for small LZFSE blocks and decmpfs types 7/8.
use alloc::vec::Vec;

use crate::kernels::{copy_match, load4, match_len, match_len_back};
use crate::Error;

const EOS: u8 = 0x06;
const MAX_DISTANCE: usize = 0xFFFF;
const MAX_HASH_BITS: u32 = 14;
// Distance placeholder meaning "reuse the previous distance".
const PREV: usize = usize::MAX;

// Tell whether an opcode is reserved (undefined) and must be rejected.
fn undefined(op: u8) -> bool {
    matches!(op, 0x1E | 0x26 | 0x2E | 0x36 | 0x3E | 0x70..=0x7F | 0xD0..=0xDF)
}

/// Decode an LZVN payload into `out[pos..end]`, where `out[..pos]` is earlier output that matches may refer to.
/// The payload must end with the end-of-stream opcode exactly at its last byte and fill the range exactly.
pub fn decode_block(payload: &[u8], out: &mut [u8], pos: usize, end: usize) -> Result<(), Error> {
    let (used, at) = decode_run(payload, &mut out[..end], pos)?;
    if used != payload.len() || at != end {
        return Err(Error::Corrupt);
    }
    Ok(())
}

/// Decode a standalone LZVN stream into `dst` and return the number of bytes written.
pub fn decode(src: &[u8], dst: &mut [u8]) -> Result<usize, Error> {
    decode_run(src, dst, 0).map(|(_, n)| n)
}

// Run opcodes from `src` into `out` starting at `pos` until end-of-stream; return (bytes read, output end).
fn decode_run(src: &[u8], out: &mut [u8], mut pos: usize) -> Result<(usize, usize), Error> {
    let mut sp = 0usize;
    let mut d_prev = 0usize;
    loop {
        let op = *src.get(sp).ok_or(Error::Truncated)?;
        let avail = src.len() - sp;
        // (opcode length, literal length, match length, distance or PREV)
        let (opc, l, m, d) = match op {
            EOS => {
                if avail < 8 {
                    return Err(Error::Truncated);
                }
                return Ok((sp + 8, pos));
            }
            0x0E | 0x16 => (1, 0, 0, None),
            0xE0 => (
                2,
                16 + *src.get(sp + 1).ok_or(Error::Truncated)? as usize,
                0,
                None,
            ),
            0xE1..=0xEF => (1, (op & 0x0F) as usize, 0, None),
            0xF0 => (
                2,
                0,
                16 + *src.get(sp + 1).ok_or(Error::Truncated)? as usize,
                Some(PREV),
            ),
            0xF1..=0xFF => (1, 0, (op & 0x0F) as usize, Some(PREV)),
            _ if undefined(op) => return Err(Error::Corrupt),
            0xA0..=0xBF => {
                if avail < 3 {
                    return Err(Error::Truncated);
                }
                let w = u16::from_le_bytes([src[sp + 1], src[sp + 2]]) as usize;
                (
                    3,
                    ((op >> 3) & 3) as usize,
                    ((((op & 7) as usize) << 2) | (w & 3)) + 3,
                    Some(w >> 2),
                )
            }
            _ => {
                let (l, m) = ((op >> 6) as usize, ((op >> 3) & 7) as usize + 3);
                match op & 7 {
                    7 => {
                        if avail < 3 {
                            return Err(Error::Truncated);
                        }
                        (
                            3,
                            l,
                            m,
                            Some(u16::from_le_bytes([src[sp + 1], src[sp + 2]]) as usize),
                        )
                    }
                    6 => (1, l, m, Some(PREV)),
                    hi => {
                        if avail < 2 {
                            return Err(Error::Truncated);
                        }
                        (2, l, m, Some(((hi as usize) << 8) | src[sp + 1] as usize))
                    }
                }
            }
        };
        // Every opcode but end-of-stream needs at least one more byte after its literals.
        if avail <= opc + l {
            return Err(Error::Truncated);
        }
        sp += opc;
        if l > 0 {
            let dst = out.get_mut(pos..pos + l).ok_or(Error::OutputTooSmall)?;
            dst.copy_from_slice(&src[sp..sp + l]);
            sp += l;
            pos += l;
        }
        if let Some(d) = d {
            let d = if d == PREV { d_prev } else { d };
            if d == 0 || d > pos {
                return Err(Error::Corrupt);
            }
            if pos + m > out.len() {
                return Err(Error::OutputTooSmall);
            }
            copy_match(out, pos, d, m);
            pos += m;
            d_prev = d;
        }
    }
}

// Largest match length a 3-bit-length opcode can carry alongside `l` literals (reserved opcodes cut it short).
fn max_short_m(l: usize) -> usize {
    [10, 8, 6, 4][l]
}

// Append literal-only opcodes for `lits`.
fn emit_literals(out: &mut Vec<u8>, mut lits: &[u8]) {
    while !lits.is_empty() {
        let n = lits.len().min(271);
        if n >= 16 {
            out.extend_from_slice(&[0xE0, (n - 16) as u8]);
        } else {
            out.push(0xE0 | n as u8);
        }
        out.extend_from_slice(&lits[..n]);
        lits = &lits[n..];
    }
}

// Append match-only opcodes that repeat the previous distance for `m` bytes.
fn emit_repeat(out: &mut Vec<u8>, mut m: usize) {
    while m > 0 {
        let n = m.min(271);
        if n >= 16 {
            out.extend_from_slice(&[0xF0, (n - 16) as u8]);
        } else {
            out.push(0xF0 | n as u8);
        }
        m -= n;
    }
}

// Append the opcodes for literals `lits` followed by a match of `m` (>= 3) bytes at distance `d` (<= 65535).
fn emit_token(out: &mut Vec<u8>, lits: &[u8], m: usize, d: usize, d_prev: usize) {
    let (big, small) = if lits.len() <= 3 {
        (&lits[..0], lits)
    } else {
        (lits, &lits[..0])
    };
    emit_literals(out, big);
    let l = small.len();
    if d == d_prev && l == 0 {
        emit_repeat(out, m);
        return;
    }
    let first = if d == d_prev {
        let m1 = m.min(max_short_m(l));
        out.push(((l << 6) | ((m1 - 3) << 3) | 6) as u8);
        m1
    } else if d < 16384 && (m > max_short_m(l) || d >= 1536) {
        let m1 = m.min(34);
        let w = (d << 2) | ((m1 - 3) & 3);
        out.extend_from_slice(&[
            (0xA0 | (l << 3) | ((m1 - 3) >> 2)) as u8,
            w as u8,
            (w >> 8) as u8,
        ]);
        m1
    } else if d < 1536 {
        let m1 = m.min(max_short_m(l));
        out.extend_from_slice(&[((l << 6) | ((m1 - 3) << 3) | (d >> 8)) as u8, d as u8]);
        m1
    } else {
        let m1 = m.min(max_short_m(l));
        out.extend_from_slice(&[
            ((l << 6) | ((m1 - 3) << 3) | 7) as u8,
            d as u8,
            (d >> 8) as u8,
        ]);
        m1
    };
    out.extend_from_slice(small);
    emit_repeat(out, m - first);
}

// Hash 4 bytes into a table index.
#[inline(always)]
fn hash(x: u32, shift: u32) -> usize {
    (x.wrapping_mul(0x9E37_79B1) >> shift) as usize
}

/// Compress `src` as an LZVN payload (ending with the end-of-stream opcode) appended to `out`.
pub fn encode(src: &[u8], out: &mut Vec<u8>) {
    encode_with_table(src, out, &mut Vec::new());
}

// Encode with a caller-owned hash table, which is resized to suit the input (at most 64 KiB) and cleared.
pub(crate) fn encode_with_table(src: &[u8], out: &mut Vec<u8>, table: &mut Vec<u32>) {
    // Small inputs get small tables: about one bucket per 4 input bytes, between 2^8 and 2^14.
    let bits = (usize::BITS - src.len().leading_zeros())
        .saturating_sub(2)
        .clamp(8, MAX_HASH_BITS);
    let shift = 32 - bits;
    table.clear();
    table.resize(1 << bits, 0);
    let n = src.len();
    let (mut i, mut lit, mut d_prev) = (0usize, 0usize, 0usize);
    while i + 4 <= n {
        let x = load4(src, i);
        let h = hash(x, shift);
        let cand = table[h] as usize;
        table[h] = (i + 1) as u32;
        let (mut best_len, mut best_d) = (0usize, 0usize);
        if d_prev > 0 && d_prev <= i {
            let len = match_len(src, i - d_prev, i);
            if len >= 3 {
                (best_len, best_d) = (len, d_prev);
            }
        }
        if cand > 0 {
            let c = cand - 1;
            let d = i - c;
            if d <= MAX_DISTANCE && load4(src, c) == x {
                let len = 4 + match_len(src, c + 4, i + 4);
                if len > best_len + 1 {
                    (best_len, best_d) = (len, d);
                }
            }
        }
        if best_len < 3 {
            i += 1;
            continue;
        }
        // Extend backwards over pending literals.
        let back = match_len_back(src, i - best_d, i, i - lit);
        let start = i - back;
        let m = best_len + back;
        emit_token(out, &src[lit..start], m, best_d, d_prev);
        d_prev = best_d;
        let end = start + m;
        let mut j = i + 1;
        while j + 4 <= n && j < end {
            table[hash(load4(src, j), shift)] = (j + 1) as u32;
            j += 1;
        }
        i = end;
        lit = end;
    }
    emit_literals(out, &src[lit..]);
    out.extend_from_slice(&[EOS, 0, 0, 0, 0, 0, 0, 0]);
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    // Round-trip a buffer through the encoder and decoder.
    fn round_trip(data: &[u8]) -> Vec<u8> {
        let mut enc = Vec::new();
        encode(data, &mut enc);
        let mut dec = vec![0u8; data.len()];
        decode_block(&enc, &mut dec, 0, data.len()).unwrap();
        assert_eq!(dec, data);
        enc
    }

    // Text, repeats, long runs, short inputs and far distances all round-trip.
    #[test]
    fn round_trips() {
        round_trip(b"");
        round_trip(b"a");
        round_trip(b"abc");
        round_trip(b"hello hello hello hello world");
        let text = b"hello compressible text line\n".repeat(4000);
        let enc = round_trip(&text);
        assert!(enc.len() < 2000, "{}", enc.len());
        round_trip(&[7u8; 100_000]);
        let mut far: Vec<u8> = (0..70_000u32)
            .map(|i| (i.wrapping_mul(2654435761) >> 13) as u8)
            .collect();
        far.extend_from_within(10..3000);
        far.extend_from_within(60_000..62_000);
        round_trip(&far);
        let mixed: Vec<u8> = (0..20_000u32)
            .map(|i| {
                if i % 97 < 50 {
                    (i % 7) as u8
                } else {
                    (i * 31 % 251) as u8
                }
            })
            .collect();
        round_trip(&mixed);
    }

    // Every (literal count, match length, distance class) combination encodes to opcodes the decoder accepts.
    #[test]
    fn every_token_shape_round_trips() {
        let base: Vec<u8> = (0..70_000u32)
            .map(|i| (i.wrapping_mul(2246822519) >> 11) as u8)
            .collect();
        for l in 0..6 {
            for m in [3usize, 4, 5, 6, 7, 8, 9, 10, 11, 20, 34, 35, 300, 600] {
                for d in [1usize, 5, 255, 1535, 1536, 16383, 16384, 65535] {
                    for prev in [0usize, d] {
                        let pos = 66_000;
                        let mut out = Vec::new();
                        emit_token(&mut out, &base[pos..pos + l], m, d, prev);
                        out.push(EOS);
                        out.extend_from_slice(&[0; 7]);
                        let mut want = base[..pos + l].to_vec();
                        for _ in 0..m {
                            want.push(want[want.len() - d]);
                        }
                        let mut got = base[..pos].to_vec();
                        got.resize(want.len(), 0);
                        // Seed the decoder's previous distance with a match when the token relies on it.
                        if prev != 0 {
                            let mut pre = Vec::new();
                            emit_token(&mut pre, &[], 3, d, 0);
                            pre.extend_from_slice(&out);
                            let mut want2 = base[..pos - 3].to_vec();
                            for _ in 0..3 {
                                want2.push(want2[want2.len() - d]);
                            }
                            want2.extend_from_slice(&want[pos..]);
                            let mut got2 = base[..pos - 3].to_vec();
                            got2.resize(want2.len(), 0);
                            let end = want2.len();
                            // The repeated distance copies from the decoded prefix, so rebuild the expectation from it.
                            let mut expect = base[..pos - 3].to_vec();
                            for _ in 0..3 {
                                expect.push(expect[expect.len() - d]);
                            }
                            expect.extend_from_slice(&base[pos..pos + l]);
                            for _ in 0..m {
                                expect.push(expect[expect.len() - d]);
                            }
                            decode_block(&pre, &mut got2, pos - 3, end).unwrap();
                            assert_eq!(got2, expect, "l={l} m={m} d={d} prev");
                            continue;
                        }
                        let end = want.len();
                        decode_block(&out, &mut got, pos, end).unwrap();
                        assert_eq!(got, want, "l={l} m={m} d={d}");
                    }
                }
            }
        }
    }

    // Truncated, reserved-opcode and bad-distance streams are errors, never panics.
    #[test]
    fn bad_streams_are_errors() {
        let mut enc = Vec::new();
        encode(&b"abcabcabcabcabcabc xyz xyz xyz".repeat(20), &mut enc);
        let mut out = vec![0u8; 1000];
        for cut in 0..enc.len() {
            assert!(decode(&enc[..cut], &mut out).is_err());
        }
        for op in [0x1E, 0x70, 0xD5] {
            assert_eq!(
                decode(&[op, 0, 0, 0, 0, 0, 0, 0, 0], &mut out),
                Err(Error::Corrupt)
            );
        }
        assert_eq!(
            decode(&[0xF1, 6, 0, 0, 0, 0, 0, 0, 0], &mut out),
            Err(Error::Corrupt)
        );
        assert_eq!(
            decode(&[0xE1, b'a', 0x00, 2, 6, 0, 0, 0, 0, 0, 0, 0], &mut out),
            Err(Error::Corrupt)
        );
        assert_eq!(
            decode(&[0xE3, 1, 2, 3, 6, 0, 0, 0, 0, 0, 0, 0], &mut [0u8; 2]),
            Err(Error::OutputTooSmall)
        );
    }
}
