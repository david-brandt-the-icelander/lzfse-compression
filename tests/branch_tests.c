// Tests written to reach the branches the other tests miss: error paths for damaged streams and headers, the
// decmpfs container cases, option clamping and odd encoder inputs. unit_tests.c includes this file (so both share the
// test framework's failure count and helpers), not the Makefile.

// --- Helpers ---

// A stream of every block type in one: LZVN, uncompressed, v1 and v2 LZFSE blocks, then the end marker.
static lzfse_zazzcat_buffer mixed_stream(void) {
    lzfse_zazzcat_buffer stream = {0};
    lzfse_zazzcat_buffer text = words(6000), tiny = noise(5, 1), lzvn_text = words(300), noisy = noise(3000, 9);
    lzfse_zazzcat_buffer v2 = encode_buffer(text.data, text.length), small_lzvn = encode_buffer(lzvn_text.data, 300);
    lzfse_zazzcat_buffer raw = encode_buffer(tiny.data, tiny.length), random = encode_buffer(noisy.data, 3000);
    lzfse_zazzcat_buffer *parts[] = {&small_lzvn, &raw, &random, &v2};
    for (int i = 0; i < 4; i++) bare_buffer_append(&stream, parts[i]->data, parts[i]->length - 4);
    // The v2 block again as a v1 block with the same payload.
    block_header h;
    size_t size;
    if (parse_v2_header(v2.data, v2.length, &h, &size) == LZFSE_ZAZZCAT_OK) {
        bare_write_v1_header(&h, &stream);
        bare_buffer_append(&stream, v2.data + size, h.literal_payload_size + h.token_payload_size);
    }
    uint8_t end[4];
    store_u32(end, MAGIC_END_OF_STREAM);
    bare_buffer_append(&stream, end, 4);
    lzfse_zazzcat_buffer *all[] = {&text, &tiny, &lzvn_text, &noisy, &v2, &small_lzvn, &raw, &random};
    for (int i = 0; i < 8; i++) lzfse_zazzcat_buffer_free(all[i]);
    return stream;
}

// Run every decoding entry point over a damaged stream; none may crash, and the decoder and the length reader must
// agree that a stream that decodes is one they accept.
static void decode_every_way(const uint8_t *data, size_t n, lzfse_zazzcat_buffer *out, uint8_t *room,
                             size_t room_size) {
    size_t written, length;
    bare_decode_buffer(data, n, SIZE_MAX / 2, out);
    bare_lzfse_zazzcat_decode_into(data, n, room, room_size, &written);
    bare_lzfse_zazzcat_decode_into(data, n, room, room_size / 4, &written);
    bare_lzfse_zazzcat_decode_into(data, n, room, 0, &written);
    bare_lzfse_zazzcat_decoded_length(data, n, &length);
    lzfse_zazzcat_stream_stats stats;
    clear_bytes(&stats, sizeof stats);
    bare_lzfse_zazzcat_stream_stats_add(&stats, data, n);
}

// --- Damaged streams ---

// Every truncation, and every byte replaced by a few values (all 256 near the start of each block), of a stream that
// holds all block types.
static void test_mixed_stream_damage(void) {
    lzfse_zazzcat_buffer stream = mixed_stream(), decoded = {0};
    size_t room_size = 20000;
    uint8_t *room = raw_malloc(room_size);
    size_t length = 0;
    CHECK(decode_buffer(stream.data, stream.length, SIZE_MAX / 2, &decoded) == LZFSE_ZAZZCAT_OK,
          "the mixed stream decodes");
    CHECK(lzfse_zazzcat_decoded_length(stream.data, stream.length, &length) == LZFSE_ZAZZCAT_OK &&
              length == decoded.length,
          "decoded length");
    // Block starts: every offset where a block magic sits.
    for (size_t cut = 0; cut <= stream.length; cut++) decode_every_way(stream.data, cut, &decoded, room, room_size);
    for (size_t i = 0; i < stream.length; i++) {
        uint8_t original = stream.data[i];
        uint8_t values[] = {0x00, 0xFF, 0x7F, 0x80, 0x01, (uint8_t)~original};
        for (size_t v = 0; v < sizeof values; v++) {
            stream.data[i] = values[v];
            decode_every_way(stream.data, stream.length, &decoded, room, room_size);
        }
        stream.data[i] = original;
    }
    // All 256 values of every byte in each block header.
    size_t offset = 0;
    while (offset + 8 <= stream.length) {
        uint32_t magic = load_u32(stream.data + offset);
        if (magic == MAGIC_END_OF_STREAM) break;
        size_t header_size = 4, next = 0;
        block_header h;
        if (magic == MAGIC_UNCOMPRESSED) {
            header_size = UNCOMPRESSED_HEADER_SIZE;
            next = offset + header_size + load_u32(stream.data + offset + 4);
        } else if (magic == MAGIC_LZVN) {
            header_size = LZVN_HEADER_SIZE;
            next = offset + header_size + load_u32(stream.data + offset + 8);
        } else if (magic == MAGIC_LZFSE_V1 || magic == MAGIC_LZFSE_V2) {
            lzfse_zazzcat_status status =
                magic == MAGIC_LZFSE_V1
                    ? parse_v1_header(stream.data + offset, stream.length - offset, &h, &header_size)
                    : parse_v2_header(stream.data + offset, stream.length - offset, &h, &header_size);
            REQUIRE(status == LZFSE_ZAZZCAT_OK, "header of a block in the mixed stream");
            next = offset + header_size + h.literal_payload_size + h.token_payload_size;
        } else {
            break;
        }
        size_t span = magic == MAGIC_LZFSE_V1 ? 60 : header_size;
        for (size_t i = offset; i < offset + span && i < stream.length; i++) {
            uint8_t original = stream.data[i];
            for (unsigned value = 0; value < 256; value++) {
                stream.data[i] = (uint8_t)value;
                decode_every_way(stream.data, stream.length, &decoded, room, room_size);
            }
            stream.data[i] = original;
        }
        offset = next;
    }
    raw_free(room);
    lzfse_zazzcat_buffer_free(&stream);
    lzfse_zazzcat_buffer_free(&decoded);
}

