// Command-line front end: encode, decode and inspect LZFSE streams.
use std::process::ExitCode;
use std::{env, fs};

use lzfse_zazzcat as lzfse;

const USAGE: &str = "usage: lzfse-zazzcat encode|decode <input> <output>
       lzfse-zazzcat inspect <input>";

// Read a file or report the error and exit.
fn read(path: &str) -> Result<Vec<u8>, String> {
    fs::read(path).map_err(|e| format!("{path}: {e}"))
}

// Write a file or report the error.
fn write(path: &str, data: &[u8]) -> Result<(), String> {
    fs::write(path, data).map_err(|e| format!("{path}: {e}"))
}

// Print each block's type and sizes.
fn inspect(src: &[u8]) -> Result<(), String> {
    let mut at = 0usize;
    loop {
        let magic = src.get(at..at + 4).ok_or("truncated")?;
        let word = |o: usize| {
            src.get(at + o..at + o + 4)
                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize)
                .ok_or("truncated")
        };
        let name = String::from_utf8_lossy(magic).into_owned();
        let (raw, size) = match magic {
            b"bvx$" => {
                println!("{at:>10}  bvx$  end of stream");
                return Ok(());
            }
            b"bvx-" => (word(4)?, 8 + word(4)?),
            b"bvxn" => (word(4)?, 12 + word(8)?),
            b"bvx2" => {
                let header = word(24)?;
                let w = |i: usize| {
                    src.get(at + 8 + 8 * i..at + 16 + 8 * i)
                        .map(|b| u64::from_le_bytes(b.try_into().unwrap()))
                        .ok_or("truncated")
                };
                let (w0, w1) = (w(0)?, w(1)?);
                let payload = ((w0 >> 20) & 0xFFFFF) as usize + ((w1 >> 40) & 0xFFFFF) as usize;
                println!(
                    "{at:>10}  bvx2  raw {:>8}  literals {:>6}  matches {:>6}  header {header}",
                    word(4)?,
                    w0 & 0xFFFFF,
                    (w0 >> 40) & 0xFFFFF
                );
                at += header + payload;
                continue;
            }
            b"bvx1" => (word(4)?, 772 + word(8)?),
            _ => return Err(format!("unknown block magic {name:?} at {at}")),
        };
        println!("{at:>10}  {name}  raw {raw:>8}  stored {size:>8}");
        at += size;
    }
}

// Run one command.
fn run(args: &[String]) -> Result<(), String> {
    match args {
        [cmd, input, output] if cmd == "encode" => write(output, &lzfse::encode(&read(input)?)),
        [cmd, input, output] if cmd == "decode" => {
            let data = lzfse::decode(&read(input)?, usize::MAX).map_err(|e| e.to_string())?;
            write(output, &data)
        }
        [cmd, input] if cmd == "inspect" => inspect(&read(input)?),
        _ => Err(USAGE.into()),
    }
}

// Parse arguments and report errors on stderr.
fn main() -> ExitCode {
    let args: Vec<String> = env::args().skip(1).collect();
    match run(&args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::FAILURE
        }
    }
}
