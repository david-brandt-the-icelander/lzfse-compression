// Diagnostics: totals and token distributions of an LZFSE stream, read from its block headers and match stream
// without decoding any output. Used by the benchmark to compare encoders' choices on the same input.
use crate::bits::BitReader;
use crate::format::*;
use crate::fse::{self, ValueEntry};
use crate::Error;

/// Upper bounds (exclusive) of the match-length buckets.
pub const LEN_BUCKETS: [u32; 8] = [8, 16, 32, 64, 256, 1024, 4096, u32::MAX];
/// Upper bounds (exclusive) of the distance buckets.
pub const DIST_BUCKETS: [u32; 6] = [16, 256, 4096, 65_536, 131_072, u32::MAX];

/// Totals and distributions accumulated over one or more streams.
#[derive(Debug, Default, Clone)]
pub struct StreamStats {
    /// Decoded bytes.
    pub raw: u64,
    /// Stored bytes: whole streams, headers included.
    pub stored: u64,
    /// Blocks by type: LZFSE (`bvx1`/`bvx2`), LZVN (`bvxn`) and uncompressed (`bvx-`).
    pub blocks: [u64; 3],
    /// Bytes of LZFSE block headers (frequency tables included).
    pub header_bytes: u64,
    /// Bytes of LZFSE literal streams.
    pub literal_bytes: u64,
    /// Bytes of LZFSE match (L, M, D) streams.
    pub lmd_bytes: u64,
    /// Bytes stored in LZVN and uncompressed blocks, headers included.
    pub other_bytes: u64,
    /// Literals in LZFSE blocks.
    pub literals: u64,
    /// Tokens in LZFSE blocks.
    pub tokens: u64,
    /// Tokens with no match (literals only).
    pub literal_only: u64,
    /// Matches that reuse the previous distance (D coded as 0).
    pub repeats: u64,
    /// Matches by length bucket (`LEN_BUCKETS`), and the bytes they cover.
    pub by_len: [(u64, u64); 8],
    /// Matches by distance bucket (`DIST_BUCKETS`), and the bytes they cover.
    pub by_dist: [(u64, u64); 6],
}

// Index of the first bucket whose bound exceeds `v`.
fn bucket(bounds: &[u32], v: u32) -> usize {
    bounds
        .iter()
        .position(|&b| v < b)
        .unwrap_or(bounds.len() - 1)
}

impl StreamStats {
    /// Add the blocks of one complete stream.
    pub fn add(&mut self, src: &[u8]) -> Result<(), Error> {
        let mut at = 0usize;
        let (mut l, mut m, mut d) = (
            [ValueEntry::default(); L_STATES as usize],
            [ValueEntry::default(); M_STATES as usize],
            [ValueEntry::default(); D_STATES as usize],
        );
        loop {
            match read_u32(src, at)? {
                MAGIC_EOS => {
                    self.stored += at as u64 + 4;
                    return Ok(());
                }
                MAGIC_RAW => {
                    let n = read_u32(src, at + 4)? as usize;
                    self.blocks[2] += 1;
                    self.raw += n as u64;
                    self.other_bytes += (RAW_HEADER_SIZE + n) as u64;
                    at += RAW_HEADER_SIZE + n;
                }
                MAGIC_LZVN => {
                    let size = read_u32(src, at + 8)? as usize;
                    self.blocks[1] += 1;
                    self.raw += read_u32(src, at + 4)? as u64;
                    self.other_bytes += (LZVN_HEADER_SIZE + size) as u64;
                    at += LZVN_HEADER_SIZE + size;
                }
                magic @ (MAGIC_V1 | MAGIC_V2) => {
                    let (h, size) = if magic == MAGIC_V1 {
                        parse_v1(&src[at..])?
                    } else {
                        parse_v2(&src[at..])?
                    };
                    h.validate()?;
                    let lmd_start = at + size + h.n_lit_payload as usize;
                    let lmd_end = lmd_start + h.n_lmd_payload as usize;
                    let lmd = src.get(lmd_start..lmd_end).ok_or(Error::Truncated)?;
                    self.blocks[0] += 1;
                    self.raw += h.n_raw as u64;
                    self.header_bytes += size as u64;
                    self.literal_bytes += h.n_lit_payload as u64;
                    self.lmd_bytes += h.n_lmd_payload as u64;
                    self.literals += h.n_literals as u64;
                    self.tokens += h.n_matches as u64;
                    fse::value_table(&h.l_freq, L_STATES, &L_EXTRA, &L_BASE, &mut l)?;
                    fse::value_table(&h.m_freq, M_STATES, &M_EXTRA, &M_BASE, &mut m)?;
                    fse::value_table(&h.d_freq, D_STATES, &D_EXTRA, &D_BASE, &mut d)?;
                    let mut r = BitReader::new(lmd, h.lmd_bits)?;
                    let (mut ls, mut ms, mut ds) =
                        (h.l_state as u32, h.m_state as u32, h.d_state as u32);
                    let mut d_prev = 0u32;
                    for _ in 0..h.n_matches {
                        r.refill()?;
                        fse::decode_value(&mut ls, &l, &mut r);
                        let mv = fse::decode_value(&mut ms, &m, &mut r);
                        let dv = fse::decode_value(&mut ds, &d, &mut r);
                        // Literal-only tokens carry a distance too, and it becomes the previous distance.
                        let dist = if dv == 0 { d_prev } else { dv };
                        d_prev = dist;
                        if mv == 0 {
                            self.literal_only += 1;
                            continue;
                        }
                        if dv == 0 {
                            self.repeats += 1;
                        }
                        let lb = &mut self.by_len[bucket(&LEN_BUCKETS, mv)];
                        lb.0 += 1;
                        lb.1 += mv as u64;
                        let db = &mut self.by_dist[bucket(&DIST_BUCKETS, dist)];
                        db.0 += 1;
                        db.1 += mv as u64;
                    }
                    at = lmd_end;
                }
                _ => return Err(Error::Corrupt),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec::Vec;

    // Totals read back from our own streams agree with what was encoded, for every block type.
    #[test]
    fn totals_match_the_input() {
        let text: Vec<u8> = b"the quick brown fox jumps over the lazy dog "
            .iter()
            .cycle()
            .take(300_000)
            .copied()
            .collect();
        let noise: Vec<u8> = (0..20_000u32)
            .map(|i| (i.wrapping_mul(2_654_435_761) >> 13) as u8)
            .collect();
        for data in [&text[..], &noise[..], &text[..1000], &[1u8, 2, 3][..]] {
            let packed = crate::encode(data);
            let mut s = StreamStats::default();
            s.add(&packed).unwrap();
            assert_eq!(s.raw, data.len() as u64);
            assert_eq!(s.stored, packed.len() as u64);
            assert_eq!(
                s.header_bytes + s.literal_bytes + s.lmd_bytes + s.other_bytes + 4,
                s.stored
            );
            // In all-LZFSE streams every byte is a literal or matched; each block pads its literals to a multiple of 4.
            if s.blocks == [s.blocks[0], 0, 0] && s.blocks[0] > 0 {
                let matched: u64 = s.by_len.iter().map(|b| b.1).sum();
                let unpadded = s.raw - matched;
                assert!(s.literals >= unpadded && s.literals < unpadded + 4 * s.blocks[0]);
            }
        }
    }
}