// The statistics of a stream with every block type count each type once.
static void test_stats_of_every_block_type(void) {
    lzfse_zazzcat_buffer stream = mixed_stream();
    lzfse_zazzcat_stream_stats s;
    clear_bytes(&s, sizeof s);
    CHECK(lzfse_zazzcat_stream_stats_add(&s, stream.data, stream.length) == LZFSE_ZAZZCAT_OK, "stats");
    CHECK(s.blocks[0] == 2 && s.blocks[1] >= 1 && s.blocks[2] >= 1, "blocks by type %llu %llu %llu",
          (unsigned long long)s.blocks[0], (unsigned long long)s.blocks[1], (unsigned long long)s.blocks[2]);
    CHECK(s.stored_bytes == stream.length, "stored bytes");
    // Stats on a stream that cuts off inside each block header field.
    for (size_t cut = 0; cut < stream.length; cut++) {
        clear_bytes(&s, sizeof s);
        CHECK(lzfse_zazzcat_stream_stats_add(&s, stream.data, cut) != LZFSE_ZAZZCAT_OK, "cut at %zu", cut);
    }
    lzfse_zazzcat_buffer_free(&stream);
}

// Streams with LZFSE blocks that have valid headers but corrupt token streams: bad distances, repeats at the start
// of a block, and tables that cannot be built.
static void test_crafted_lzfse_blocks(void) {
    lzfse_zazzcat_buffer data = words(30000), stream = encode_buffer(data.data, data.length), out = {0};
    block_header h;
    size_t size;
    REQUIRE(parse_v2_header(stream.data, stream.length, &h, &size) == LZFSE_ZAZZCAT_OK, "parse");
    size_t payload = h.literal_payload_size + h.token_payload_size;
    uint8_t *room = raw_malloc(data.length + 64);
    size_t written;
    // Each header field set to each of a few extreme values, re-written as a v2 header over the same payload.
    for (int field = 0; field < 20; field++) {
        for (int pick = 0; pick < 4; pick++) {
            block_header m = h;
            uint32_t values[4] = {0, 1, 63, 1000};
            uint32_t v = values[pick];
            switch (field) {
            case 0:
                m.literal_count = v;
                break;
            case 1:
                m.token_count = v;
                break;
            case 2:
                m.literal_payload_size = v;
                break;
            case 3:
                m.token_payload_size = v;
                break;
            case 4:
                m.literal_states[pick] = 1023;
                break;
            case 5:
                m.literal_length_state = (uint16_t)v;
                break;
            case 6:
                m.match_length_state = (uint16_t)v;
                break;
            case 7:
                m.distance_state = (uint16_t)v;
                break;
            case 8:
                m.literal_padding_bits = -(int32_t)(v % 8);
                break;
            case 9:
                m.token_padding_bits = -(int32_t)(v % 8);
                break;
            case 10:
                clear_bytes(m.literal_frequencies, sizeof m.literal_frequencies);
                break;
            case 11:
                clear_bytes(m.literal_length_frequencies, sizeof m.literal_length_frequencies);
                break;
            case 12:
                clear_bytes(m.match_length_frequencies, sizeof m.match_length_frequencies);
                break;
            case 13:
                clear_bytes(m.distance_frequencies, sizeof m.distance_frequencies);
                break;
            case 14:
                m.distance_frequencies[0] = (uint16_t)(m.distance_frequencies[0] + 1 + pick);
                break;
            case 15:
                m.literal_length_frequencies[pick] = (uint16_t)(m.literal_length_frequencies[pick] + 1);
                break;
            case 16:
                m.match_length_frequencies[pick] = (uint16_t)(m.match_length_frequencies[pick] + 1);
                break;
            case 17:
                m.literal_frequencies[pick] = (uint16_t)(m.literal_frequencies[pick] + 1);
                break;
            case 18:
                m.literal_count = (uint32_t)LITERALS_PER_BLOCK + v;
                break;
            default:
                m.token_count = (uint32_t)TOKENS_PER_BLOCK + v;
                break;
            }
            lzfse_zazzcat_buffer bad = {0};
            bare_write_v2_header(&m, &bad);
            bare_buffer_append(&bad, stream.data + size, payload);
            uint8_t end[4];
            store_u32(end, MAGIC_END_OF_STREAM);
            bare_buffer_append(&bad, end, 4);
            decode_every_way(bad.data, bad.length, &out, room, data.length + 64);
            bare_lzfse_zazzcat_decode_into(bad.data, bad.length, room, data.length + 64, &written);
            lzfse_zazzcat_buffer_free(&bad);
        }
    }
    raw_free(room);
    lzfse_zazzcat_buffer_free(&data);
    lzfse_zazzcat_buffer_free(&stream);
    lzfse_zazzcat_buffer_free(&out);
}

