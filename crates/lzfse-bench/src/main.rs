// Benchmark harness: compares lzfse-zazzcat with Apple's libcompression in-process and prints a Markdown report.
//
// Usage: lzfse-bench [--runs N] [--chunk BYTES]... [--synthetic] [FILE]...
// Every encoded output is decoded back (and on macOS also by Apple's decoder) before its timing counts.
use std::time::Instant;
use std::{env, fs, thread};

use lzfse_zazzcat as lzfse;

// One input to benchmark.
struct Input {
    name: String,
    data: Vec<u8>,
}

// One measured contender on one input and chunk shape.
struct Row {
    contender: &'static str,
    input: String,
    shape: String,
    size: usize,
    packed: usize,
    enc_mbs: f64,
    dec_mbs: f64,
}

#[cfg(target_os = "macos")]
mod apple {
    use std::ffi::{c_int, c_void};

    pub const LZFSE: c_int = 0x801;

    #[link(name = "compression")]
    extern "C" {
        fn compression_encode_buffer(
            dst: *mut u8,
            dst_size: usize,
            src: *const u8,
            src_size: usize,
            scratch: *mut c_void,
            algorithm: c_int,
        ) -> usize;
        fn compression_decode_buffer(
            dst: *mut u8,
            dst_size: usize,
            src: *const u8,
            src_size: usize,
            scratch: *mut c_void,
            algorithm: c_int,
        ) -> usize;
        fn compression_encode_scratch_buffer_size(algorithm: c_int) -> usize;
        fn compression_decode_scratch_buffer_size(algorithm: c_int) -> usize;
    }

    // Reusable scratch buffers so allocation stays out of the timing, as for our encoder's tables.
    pub struct Codec {
        enc: Vec<u8>,
        dec: Vec<u8>,
    }

    impl Codec {
        // Allocate scratch space for LZFSE.
        pub fn new() -> Self {
            // SAFETY: pure size queries.
            let (e, d) = unsafe {
                (
                    compression_encode_scratch_buffer_size(LZFSE),
                    compression_decode_scratch_buffer_size(LZFSE),
                )
            };
            Codec {
                enc: vec![0; e + 1],
                dec: vec![0; d + 1],
            }
        }

        // Compress with Apple's LZFSE.
        pub fn encode(&mut self, src: &[u8]) -> Vec<u8> {
            let mut dst = vec![0u8; src.len() + src.len() / 2 + 1024];
            // SAFETY: buffers are valid for the sizes passed.
            let n = unsafe {
                compression_encode_buffer(
                    dst.as_mut_ptr(),
                    dst.len(),
                    src.as_ptr(),
                    src.len(),
                    self.enc.as_mut_ptr().cast(),
                    LZFSE,
                )
            };
            dst.truncate(n);
            dst
        }

        // Decompress with Apple's LZFSE into `dst`, returning the byte count.
        pub fn decode(&mut self, src: &[u8], dst: &mut [u8]) -> usize {
            // SAFETY: buffers are valid for the sizes passed.
            unsafe {
                compression_decode_buffer(
                    dst.as_mut_ptr(),
                    dst.len(),
                    src.as_ptr(),
                    src.len(),
                    self.dec.as_mut_ptr().cast(),
                    LZFSE,
                )
            }
        }
    }
}

// Deterministic generators for the built-in corpus (no downloads needed).
fn synthetic() -> Vec<Input> {
    let mut x = 0x2545_F491u32;
    let mut rnd = move || {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        x
    };
    let vocab = [
        "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "a ", "lazy ", "dog", ".\n", ", ",
        "and ", "of ",
    ];
    let text: Vec<u8> = (0..4_000_000)
        .flat_map(|_| vocab[rnd() as usize % vocab.len()].bytes())
        .take(8 << 20)
        .collect();
    let b64 = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut jsonl = Vec::new();
    while jsonl.len() < 8 << 20 {
        let n = rnd() % 4;
        jsonl.extend_from_slice(
            format!(
                "{{\"type\":\"message\",\"id\":{},\"role\":\"assistant\",\"text\":\"",
                rnd()
            )
            .as_bytes(),
        );
        if n == 0 {
            jsonl.extend_from_slice(b"\",\"image\":\"data:image/png;base64,");
            jsonl.extend((0..40_000).map(|_| b64[(rnd() & 63) as usize]));
        } else {
            jsonl.extend((0..300).map(|_| vocab[rnd() as usize % vocab.len()].as_bytes()[0]));
        }
        jsonl.extend_from_slice(b"\"}\n");
    }
    let noise: Vec<u8> = (0..4 << 20).map(|_| rnd() as u8).collect();
    let mut binary = Vec::new();
    while binary.len() < 8 << 20 {
        let op = rnd();
        binary.extend_from_slice(&[0x48, 0x89, (op & 0xFF) as u8, 0xE8]);
        binary.extend_from_slice(&(op >> 8 & 0xFFFF).to_le_bytes());
        binary.extend_from_slice(&[0, 0]);
    }
    vec![
        Input {
            name: "text".into(),
            data: text,
        },
        Input {
            name: "jsonl+base64".into(),
            data: jsonl,
        },
        Input {
            name: "binary".into(),
            data: binary,
        },
        Input {
            name: "noise".into(),
            data: noise,
        },
        Input {
            name: "zeros".into(),
            data: vec![0; 8 << 20],
        },
    ]
}

