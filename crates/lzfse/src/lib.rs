//! LZFSE and LZVN compression, written from scratch in Rust.
//!
//! The streams are byte-compatible with Apple's `libcompression` (`COMPRESSION_LZFSE`): Apple's decoder reads
//! what this crate writes, and this crate decodes what Apple's encoder writes. The output is not byte-identical
//! to Apple's.
//!
//! ```
//! let data = b"hello hello hello hello hello world".repeat(200);
//! let packed = lzfse_zazzcat::encode(&data);
//! let unpacked = lzfse_zazzcat::decode(&packed, data.len()).unwrap();
//! assert_eq!(unpacked, data);
//! ```
//!
//! The crate is `no_std` with `alloc`. The default `std` feature adds block-parallel chunk compression.
#![no_std]
// Unsafe code is denied everywhere except the copy kernels in `kernels`, each checked against a safe reference.
#![deny(unsafe_code)]

extern crate alloc;
#[cfg(feature = "std")]
extern crate std;

mod bits;
pub mod decmpfs;
mod decode;
mod encode;
mod error;
mod format;
mod fse;
mod kernels;
pub mod lzvn;
#[cfg(feature = "std")]
mod parallel;

pub use decode::{decode, decode_into, decoded_len, Decoder};
pub use encode::{encode, encode_with, Encoder, Options, LZVN_THRESHOLD};
pub use error::Error;
#[cfg(feature = "std")]
pub use parallel::{encode_chunks, encode_chunks_with};

#[cfg(test)]
mod tests;
