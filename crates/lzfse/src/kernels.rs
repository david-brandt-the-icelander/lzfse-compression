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

// Exact slice copy, kept out of line so the fast paths' callers keep their registers.
#[cold]
#[inline(never)]
fn copy_exact(dst: &mut [u8], pos: usize, src: &[u8], at: usize, n: usize) {
    dst[pos..pos + n].copy_from_slice(&src[at..at + n]);
}

// Exact match copy (short distances and buffer ends), kept out of line for the same reason.
#[cold]
#[inline(never)]
pub fn copy_match_cold(out: &mut [u8], pos: usize, d: usize, m: usize) {
    copy_match(out, pos, d, m);
}

// Tell whether a buffer of `len` bytes has room for wide copies of `n` bytes starting at `at`: each kernel's
// first 16-byte step always runs, and later steps end at most 15 bytes past `at + n`.
#[inline(always)]
fn room(len: usize, at: usize, n: usize) -> bool {
    at.checked_add(n)
        .and_then(|e| e.checked_add(16))
        .is_some_and(|e| e <= len)
}

// Copy 16 bytes from `s` to `d` with one unaligned load and store.
// SAFETY (caller): both pointers are valid for 16 bytes.
#[inline(always)]
#[allow(unsafe_code)]
unsafe fn copy16(s: *const u8, d: *mut u8) {
    // SAFETY: guaranteed by the caller.
    unsafe {
        core::ptr::write_unaligned(
            d.cast::<[u8; 16]>(),
            core::ptr::read_unaligned(s.cast::<[u8; 16]>()),
        )
    }
}

// Copy 8 bytes from `s` to `d` with one unaligned load and store.
// SAFETY (caller): both pointers are valid for 8 bytes.
#[inline(always)]
#[allow(unsafe_code)]
unsafe fn copy8(s: *const u8, d: *mut u8) {
    // SAFETY: guaranteed by the caller.
    unsafe {
        core::ptr::write_unaligned(d.cast::<u64>(), core::ptr::read_unaligned(s.cast::<u64>()))
    }
}

// Copy `n` bytes from `src[at..]` to `dst[pos..]` in 16-byte steps, possibly writing up to 15 bytes past
// `pos + n`. Falls back to an exact copy if either buffer lacks the 15 spare bytes.
//
// The first two steps are unrolled so short copies (the common case) stay plain loads and stores instead of the
// `memcpy` call LLVM substitutes for a copy loop.
#[inline(always)]
#[allow(unsafe_code)]
pub fn wild_copy(dst: &mut [u8], pos: usize, src: &[u8], at: usize, n: usize) {
    // The first 16-byte step always runs (even for n = 0), so both buffers need `n + 16` bytes from the start.
    if !room(src.len(), at, n) || !room(dst.len(), pos, n) {
        copy_exact(dst, pos, src, at, n);
        return;
    }
    let (s, d) = (src[at..].as_ptr(), dst[pos..].as_mut_ptr());
    // SAFETY: the check above keeps every 16-byte step inside both buffers; `src` and `dst` are distinct slices.
    unsafe {
        copy16(s, d);
        if n > 16 {
            copy16(s.add(16), d.add(16));
            let mut c = 32;
            while c < n {
                copy16(s.add(c), d.add(c));
                c += 16;
            }
        }
    }
}

// Execute one decoded token: copy `l` literals from `lits[lit..]` to `out[pos..]`, then `m` bytes from distance
// `d` behind the new end. One combined check stands for all the copies' bounds, and the body makes no calls, so
// the decoder's hot loop keeps its state in registers. Returns false, having written nothing, when the token
// needs the careful path: too close to the end of either buffer, an invalid distance, or a long run of a
// pattern shorter than 8 bytes.
#[inline(always)]
#[allow(unsafe_code)]
pub fn token_wild(
    out: &mut [u8],
    pos: usize,
    lits: &[u8],
    lit: usize,
    l: usize,
    d: usize,
    m: usize,
) -> bool {
    let end = pos + l;
    let ok = l <= MAX_TOKEN
        && m <= MAX_TOKEN
        && end + m + SLACK <= out.len()
        && lit + l + 16 <= lits.len()
        && d.wrapping_sub(1) < end
        && (d >= 8 || m <= 64);
    if !ok {
        return false;
    }
    // SAFETY: the check above (with `l`, `m` below MAX_TOKEN so nothing overflows) keeps every step inside both
    // buffers: literal steps read at most `lit + l + 15 < lits.len()` and write at most `end + 15`; match steps
    // write at most `end + m + 15 < out.len()` and read from `end - d >= 0` onward. `lits` and `out` are distinct.
    // Each match step copies at most `d` bytes (or, for `d < 8`, one byte at a time and then 8 bytes from a
    // multiple of `d` at least 8 back), so it only reads bytes already written.
    unsafe {
        let s = lits.as_ptr().add(lit);
        let o = out.as_mut_ptr().add(pos);
        copy16(s, o);
        if l > 16 {
            let mut c = 16;
            while c < l {
                copy16(s.add(c), o.add(c));
                c += 16;
            }
        }
        let o = o.add(l);
        let src = o.sub(d);
        if d >= 16 {
            copy16(src, o);
            let mut c = 16;
            while c < m {
                copy16(src.add(c), o.add(c));
                c += 16;
            }
        } else if d >= 8 {
            copy8(src, o);
            let mut c = 8;
            while c < m {
                copy8(src.add(c), o.add(c));
                c += 8;
            }
        } else {
            for i in 0..8 {
                *o.add(i) = *src.add(i);
            }
            let p = d * 8usize.div_ceil(d);
            let mut c = 8;
            while c < m {
                copy8(o.add(c).sub(p), o.add(c));
                c += 8;
            }
        }
    }
    true
}

