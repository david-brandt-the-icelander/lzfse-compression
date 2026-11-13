// Every input must survive encode then decode unchanged.
#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let packed = lzfse_zazzcat::encode(data);
    assert_eq!(lzfse_zazzcat::decode(&packed, data.len()).unwrap(), data);
    let mut lzvn = Vec::new();
    lzfse_zazzcat::lzvn::encode(data, &mut lzvn);
    let mut out = vec![0u8; data.len()];
    assert_eq!(lzfse_zazzcat::lzvn::decode(&lzvn, &mut out), Ok(data.len()));
    assert_eq!(out, data);
});
