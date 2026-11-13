// Compatibility with Apple's libcompression and the macOS kernel (macOS only): both directions of the stream
// format, LZVN, the compression ratio, the system's own compressed files and files we compress ourselves.
#![cfg(target_os = "macos")]

use std::ffi::{c_char, c_int, c_void, CString};
use std::fs;
use std::os::macos::fs::MetadataExt;
use std::path::{Path, PathBuf};

use lzfse_zazzcat::{decmpfs, decode, encode, lzvn};

const LZFSE: c_int = 0x801;
const LZVN: c_int = 0x900;
const UF_COMPRESSED: u32 = 0x20;
const XATTR_SHOWCOMPRESSION: c_int = 0x20;

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
}

extern "C" {
    fn getxattr(
        path: *const c_char,
        name: *const c_char,
        value: *mut c_void,
        size: usize,
        position: u32,
        options: c_int,
    ) -> isize;
    fn setxattr(
        path: *const c_char,
        name: *const c_char,
        value: *const c_void,
        size: usize,
        position: u32,
        options: c_int,
    ) -> c_int;
    fn chflags(path: *const c_char, flags: u32) -> c_int;
}

// Compress with Apple's encoder.
fn apple_encode(src: &[u8], algorithm: c_int) -> Vec<u8> {
    let mut dst = vec![0u8; src.len() * 2 + 4096];
    // SAFETY: both buffers are valid for the sizes passed; a null scratch buffer is allowed.
    let n = unsafe {
        compression_encode_buffer(
            dst.as_mut_ptr(),
            dst.len(),
            src.as_ptr(),
            src.len(),
            std::ptr::null_mut(),
            algorithm,
        )
    };
    assert!(n > 0 || src.is_empty(), "apple encode failed");
    dst.truncate(n);
    dst
}

// Decompress with Apple's decoder, expecting exactly `len` bytes.
fn apple_decode(src: &[u8], len: usize, algorithm: c_int) -> Vec<u8> {
    let mut dst = vec![0u8; len + 1];
    // SAFETY: both buffers are valid for the sizes passed; a null scratch buffer is allowed.
    let n = unsafe {
        compression_decode_buffer(
            dst.as_mut_ptr(),
            dst.len(),
            src.as_ptr(),
            src.len(),
            std::ptr::null_mut(),
            algorithm,
        )
    };
    dst.truncate(n);
    dst
}

// Inputs covering text, binary, noise, runs and every size threshold.
fn corpus() -> Vec<(String, Vec<u8>)> {
    let mut x = 12345u32;
    let mut noise = |n: usize| -> Vec<u8> {
        (0..n)
            .map(|_| {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                x as u8
            })
            .collect()
    };
    let words: Vec<u8> = noise(400_000)
        .iter()
        .flat_map(|b| {
            [
                "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "a ", "lazy ", "dog.\n",
            ][*b as usize % 9]
                .bytes()
        })
        .collect();
    let mut sources = Vec::new();
    for dir in ["src", "src/encode", "tests"] {
        if let Ok(entries) = fs::read_dir(Path::new(env!("CARGO_MANIFEST_DIR")).join(dir)) {
            for e in entries.flatten() {
                if e.path().is_file() {
                    sources.extend(fs::read(e.path()).unwrap());
                }
            }
        }
    }
    let base64: Vec<u8> = noise(300_000)
        .iter()
        .map(|b| {
            b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[(*b & 63) as usize]
        })
        .collect();
    let mut out = vec![
        ("empty".into(), vec![]),
        ("tiny".into(), b"abc".to_vec()),
        ("zeros".into(), vec![0u8; 1 << 20]),
    ];
    for n in [7, 8, 100, 4095, 4096, 4097, 65_536, 1_000_000] {
        out.push((format!("words {n}"), words[..n.min(words.len())].to_vec()));
    }
    out.push(("noise".into(), noise(200_000)));
    out.push(("base64".into(), base64));
    out.push(("sources".into(), sources));
    out
}

// Every stream Apple writes, we decode; every stream we write, Apple decodes.
#[test]
fn lzfse_both_directions() {
    for (name, data) in corpus() {
        // Apple's encoder reports failure (0 bytes) for empty input, so that direction starts at 1 byte.
        if !data.is_empty() {
            let theirs = apple_encode(&data, LZFSE);
            assert_eq!(
                decode(&theirs, data.len()).as_deref(),
                Ok(&data[..]),
                "decode apple's {name}"
            );
        }
        let ours = encode(&data);
        assert_eq!(
            apple_decode(&ours, data.len(), LZFSE),
            data,
            "apple decodes our {name}"
        );
    }
}

// Raw LZVN streams interoperate both ways.
#[test]
fn lzvn_both_directions() {
    for (name, data) in corpus() {
        if data.len() < 8 || data.len() > 300_000 {
            continue;
        }
        let theirs = apple_encode(&data, LZVN);
        let mut out = vec![0u8; data.len()];
        assert_eq!(
            lzvn::decode(&theirs, &mut out),
            Ok(data.len()),
            "decode apple's lzvn {name}"
        );
        assert_eq!(out, data);
        let mut ours = Vec::new();
        lzvn::encode(&data, &mut ours);
        assert_eq!(
            apple_decode(&ours, data.len(), LZVN),
            data,
            "apple decodes our lzvn {name}"
        );
    }
}

