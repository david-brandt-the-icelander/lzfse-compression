// Decoding arbitrary LZVN bytes must never panic or write past the output.
#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let mut out = vec![0u8; 1 << 16];
    let _ = lzfse_zazzcat::lzvn::decode(data, &mut out);
});
