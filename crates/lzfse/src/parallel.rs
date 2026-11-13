// Block-parallel compression of independent chunks on a persistent thread pool (each chunk becomes its own
// complete stream). Consecutive chunks are grouped into a few tasks per thread; each task encodes its chunks with
// its worker's cached encoder into one buffer reserved up front. Those buffers, the per-chunk length table and the
// encoders persist between calls, so a warmed-up call allocates nothing.
use core::cell::RefCell;
use rayon::prelude::*;
use std::vec::Vec;

use crate::encode::{Encoder, Options};

// Tasks per pool thread: enough for load balancing, few enough that each task's buffer is large.
const TASKS_PER_THREAD: usize = 4;

std::thread_local! {
    // This worker's encoder, created on first use and kept until the thread exits.
    static ENCODER: RefCell<Option<Encoder>> = const { RefCell::new(None) };
    // The calling thread's task buffers and per-chunk end offsets, kept (with their capacity) between calls.
    static SCRATCH: RefCell<(Vec<Vec<u8>>, Vec<usize>)> = const { RefCell::new((Vec::new(), Vec::new())) };
}

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

// Run `f` with this thread's task buffers and chunk-end table (fresh ones if they are already in use).
fn with_scratch<R>(f: impl FnOnce(&mut Vec<Vec<u8>>, &mut Vec<usize>) -> R) -> R {
    SCRATCH.with(|cell| match cell.try_borrow_mut() {
        Ok(mut s) => {
            let (bufs, ends) = &mut *s;
            f(bufs, ends)
        }
        Err(_) => f(&mut Vec::new(), &mut Vec::new()),
    })
}

// Room to reserve for encoding `len` bytes as `chunks` streams: the encoder may briefly hold a block entropy-coded
// at up to 10 bits per literal (plus its header) before deciding to store it raw.
fn task_bound(len: usize, chunks: usize) -> usize {
    len + len / 4 + chunks * 4096
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
    let (mut out, mut offsets) = (Vec::new(), Vec::new());
    encode_chunks_into(src, chunk, opts, &mut out, &mut offsets);
    offsets
        .windows(2)
        .map(|w| out[w[0]..w[1]].to_vec())
        .collect()
}

/// Compress `src` in independent `chunk`-byte pieces on all cores, appending the streams back to back to `out`.
/// `offsets` is cleared and filled with `n + 1` positions in `out`: stream `i` is `out[offsets[i]..offsets[i + 1]]`.
/// This is the layout of an APFS resource fork, and once `out`, `offsets` and the pool's workers have warmed up,
/// repeated calls allocate nothing.
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
    let n = src.len().div_ceil(chunk);
    if n == 0 {
        return;
    }
    let per = n.div_ceil(rayon::current_num_threads() * TASKS_PER_THREAD);
    let tasks = n.div_ceil(per);
    with_scratch(|bufs, ends| {
        if bufs.len() < tasks {
            bufs.resize_with(tasks, Vec::new);
        }
        ends.clear();
        ends.resize(n, 0);
        let bufs = &mut bufs[..tasks];
        // Task t encodes chunks [t * per, (t + 1) * per) into bufs[t], recording where each chunk's stream ends.
        bufs.par_iter_mut()
            .zip(ends.par_chunks_mut(per))
            .zip(src.par_chunks(chunk.saturating_mul(per)))
            .for_each(|((buf, task_ends), part)| {
                buf.clear();
                buf.reserve(task_bound(part.len(), task_ends.len()));
                with_encoder(opts, |e| {
                    for (end, piece) in task_ends.iter_mut().zip(part.chunks(chunk)) {
                        e.encode(piece, buf);
                        *end = buf.len();
                    }
                });
            });
        out.reserve(bufs.iter().map(Vec::len).sum());
        offsets.reserve(n);
        for (buf, task_ends) in bufs.iter().zip(ends.chunks(per)) {
            let at = out.len();
            out.extend_from_slice(buf);
            offsets.extend(task_ends.iter().map(|&e| at + e));
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
