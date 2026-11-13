// Allocation behaviour of each entry point, measured with a counting global allocator: reused encoders and
// decoders must not allocate at all, and the one-shot functions only allocate what they return plus scratch.
// Run with `--nocapture` to see the numbers.
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering::Relaxed};
use std::sync::Mutex;

use lzfse_zazzcat as lzfse;

// Counts allocations (including reallocations) and the bytes they request; frees are not counted.
struct Counting;

static CALLS: AtomicUsize = AtomicUsize::new(0);
static BYTES: AtomicUsize = AtomicUsize::new(0);
// Tests in this file run one at a time so their counts do not mix.
static SERIAL: Mutex<()> = Mutex::new(());
// Count every thread's allocations (for code that spawns threads) instead of only the measuring thread's.
static ALL_THREADS: AtomicBool = AtomicBool::new(false);

thread_local! {
    // Whether this thread is being measured; other threads (such as the test harness) are ignored.
    static MEASURED: Cell<bool> = const { Cell::new(false) };
}

// Tell whether the current allocation should be counted.
fn counted() -> bool {
    ALL_THREADS.load(Relaxed) || MEASURED.try_with(Cell::get).unwrap_or(false)
}

// SAFETY: every method forwards to the system allocator unchanged.
unsafe impl GlobalAlloc for Counting {
    // Count, then allocate.
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        if counted() {
            CALLS.fetch_add(1, Relaxed);
            BYTES.fetch_add(l.size(), Relaxed);
        }
        // SAFETY: forwarded unchanged.
        unsafe { System.alloc(l) }
    }

    // Count, then allocate zeroed memory.
    unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
        if counted() {
            CALLS.fetch_add(1, Relaxed);
            BYTES.fetch_add(l.size(), Relaxed);
        }
        // SAFETY: forwarded unchanged.
        unsafe { System.alloc_zeroed(l) }
    }

    // Count a reallocation as one allocation of the new size.
    unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
        if counted() {
            CALLS.fetch_add(1, Relaxed);
            BYTES.fetch_add(n, Relaxed);
        }
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

// Run `f` and return the (allocations, bytes requested) it made on this thread.
fn count(f: impl FnOnce()) -> (usize, usize) {
    let (c, b) = (CALLS.load(Relaxed), BYTES.load(Relaxed));
    MEASURED.with(|m| m.set(true));
    f();
    MEASURED.with(|m| m.set(false));
    (CALLS.load(Relaxed) - c, BYTES.load(Relaxed) - b)
}

// Run `f` and return the (allocations, bytes requested) made on any thread meanwhile.
fn count_all_threads(f: impl FnOnce()) -> (usize, usize) {
    ALL_THREADS.store(true, Relaxed);
    let r = count(f);
    ALL_THREADS.store(false, Relaxed);
    r
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

// Chunked compression returns exact-size streams, and the contiguous form allocates nothing once its buffers,
// the workers' encoders and the buffer pool have warmed up.
#[test]
fn chunked_encoding() {
    let _g = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let big = text(4 << 20);
    let opts = lzfse::Options::default();
    let mut parts = Vec::new();
    let c = count_all_threads(|| parts = lzfse::encode_chunks_with(&big, 65_536, &opts));
    println!("encode_chunks_with 4 MiB in 64 KiB chunks: {c:?}");
    assert!(
        parts.iter().all(|p| p.capacity() == p.len()),
        "chunk outputs carry unused capacity"
    );
    let (mut arena, mut offsets) = (Vec::new(), Vec::new());
    for _ in 0..3 {
        arena.clear();
        lzfse::encode_chunks_into(&big, 65_536, &opts, &mut arena, &mut offsets);
    }
    arena.clear();
    let c = count_all_threads(|| {
        lzfse::encode_chunks_into(&big, 65_536, &opts, &mut arena, &mut offsets)
    });
    println!("encode_chunks_into, warmed up: {c:?}");
    assert_eq!(offsets.len(), parts.len() + 1);
    for (w, p) in offsets.windows(2).zip(&parts) {
        assert_eq!(arena[w[0]..w[1]], p[..]);
    }
    assert!(
        c.0 <= 4,
        "warmed-up chunked encoding made {} allocations",
        c.0
    );
}

// Breakdown of the cold start (run alone: `cargo test --release --test alloc cold_start -- --exact --nocapture`).
#[test]
#[ignore]
fn cold_start() {
    let _g = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let big = text(4 << 20);
    let opts = lzfse::Options::default();
    let pool = count_all_threads(|| {
        lzfse::parallelism();
    });
    let (mut arena, mut offsets) = (Vec::new(), Vec::new());
    let first = count_all_threads(|| {
        lzfse::encode_chunks_into(&big, 65_536, &opts, &mut arena, &mut offsets)
    });
    arena.clear();
    let second = count_all_threads(|| {
        lzfse::encode_chunks_into(&big, 65_536, &opts, &mut arena, &mut offsets)
    });
    arena.clear();
    let third = count_all_threads(|| {
        lzfse::encode_chunks_into(&big, 65_536, &opts, &mut arena, &mut offsets)
    });
    let owned = count_all_threads(|| drop(lzfse::encode_chunks_with(&big, 65_536, &opts)));
    println!("threads {}", lzfse::parallelism());
    println!("pool start {pool:?}\nfirst encode_chunks_into {first:?}\nsecond {second:?}\nthird {third:?}\nencode_chunks_with (owned) {owned:?}");
}
