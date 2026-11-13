// Block-parallel compression of independent chunks (each chunk becomes its own complete stream).
use std::sync::atomic::{AtomicUsize, Ordering};
use std::thread;
use std::vec::Vec;

use crate::encode::{encode_with, Options};

/// Compress `src` in independent `chunk`-byte pieces on all available cores; each result is a complete stream.
pub fn encode_chunks(src: &[u8], chunk: usize) -> Vec<Vec<u8>> {
    let threads = thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1);
    encode_chunks_with(src, chunk, threads, &Options::default())
}

/// Compress `src` in independent `chunk`-byte pieces on up to `threads` threads; each result is a complete stream.
pub fn encode_chunks_with(
    src: &[u8],
    chunk: usize,
    threads: usize,
    opts: &Options,
) -> Vec<Vec<u8>> {
    assert!(chunk > 0, "chunk size must be positive");
    let pieces: Vec<&[u8]> = src.chunks(chunk).collect();
    let threads = threads.clamp(1, pieces.len().max(1));
    if threads == 1 {
        return pieces.iter().map(|p| encode_with(p, opts)).collect();
    }
    // Workers take the next unclaimed chunk until none are left, then results are put back in order.
    let next = AtomicUsize::new(0);
    let mut results: Vec<(usize, Vec<u8>)> = thread::scope(|s| {
        let workers: Vec<_> = (0..threads)
            .map(|_| {
                s.spawn(|| {
                    let mut done = Vec::new();
                    loop {
                        let i = next.fetch_add(1, Ordering::Relaxed);
                        let Some(p) = pieces.get(i) else { return done };
                        done.push((i, encode_with(p, opts)));
                    }
                })
            })
            .collect();
        workers
            .into_iter()
            .flat_map(|w| w.join().expect("worker panicked"))
            .collect()
    });
    results.sort_unstable_by_key(|r| r.0);
    results.into_iter().map(|r| r.1).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    // Parallel chunks decode to the original data in order, whatever the thread count.
    #[test]
    fn chunks_round_trip_in_order() {
        let data: Vec<u8> = (0..300_000u32)
            .map(|i| ((i / 3) % 251) as u8 ^ (i % 7) as u8)
            .collect();
        for threads in [1, 3, 8] {
            let parts = encode_chunks_with(&data, 65_536, threads, &Options::default());
            assert_eq!(parts.len(), 5);
            let mut joined = Vec::new();
            for p in &parts {
                joined.extend(crate::decode(p, 65_536).unwrap());
            }
            assert_eq!(joined, data);
        }
        assert!(encode_chunks(&[], 10).is_empty());
    }
}
