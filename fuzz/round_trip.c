// Fuzz target: every input must survive LZFSE and LZVN encoding and decoding unchanged.
#include <stdlib.h>
#include <string.h>

#include "lzfse_zazzcat.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    lzfse_zazzcat_buffer packed = {0}, unpacked = {0}, lzvn = {0};
    if (lzfse_zazzcat_encode(data, size, NULL, &packed) != LZFSE_ZAZZCAT_OK) abort();
    if (lzfse_zazzcat_decode(packed.data, packed.length, size, &unpacked) != LZFSE_ZAZZCAT_OK) abort();
    if (unpacked.length != size || (size > 0 && memcmp(unpacked.data, data, size) != 0)) abort();
    if (lzfse_zazzcat_lzvn_encode(data, size, &lzvn) != LZFSE_ZAZZCAT_OK) abort();
    uint8_t *out = malloc(size + 1);
    size_t written = 0;
    if (lzfse_zazzcat_lzvn_decode(lzvn.data, lzvn.length, out, size, &written) != LZFSE_ZAZZCAT_OK) abort();
    if (written != size || (size > 0 && memcmp(out, data, size) != 0)) abort();
    free(out);
    lzfse_zazzcat_buffer_free(&packed);
    lzfse_zazzcat_buffer_free(&unpacked);
    lzfse_zazzcat_buffer_free(&lzvn);
    return 0;
}
