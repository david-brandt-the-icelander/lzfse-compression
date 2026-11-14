// Fuzz target: parsing arbitrary decmpfs attributes must never crash. The first byte says where the input splits
// into the `com.apple.decmpfs` attribute and the resource fork.
#include "bare_functions.h"
#include "lzfse_zazzcat.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t split = size > 0 ? data[0] : 0;
    if (split > size) split = size;
    lzfse_zazzcat_buffer out = {0};
    bare_lzfse_zazzcat_decmpfs_decompress(data, split, data + split, size - split, 1 << 22, &out);
    lzfse_zazzcat_buffer_free(&out);
    return 0;
}
