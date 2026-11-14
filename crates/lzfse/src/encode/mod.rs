// LZFSE stream encoder: chooses the block type, finds matches and hands tokens to the block writer.
mod block;
mod small;

use alloc::vec::Vec;

use crate::format::*;
use crate::kernels::{load4, load8, match_len, match_len_back};
use crate::lzvn;
use block::Blocks;
use small::{SmallBucket, SMALL_MAX};

#[cfg(test)]
pub(crate) use block::to_v1;

/// Inputs below this size are encoded as a single LZVN block, as Apple's encoder does.
pub const LZVN_THRESHOLD: usize = 4096;
// Positions are stored as u32, so very large inputs are cut into independent segments of this size.
const SEGMENT: usize = 1 << 30;
const WAYS: usize = 4;
// The long table (keyed on 8 bytes, one position per slot) has 2^LONG_EXTRA times as many slots as the short
// table has buckets.
const LONG_EXTRA: u32 = 2;

/// Encoder tuning knobs. The defaults aim for Apple's compression ratio at higher speed.
#[derive(Debug, Clone, Copy)]
pub struct Options {
    /// log2 of the number of match-finder hash buckets (each holds 4 candidates keyed on 4 bytes); the second
    /// table, keyed on 8 bytes, has four times as many single-position slots.
    pub hash_bits: u32,
    /// Stop searching (and skip lazy evaluation) once a match is at least this long.
    pub good_match: usize,
    /// Try a match one byte later before committing to the current one, if the current one is shorter than this
    /// (0 disables). Longer matches rarely improve, so checking them costs speed for no ratio.
    pub lazy_below: usize,
    /// After `2^skip_shift` consecutive positions without a match, step further ahead (0 disables skipping).
    pub skip_shift: u32,
}

