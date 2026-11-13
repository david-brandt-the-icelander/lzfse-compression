// Parsing arbitrary decmpfs attributes must never panic.
#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let split = data.first().map_or(0, |&b| b as usize).min(data.len());
    let (xattr, fork) = data.split_at(split);
    let _ = lzfse_zazzcat::decmpfs::decompress(xattr, fork, 1 << 22);
});
