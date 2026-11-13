//! APFS/HFS+ transparent compression containers (decmpfs) for LZFSE (types 11 and 12) and LZVN (types 7 and 8).
//!
//! A compressed file has an empty data fork, a `com.apple.decmpfs` extended attribute starting with a 16-byte
//! header, and, for the resource-fork types, its data in the `com.apple.ResourceFork` attribute: a table of
//! little-endian `u32` offsets (one per 64 KiB chunk plus one for the end) followed by the chunks.
//! This module only builds and parses the bytes; applying them to a file is up to the caller.
use alloc::vec::Vec;

use crate::{lzvn, Error};

/// Uncompressed size of each chunk.
pub const CHUNK: usize = 65_536;
/// Largest `com.apple.decmpfs` attribute the kernel accepts.
pub const MAX_XATTR: usize = 3802;
/// Header magic, `fpmc` on disk.
pub const MAGIC: [u8; 4] = *b"fpmc";
/// LZVN data stored inline in the attribute.
pub const TYPE_LZVN_INLINE: u32 = 7;
/// LZVN data in the resource fork.
pub const TYPE_LZVN_FORK: u32 = 8;
/// LZFSE data stored inline in the attribute.
pub const TYPE_LZFSE_INLINE: u32 = 11;
/// LZFSE data in the resource fork.
pub const TYPE_LZFSE_FORK: u32 = 12;

/// The attributes that make a file transparently compressed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Compressed {
    /// Contents of `com.apple.decmpfs`.
    pub xattr: Vec<u8>,
    /// Contents of `com.apple.ResourceFork`, empty for the inline types.
    pub fork: Vec<u8>,
}

impl Compressed {
    /// Bytes the compressed form occupies across both attributes.
    pub fn stored_len(&self) -> usize {
        self.xattr.len() + self.fork.len()
    }
}

/// Build the 16-byte `com.apple.decmpfs` header.
pub fn header(kind: u32, size: u64) -> [u8; 16] {
    let mut h = [0u8; 16];
    h[..4].copy_from_slice(&MAGIC);
    h[4..8].copy_from_slice(&kind.to_le_bytes());
    h[8..].copy_from_slice(&size.to_le_bytes());
    h
}

/// Lay out already-compressed LZFSE chunks (one complete stream per 64 KiB of `size`) as decmpfs attributes,
/// inline when a single small chunk fits in the attribute, otherwise in the resource fork.
pub fn from_chunks(size: u64, chunks: &[Vec<u8>]) -> Compressed {
    if chunks.len() == 1 && 16 + chunks[0].len() <= MAX_XATTR {
        let mut xattr = header(TYPE_LZFSE_INLINE, size).to_vec();
        xattr.extend_from_slice(&chunks[0]);
        return Compressed {
            xattr,
            fork: Vec::new(),
        };
    }
    let table = 4 * (chunks.len() + 1);
    let mut fork = Vec::with_capacity(table + chunks.iter().map(Vec::len).sum::<usize>());
    let mut offset = table as u32;
    fork.extend_from_slice(&offset.to_le_bytes());
    for c in chunks {
        offset += c.len() as u32;
        fork.extend_from_slice(&offset.to_le_bytes());
    }
    for c in chunks {
        fork.extend_from_slice(c);
    }
    Compressed {
        xattr: header(TYPE_LZFSE_FORK, size).to_vec(),
        fork,
    }
}

/// Compress file contents into decmpfs attributes (LZFSE), using all cores when the `std` feature is on.
pub fn compress(data: &[u8]) -> Compressed {
    let n = data.len().div_ceil(CHUNK);
    let table = 4 * (n + 1);
    // Encode the chunks straight into the fork after a placeholder for the offset table, then fill in the table.
    let mut fork = alloc::vec![0u8; table];
    #[cfg(feature = "std")]
    let offsets = {
        let mut offsets = Vec::new();
        crate::encode_chunks_into(
            data,
            CHUNK,
            &crate::Options::default(),
            &mut fork,
            &mut offsets,
        );
        offsets
    };
    #[cfg(not(feature = "std"))]
    let offsets = {
        let mut offsets = alloc::vec![table];
        let mut enc = crate::Encoder::new(crate::Options::default());
        for piece in data.chunks(CHUNK) {
            enc.encode(piece, &mut fork);
            offsets.push(fork.len());
        }
        offsets
    };
    if n == 1 && 16 + fork.len() - table <= MAX_XATTR {
        let mut xattr = header(TYPE_LZFSE_INLINE, data.len() as u64).to_vec();
        xattr.extend_from_slice(&fork[table..]);
        return Compressed {
            xattr,
            fork: Vec::new(),
        };
    }
    for (i, &o) in offsets.iter().enumerate() {
        fork[4 * i..4 * i + 4].copy_from_slice(&(o as u32).to_le_bytes());
    }
    Compressed {
        xattr: header(TYPE_LZFSE_FORK, data.len() as u64).to_vec(),
        fork,
    }
}