impl Default for Options {
    // Defaults tuned on the benchmark corpus (docs/benchmarks).
    fn default() -> Self {
        Options {
            hash_bits: 14,
            good_match: 40,
            lazy_below: 8,
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
    Encoder::new(*opts).encode(src, &mut out);
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

/// A reusable encoder: its match-finder table and block buffers carry over between calls, so compressing many
/// small inputs (such as 64 KiB chunks) avoids reallocating and clearing them each time.
pub struct Encoder {
    opts: Options,
    table: Vec<Bucket>,
    // Positions keyed on 8 bytes: common short strings cannot evict the starts of long repeats from it.
    long: Vec<u32>,
    // Added to positions before they are stored, so entries from earlier inputs are recognisably stale.
    base: u32,
    blocks: Blocks,
    lzvn_table: Vec<u32>,
    // Tables of the 64 KiB fast path (16-bit positions, cleared per input).
    small: Vec<SmallBucket>,
    small_long: Vec<u16>,
}

impl Encoder {
    /// Change the options for later calls; the tables are kept (and grown if needed).
    pub fn set_options(&mut self, opts: Options) {
        self.opts = opts;
    }

    /// Create an encoder with the given options.
    pub fn new(opts: Options) -> Self {
        Encoder {
            opts,
            table: Vec::new(),
            long: Vec::new(),
            base: 1,
            blocks: Blocks::new(),
            lzvn_table: Vec::new(),
            small: Vec::new(),
            small_long: Vec::new(),
        }
    }

    /// Append the complete LZFSE stream for `src` to `out`.
    pub fn encode(&mut self, src: &[u8], out: &mut Vec<u8>) {
        if src.len() < 8 {
            push_raw(out, src);
        } else if src.len() < LZVN_THRESHOLD {
            let mark = out.len();
            out.extend_from_slice(&[0; LZVN_HEADER_SIZE]);
            lzvn::encode_with_table(src, out, &mut self.lzvn_table);
            let payload = out.len() - mark - LZVN_HEADER_SIZE;
            if payload < src.len() {
                out[mark..mark + 4].copy_from_slice(&MAGIC_LZVN.to_le_bytes());
                out[mark + 4..mark + 8].copy_from_slice(&(src.len() as u32).to_le_bytes());
                out[mark + 8..mark + 12].copy_from_slice(&(payload as u32).to_le_bytes());
            } else {
                out.truncate(mark);
                push_raw(out, src);
            }
        } else {
            for seg in src.chunks(SEGMENT) {
                self.encode_segment(seg, out);
            }
        }
        out.extend_from_slice(&MAGIC_EOS.to_le_bytes());
    }

    // Make the table ready for an input of `len` bytes: allocate it once, and clear it only when the position
    // base would overflow.
    // Returns the hash bits to use: inputs smaller than the table would fill use a smaller part of it.
    fn prepare(&mut self, len: usize) -> u32 {
        let fit = (usize::BITS - len.leading_zeros())
            .saturating_sub(2)
            .max(10);
        let bits = self.opts.hash_bits.min(fit);
        // Grow (never shrink) the table; stale entries are rejected by the position base, so no clearing is needed
        // unless the base would overflow.
        if self.table.len() < 1 << bits || self.base as u64 + len as u64 + 2 > u32::MAX as u64 {
            let size = (1usize << bits).max(self.table.len());
            self.table.clear();
            self.table.resize(size, Bucket::default());
            self.long.clear();
            self.long.resize(size << LONG_EXTRA, 0);
            self.base = 1;
        }
        bits
    }

    // Tokenise one segment and append its blocks to `out`.
    fn encode_segment(&mut self, src: &[u8], out: &mut Vec<u8>) {
        let opts = self.opts;
        self.blocks.reset();
        if src.len() <= SMALL_MAX {
            small::encode(
                src,
                &mut self.small,
                &mut self.small_long,
                &mut self.blocks,
                &opts,
                out,
            );
            return;
        }
        let bits = self.prepare(src.len());
        let mut f = Finder {
            src,
            table: &mut self.table,
            long: &mut self.long,
            lshift: 64 - bits - LONG_EXTRA,
            shift: 32 - bits,
            good: opts.good_match,
            base: self.base,
        };
        parse(src, &mut f, &mut self.blocks, &opts, out);
        // The next input's positions start past everything stored now, so none of these entries look valid then.
        self.base += src.len() as u32 + 1;
    }
}

/// EXPERIMENT (temporary): the build-time parse switches this crate was compiled with, for benchmark reports.
#[doc(hidden)]
pub fn experiment() -> (&'static str, &'static str) {
    (
        option_env!("LZFSE_MERGE").map_or("off", |_| "on"),
        option_env!("LZFSE_INSIDE").unwrap_or("edge"),
    )
}

// A match finder: the best match at a position, and indexing of positions skipped over by matches.
trait Find {
    // Find the best match at `i` (not starting before `lit`), checking the previous distance too, and index `i`.
    fn find(&mut self, i: usize, lit: usize, d_prev: usize) -> Option<Match>;
    // Index position `i` without searching.
    fn insert(&mut self, i: usize);
    // Index position `i` in the long table only.
    fn insert_long(&mut self, i: usize);
}

// Tokenise `src` with match finder `f` (greedy with one-step lazy evaluation) and write its blocks to `out`.
#[inline(always)]
fn parse<F: Find>(src: &[u8], f: &mut F, blocks: &mut Blocks, opts: &Options, out: &mut Vec<u8>) {
    let limit = src.len().saturating_sub(8);
    // EXPERIMENT (temporary): build-time switches for comparing parse variants on the owner's data; see
    // `experiment()`. They will be replaced by the winning behaviour.
    let merge = option_env!("LZFSE_MERGE").is_some();
    let inside = option_env!("LZFSE_INSIDE").unwrap_or("edge");
    let (mut i, mut lit, mut d_prev, mut misses) = (0usize, 0usize, 0usize, 0usize);
    // The last match found, not yet emitted: a later match may still swallow it. Its literals start at `lit`.
    let mut pend: Option<Match> = None;
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
        if m.len < opts.lazy_below && i + 1 < limit {
            if let Some(next) = f.find(i + 1, lit, d_prev) {
                if next.len > m.len {
                    m = next;
                }
            }
        }
        // Emit the held match, unless the new one reaches back over all of it (with LZFSE_MERGE) and replaces it.
        // A new match that reaches back only partly is cut where the held one ends.
        if let Some(p) = pend {
            let p_end = p.start + p.len;
            if !(merge && m.start <= p.start) {
                if m.start < p_end {
                    let cut = p_end - m.start;
                    m.start += cut;
                    m.len -= cut;
                }
                blocks.push(src, lit, p.start, p.len as u32, p.dist as u32, out);
                lit = p_end;
            }
        }
        pend = Some(m);
        d_prev = m.dist;
        let end = m.start + m.len;
        // Index positions inside the match so later data can refer to them. By default long matches are indexed
        // only at their edges; LZFSE_INSIDE=long also indexes their interior in the long table, =all in both.
        let mut j = (i + 2).max(m.start + 1);
        while j < end.min(limit) {
            let edge = !(m.len > 64 && j + 16 < end && j > m.start + 16);
            if inside == "all" || edge {
                f.insert(j);
                j += 1;
            } else if inside == "long" {
                f.insert_long(j);
                j += 1;
            } else {
                j = end - 16;
            }
        }
        i = end;
    }
    if let Some(p) = pend {
        blocks.push(src, lit, p.start, p.len as u32, p.dist as u32, out);
        lit = p.start + p.len;
    }
    blocks.push(src, lit, src.len(), 0, 0, out);
    blocks.flush(src, out);
}

// One hash bucket: the most recent stored positions (base + position) and the 4 bytes found there.
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

// The hash table plus what is needed to find matches in one segment.
struct Finder<'a> {
    src: &'a [u8],
    table: &'a mut [Bucket],
    long: &'a mut [u32],
    lshift: u32,
    shift: u32,
    good: usize,
    base: u32,
}

impl Finder<'_> {
    // Hash 4 bytes into a bucket index.
    #[inline(always)]
    fn hash(&self, x: u32) -> usize {
        (x.wrapping_mul(0x9E37_79B1) >> self.shift) as usize
    }

