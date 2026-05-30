#include "elh_frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char* msg =
        "ELH frame API example. ELH frame API example. "
        "Repeated text compresses well.\n";
    int srcSize = (int)strlen(msg);
    elh_frame_params_t params = ELH_FRAME_PARAMS_DEFAULT;

    int bound = elh_frame_compress_bound(srcSize, params.chunk_size);
    char* compressed = (char*)malloc((size_t)bound);
    char* restored = (char*)malloc((size_t)srcSize);
    if (!compressed || !restored) {
        fprintf(stderr, "allocation failed\n");
        free(compressed);
        free(restored);
        return 1;
    }

    int compressedSize = elh_frame_compress(msg, srcSize,
                                            compressed, bound,
                                            params);
    if (compressedSize < 0) {
        fprintf(stderr, "compression failed\n");
        free(compressed);
        free(restored);
        return 1;
    }

    int originalSize = elh_frame_get_original_size(compressed, compressedSize);
    int restoredSize = elh_frame_decompress(compressed, compressedSize,
                                            restored, originalSize);
    if (restoredSize != srcSize || memcmp(msg, restored, (size_t)srcSize) != 0) {
        fprintf(stderr, "roundtrip failed\n");
        free(compressed);
        free(restored);
        return 1;
    }

    printf("ELH frame roundtrip: %d -> %d bytes\n", srcSize, compressedSize);
    free(compressed);
    free(restored);
    return 0;
}
