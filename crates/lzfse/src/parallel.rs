// Block-parallel compression of independent chunks on a persistent thread pool (each chunk becomes its own
// complete stream). Each worker thread keeps its own encoder, and chunk outputs go through a pool of reused
// buffers, so repeated calls allocate nothing once warmed up.
use core::cell::RefCell;
use rayon::prelude::*;
use std::sync::Mutex;
use std::vec::Vec;

use crate::encode::{Encoder, Options};

// Buffers kept in the pool at most; more than this are freed.
const POOL_MAX: usize = 1024;

std::thread_local! {
    // This worker's encoder, created on first use and kept until the thread exits.
    static ENCODER: RefCell<Option<Encoder>> = const { RefCell::new(None) };
    // The calling thread's per-call slot vector, reused across calls.
    static SLOTS: RefCell<Vec<Vec<u8>>> = const { RefCell::new(Vec::new()) };
}

// Chunk output buffers handed back after their contents were copied out, kept for their capacity.
static BUFFERS: Mutex<Vec<Vec<u8>>> = Mutex::new(Vec::new());

// Run `f` with this thread's encoder set to `opts` (a fresh one if the cached one is already in use).
fn with_encoder<R>(opts: &Options, f: impl FnOnce(&mut Encoder) -> R) -> R {
    ENCODER.with(|cell| match cell.try_borrow_mut() {
        Ok(mut slot) => {
            let enc = slot.get_or_insert_with(|| Encoder::new(*opts));
            enc.set_options(*opts);
            f(enc)
        }
        Err(_) => f(&mut Encoder::new(*opts)),
    })
}

// Run `f` with this thread's reusable slot vector (empty on entry, emptied again afterwards).
fn with_slots<R>(f: impl FnOnce(&mut Vec<Vec<u8>>) -> R) -> R {
    SLOTS.with(|cell| match cell.try_borrow_mut() {
        Ok(mut slots) => {
            let r = f(&mut slots);
            slots.clear();
            r
        }
        Err(_) => f(&mut Vec::new()),
    })
}

// Take an empty buffer from the pool (or a new one).
fn take_buffer() -> Vec<u8> {
    BUFFERS
        .lock()
        .ok()
        .and_then(|mut p| p.pop())
        .unwrap_or_default()
}

// Return a buffer to the pool, keeping its capacity.
fn give_buffer(mut b: Vec<u8>) {
    b.clear();
    if let Ok(mut p) = BUFFERS.lock() {
        if p.len() < POOL_MAX {
            p.push(b);
        }
    }
}

/// Number of threads the chunked encoders use (the size of the global thread pool).
pub fn parallelism() -> usize {
    rayon::current_num_threads()
}

/// Compress `src` in independent `chunk`-byte pieces on all cores; each result is a complete stream.
pub fn encode_chunks(src: &[u8], chunk: usize) -> Vec<Vec<u8>> {
    encode_chunks_with(src, chunk, &Options::default())
}

/// Like [`encode_chunks`], with encoder options. Each returned stream is exactly sized.
pub fn encode_chunks_with(src: &[u8], chunk: usize, opts: &Options) -> Vec<Vec<u8>> {
    let (mut out, mut offsets) = (take_buffer(), Vec::new());
    encode_chunks_into(src, chunk, opts, &mut out, &mut offsets);
    let parts = offsets
        .windows(2)
        .map(|w| out[w[0]..w[1]].to_vec())
        .collect();
    give_buffer(out);
    parts
}

/// Compress `src` in independent `chunk`-byte pieces on all cores, appending the streams back to back to `out`.
/// `offsets` is cleared and filled with `n + 1` positions in `out`: stream `i` is `out[offsets[i]..offsets[i + 1]]`.
/// This is the layout of an APFS resource fork, and once `out` and `offsets` have grown, repeated calls allocate
/// nothing.
pub fn encode_chunks_into(
    src: &[u8],
    chunk: usize,
    opts: &Options,
    out: &mut Vec<u8>,
    offsets: &mut Vec<usize>,
) {
    assert!(chunk > 0, "chunk size must be positive");
    offsets.clear();
    offsets.push(out.len());
    with_slots(|slots| {
        slots.resize_with(src.len().div_ceil(chunk), Vec::new);
        // Each worker encodes into a pooled buffer with its own cached encoder.
        slots
            .par_iter_mut()
            .zip(src.par_chunks(chunk))
            .for_each(|(slot, piece)| {
                let mut buf = take_buffer();
                with_encoder(opts, |e| e.encode(piece, &mut buf));
                *slot = buf;
            });
        out.reserve(slots.iter().map(Vec::len).sum());
        offsets.reserve(slots.len());
        for buf in slots.drain(..) {
            out.extend_from_slice(&buf);
            offsets.push(out.len());
            give_buffer(buf);
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    // Parallel chunks decode to the original data in order, in both the owned and the contiguous forms.
    #[test]
    fn chunks_round_trip_in_order() {
        let data: Vec<u8> = (0..300_000u32)
            .map(|i| ((i / 3) % 251) as u8 ^ (i % 7) as u8)
            .collect();
        let parts = encode_chunks(&data, 65_536);
        assert_eq!(parts.len(), 5);
        let mut joined = Vec::new();
        for p in &parts {
            joined.extend(crate::decode(p, 65_536).unwrap());
        }
        assert_eq!(joined, data);
        let (mut out, mut offsets) = (b"prefix".to_vec(), Vec::new());
        encode_chunks_into(&data, 65_536, &Options::default(), &mut out, &mut offsets);
        assert_eq!(offsets.len(), 6);
        assert_eq!(offsets[0], 6);
        for (i, w) in offsets.windows(2).enumerate() {
            assert_eq!(out[w[0]..w[1]], parts[i][..]);
        }
        assert!(encode_chunks(&[], 10).is_empty());
    }
}
