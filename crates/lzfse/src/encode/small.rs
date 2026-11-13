// Match finder for inputs of at most 64 KiB (one APFS chunk): 16-bit positions in tables small enough to stay in
// the L1 cache, cleared per input, and no distance limit to check since every distance is below MAX_D.
use alloc::vec::Vec;

use super::block::Blocks;
use super::{parse, Find, Match, Options};
use crate::kernels::{load4, load8, match_len, match_len_back};

/// Largest input the fast path handles; positions then fit in 16 bits.
pub const SMALL_MAX: usize = 1 << 16;
const WAYS: usize = 4;

// Tokenise `src` (at most SMALL_MAX bytes) into `blocks` and `out`, with tables sized to it: 13 hash bits for the
// 4-byte table at 64 KiB (192 KiB), down to 10 bits at 8 KiB and below, and one bit more for the 8-byte table.
// The size is a compile-time constant per variant, which keeps the hash shifts out of registers.
pub fn encode(
    src: &[u8],
    table: &mut Vec<SmallBucket>,
    long: &mut Vec<u16>,
    blocks: &mut Blocks,
    opts: &Options,
    out: &mut Vec<u8>,
) {
    let bits = (usize::BITS - (src.len() - 1).leading_zeros()).saturating_sub(3);
    match bits {
        13.. => parse(
            src,
            &mut SmallFinder::<13>::new(src, table, long, opts.good_match),
            blocks,
            opts,
            out,
        ),
        12 => parse(
            src,
            &mut SmallFinder::<12>::new(src, table, long, opts.good_match),
            blocks,
            opts,
            out,
        ),
        11 => parse(
            src,
            &mut SmallFinder::<11>::new(src, table, long, opts.good_match),
            blocks,
            opts,
            out,
        ),
        _ => parse(
            src,
            &mut SmallFinder::<10>::new(src, table, long, opts.good_match),
            blocks,
            opts,
            out,
        ),
    }
}

// One bucket of the 4-byte table: the most recent positions and the 4 bytes found there.
#[derive(Clone, Copy, Default)]
pub struct SmallBucket {
    pos: [u16; WAYS],
    val: [u32; WAYS],
}

// The fast path's tables (2^BITS buckets, 2^(BITS+1) long slots) plus the input they index.
struct SmallFinder<'a, const BITS: u32> {
    src: &'a [u8],
    table: &'a mut [SmallBucket],
    long: &'a mut [u16],
    good: usize,
}

impl<'a, const BITS: u32> SmallFinder<'a, BITS> {
    // Reset the tables for `src`. Every entry starts as position 0 with its true bytes, so an unused entry is a
    // genuine (if useless) candidate and needs no separate validity check.
    fn new(
        src: &'a [u8],
        table: &'a mut Vec<SmallBucket>,
        long: &'a mut Vec<u16>,
        good: usize,
    ) -> Self {
        let first = SmallBucket {
            pos: [0; WAYS],
            val: [load4(src, 0); WAYS],
        };
        table.clear();
        table.resize(1 << BITS, first);
        long.clear();
        long.resize(2 << BITS, 0);
        SmallFinder {
            src,
            table,
            long,
            good,
        }
    }

    // Hash 4 bytes into a bucket index.
    #[inline(always)]
    fn hash(&self, x: u32) -> usize {
        (x.wrapping_mul(0x9E37_79B1) >> (32 - BITS)) as usize
    }

    // Hash 8 bytes into a long-table slot.
    #[inline(always)]
    fn lhash(&self, y: u64) -> usize {
        (y.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> (63 - BITS)) as usize
    }

    // Record position `i` (whose 4 bytes are `x`) in its bucket, evicting the oldest entry.
    #[inline(always)]
    fn insert_short(&mut self, i: usize, x: u32) {
        let h = self.hash(x);
        let b = &mut self.table[h];
        b.pos = [i as u16, b.pos[0], b.pos[1], b.pos[2]];
        b.val = [x, b.val[0], b.val[1], b.val[2]];
    }
}

impl<const BITS: u32> Find for SmallFinder<'_, BITS> {
    // Record position `i` in both tables.
    #[inline(always)]
    fn insert(&mut self, i: usize) {
        let y = load8(self.src, i);
        self.insert_short(i, y as u32);
        let lh = self.lhash(y);
        self.long[lh] = i as u16;
    }

    // Find the best match at `i` (not starting before `lit`) among the previous distance, the long table and the
    // bucket, then insert `i`.
    #[inline(always)]
    fn find(&mut self, i: usize, lit: usize, d_prev: usize) -> Option<Match> {
        let src = self.src;
        let y = load8(src, i);
        let x = y as u32;
        let b = self.table[self.hash(x)];
        let lh = self.lhash(y);
        let lp = self.long[lh] as usize;
        self.long[lh] = i as u16;
        let mut best = Match {
            start: i,
            len: 0,
            dist: 0,
        };
        if d_prev != 0 && d_prev <= i && load4(src, i - d_prev) == x {
            best.len = 4 + match_len(src, i - d_prev + 4, i + 4);
            best.dist = d_prev;
        }
        // Slot 0 at i = 0 (or a stale slot pointing at `i` itself) gives distance 0 and is skipped.
        if best.len < self.good && lp < i && load8(src, lp) == y {
            let len = 8 + match_len(src, lp + 8, i + 8);
            if len > best.len {
                best = Match {
                    start: i,
                    len,
                    dist: i - lp,
                };
            }
        }
        for k in 0..WAYS {
            if best.len >= self.good {
                break;
            }
            let p = b.pos[k] as usize;
            if b.val[k] != x || p >= i {
                continue;
            }
            let len = 4 + match_len(src, p + 4, i + 4);
            if len > best.len {
                best = Match {
                    start: i,
                    len,
                    dist: i - p,
                };
            }
        }
        self.insert_short(i, x);
        if best.len < 4 {
            return None;
        }
        let back = match_len_back(src, i - best.dist, i, i - lit);
        best.start -= back;
        best.len += back;
        Some(best)
    }
}
