// Decoding arbitrary bytes must never panic, and both decode entry points must agree.
#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let whole = lzfse_zazzcat::decode(data, 1 << 24);
    let mut buf = vec![0u8; 1 << 16];
    let into = lzfse_zazzcat::decode_into(data, &mut buf);
    if let (Ok(w), Ok(n)) = (&whole, &into) {
        assert_eq!(&buf[..*n], &w[..]);
    }
});