// block_header_validate rejects each out-of-range field on its own.
static void test_validate_rejects_each_field(void) {
    block_header good = sample_header();
    CHECK(block_header_validate(&good) == LZFSE_ZAZZCAT_OK, "sample is valid");
    block_header h = good;
    h.literal_count = LITERALS_PER_BLOCK + 1;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "literal count");
    h = good;
    h.token_count = TOKENS_PER_BLOCK + 1;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "token count");
    h = good;
    h.literal_length_state = LITERAL_LENGTH_STATES;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "literal length state");
    h = good;
    h.match_length_state = MATCH_LENGTH_STATES;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "match length state");
    h = good;
    h.distance_state = DISTANCE_STATES;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "distance state");
    h = good;
    h.literal_padding_bits = -8;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "literal padding below range");
    h = good;
    h.literal_padding_bits = 1;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "literal padding above range");
    h = good;
    h.token_padding_bits = -8;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "token padding below range");
    h = good;
    h.token_padding_bits = 1;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "token padding above range");
    h = good;
    h.match_length_frequencies[0] = 65;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "match length frequencies");
    h = good;
    h.distance_frequencies[0] = 256;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "distance frequencies");
    h = good;
    h.literal_frequencies[0] = 1024;
    CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "literal frequencies");
    for (int i = 0; i < 4; i++) {
        h = good;
        h.literal_states[i] = LITERAL_STATES;
        CHECK(block_header_validate(&h) == LZFSE_ZAZZCAT_CORRUPT, "literal state %d", i);
    }
}

// A v2 header with no frequency tables decodes with every frequency zero; a header whose tables end early or run past
// the header is corrupt.
static void test_v2_header_table_edges(void) {
    block_header h = sample_header(), back;
    lzfse_zazzcat_buffer v2 = {0};
    bare_write_v2_header(&h, &v2);
    uint8_t fixed[V2_FIXED_HEADER_SIZE];
    copy_bytes(fixed, v2.data, sizeof fixed);
    store_u32(fixed + 24, V2_FIXED_HEADER_SIZE);
    size_t size = 0;
    CHECK(parse_v2_header(fixed, sizeof fixed, &back, &size) == LZFSE_ZAZZCAT_OK && size == V2_FIXED_HEADER_SIZE,
          "no tables");
    CHECK(back.literal_frequencies[3] == 0 && back.distance_frequencies[63] == 0, "frequencies are zero");
    // Pad the tables with extra bytes, then cut them short: both are corrupt or truncated, never accepted.
    for (size_t extra = 1; extra < 14; extra++) {
        lzfse_zazzcat_buffer longer = copy_of(v2.data, v2.length);
        for (size_t i = 0; i < extra; i++) bare_buffer_append(&longer, "\xFF", 1);
        store_u32(longer.data + 24, (uint32_t)longer.length);
        CHECK(parse_v2_header(longer.data, longer.length, &back, &size) == LZFSE_ZAZZCAT_CORRUPT, "extra bytes %zu",
              extra);
        lzfse_zazzcat_buffer_free(&longer);
    }
    for (size_t keep = V2_FIXED_HEADER_SIZE + 1; keep < v2.length; keep++) {
        lzfse_zazzcat_buffer shorter = copy_of(v2.data, keep);
        store_u32(shorter.data + 24, (uint32_t)keep);
        CHECK(parse_v2_header(shorter.data, shorter.length, &back, &size) == LZFSE_ZAZZCAT_CORRUPT, "tables cut to %zu",
              keep);
        lzfse_zazzcat_buffer_free(&shorter);
    }
    lzfse_zazzcat_buffer_free(&v2);
}

// --- LZVN ---

