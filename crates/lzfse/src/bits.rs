// Bit streams: the encoder writes forward, least-significant bit first; the decoder reads backwards from the end.
use alloc::vec::Vec;

use crate::Error;

// Forward bit writer with a 64-bit accumulator that appends whole bytes to a vector.
pub struct BitWriter {
    accum: u64,
    nbits: u32,
}

impl BitWriter {
    // Start an empty writer.
    pub fn new() -> Self {
        BitWriter { accum: 0, nbits: 0 }
    }

    // Append the low `n` bits of `value`; the caller flushes before 64 bits accumulate.
    #[inline(always)]
    pub fn push(&mut self, n: u32, value: u64) {
        debug_assert!(self.nbits + n <= 64 && (n == 64 || value >> n == 0));
        if n != 0 {
            self.accum |= value << self.nbits;
            self.nbits += n;
        }
    }

    // Move whole bytes from the accumulator to `out`, leaving at most 7 bits.
    #[inline(always)]
    pub fn flush(&mut self, out: &mut Vec<u8>) {
        let bytes = (self.nbits / 8) as usize;
        out.extend_from_slice(&self.accum.to_le_bytes()[..bytes]);
        self.accum = if bytes == 8 {
            0
        } else {
            self.accum >> (8 * bytes)
        };
        self.nbits -= 8 * bytes as u32;
    }

    // Write the remaining bits zero-padded to a byte and return the padding as a count in [-7, 0].
    pub fn finish(mut self, out: &mut Vec<u8>) -> i32 {
        self.flush(out);
        if self.nbits == 0 {
            return 0;
        }
        out.push(self.accum as u8);
        self.nbits as i32 - 8
    }
}

// Backward bit reader over `buf`, holding 56 to 63 bits after each refill, as Apple's decoder does.
pub struct BitReader<'a> {
    buf: &'a [u8],
    pos: usize,
    accum: u64,
    nbits: u32,
}

impl<'a> BitReader<'a> {
    // Start reading at the end of `buf`, whose last byte holds `-bits` padding bits (`bits` in [-7, 0]).
    pub fn new(buf: &'a [u8], bits: i32) -> Result<Self, Error> {
        let take = if bits == 0 { 7 } else { 8 };
        let pos = buf.len().checked_sub(take).ok_or(Error::Corrupt)?;
        let mut le = [0u8; 8];
        le[..take].copy_from_slice(&buf[pos..]);
        let accum = u64::from_le_bytes(le);
        let nbits = (8 * take as i32 + bits) as u32;
        if nbits < 56 || (nbits < 64 && accum >> nbits != 0) {
            return Err(Error::Corrupt);
        }
        Ok(BitReader {
            buf,
            pos,
            accum,
            nbits,
        })
    }

    // Load whole bytes from before the current position so at least 56 bits are available.
    #[inline(always)]
    pub fn refill(&mut self) -> Result<(), Error> {
        let bytes = ((63 - self.nbits) / 8) as usize;
        if bytes == 0 {
            return Ok(());
        }
        let start = self.pos.checked_sub(bytes).ok_or(Error::Corrupt)?;
        let mut le = [0u8; 8];
        le[..bytes].copy_from_slice(&self.buf[start..self.pos]);
        self.accum = (self.accum << (8 * bytes)) | u64::from_le_bytes(le);
        self.nbits += 8 * bytes as u32;
        self.pos = start;
        Ok(())
    }

    // Take the `n` most recently written bits; a refill must have made them available.
    #[inline(always)]
    pub fn pull(&mut self, n: u32) -> u32 {
        // Refills keep at least 56 bits and no caller pulls more than 54 between refills.
        debug_assert!(n <= 32 && n <= self.nbits);
        self.nbits -= n;
        let value = (self.accum >> self.nbits) as u32;
        self.accum &= (1u64 << self.nbits) - 1;
        value
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Bits written in order come back in reverse order, for every padding amount.
    #[test]
    fn writes_read_back_in_reverse() {
        for extra in 0..16u32 {
            let mut out = Vec::new();
            let mut w = BitWriter::new();
            let items: Vec<(u32, u64)> = (0..200u32)
                .map(|i| {
                    (
                        i % 17 + extra % 3,
                        (i as u64 * 2654435761) & ((1 << (i % 17 + extra % 3)) - 1),
                    )
                })
                .collect();
            for &(n, v) in &items {
                w.push(n, v);
                w.flush(&mut out);
            }
            let bits = w.finish(&mut out);
            assert!((-7..=0).contains(&bits));
            let mut padded = alloc::vec![0u8; 8];
            padded.extend_from_slice(&out);
            let mut r = BitReader::new(&padded, bits).unwrap();
            for &(n, v) in items.iter().rev() {
                r.refill().unwrap();
                assert_eq!(r.pull(n) as u64, v);
            }
        }
    }

    // A reader rejects buffers that are too short or have stray bits above the stated end.
    #[test]
    fn reader_rejects_bad_starts() {
        assert!(BitReader::new(&[0; 6], 0).is_err());
        assert!(BitReader::new(&[0; 7], -1).is_err());
        assert!(BitReader::new(&[0, 0, 0, 0, 0, 0, 0, 0x80], -1).is_err());
        assert!(BitReader::new(&[0, 0, 0, 0, 0, 0, 0, 0x40], -1).is_ok());
        let mut r = BitReader::new(&[0; 8], -1).unwrap();
        r.pull(32);
        r.pull(31);
        assert!(r.refill().is_err());
    }
}
