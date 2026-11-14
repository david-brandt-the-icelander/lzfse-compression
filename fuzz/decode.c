// Fuzz target: decoding arbitrary bytes must never crash or touch memory out of bounds (AddressSanitizer checks),
// and the two decoding entry points must agree.
#include <stdlib.h>
#include <string.h>

#include "lzfse_zazzcat.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    lzfse_zazzcat_buffer whole = {0};
    lzfse_zazzcat_status whole_status = lzfse_zazzcat_decode(data, size, 1 << 24, &whole);
    size_t capacity = 1 << 16, written = 0;
    uint8_t *into = malloc(capacity);
    lzfse_zazzcat_status into_status = lzfse_zazzcat_decode_into(data, size, into, capacity, &written);
    if (whole_status == LZFSE_ZAZZCAT_OK && into_status == LZFSE_ZAZZCAT_OK) {
        if (written != whole.length || (written > 0 && memcmp(into, whole.data, written) != 0)) abort();
    }
    free(into);
    lzfse_zazzcat_buffer_free(&whole);
    return 0;
}
