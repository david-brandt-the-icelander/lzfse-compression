// Whole-stream tests: round trips, block-type policy, the v1 header path and hostile input.
use alloc::vec;
use alloc::vec::Vec;

use crate::format::*;
use crate::{decode, decode_into, decoded_len, encode, encode_with, Error, Options};

// Deterministic pseudo-random bytes (xorshift).
fn noise(n: usize, seed: u32) -> Vec<u8> {
    let mut x = seed | 1;
    (0..n)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            x as u8
        })
        .collect()
}

// Text-like data with words drawn from a small vocabulary.
fn words(n: usize) -> Vec<u8> {
    let vocab = [
        "lorem ",
        "ipsum ",
        "dolor ",
        "sit ",
        "amet, ",
        "consectetur ",
        "adipiscing ",
        "elit.\n",
        "sed ",
        "do ",
    ];
    let r = noise(n, 99);
    let mut out = Vec::with_capacity(n + 16);
    let mut i = 0;
    while out.len() < n {
        out.extend_from_slice(vocab[r[i % r.len()] as usize % vocab.len()].as_bytes());
        i += 1;
    }
    out.truncate(n);
    out
}

// Encode, check the block types used, decode both ways and return the stream.
fn round_trip(data: &[u8]) -> Vec<u8> {
    let enc = encode(data);
    assert_eq!(decoded_len(&enc).unwrap(), data.len());
    assert_eq!(decode(&enc, data.len()).unwrap(), data);
    let mut buf = vec![0u8; data.len() + 10];
    assert_eq!(decode_into(&enc, &mut buf).unwrap(), data.len());
    assert_eq!(&buf[..data.len()], data);
    enc
}

// Return the magic of the first block.
fn first_magic(stream: &[u8]) -> u32 {
    read_u32(stream, 0).unwrap()
}

// Sizes around every threshold and block limit round-trip, with the expected block type.
#[test]
fn sizes_and_block_types() {
    assert_eq!(round_trip(b""), MAGIC_EOS.to_le_bytes());
    assert_eq!(first_magic(&round_trip(b"1234567")), MAGIC_RAW);
    assert_eq!(first_magic(&round_trip(&words(4095))), MAGIC_LZVN);
    assert_eq!(first_magic(&round_trip(&noise(4095, 1))), MAGIC_RAW);
    assert_eq!(first_magic(&round_trip(&words(4096))), MAGIC_V2);
    assert_eq!(first_magic(&round_trip(&noise(100_000, 2))), MAGIC_RAW);
    for n in [8, 9, 100, 4097, 65_536, 65_537, 200_003] {
        round_trip(&words(n));
        round_trip(&noise(n, n as u32));
    }
}

// Highly compressible data compresses well and long matches split correctly.
#[test]
fn compressible_inputs() {
    let zeros = vec![0u8; 1_000_000];
    assert!(round_trip(&zeros).len() < 2_000);
    let text = words(500_000);
    let enc = round_trip(&text);
    assert!(
        enc.len() < text.len() / 3,
        "{} of {}",
        enc.len(),
        text.len()
    );
    let rep = b"hello compressible text line\n".repeat(40_000);
    assert!(round_trip(&rep).len() < 5_000);
}

// Many short matches fill blocks to their token and literal limits and still round-trip.
#[test]
fn block_limits() {
    let mut data = Vec::new();
    let r = noise(400_000, 7);
    for i in 0..60_000 {
        data.extend_from_slice(&r[i * 5..i * 5 + 3]);
        data.extend_from_slice(b"abcd");
    }
    round_trip(&data);
    let mut lit_heavy = noise(300_000, 9);
    for i in (0..lit_heavy.len() - 100).step_by(1000) {
        lit_heavy.copy_within(i..i + 8, i + 500);
    }
    round_trip(&lit_heavy);
}

// Matches far back (up to the maximum distance) and across block boundaries round-trip.
#[test]
fn far_matches() {
    let mut data = noise(300_000, 3);
    let len = data.len();
    data.extend_from_within(len - MAX_D as usize..len - MAX_D as usize + 5000);
    data.extend_from_within(10..20_000);
    let enc = round_trip(&data);
    assert!(enc.len() < data.len());
}