// Every opcode byte followed by few or many bytes, decoded into small and large outputs: each kind of opcode, each
// truncation and each output-limit check.
static void test_lzvn_opcode_sweep(void) {
    uint8_t source[40], destination[600];
    size_t written;
    test_random r = {5};
    for (unsigned op = 0; op < 256; op++) {
        for (size_t following = 0; following < 12; following++) {
            for (int fill = 0; fill < 3; fill++) {
                source[0] = (uint8_t)op;
                for (size_t i = 1; i < sizeof source; i++) {
                    source[i] = fill == 0 ? 0 : fill == 1 ? 0x01 : (uint8_t)test_random_next(&r);
                }
                size_t n = 1 + following;
                bare_lzfse_zazzcat_lzvn_decode(source, n, destination, sizeof destination, &written);
                bare_lzfse_zazzcat_lzvn_decode(source, n, destination, 3, &written);
                bare_lzfse_zazzcat_lzvn_decode(source, n, destination, 0, &written);
                // Followed by the end of stream opcode, so a few valid prefixes reach the final checks.
                uint8_t tail[48];
                copy_bytes(tail, source, n);
                clear_bytes(tail + n, 8);
                tail[n] = 0x06;
                bare_lzfse_zazzcat_lzvn_decode(tail, n + 8, destination, sizeof destination, &written);
                bare_lzfse_zazzcat_lzvn_decode(tail, n + 8, destination, 7, &written);
            }
        }
    }
    // A match whose opcode comes first has no distance to use.
    uint8_t no_history[] = {0xF1, 0, 0, 0, 0, 0, 0, 0, 0x06, 0, 0, 0, 0, 0, 0, 0};
    CHECK(lzfse_zazzcat_lzvn_decode(no_history, sizeof no_history, destination, sizeof destination, &written) !=
              LZFSE_ZAZZCAT_OK,
          "match with no history");
    // NOP opcodes are skipped.
    uint8_t nops[] = {0x0E, 0x0E, 0x06, 0, 0, 0, 0, 0, 0, 0};
    CHECK(lzfse_zazzcat_lzvn_decode(nops, sizeof nops, destination, sizeof destination, &written) == LZFSE_ZAZZCAT_OK &&
              written == 0,
          "nops");
}

// A payload that is valid but longer than its block says, or that decodes to a different length, is corrupt.
static void test_lzvn_block_size_mismatches(void) {
    lzfse_zazzcat_buffer data = words(200), payload = {0}, block = {0}, out = {0};
    REQUIRE(lzfse_zazzcat_lzvn_encode(data.data, data.length, &payload) == LZFSE_ZAZZCAT_OK, "encode");
    for (int variant = 0; variant < 3; variant++) {
        block.length = 0;
        uint8_t header[12];
        uint32_t decoded_size = variant == 0 ? (uint32_t)data.length + 1 : (uint32_t)data.length;
        uint32_t payload_size = (uint32_t)payload.length;
        bare_buffer_append(&payload, "\0\0\0\0", variant == 1 ? 4 : 0);
        if (variant == 1) payload_size += 4;
        store_u32_x3(header, MAGIC_LZVN, decoded_size, payload_size);
        bare_buffer_append(&block, header, 12);
        bare_buffer_append(&block, payload.data, payload.length);
        uint8_t end[4];
        store_u32(end, MAGIC_END_OF_STREAM);
        bare_buffer_append(&block, end, 4);
        CHECK(decode_buffer(block.data, block.length, SIZE_MAX / 2, &out) == LZFSE_ZAZZCAT_CORRUPT, "variant %d",
              variant);
        uint8_t room[400];
        size_t written;
        bare_lzfse_zazzcat_decode_into(block.data, block.length, room, sizeof room, &written);
    }
    lzfse_zazzcat_buffer_free(&data);
    lzfse_zazzcat_buffer_free(&payload);
    lzfse_zazzcat_buffer_free(&block);
    lzfse_zazzcat_buffer_free(&out);
}

// --- Encoder ---

static void test_options_are_clamped(void) {
    lzfse_zazzcat_buffer data = words(300000), out = {0}, decoded = {0};
    lzfse_zazzcat_options low = lzfse_zazzcat_default_options(), high = low;
    low.hash_bits = 0;
    high.hash_bits = 99;
    const lzfse_zazzcat_options *all[] = {&low, &high};
    for (int i = 0; i < 2; i++) {
        out.length = 0;
        CHECK(lzfse_zazzcat_encode(data.data, data.length, all[i], &out) == LZFSE_ZAZZCAT_OK, "encode %d", i);
        CHECK(decode_buffer(out.data, out.length, data.length, &decoded) == LZFSE_ZAZZCAT_OK &&
                  same_bytes(decoded.data, decoded.length, data.data, data.length),
              "round trip %d", i);
    }
    lzfse_zazzcat_buffer_free(&data);
    lzfse_zazzcat_buffer_free(&out);
    lzfse_zazzcat_buffer_free(&decoded);
}