// Our compression ratio stays within 2% of Apple's on compressible inputs (milestone M2).
#[test]
fn ratio_close_to_apple() {
    for (name, data) in corpus() {
        if data.len() < 65_536 || name == "noise" {
            continue;
        }
        let (theirs, ours) = (apple_encode(&data, LZFSE).len(), encode(&data).len());
        println!(
            "{name:>14}: apple {theirs:>9}  ours {ours:>9}  ({:+.2}%)",
            100.0 * (ours as f64 / theirs as f64 - 1.0)
        );
        assert!(
            ours as f64 <= theirs as f64 * 1.02 + 64.0,
            "{name}: ours {ours} vs apple {theirs}"
        );
    }
}

// Read one extended attribute, including the hidden compression ones.
fn xattr(path: &Path, name: &str) -> Option<Vec<u8>> {
    let p = CString::new(path.as_os_str().as_encoded_bytes()).ok()?;
    let n = CString::new(name).unwrap();
    // SAFETY: valid C strings; a null buffer asks for the size.
    let size = unsafe {
        getxattr(
            p.as_ptr(),
            n.as_ptr(),
            std::ptr::null_mut(),
            0,
            0,
            XATTR_SHOWCOMPRESSION,
        )
    };
    if size < 0 {
        return None;
    }
    let mut buf = vec![0u8; size as usize];
    // SAFETY: the buffer is valid for `size` bytes.
    let got = unsafe {
        getxattr(
            p.as_ptr(),
            n.as_ptr(),
            buf.as_mut_ptr().cast(),
            buf.len(),
            0,
            XATTR_SHOWCOMPRESSION,
        )
    };
    (got >= 0).then(|| {
        buf.truncate(got as usize);
        buf
    })
}

// Collect regular files under `dir` (not following links), up to `max`.
fn walk(dir: &Path, max: usize, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for e in entries.flatten() {
        if out.len() >= max {
            return;
        }
        let Ok(md) = fs::symlink_metadata(e.path()) else {
            continue;
        };
        if md.is_dir() {
            walk(&e.path(), max, out);
        } else if md.is_file() && md.st_flags() & UF_COMPRESSED != 0 {
            out.push(e.path());
        }
    }
}

// Files macOS itself compressed decode with our decmpfs parser to the same bytes the kernel returns.
#[test]
fn system_compressed_files() {
    let mut files = Vec::new();
    for dir in [
        "/usr/bin",
        "/usr/libexec",
        "/System/Library/CoreServices",
        "/System/Library/Frameworks",
    ] {
        walk(Path::new(dir), 4000, &mut files);
    }
    let mut by_type = std::collections::BTreeMap::new();
    let mut checked = 0;
    for path in &files {
        let Some(attr) = xattr(path, "com.apple.decmpfs") else {
            continue;
        };
        let Ok((kind, size)) = decmpfs::parse_header(&attr) else {
            continue;
        };
        *by_type.entry(kind).or_insert(0) += 1;
        if ![7, 8, 11, 12].contains(&kind) || size > 50_000_000 {
            continue;
        }
        let fork = xattr(path, "com.apple.ResourceFork").unwrap_or_default();
        let Ok(want) = fs::read(path) else { continue };
        let got = decmpfs::decompress(&attr, &fork, u64::MAX);
        assert_eq!(
            got.as_deref(),
            Ok(&want[..]),
            "{} (type {kind})",
            path.display()
        );
        checked += 1;
    }
    println!("compressed files by type: {by_type:?}; verified {checked}");
    assert!(
        checked > 0,
        "no LZFSE/LZVN-compressed system files found to check"
    );
}

// Set an extended attribute or fail the test.
fn set(path: &CString, name: &str, value: &[u8]) {
    let n = CString::new(name).unwrap();
    // SAFETY: valid C strings and a buffer valid for its length.
    let rc = unsafe {
        setxattr(
            path.as_ptr(),
            n.as_ptr(),
            value.as_ptr().cast(),
            value.len(),
            0,
            0,
        )
    };
    assert_eq!(
        rc,
        0,
        "setxattr {name}: {}",
        std::io::Error::last_os_error()
    );
}

// Files we compress (inline and resource-fork layouts) read back through the kernel unchanged.
#[test]
fn kernel_reads_our_files() {
    let dir = std::env::temp_dir().join(format!("lzfse-zazzcat-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    let samples = corpus();
    for (name, data) in samples.iter().filter(|(_, d)| !d.is_empty()) {
        let path = dir.join(name.replace(' ', "_"));
        fs::write(&path, b"").unwrap();
        let c = decmpfs::compress(data);
        let p = CString::new(path.as_os_str().as_encoded_bytes()).unwrap();
        if !c.fork.is_empty() {
            set(&p, "com.apple.ResourceFork", &c.fork);
        }
        set(&p, "com.apple.decmpfs", &c.xattr);
        // SAFETY: valid C string.
        let rc = unsafe { chflags(p.as_ptr(), UF_COMPRESSED) };
        assert_eq!(rc, 0, "chflags: {}", std::io::Error::last_os_error());
        let md = fs::symlink_metadata(&path).unwrap();
        assert_ne!(
            md.st_flags() & UF_COMPRESSED,
            0,
            "{name}: kernel refused the compressed flag"
        );
        assert_eq!(md.len(), data.len() as u64, "{name}: size");
        assert_eq!(
            &fs::read(&path).unwrap(),
            data,
            "{name}: contents through the kernel"
        );
        println!(
            "{name:>14}: {} bytes stored as {} ({} blocks on disk)",
            data.len(),
            c.stored_len(),
            md.st_blocks()
        );
    }
    fs::remove_dir_all(&dir).ok();
}
