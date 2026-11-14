// Fuzz target: decoding arbitrary LZVN bytes must never crash or write past the output.
#include <stdlib.h>

#include "lzfse_zazzcat.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t capacity = 1 << 16, written = 0;
    uint8_t *out = malloc(capacity);
    lzfse_zazzcat_lzvn_decode(data, size, out, capacity, &written);
    free(out);
    return 0;
}