// One encoder reused across many inputs, of sizes on both sides of every table-size boundary and with every kind of
// content, so the large finder sees stale entries, matches at all distances and repeats.
static void test_encoder_reuse_across_sizes(void) {
    lzfse_zazzcat_encoder *encoder = lzfse_zazzcat_encoder_create(NULL);
    REQUIRE(encoder != NULL, "create");
    lzfse_zazzcat_buffer out = {0}, decoded = {0};
    size_t sizes[] = {8, 100, 1000, 70000, 5000, 400000, 70000, 300000, 20000, 1 << 21, 70001, 900000};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        lzfse_zazzcat_buffer data = k % 3 == 0 ? noise(sizes[k], (uint32_t)k + 3) : words(sizes[k]);
        // Make the data repeat at a few distances and in runs.
        for (size_t i = 5000; i < data.length; i += 977) data.data[i] = data.data[i - 4999];
        for (size_t i = 0; k % 4 == 1 && i + 300 < data.length; i += 20000) {
            for (size_t j = 0; j < 300; j++) data.data[i + j] = 'z';
        }
        out.length = 0;
        CHECK(lzfse_zazzcat_encoder_encode(encoder, data.data, data.length, &out) == LZFSE_ZAZZCAT_OK, "encode %zu", k);
        CHECK(decode_buffer(out.data, out.length, data.length, &decoded) == LZFSE_ZAZZCAT_OK &&
                  same_bytes(decoded.data, decoded.length, data.data, data.length),
              "round trip %zu", k);
        lzfse_zazzcat_buffer_free(&data);
    }
    lzfse_zazzcat_encoder_destroy(encoder);
    lzfse_zazzcat_buffer_free(&out);
    lzfse_zazzcat_buffer_free(&decoded);
}

// Inputs that make LZFSE blocks no smaller than the data (so the encoder stores them raw) at several sizes.
static void test_incompressible_blocks_are_stored(void) {
    size_t sizes[] = {70000, 200000, 600000};
    for (size_t k = 0; k < 3; k++) {
        lzfse_zazzcat_buffer data = noise(sizes[k], (uint32_t)k + 20);
        // A few matches amid the noise, so the block has tokens but still cannot beat raw storage.
        for (size_t i = 100000 % sizes[k]; i + 40 < data.length; i += 30001) {
            for (size_t j = 0; j < 40; j++) data.data[i + j] = data.data[j + 7];
        }
        lzfse_zazzcat_buffer stream = round_trip(data.data, data.length);
        lzfse_zazzcat_buffer_free(&stream);
        lzfse_zazzcat_buffer_free(&data);
    }
}

// Chunked encoding of edge-case inputs.
static void test_chunk_edge_cases(void) {
    lzfse_zazzcat_buffer out = {0};
    lzfse_zazzcat_offsets offsets = {0};
    CHECK(lzfse_zazzcat_encode_chunks("x", 1, 0, NULL, &out, &offsets) == LZFSE_ZAZZCAT_INVALID_ARGUMENT,
          "zero chunk size");
    CHECK(lzfse_zazzcat_encode_chunks(NULL, 0, 100, NULL, &out, &offsets) == LZFSE_ZAZZCAT_OK && offsets.length == 1,
          "empty input has one offset");
    lzfse_zazzcat_buffer data = words(5000);
    lzfse_zazzcat_options options = lzfse_zazzcat_default_options();
    CHECK(lzfse_zazzcat_encode_chunks(data.data, data.length, 100, &options, &out, &offsets) == LZFSE_ZAZZCAT_OK &&
              offsets.length == 51,
          "explicit options, 50 chunks");
    CHECK(lzfse_zazzcat_encode_chunks(data.data, data.length, 4096, NULL, &out, &offsets) == LZFSE_ZAZZCAT_OK &&
              offsets.length == 3,
          "two chunks");
    lzfse_zazzcat_offsets_free(&offsets);
    lzfse_zazzcat_buffer_free(&out);
    lzfse_zazzcat_buffer_free(&data);
}

// --- FSE ---

// Normalizing counts of many shapes (few and many symbols, very uneven, at every table size) always sums exactly.
static void test_normalize_shapes(void) {
    test_random r = {31337};
    uint32_t state_counts[] = {64, 256, 1024};
    for (int round = 0; round < 3000; round++) {
        uint32_t state_count = state_counts[round % 3];
        size_t symbols = 1 + test_random_next(&r) % 256;
        if (symbols > state_count) symbols = state_count;
        uint32_t counts[256];
        uint32_t total = 0;
        for (size_t i = 0; i < symbols; i++) {
            uint32_t shape = test_random_next(&r) % 6;
            counts[i] = shape == 0   ? 0
                        : shape == 1 ? 1
                        : shape == 2 ? 2 + test_random_next(&r) % 5
                        : shape == 3 ? test_random_next(&r) % 1000
                        : shape == 4 ? 100000
                                     : test_random_next(&r) % 5000000;
            total += counts[i];
        }
        if (total == 0) {
            counts[0] = 1;
            total = 1;
        }
        uint16_t frequencies[256];
        fse_normalize(counts, symbols, state_count, frequencies);
        uint32_t sum = 0;
        for (size_t i = 0; i < symbols; i++) sum += frequencies[i];
        CHECK(sum == state_count, "round %d sums to %u, wanted %u", round, sum, state_count);
        if (sum != state_count) return;
    }
}

// --- decmpfs ---