// Run `f` repeatedly and return the median seconds per run.
fn time(runs: usize, mut f: impl FnMut()) -> f64 {
    f();
    let mut samples: Vec<f64> = (0..runs)
        .map(|_| {
            let t = Instant::now();
            f();
            t.elapsed().as_secs_f64()
        })
        .collect();
    samples.sort_by(|a, b| a.total_cmp(b));
    samples[samples.len() / 2]
}

// Megabytes (10^6 bytes) per second.
fn mbs(bytes: usize, secs: f64) -> f64 {
    bytes as f64 / secs / 1e6
}

// Encoded chunks plus the timing of producing them, checked to decode back to the input.
fn measure_ours(
    input: &Input,
    chunk: usize,
    runs: usize,
    threads: usize,
    contender: &'static str,
    shape: &str,
) -> Row {
    let pieces: Vec<&[u8]> = input.data.chunks(chunk).collect();
    let opts = lzfse::Options::default();
    let encode_all = || -> Vec<Vec<u8>> {
        if threads > 1 {
            lzfse::encode_chunks_with(&input.data, chunk, threads, &opts)
        } else {
            {
                let mut enc = lzfse::Encoder::new(opts);
                pieces
                    .iter()
                    .map(|p| {
                        let mut out = Vec::with_capacity(p.len() / 2 + 64);
                        enc.encode(p, &mut out);
                        out
                    })
                    .collect()
            }
        }
    };
    let packed = encode_all();
    let mut out = vec![0u8; chunk];
    for (p, src) in packed.iter().zip(&pieces) {
        let n = lzfse::decode_into(p, &mut out).expect("our stream decodes");
        assert_eq!(&out[..n], *src, "round trip of {}", input.name);
        #[cfg(target_os = "macos")]
        {
            let mut a = apple::Codec::new();
            let mut buf = vec![0u8; src.len() + 1];
            let n = a.decode(p, &mut buf);
            assert_eq!(&buf[..n], *src, "Apple decodes our {}", input.name);
        }
    }
    let enc = time(runs, || {
        std::hint::black_box(encode_all());
    });
    let mut decoder = lzfse::Decoder::new();
    let dec = if threads > 1 {
        // Each worker decodes every `threads`-th chunk with its own decoder and output buffer.
        time(runs, || {
            thread::scope(|sc| {
                for t in 0..threads {
                    let packed = &packed;
                    sc.spawn(move || {
                        let (mut d, mut buf) = (lzfse::Decoder::new(), vec![0u8; chunk]);
                        for p in packed.iter().skip(t).step_by(threads) {
                            std::hint::black_box(d.decode_into(p, &mut buf).unwrap());
                        }
                    });
                }
            })
        })
    } else {
        time(runs, || {
            for p in &packed {
                std::hint::black_box(decoder.decode_into(p, &mut out).unwrap());
            }
        })
    };
    Row {
        contender,
        input: input.name.clone(),
        shape: shape.into(),
        size: input.data.len(),
        packed: packed.iter().map(Vec::len).sum(),
        enc_mbs: mbs(input.data.len(), enc),
        dec_mbs: mbs(input.data.len(), dec),
    }
}

