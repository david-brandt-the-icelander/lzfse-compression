// Error type shared by the decoder and the encoder.
use core::fmt;

/// Why a stream could not be decoded or encoded.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// The input ended in the middle of a block or before the end-of-stream marker.
    Truncated,
    /// The input is not a valid stream (bad magic, header, table or payload).
    Corrupt,
    /// The output does not fit in the buffer or limit the caller provided.
    OutputTooSmall,
}

impl fmt::Display for Error {
    // Describe the error in a few words.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Error::Truncated => "truncated input",
            Error::Corrupt => "corrupt input",
            Error::OutputTooSmall => "output buffer too small",
        })
    }
}

#[cfg(feature = "std")]
impl std::error::Error for Error {}