// A decmpfs attribute: the 16-byte header (type, size) and `payload`.
static lzfse_zazzcat_buffer make_xattr(uint32_t type, uint64_t size, const void *payload, size_t payload_length) {
    lzfse_zazzcat_buffer x = {0};
    uint8_t header[16] = {'f', 'p', 'm', 'c'};
    store_u32(header + 4, type);
    store_u64(header + 8, size);
    bare_buffer_append(&x, header, 16);
    if (payload_length > 0) bare_buffer_append(&x, payload, payload_length);
    return x;
}

// A resource fork holding `chunks` (each with its length), preceded by the offset table.
static lzfse_zazzcat_buffer make_fork(const lzfse_zazzcat_buffer *chunks, size_t count) {
    lzfse_zazzcat_buffer fork = {0};
    uint32_t at = (uint32_t)(4 * (count + 1));
    for (size_t i = 0; i <= count; i++) {
        uint8_t entry[4];
        store_u32(entry, at);
        bare_buffer_append(&fork, entry, 4);
        if (i < count) at += (uint32_t)chunks[i].length;
    }
    for (size_t i = 0; i < count; i++) bare_buffer_append(&fork, chunks[i].data, chunks[i].length);
    return fork;
}

static lzfse_zazzcat_status decompress_parts(const lzfse_zazzcat_buffer *xattr, const lzfse_zazzcat_buffer *fork,
                                             uint64_t limit, lzfse_zazzcat_buffer *out) {
    out->length = 0;
    return lzfse_zazzcat_decmpfs_decompress(xattr->data, xattr->length, fork != NULL ? fork->data : NULL,
                                            fork != NULL ? fork->length : 0, limit, out);
}