// Largest literal or match length a token can carry (MAX_M), so sums of positions never overflow.
const MAX_TOKEN: usize = 1 << 16;

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
        let src: alloc::vec::Vec<u8> = (0..120u8).collect();
        for n in 0..90 {
            let (mut a, mut b) = ([0u8; 120], [0u8; 120]);
            wild_copy(&mut a, 3, &src, 5, n);
            reference::wild_copy(&mut b, 3, &src, 5, n);
            assert_eq!(a[..3 + n], b[..3 + n], "n={n}");
            assert!(
                a[3 + n + 16..].iter().all(|&v| v == 0),
                "wrote past the slack: n={n}"
            );
        }
        // Without slack the kernels fall back to exact copies and stay in bounds.
        let mut tight = [0u8; 20];
        wild_copy(&mut tight, 0, &src, 0, 20);
        assert_eq!(tight[..], src[..20]);
    }

    // The token kernel matches literal-then-match copies done byte by byte, never writes past its slack, and
    // declines tokens it cannot do safely.
    #[test]
    fn token_wild_matches_reference() {
        let lits: alloc::vec::Vec<u8> = (100..220u8).collect();
        for d in 1..40usize {
            for l in [0usize, 1, 7, 16, 17, 33] {
                for m in [0usize, 1, 3, 8, 9, 16, 31, 64, 65, 90] {
                    let mut a: alloc::vec::Vec<u8> =
                        (0..60u8).chain(core::iter::repeat(0).take(300)).collect();
                    let before = a.clone();
                    let done = token_wild(&mut a, 60, &lits, 5, l, d, m);
                    if d < 8 && m > 64 {
                        assert!(!done && a == before, "d={d} l={l} m={m} should decline");
                        continue;
                    }
                    assert!(done, "d={d} l={l} m={m}");
                    let mut b = before.clone();
                    b[60..60 + l].copy_from_slice(&lits[5..5 + l]);
                    for i in 0..m {
                        b[60 + l + i] = b[60 + l + i - d];
                    }
                    let e = 60 + l + m;
                    assert_eq!(a[..e], b[..e], "d={d} l={l} m={m}");
                    assert_eq!(
                        a[e + 16..],
                        before[e + 16..],
                        "wrote past the slack: d={d} l={l} m={m}"
                    );
                }
            }
        }
        // Declines: distance 0 or beyond the start, too little room in the output or the literals.
        let mut out = [0u8; 100];
        assert!(!token_wild(&mut out, 10, &lits, 0, 2, 0, 4));
        assert!(!token_wild(&mut out, 10, &lits, 0, 2, 13, 4));
        assert!(!token_wild(&mut out, 60, &lits, 0, 2, 5, 10));
        assert!(!token_wild(&mut out, 10, &lits, 110, 2, 5, 4));
        assert_eq!(out, [0u8; 100]);
    }

    // The room check counts the always-run first step: zero-length copies still need 16 bytes, and nothing
    // overflows near usize::MAX.
    #[test]
    fn room_counts_the_first_step() {
        assert!(room(16, 0, 0));
        assert!(!room(15, 0, 0));
        assert!(room(20, 4, 0));
        assert!(!room(20, 5, 0));
        assert!(room(40, 4, 20));
        assert!(!room(39, 4, 20));
        assert!(!room(usize::MAX, usize::MAX - 4, 3));
    }

    // Kernels on buffers with no spare room stay in bounds (exact fallbacks), including zero-length copies.
    #[test]
    fn tight_buffers_use_exact_copies() {
        let src: alloc::vec::Vec<u8> = (0..40u8).collect();
        for at in 0..40 {
            for n in 0..(40 - at).min(20) {
                let mut dst = alloc::vec![0u8; n];
                wild_copy(&mut dst, 0, &src, at, n);
                assert_eq!(dst[..], src[at..at + n]);
            }
        }
        for d in 1..12 {
            for m in 0..20 {
                let mut out: alloc::vec::Vec<u8> =
                    (0..12u8).chain(core::iter::repeat(0).take(m)).collect();
                let before = out.clone();
                // Without room for its slack, the token kernel declines and leaves the buffer alone.
                assert!(!token_wild(&mut out, 12, &src, 0, 0, d, m), "d={d} m={m}");
                assert_eq!(out, before, "d={d} m={m}");
            }
        }
    }

    // The wide copies produce the same bytes as the exact ones inside the requested range.
    #[test]
    fn wild_copies_match_exact() {
        for d in 1..40 {
            for m in 0..90 {
                let mut a: alloc::vec::Vec<u8> =
                    (0..200u8).chain(core::iter::repeat(0).take(200)).collect();
                let mut b = a.clone();
                if token_wild(&mut a, 200, &[0; 16], 0, 0, d, m) {
                    copy_match(&mut b, 200, d, m);
                    assert_eq!(a[..200 + m], b[..200 + m], "d={d} m={m}");
                } else {
                    assert!(d < 8 && m > 64, "declined d={d} m={m}");
                }
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
