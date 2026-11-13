// LZFSE stream encoder: chooses the block type, finds matches and hands tokens to the block writer.
mod block;

use alloc::vec;
use alloc::vec::Vec;

use crate::format::*;
use crate::kernels::{load4, match_len, match_len_back};
use crate::lzvn;
use block::Blocks;

#[cfg(test)]
pub(crate) use block::to_v1;

/// Inputs below this size are encoded as a single LZVN block, as Apple's encoder does.
pub const LZVN_THRESHOLD: usize = 4096;
// Positions are stored as u32, so very large inputs are cut into independent segments of this size.
const SEGMENT: usize = 1 << 30;
const WAYS: usize = 4;

/// Encoder tuning knobs. The defaults aim for Apple's compression ratio at higher speed.
#[derive(Debug, Clone, Copy)]
pub struct Options {
    /// log2 of the number of match-finder hash buckets (each holds 4 candidates).
    pub hash_bits: u32,
    /// Stop searching (and skip lazy evaluation) once a match is at least this long.
    pub good_match: usize,
    /// Try a match one byte later before committing to the current one.
    pub lazy: bool,
    /// After `2^skip_shift` consecutive positions without a match, step further ahead (0 disables skipping).
    pub skip_shift: u32,
}

impl Default for Options {
    // Defaults tuned on the benchmark corpus (docs/benchmarks).
    fn default() -> Self {
        Options {
            hash_bits: 14,
            good_match: 40,
            lazy: true,
            skip_shift: 6,
        }
    }
}

/// Compress `src` into a complete LZFSE stream with default options.
pub fn encode(src: &[u8]) -> Vec<u8> {
    encode_with(src, &Options::default())
}

/// Compress `src` into a complete LZFSE stream.
pub fn encode_with(src: &[u8], opts: &Options) -> Vec<u8> {
    let mut out = Vec::with_capacity(src.len() / 2 + 64);
    if src.len() < 8 {
        push_raw(&mut out, src);
    } else if src.len() < LZVN_THRESHOLD {
        let mut payload = Vec::with_capacity(src.len() + 16);
        lzvn::encode(src, &mut payload);
        if payload.len() < src.len() {
            out.extend_from_slice(&MAGIC_LZVN.to_le_bytes());
            out.extend_from_slice(&(src.len() as u32).to_le_bytes());
            out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
            out.extend_from_slice(&payload);
        } else {
            push_raw(&mut out, src);
        }
    } else {
        for seg in src.chunks(SEGMENT) {
            out = encode_segment(seg, opts, out);
        }
    }
    out.extend_from_slice(&MAGIC_EOS.to_le_bytes());
    out
}

// Append a raw (`bvx-`) block holding `src`, unless it is empty.
fn push_raw(out: &mut Vec<u8>, src: &[u8]) {
    if src.is_empty() {
        return;
    }
    out.extend_from_slice(&MAGIC_RAW.to_le_bytes());
    out.extend_from_slice(&(src.len() as u32).to_le_bytes());
    out.extend_from_slice(src);
}

// One hash bucket: the most recent positions (+1, 0 = empty) and the 4 bytes found there.
#[derive(Clone, Copy, Default)]
struct Bucket {
    pos: [u32; WAYS],
    val: [u32; WAYS],
}

// A candidate match: where it starts, how long it is and how far back it points.
#[derive(Clone, Copy)]
struct Match {
    start: usize,
    len: usize,
    dist: usize,
}

// Hash table plus the state needed to find matches in one segment.
struct Finder<'a> {
    src: &'a [u8],
    table: Vec<Bucket>,
    shift: u32,
    good: usize,
}

impl<'a> Finder<'a> {
    // Hash 4 bytes into a bucket index.
    #[inline(always)]
    fn hash(&self, x: u32) -> usize {
        (x.wrapping_mul(0x9E37_79B1) >> self.shift) as usize
    }

    // Record position `i` (whose 4 bytes are `x`) in its bucket, evicting the oldest entry.
    #[inline(always)]
    fn insert(&mut self, i: usize, x: u32) {
        let b = &mut self.table[(x.wrapping_mul(0x9E37_79B1) >> self.shift) as usize];
        b.pos = [i as u32 + 1, b.pos[0], b.pos[1], b.pos[2]];
        b.val = [x, b.val[0], b.val[1], b.val[2]];
    }

    // Find the best match at `i` (not starting before `lit`), checking the bucket and the previous distance, then
    // insert `i`.
    #[inline(always)]
    fn find(&mut self, i: usize, lit: usize, d_prev: usize) -> Option<Match> {
        let x = load4(self.src, i);
        let b = self.table[self.hash(x)];
        let mut best = Match {
            start: i,
            len: 0,
            dist: 0,
        };
        if d_prev != 0 && d_prev <= i && load4(self.src, i - d_prev) == x {
            best.len = 4 + match_len(self.src, i - d_prev + 4, i + 4);
            best.dist = d_prev;
        }
        for k in 0..WAYS {
            if best.len >= self.good {
                break;
            }
            let p = b.pos[k] as usize;
            if p == 0 || b.val[k] != x {
                continue;
            }
            let c = p - 1;
            let dist = i - c;
            if dist > MAX_D as usize {
                break;
            }
            let len = 4 + match_len(self.src, c + 4, i + 4);
            if len > best.len {
                best = Match {
                    start: i,
                    len,
                    dist,
                };
            }
        }
        self.insert(i, x);
        if best.len < 4 {
            return None;
        }
        let back = match_len_back(self.src, i - best.dist, i, i - lit);
        best.start -= back;
        best.len += back;
        Some(best)
    }
}

// Tokenise one segment and append its blocks to `out`.
fn encode_segment(src: &[u8], opts: &Options, out: Vec<u8>) -> Vec<u8> {
    let mut f = Finder {
        src,
        table: vec![Bucket::default(); 1 << opts.hash_bits],
        shift: 32 - opts.hash_bits,
        good: opts.good_match,
    };
    let mut blocks = Blocks::new(src, 0, out);
    let limit = src.len().saturating_sub(8);
    let (mut i, mut lit, mut d_prev, mut misses) = (0usize, 0usize, 0usize, 0usize);
    while i < limit {
        let Some(mut m) = f.find(i, lit, d_prev) else {
            misses += 1;
            i += if opts.skip_shift == 0 {
                1
            } else {
                1 + (misses >> opts.skip_shift)
            };
            continue;
        };
        misses = 0;
        if opts.lazy && m.len < opts.good_match && i + 1 < limit {
            if let Some(next) = f.find(i + 1, lit, d_prev) {
                if next.len > m.len {
                    m = next;
                }
            }
        }
        blocks.push(&src[lit..m.start], m.len as u32, m.dist as u32);
        d_prev = m.dist;
        let end = m.start + m.len;
        // Index positions inside the match so later data can refer to them; long matches only at their edges.
        let mut j = (i + 2).max(m.start + 1);
        while j < end.min(limit) {
            f.insert(j, load4(src, j));
            j += if m.len > 64 && j + 16 < end && j > m.start + 16 {
                end - 16 - j
            } else {
                1
            };
        }
        i = end;
        lit = end;
    }
    blocks.push(&src[lit..], 0, 0);
    blocks.flush();
    blocks.out
}