    // Hash 8 bytes into a long-table slot.
    #[inline(always)]
    fn lhash(&self, y: u64) -> usize {
        (y.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> self.lshift) as usize
    }

    // Record position `i` in its bucket of the 4-byte table, evicting the oldest entry.
    #[inline(always)]
    fn insert_short(&mut self, i: usize, x: u32) {
        let h = self.hash(x);
        let b = &mut self.table[h];
        b.pos = [self.base + i as u32, b.pos[0], b.pos[1], b.pos[2]];
        b.val = [x, b.val[0], b.val[1], b.val[2]];
    }
}

impl Find for Finder<'_> {
    // Record position `i` in the long table only.
    #[inline(always)]
    fn insert_long(&mut self, i: usize) {
        let lh = self.lhash(load8(self.src, i));
        self.long[lh] = self.base + i as u32;
    }

    // Record position `i` in both tables.
    #[inline(always)]
    fn insert(&mut self, i: usize) {
        let y = load8(self.src, i);
        self.insert_short(i, y as u32);
        let lh = self.lhash(y);
        self.long[lh] = self.base + i as u32;
    }

    // Find the best match at `i` (not starting before `lit`) among the previous distance, the long table and the
    // bucket, then insert `i`.
    #[inline(always)]
    fn find(&mut self, i: usize, lit: usize, d_prev: usize) -> Option<Match> {
        let y = load8(self.src, i);
        let x = y as u32;
        let b = self.table[self.hash(x)];
        let lh = self.lhash(y);
        let lp = self.long[lh];
        let mut best = Match {
            start: i,
            len: 0,
            dist: 0,
        };
        if d_prev != 0 && d_prev <= i && load4(self.src, i - d_prev) == x {
            best.len = 4 + match_len(self.src, i - d_prev + 4, i + 4);
            best.dist = d_prev;
        }
        let here = self.base + i as u32;
        self.long[lh] = here;
        let dist = here.wrapping_sub(lp) as usize;
        if best.len < self.good
            && dist != 0
            && dist <= i
            && dist <= MAX_D as usize
            && load8(self.src, i - dist) == y
        {
            let len = 8 + match_len(self.src, i - dist + 8, i + 8);
            if len > best.len {
                best = Match {
                    start: i,
                    len,
                    dist,
                };
            }
        }
        for k in 0..WAYS {
            if best.len >= self.good {
                break;
            }
            // Empty entries and entries from earlier inputs are more than `i` behind and fail this range check.
            // Comparing the stored bytes first rejects most candidates with one test.
            if b.val[k] != x {
                continue;
            }
            let dist = here.wrapping_sub(b.pos[k]) as usize;
            if dist == 0 || dist > i || dist > MAX_D as usize {
                continue;
            }
            let len = 4 + match_len(self.src, i - dist + 4, i + 4);
            if len > best.len {
                best = Match {
                    start: i,
                    len,
                    dist,
                };
            }
        }
        self.insert_short(i, x);
        if best.len < 4 {
            return None;
        }
        let back = match_len_back(self.src, i - best.dist, i, i - lit);
        best.start -= back;
        best.len += back;
        Some(best)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The 64 KiB fast path round-trips at each table-size boundary and on both sides of SMALL_MAX, and finds matches
    // that reach from the last 16-bit positions back to the first ones.
    #[test]
    fn small_path_boundaries() {
        for n in [
            4096usize, 8192, 8193, 16384, 16385, 32768, 32769, 65535, 65536, 65537,
        ] {
            let mut data: Vec<u8> = (0..n as u32)
                .map(|j| (j.wrapping_mul(2_654_435_761) >> 13) as u8)
                .collect();
            let q = n / 4;
            let head = data[..q].to_vec();
            data[n - q..].copy_from_slice(&head);
            let packed = encode(&data);
            assert_eq!(crate::decode(&packed, n).unwrap(), data, "size {n}");
            assert!(packed.len() < n - q + n / 16, "size {n}: far repeat missed");
        }
    }

    // One encoder reused across many inputs gives the same streams as fresh encoders.
    #[test]
    fn reused_encoder_matches_fresh() {
        let mut enc = Encoder::new(Options::default());
        for i in 0..40u32 {
            let data: Vec<u8> = (0..5000 + i * 997)
                .map(|j| (((j * (i + 3)) % 251) ^ (j / 13)) as u8)
                .collect();
            let mut reused = Vec::new();
            enc.encode(&data, &mut reused);
            assert_eq!(reused, encode(&data), "input {i}");
        }
    }
}