/// Return (type, uncompressed size) from a `com.apple.decmpfs` attribute.
pub fn parse_header(xattr: &[u8]) -> Result<(u32, u64), Error> {
    if xattr.len() < 16 || xattr[..4] != MAGIC {
        return Err(Error::Corrupt);
    }
    let kind = u32::from_le_bytes([xattr[4], xattr[5], xattr[6], xattr[7]]);
    let mut s = [0u8; 8];
    s.copy_from_slice(&xattr[8..16]);
    Ok((kind, u64::from_le_bytes(s)))
}

// Decode one chunk of `kind` into `dst`; a chunk that is not a stream is stored raw after a one-byte marker.
fn decode_chunk(kind: u32, chunk: &[u8], dst: &mut [u8]) -> Result<(), Error> {
    let lzfse = kind == TYPE_LZFSE_INLINE || kind == TYPE_LZFSE_FORK;
    let raw = match chunk.first() {
        Some(b'b') if lzfse => None,
        Some(0x06) if !lzfse && chunk.len() == dst.len() + 1 => Some(&chunk[1..]),
        Some(_) if lzfse && chunk.len() == dst.len() + 1 => Some(&chunk[1..]),
        _ if !lzfse => None,
        _ => return Err(Error::Corrupt),
    };
    if let Some(raw) = raw {
        dst.copy_from_slice(raw);
        return Ok(());
    }
    let n = if lzfse {
        crate::decode_into(chunk, dst)?
    } else {
        lzvn::decode(chunk, dst)?
    };
    if n != dst.len() {
        return Err(Error::Corrupt);
    }
    Ok(())
}

/// Decompress a file from its decmpfs attributes (LZFSE or LZVN types), refusing sizes above `limit`.
pub fn decompress(xattr: &[u8], fork: &[u8], limit: u64) -> Result<Vec<u8>, Error> {
    let (kind, size) = parse_header(xattr)?;
    if size > limit {
        return Err(Error::OutputTooSmall);
    }
    let size = usize::try_from(size).map_err(|_| Error::OutputTooSmall)?;
    let mut out = alloc::vec![0u8; size];
    match kind {
        TYPE_LZFSE_INLINE | TYPE_LZVN_INLINE => decode_chunk(kind, &xattr[16..], &mut out)?,
        TYPE_LZFSE_FORK | TYPE_LZVN_FORK => {
            let n = size.div_ceil(CHUNK);
            let off = |i: usize| -> Result<usize, Error> {
                let b = fork.get(4 * i..4 * i + 4).ok_or(Error::Truncated)?;
                Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize)
            };
            for (i, dst) in out.chunks_mut(CHUNK).enumerate().take(n) {
                let chunk = fork.get(off(i)?..off(i + 1)?).ok_or(Error::Corrupt)?;
                decode_chunk(kind, chunk, dst)?;
            }
        }
        _ => return Err(Error::Corrupt),
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    // Small files go inline, large ones into the fork, and both decompress to the original.
    #[test]
    fn layouts_round_trip() {
        let small = b"tiny file contents ".repeat(20);
        let c = compress(&small);
        assert_eq!(
            parse_header(&c.xattr).unwrap(),
            (TYPE_LZFSE_INLINE, small.len() as u64)
        );
        assert!(c.fork.is_empty());
        assert_eq!(decompress(&c.xattr, &c.fork, u64::MAX).unwrap(), small);
        let big: Vec<u8> = (0..200_000u32)
            .map(|i| (i % 13 * (i / 1000 % 7)) as u8)
            .collect();
        let c = compress(&big);
        assert_eq!(parse_header(&c.xattr).unwrap().0, TYPE_LZFSE_FORK);
        assert_eq!(
            u32::from_le_bytes(c.fork[..4].try_into().unwrap()),
            20,
            "4 chunks need 5 offsets"
        );
        assert_eq!(decompress(&c.xattr, &c.fork, u64::MAX).unwrap(), big);
        assert_eq!(
            decompress(&c.xattr, &c.fork, 10),
            Err(Error::OutputTooSmall)
        );
        assert!(decompress(&c.xattr, &c.fork[..20], u64::MAX).is_err());
    }
}