#[cfg(target_os = "macos")]
// Time Apple's LZFSE on the same chunks, checking its output with our decoder.
fn measure_apple(input: &Input, chunk: usize, runs: usize, shape: &str) -> Row {
    let pieces: Vec<&[u8]> = input.data.chunks(chunk).collect();
    let mut codec = apple::Codec::new();
    let packed: Vec<Vec<u8>> = pieces.iter().map(|p| codec.encode(p)).collect();
    let mut out = vec![0u8; chunk];
    for (p, src) in packed.iter().zip(&pieces) {
        let n = lzfse::decode_into(p, &mut out).expect("we decode Apple's stream");
        assert_eq!(&out[..n], *src);
    }
    let enc = time(runs, || {
        for p in &pieces {
            std::hint::black_box(codec.encode(p));
        }
    });
    let dec = time(runs, || {
        for p in &packed {
            std::hint::black_box(codec.decode(p, &mut out));
        }
    });
    Row {
        contender: "apple libcompression",
        input: input.name.clone(),
        shape: shape.into(),
        size: input.data.len(),
        packed: packed.iter().map(Vec::len).sum(),
        enc_mbs: mbs(input.data.len(), enc),
        dec_mbs: mbs(input.data.len(), dec),
    }
}

// Describe a chunk size for the report.
fn shape(chunk: usize, len: usize) -> String {
    match chunk {
        c if c >= len => "whole".into(),
        c if c % (1 << 20) == 0 => format!("{} MiB chunks", c >> 20),
        c => format!("{} KiB chunks", c >> 10),
    }
}

// Print the machine description and the results as Markdown.
fn report(rows: &[Row], threads: usize) {
    println!("## Environment\n");
    println!("- OS: {} {}", env::consts::OS, env::consts::ARCH);
    #[cfg(target_os = "macos")]
    for (label, cmd, args) in [
        ("Chip", "sysctl", &["-n", "machdep.cpu.brand_string"][..]),
        ("macOS", "sw_vers", &["-productVersion"][..]),
    ] {
        if let Ok(o) = std::process::Command::new(cmd).args(args).output() {
            println!("- {label}: {}", String::from_utf8_lossy(&o.stdout).trim());
        }
    }
    println!("- Threads for the parallel rows: {threads}");
    println!("- Throughput is MB/s (10^6 bytes) of uncompressed data; median of the timed runs.\n");
    println!("| Input | Shape | Contender | Ratio | Encode MB/s | Decode MB/s |");
    println!("|---|---|---|---:|---:|---:|");
    for r in rows {
        println!(
            "| {} | {} | {} | {:.3} | {:.0} | {:.0} |",
            r.input,
            r.shape,
            r.contender,
            r.packed as f64 / r.size as f64,
            r.enc_mbs,
            r.dec_mbs
        );
    }
}

// Parse arguments, load inputs and run every contender on every shape.
fn main() {
    let mut args = env::args().skip(1);
    let (mut runs, mut chunks, mut inputs, mut use_synth) = (7usize, Vec::new(), Vec::new(), false);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--runs" => runs = args.next().and_then(|v| v.parse().ok()).unwrap_or(runs),
            "--chunk" => chunks.extend(args.next().and_then(|v| v.parse::<usize>().ok())),
            "--synthetic" => use_synth = true,
            "-h" | "--help" => {
                println!(
                    "usage: lzfse-bench [--runs N] [--chunk BYTES]... [--synthetic] [FILE]..."
                );
                return;
            }
            path => inputs.push(Input {
                name: path.rsplit('/').next().unwrap_or(path).into(),
                data: fs::read(path).unwrap_or_else(|e| panic!("{path}: {e}")),
            }),
        }
    }
    if use_synth || inputs.is_empty() {
        inputs.extend(synthetic());
    }
    if chunks.is_empty() {
        chunks = vec![65_536, 1 << 20, usize::MAX];
    }
    let threads = thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1);
    let mut rows = Vec::new();
    for input in &inputs {
        for &chunk in &chunks {
            let chunk = chunk.min(input.data.len().max(1));
            let s = shape(chunk, input.data.len());
            #[cfg(target_os = "macos")]
            rows.push(measure_apple(input, chunk, runs, &s));
            rows.push(measure_ours(input, chunk, runs, 1, "lzfse-zazzcat", &s));
            if threads > 1 && chunk < input.data.len() {
                rows.push(measure_ours(
                    input,
                    chunk,
                    runs,
                    threads,
                    "lzfse-zazzcat parallel",
                    &s,
                ));
            }
            eprintln!("done: {} {s}", input.name);
        }
    }
    report(&rows, threads);
}
