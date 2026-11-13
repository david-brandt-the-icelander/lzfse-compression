// Hot loops behind narrow interfaces, so faster backends (NEON, C, assembly) can replace the safe reference
// versions here and be tested against them.

// Count how many bytes match between `buf[a..]` and `buf[b..]` (a < b), stopping at the end of `buf`.
#[inline(always)]
pub fn match_len(buf: &[u8], a: usize, b: usize) -> usize {
    debug_assert!(a < b);
    let max = buf.len() - b;
    let mut n = 0;
    while n + 8 <= max {
        let x = load8(buf, a + n) ^ load8(buf, b + n);
        if x != 0 {
            return n + (x.trailing_zeros() / 8) as usize;
        }
        n += 8;
    }
    while n < max && buf[a + n] == buf[b + n] {
        n += 1;
    }
    n
}

// Count how many bytes match going backwards from `a` and `b` (exclusive), at most `max` bytes.
#[inline(always)]
pub fn match_len_back(buf: &[u8], a: usize, b: usize, max: usize) -> usize {
    let mut n = 0;
    while n < max && n < a && buf[a - 1 - n] == buf[b - 1 - n] {
        n += 1;
    }
    n
}

// Load 8 bytes little-endian; the caller guarantees `at + 8 <= buf.len()`.
#[inline(always)]
pub fn load8(buf: &[u8], at: usize) -> u64 {
    let mut b = [0u8; 8];
    b.copy_from_slice(&buf[at..at + 8]);
    u64::from_le_bytes(b)
}

// Load 4 bytes little-endian; the caller guarantees `at + 4 <= buf.len()`.
#[inline(always)]
pub fn load4(buf: &[u8], at: usize) -> u32 {
    let mut b = [0u8; 4];
    b.copy_from_slice(&buf[at..at + 4]);
    u32::from_le_bytes(b)
}

// Copy `m` bytes from distance `d` back to `out[pos..]`, repeating the pattern when the ranges overlap.
// The caller guarantees `1 <= d <= pos` and `pos + m <= out.len()`.
#[inline(always)]
pub fn copy_match(out: &mut [u8], pos: usize, d: usize, m: usize) {
    if d >= m {
        out.copy_within(pos - d..pos - d + m, pos);
        return;
    }
    // Overlapping: copy the pattern in growing chunks, each a whole number of periods.
    let mut done = 0;
    let mut chunk = d;
    while done < m {
        let n = chunk.min(m - done);
        out.copy_within(pos - d..pos - d + n, pos + done);
        done += n;
        chunk = done.max(d) - done % d;
        if chunk == 0 {
            chunk = d;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The overlapping copy matches a byte-by-byte copy for every small distance and length.
    #[test]
    fn copy_match_matches_bytewise() {
        for d in 1..20 {
            for m in 0..70 {
                let mut a: alloc::vec::Vec<u8> =
                    (0..100u8).chain(core::iter::repeat(0).take(80)).collect();
                let mut b = a.clone();
                copy_match(&mut a, 100, d, m);
                for i in 0..m {
                    b[100 + i] = b[100 + i - d];
                }
                assert_eq!(a, b, "d={d} m={m}");
            }
        }
    }

    // Match lengths stop at the first difference and at the end of the buffer.
    #[test]
    fn match_len_counts() {
        let buf = b"abcdefghabcdefghabcdXfghabcdefgh";
        assert_eq!(match_len(buf, 0, 8), 12);
        assert_eq!(match_len(buf, 0, 24), 8);
        assert_eq!(match_len_back(buf, 8, 16, 100), 8);
        assert_eq!(match_len_back(buf, 8, 16, 3), 3);
    }
}