static void test_decmpfs_headers_and_inline_chunks(void) {
    uint32_t type;
    uint64_t size;
    CHECK(lzfse_zazzcat_decmpfs_parse_header("fpmc", 4, &type, &size) == LZFSE_ZAZZCAT_CORRUPT, "short header");
    uint8_t wrong[16] = {'x', 'p', 'm', 'c'};
    CHECK(lzfse_zazzcat_decmpfs_parse_header(wrong, sizeof wrong, &type, &size) == LZFSE_ZAZZCAT_CORRUPT,
          "wrong magic");
    lzfse_zazzcat_buffer text = words(100), out = {0}, lzfse = encode_buffer(text.data, text.length), lzvn = {0};
    REQUIRE(lzfse_zazzcat_lzvn_encode(text.data, text.length, &lzvn) == LZFSE_ZAZZCAT_OK, "lzvn encode");
    // Raw chunks: a marker byte then the data. LZFSE takes any marker but 'b'; LZVN takes 0x06 only.
    lzfse_zazzcat_buffer raw_lzfse = {0}, raw_lzvn = {0}, x;
    bare_buffer_append(&raw_lzfse, "\x07", 1);
    bare_buffer_append(&raw_lzfse, text.data, text.length);
    bare_buffer_append(&raw_lzvn, "\x06", 1);
    bare_buffer_append(&raw_lzvn, text.data, text.length);
    struct {
        uint32_t type;
        uint64_t size;
        const lzfse_zazzcat_buffer *payload;
        lzfse_zazzcat_status want;
        const char *name;
    } cases[] = {
        {LZFSE_ZAZZCAT_DECMPFS_LZFSE_INLINE, 100, &lzfse, LZFSE_ZAZZCAT_OK, "lzfse stream"},
        {LZFSE_ZAZZCAT_DECMPFS_LZFSE_INLINE, 101, &lzfse, LZFSE_ZAZZCAT_CORRUPT, "lzfse stream of the wrong size"},
        {LZFSE_ZAZZCAT_DECMPFS_LZFSE_INLINE, 100, &raw_lzfse, LZFSE_ZAZZCAT_OK, "lzfse raw"},
        {LZFSE_ZAZZCAT_DECMPFS_LZFSE_INLINE, 101, &raw_lzfse, LZFSE_ZAZZCAT_CORRUPT, "lzfse raw of the wrong size"},
        {LZFSE_ZAZZCAT_DECMPFS_LZVN_INLINE, 100, &lzvn, LZFSE_ZAZZCAT_OK, "lzvn stream"},
        {LZFSE_ZAZZCAT_DECMPFS_LZVN_INLINE, 101, &lzvn, LZFSE_ZAZZCAT_CORRUPT, "lzvn stream of the wrong size"},
        {LZFSE_ZAZZCAT_DECMPFS_LZVN_INLINE, 100, &raw_lzvn, LZFSE_ZAZZCAT_OK, "lzvn raw"},
        {LZFSE_ZAZZCAT_DECMPFS_LZVN_INLINE, 90, &raw_lzvn, LZFSE_ZAZZCAT_CORRUPT, "lzvn raw of the wrong size"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        x = make_xattr(cases[i].type, cases[i].size, cases[i].payload->data, cases[i].payload->length);
        lzfse_zazzcat_status status = decompress_parts(&x, NULL, UINT64_MAX, &out);
        CHECK(status == cases[i].want, "%s: %s", cases[i].name, lzfse_zazzcat_status_string(status));
        if (cases[i].want == LZFSE_ZAZZCAT_OK)
            CHECK(same_bytes(out.data, out.length, text.data, text.length), "%s data", cases[i].name);
        lzfse_zazzcat_buffer_free(&x);
    }
    // Empty chunks: corrupt for LZFSE, not a valid stream for LZVN.
    x = make_xattr(LZFSE_ZAZZCAT_DECMPFS_LZFSE_INLINE, 0, NULL, 0);
    CHECK(decompress_parts(&x, NULL, UINT64_MAX, &out) == LZFSE_ZAZZCAT_CORRUPT, "empty lzfse chunk");
    lzfse_zazzcat_buffer_free(&x);
    x = make_xattr(LZFSE_ZAZZCAT_DECMPFS_LZVN_INLINE, 0, NULL, 0);
    CHECK(decompress_parts(&x, NULL, UINT64_MAX, &out) != LZFSE_ZAZZCAT_OK, "empty lzvn chunk");
    lzfse_zazzcat_buffer_free(&x);
    // A stream that does not decode, in each type, and an unknown type.
    x = make_xattr(LZFSE_ZAZZCAT_DECMPFS_LZFSE_INLINE, 100, "bvx2garbage-garbage-garbage-garbage-garbage", 43);
    CHECK(decompress_parts(&x, NULL, UINT64_MAX, &out) != LZFSE_ZAZZCAT_OK, "bad lzfse stream");
    lzfse_zazzcat_buffer_free(&x);
    x = make_xattr(99, 100, lzfse.data, lzfse.length);
    CHECK(decompress_parts(&x, NULL, UINT64_MAX, &out) == LZFSE_ZAZZCAT_CORRUPT, "unknown type");
    lzfse_zazzcat_buffer_free(&x);
    x = make_xattr(LZFSE_ZAZZCAT_DECMPFS_LZFSE_INLINE, 100, lzfse.data, lzfse.length);
    CHECK(decompress_parts(&x, NULL, 99, &out) == LZFSE_ZAZZCAT_OUTPUT_TOO_SMALL, "over the limit");
    lzfse_zazzcat_buffer_free(&x);
    lzfse_zazzcat_buffer *all[] = {&text, &out, &lzfse, &lzvn, &raw_lzfse, &raw_lzvn};
    for (int i = 0; i < 6; i++) lzfse_zazzcat_buffer_free(all[i]);
}

static void test_decmpfs_fork_tables(void) {
    // Two chunks of 65536 bytes and a tail, as LZVN and as LZFSE, then each damaged table entry.
    lzfse_zazzcat_buffer text = words(2 * LZFSE_ZAZZCAT_DECMPFS_CHUNK + 1000), out = {0};
    uint32_t types[2] = {LZFSE_ZAZZCAT_DECMPFS_LZVN_FORK, LZFSE_ZAZZCAT_DECMPFS_LZFSE_FORK};
    for (int t = 0; t < 2; t++) {
        lzfse_zazzcat_buffer chunks[3] = {{0}};
        for (int c = 0; c < 3; c++) {
            const uint8_t *at = text.data + (size_t)c * LZFSE_ZAZZCAT_DECMPFS_CHUNK;
            size_t n = c < 2 ? LZFSE_ZAZZCAT_DECMPFS_CHUNK : 1000;
            if (t == 0) {
                REQUIRE(lzfse_zazzcat_lzvn_encode(at, n, &chunks[c]) == LZFSE_ZAZZCAT_OK, "lzvn");
            } else {
                REQUIRE(lzfse_zazzcat_encode(at, n, NULL, &chunks[c]) == LZFSE_ZAZZCAT_OK, "lzfse");
            }
        }
        lzfse_zazzcat_buffer fork = make_fork(chunks, 3);
        lzfse_zazzcat_buffer x = make_xattr(types[t], text.length, NULL, 0);
        CHECK(decompress_parts(&x, &fork, UINT64_MAX, &out) == LZFSE_ZAZZCAT_OK &&
                  same_bytes(out.data, out.length, text.data, text.length),
              "type %u round trip", types[t]);
        // A table cut short at every length: the start or the end of some chunk is missing.
        for (size_t keep = 0; keep < 16; keep++) {
            lzfse_zazzcat_buffer cut = copy_of(fork.data, keep);
            CHECK(decompress_parts(&x, &cut, UINT64_MAX, &out) != LZFSE_ZAZZCAT_OK, "table cut to %zu", keep);
            lzfse_zazzcat_buffer_free(&cut);
        }
        // An entry that points backwards, and one past the end of the fork.
        for (size_t entry = 0; entry < 4; entry++) {
            lzfse_zazzcat_buffer bad = copy_of(fork.data, fork.length);
            store_u32(bad.data + 4 * entry, 0xFFFFFFF0u);
            CHECK(decompress_parts(&x, &bad, UINT64_MAX, &out) != LZFSE_ZAZZCAT_OK, "entry %zu past the end", entry);
            store_u32(bad.data + 4 * entry, 0);
            CHECK(decompress_parts(&x, &bad, UINT64_MAX, &out) != LZFSE_ZAZZCAT_OK || entry == 0, "entry %zu backwards",
                  entry);
            lzfse_zazzcat_buffer_free(&bad);
        }
        for (int c = 0; c < 3; c++) lzfse_zazzcat_buffer_free(&chunks[c]);
        lzfse_zazzcat_buffer_free(&fork);
        lzfse_zazzcat_buffer_free(&x);
    }
    lzfse_zazzcat_buffer_free(&text);
    lzfse_zazzcat_buffer_free(&out);
}

// A status the library does not define has a description too.
static void test_unknown_status_string(void) {
    const char *text = lzfse_zazzcat_status_string((lzfse_zazzcat_status)12345);
    CHECK(text != NULL && text[0] != 0, "unknown status has a description");
}

// A decmpfs attribute too short for its header is corrupt when decompressing, not only when parsing.
static void test_decmpfs_decompress_needs_a_header(void) {
    lzfse_zazzcat_buffer out = {0};
    CHECK(lzfse_zazzcat_decmpfs_decompress("fpmc", 4, NULL, 0, UINT64_MAX, &out) == LZFSE_ZAZZCAT_CORRUPT,
          "short xattr");
    lzfse_zazzcat_buffer_free(&out);
}

// Large inputs that start with zeros: the match finder's empty buckets look like matches of zero bytes at once.
static void test_large_input_starting_with_zeros(void) {
    lzfse_zazzcat_buffer data = noise(300000, 77);
    for (size_t i = 0; i < 500; i++) data.data[i] = 0;
    for (size_t i = 100000; i < 100600; i++) data.data[i] = 0;
    lzfse_zazzcat_buffer stream = round_trip(data.data, data.length);
    lzfse_zazzcat_buffer_free(&stream);
    lzfse_zazzcat_buffer_free(&data);
}

// The encoder numbers positions from a base that grows with every input and starts over before 32 bits run out.
// Encoding a little over 1 GiB of zeros (which takes well under a second) five times takes it past 4 GiB.
static void test_position_base_starts_over(void) {
    size_t n = ((size_t)1 << 30) + 4096;
    uint8_t *zeros = raw_calloc(n, 1);
    REQUIRE(zeros != NULL, "allocate %zu bytes", n);
    lzfse_zazzcat_encoder *encoder = lzfse_zazzcat_encoder_create(NULL);
    REQUIRE(encoder != NULL, "create");
    lzfse_zazzcat_buffer out = {0};
    for (int round = 0; round < 5; round++) {
        out.length = 0;
        size_t length = 0;
        CHECK(lzfse_zazzcat_encoder_encode(encoder, zeros, n, &out) == LZFSE_ZAZZCAT_OK, "encode %d", round);
        CHECK(lzfse_zazzcat_decoded_length(out.data, out.length, &length) == LZFSE_ZAZZCAT_OK && length == n,
              "round %d decodes to %zu bytes", round, length);
    }
    lzfse_zazzcat_encoder_destroy(encoder);
    lzfse_zazzcat_buffer_free(&out);
    raw_free(zeros);
}

// A bit writer holding a full 64 bits flushes all eight bytes; a reader rejects padding outside -7..0 that would leave
// it with too few bits, and takes more without reading past its buffer.
static void test_bit_stream_limits(void) {
    lzfse_zazzcat_buffer out = {0};
    bare_buffer_reserve(&out, 64);
    bit_writer writer;
    bit_writer_start(&writer, &out);
    bit_writer_push(&writer, 32, 0xDEADBEEFu);
    bit_writer_push(&writer, 32, 0x01234567u);
    bit_writer_flush(&writer);
    CHECK(writer.bit_count == 0 && writer.next == out.data + 8, "all eight bytes flushed");
    CHECK(load_u64(out.data) == 0x01234567DEADBEEFull, "bytes written");
    lzfse_zazzcat_buffer_free(&out);
    uint8_t zeros[8] = {0};
    bit_reader r = {0};
    CHECK(bit_reader_init(&r, zeros, 8, -9) == LZFSE_ZAZZCAT_CORRUPT, "padding of 9 bits leaves too few");
    CHECK(bit_reader_init(&r, zeros, 8, 1) == LZFSE_ZAZZCAT_OK, "positive padding is not an error here");
}

// read_u32_at reports an offset past the end as truncation, not only one that leaves fewer than four bytes.
static void test_read_u32_past_the_end(void) {
    uint8_t bytes[4] = {1, 2, 3, 4};
    uint32_t value = 0;
    CHECK(read_u32_at(bytes, 4, 0, &value) == LZFSE_ZAZZCAT_OK && value == 0x04030201u, "in range");
    CHECK(read_u32_at(bytes, 4, 1, &value) == LZFSE_ZAZZCAT_TRUNCATED, "straddling the end");
    CHECK(read_u32_at(bytes, 4, 8, &value) == LZFSE_ZAZZCAT_TRUNCATED, "past the end");
}