// Every encoder option combination produces decodable streams.
#[test]
fn options_round_trip() {
    let data = words(150_000);
    for hash_bits in [10, 14, 16] {
        for lazy_below in [0, 8, 64] {
            for skip_shift in [0, 2, 6] {
                let o = Options {
                    hash_bits,
                    good_match: 24,
                    lazy_below,
                    skip_shift,
                };
                assert_eq!(decode(&encode_with(&data, &o), data.len()).unwrap(), data);
            }
        }
    }
}

// The v1 header path decodes the same payload as v2.
#[test]
fn v1_headers_decode() {
    let data = words(50_000);
    let enc = encode(&data);
    assert_eq!(first_magic(&enc), MAGIC_V2);
    let (_, size) = parse_v2(&enc).unwrap();
    let blocks_end = enc.len() - 4;
    let (h, _) = parse_v2(&enc).unwrap();
    assert_eq!(
        size + (h.n_lit_payload + h.n_lmd_payload) as usize,
        blocks_end,
        "one block"
    );
    let mut v1 = crate::encode::to_v1(&enc[..blocks_end]);
    v1.extend_from_slice(&MAGIC_EOS.to_le_bytes());
    assert_eq!(decode(&v1, data.len()).unwrap(), data);
}

// Concatenated block sequences decode as one stream, including references into earlier blocks.
#[test]
fn multi_block_streams() {
    let a = encode(&words(10_000));
    let b = encode(&noise(10, 4));
    let mut joined = a[..a.len() - 4].to_vec();
    joined.extend_from_slice(&b);
    let mut want = words(10_000);
    want.extend(noise(10, 4));
    assert_eq!(decode(&joined, want.len()).unwrap(), want);
}

// Truncations, bit flips and random bytes are errors, never panics or over-long output.
#[test]
fn hostile_input() {
    let data = words(20_000);
    let enc = encode(&data);
    for cut in (0..enc.len()).step_by(37) {
        assert!(decode(&enc[..cut], usize::MAX).is_err());
    }
    let mut buf = vec![0u8; data.len()];
    for i in (0..enc.len()).step_by(7) {
        for bit in [0x01u8, 0x10, 0x80] {
            let mut bad = enc.clone();
            bad[i] ^= bit;
            let _ = decode(&bad, 1 << 20);
            let _ = decode_into(&bad, &mut buf);
        }
    }
    for seed in 0..200 {
        let mut junk = noise(64 + seed as usize * 3, seed);
        junk[..4].copy_from_slice(
            &[MAGIC_V2, MAGIC_LZVN, MAGIC_V1, MAGIC_RAW][seed as usize % 4].to_le_bytes(),
        );
        let _ = decode(&junk, 1 << 20);
        let _ = decode_into(&junk, &mut buf);
    }
    assert_eq!(decode(&enc, data.len() - 1), Err(Error::OutputTooSmall));
    assert_eq!(
        decode_into(&enc, &mut [0u8; 100]),
        Err(Error::OutputTooSmall)
    );
    assert_eq!(decode(b"bvx?", 10), Err(Error::Corrupt));
    assert_eq!(decode(b"bv", 10), Err(Error::Truncated));
}

proptest::proptest! {
    #![proptest_config(proptest::prelude::ProptestConfig::with_cases(64))]

    // Arbitrary bytes, and arbitrary repetitive structure, round-trip.
    #[test]
    fn prop_round_trip(data in proptest::collection::vec(proptest::prelude::any::<u8>(), 0..20_000), reps in 0usize..4) {
        let mut d = data.clone();
        for _ in 0..reps {
            d.extend_from_slice(&data);
        }
        proptest::prop_assert_eq!(decode(&encode(&d), d.len()).unwrap(), d);
    }

    // Decoding arbitrary bytes never panics.
    #[test]
    fn prop_decode_never_panics(data in proptest::collection::vec(proptest::prelude::any::<u8>(), 0..4000), magic in 0usize..5) {
        let mut d = data;
        if d.len() >= 4 && magic < 4 {
            d[..4].copy_from_slice(&[MAGIC_V2, MAGIC_LZVN, MAGIC_V1, MAGIC_RAW][magic].to_le_bytes());
        }
        let _ = decode(&d, 1 << 20);
    }
}
