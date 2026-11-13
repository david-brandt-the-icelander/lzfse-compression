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

// Spare bytes the fast copies may write past the requested end; callers leave this much room.
pub const SLACK: usize = 32;

// Copy `n` bytes from `src[at..]` to `dst[pos..]` in 16-byte steps, possibly writing up to 15 bytes past
// `pos + n`. Falls back to an exact copy if either buffer lacks the 15 spare bytes.
#[inline(always)]
#[allow(unsafe_code)]
pub fn wild_copy(dst: &mut [u8], pos: usize, src: &[u8], at: usize, n: usize) {
    if at + n + 15 > src.len() || pos + n + 15 > dst.len() {
        dst[pos..pos + n].copy_from_slice(&src[at..at + n]);
        return;
    }
    let (s, d) = (src[at..].as_ptr(), dst[pos..].as_mut_ptr());
    let mut c = 0;
    while c < n {
        // SAFETY: the check above keeps both 16-byte ranges inside their buffers, and `src` and `dst` are distinct.
        unsafe { core::ptr::copy_nonoverlapping(s.add(c), d.add(c), 16) };
        c += 16;
    }
}

// Like `copy_match`, but with wide steps that may write up to 15 bytes past `pos + m`. Falls back to the exact
// copy if `out` lacks the spare bytes or the distance is out of range.
#[inline(always)]
#[allow(unsafe_code)]
pub fn copy_match_wild(out: &mut [u8], pos: usize, d: usize, m: usize) {
    if d < 8 || d > pos || pos + m + 15 > out.len() {
        copy_match(out, pos, d, m);
        return;
    }
    let base = out.as_mut_ptr();
    let step = if d >= 16 { 16 } else { 8 };
    let mut c = 0;
    while c < m {
        // SAFETY: both ranges lie within `out` (checked above). With `step <= d` the source range ends at or before
        // the destination range starts, so they never overlap, and each source byte was written by an earlier step.
        unsafe { core::ptr::copy_nonoverlapping(base.add(pos - d + c), base.add(pos + c), step) };
        c += step;
    }
}

// Safe reference versions of the wide copies, which the unsafe ones are tested against.
#[cfg(test)]
mod reference {
    // Copy `n` bytes in 16-byte steps with checked slices.
    pub fn wild_copy(dst: &mut [u8], pos: usize, src: &[u8], at: usize, n: usize) {
        let mut c = 0;
        while c < n {
            dst[pos + c..pos + c + 16].copy_from_slice(&src[at + c..at + c + 16]);
            c += 16;
        }
    }

    // Copy a match in 8- or 16-byte steps with checked slices, or exactly for short distances.
    pub fn copy_match_wild(out: &mut [u8], pos: usize, d: usize, m: usize) {
        if d < 8 {
            super::copy_match(out, pos, d, m);
            return;
        }
        let step = if d >= 16 { 16 } else { 8 };
        let mut c = 0;
        while c < m {
            let mut t = [0u8; 16];
            t[..step].copy_from_slice(&out[pos - d + c..pos - d + c + step]);
            out[pos + c..pos + c + step].copy_from_slice(&t[..step]);
            c += step;
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

    // The unsafe wide copies write exactly what the safe reference versions write, slack included.
    #[test]
    fn unsafe_copies_match_reference() {
        for d in 1..40 {
            for m in 0..90 {
                let mut a: alloc::vec::Vec<u8> =
                    (0..200u8).chain(core::iter::repeat(0).take(200)).collect();
                let mut b = a.clone();
                copy_match_wild(&mut a, 200, d, m);
                reference::copy_match_wild(&mut b, 200, d, m);
                assert_eq!(a, b, "d={d} m={m}");
            }
        }
        let src: alloc::vec::Vec<u8> = (0..120u8).collect();
        for n in 0..90 {
            let (mut a, mut b) = ([0u8; 120], [0u8; 120]);
            wild_copy(&mut a, 3, &src, 5, n);
            reference::wild_copy(&mut b, 3, &src, 5, n);
            assert_eq!(a, b, "n={n}");
        }
        // Without slack the kernels fall back to exact copies and stay in bounds.
        let mut tight = [0u8; 20];
        wild_copy(&mut tight, 0, &src, 0, 20);
        assert_eq!(tight[..], src[..20]);
        let mut tight: alloc::vec::Vec<u8> = (0..30u8).collect();
        copy_match_wild(&mut tight, 20, 10, 10);
        assert_eq!(tight[20..], tight[10..20]);
    }

    // The wide copies produce the same bytes as the exact ones inside the requested range.
    #[test]
    fn wild_copies_match_exact() {
        for d in 1..40 {
            for m in 0..90 {
                let mut a: alloc::vec::Vec<u8> =
                    (0..200u8).chain(core::iter::repeat(0).take(200)).collect();
                let mut b = a.clone();
                copy_match_wild(&mut a, 200, d, m);
                copy_match(&mut b, 200, d, m);
                assert_eq!(a[..200 + m], b[..200 + m], "d={d} m={m}");
            }
        }
        let src: alloc::vec::Vec<u8> = (0..100u8).collect();
        let mut dst = [0u8; 100];
        wild_copy(&mut dst, 3, &src, 5, 40);
        assert_eq!(dst[3..43], src[5..45]);
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
