// Allocation behaviour of each entry point, measured with a counting global allocator: reused encoders and
// decoders must not allocate at all, and the one-shot functions only allocate what they return plus scratch.
// Run with `--nocapture` to see the numbers.
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering::Relaxed};
use std::sync::Mutex;

use lzfse_zazzcat as lzfse;

// Counts allocations (including reallocations) and the bytes they request; frees are not counted.
struct Counting;

static CALLS: AtomicUsize = AtomicUsize::new(0);
static BYTES: AtomicUsize = AtomicUsize::new(0);
// Tests in this file run one at a time so their counts do not mix.
static SERIAL: Mutex<()> = Mutex::new(());

// SAFETY: every method forwards to the system allocator unchanged.
unsafe impl GlobalAlloc for Counting {
    // Count, then allocate.
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        CALLS.fetch_add(1, Relaxed);
        BYTES.fetch_add(l.size(), Relaxed);
        // SAFETY: forwarded unchanged.
        unsafe { System.alloc(l) }
    }

    // Count, then allocate zeroed memory.
    unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
        CALLS.fetch_add(1, Relaxed);
        BYTES.fetch_add(l.size(), Relaxed);
        // SAFETY: forwarded unchanged.
        unsafe { System.alloc_zeroed(l) }
    }

    // Count a reallocation as one allocation of the new size.
    unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
        CALLS.fetch_add(1, Relaxed);
        BYTES.fetch_add(n, Relaxed);
        // SAFETY: forwarded unchanged.
        unsafe { System.realloc(p, l, n) }
    }

    // Free without counting.
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        // SAFETY: forwarded unchanged.
        unsafe { System.dealloc(p, l) }
    }
}

#[global_allocator]
static A: Counting = Counting;

// Run `f` and return (allocations, bytes requested) it made.
fn count(f: impl FnOnce()) -> (usize, usize) {
    let (c, b) = (CALLS.load(Relaxed), BYTES.load(Relaxed));
    f();
    (CALLS.load(Relaxed) - c, BYTES.load(Relaxed) - b)
}

// Text-like input of `n` bytes.
fn text(n: usize) -> Vec<u8> {
    b"the quick brown fox jumps over a lazy dog and of "
        .iter()
        .cycle()
        .enumerate()
        .map(|(i, &b)| b ^ ((i / 997) as u8 & 1))
        .take(n)
        .collect()
}

// Reused encoders and decoders allocate nothing once warmed up, at every input size and block type.
#[test]
fn reuse_is_allocation_free() {
    let _g = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut enc = lzfse::Encoder::new(lzfse::Options::default());
    let mut dec = lzfse::Decoder::new();
    let mut out = Vec::new();
    let sizes = [5usize, 1000, 4095, 65_536, 1 << 20];
    let inputs: Vec<Vec<u8>> = sizes.iter().map(|&n| text(n)).collect();
    let mut buf = vec![0u8; 1 << 20];
    // Warm up on every size first, so buffers reach their final capacity.
    for data in &inputs {
        out.clear();
        enc.encode(data, &mut out);
        dec.decode_into(&out, &mut buf).unwrap();
    }
    for data in &inputs {
        out.clear();
        let e = count(|| enc.encode(data, &mut out));
        let d = count(|| {
            dec.decode_into(&out, &mut buf).unwrap();
        });
        println!("{:>8} bytes: Encoder {e:?}  Decoder {d:?}", data.len());
        assert_eq!(
            e,
            (0, 0),
            "reused Encoder allocated for {} bytes",
            data.len()
        );
        assert_eq!(
            d,
            (0, 0),
            "reused Decoder allocated for {} bytes",
            data.len()
        );
    }
}

// The one-shot functions allocate only their result plus bounded scratch.
#[test]
fn one_shot_allocations_are_bounded() {
    let _g = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    for n in [1000usize, 65_536, 1 << 20] {
        let data = text(n);
        let packed = lzfse::encode(&data);
        let mut buf = vec![0u8; n];
        let e = count(|| drop(lzfse::encode(&data)));
        let d = count(|| drop(lzfse::decode(&packed, n).unwrap()));
        let di = count(|| {
            lzfse::decode_into(&packed, &mut buf).unwrap();
        });
        println!("{n:>8} bytes: encode {e:?}  decode {d:?}  decode_into {di:?}");
        // Encoder: output growth, hash table (at most 512 KiB) and one block's token buffers (about 190 KB).
        assert!(e.0 <= 10, "encode() made {} allocations", e.0);
        assert!(e.1 <= 2 * n + 800_000, "encode() requested {} bytes", e.1);
        // Decoder: the output plus the literal buffer (at most 40 KB).
        assert!(d.0 <= 2 && d.1 <= n + 41_000, "decode() {d:?}");
        assert!(di.0 <= 1 && di.1 <= 41_000, "decode_into() {di:?}");
    }
    // Small inputs never pay for the LZFSE buffers.
    let small = count(|| drop(lzfse::encode(&text(1000))));
    assert!(
        small.1 < 8_000,
        "a 1000-byte encode requested {} bytes",
        small.1
    );
}

// Chunked compression makes one exact-size allocation per chunk, plus a fixed amount per worker thread (the
// thread itself, its encoder and its scratch buffers).
#[test]
fn chunk_outputs_are_exact() {
    let _g = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let big = text(4 << 20);
    let mut parts = Vec::new();
    let threads = 4;
    let c = count(|| {
        parts = lzfse::encode_chunks_with(&big, 65_536, threads, &lzfse::Options::default())
    });
    println!("encode_chunks 4 MiB in 64 KiB chunks: {c:?}");
    assert!(
        parts.iter().all(|p| p.capacity() == p.len()),
        "chunk outputs carry unused capacity"
    );
    assert!(
        c.0 <= parts.len() + threads * 24 + 16,
        "{} allocations for {} chunks",
        c.0,
        parts.len()
    );
}
