#include "elh_frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

static void passfail(const char* name, int ok) {
    tests_run++;
    if (ok) {
        tests_passed++;
        printf("  PASS  %s\n", name);
    } else {
        printf("  FAIL  %s\n", name);
    }
}

static void test_roundtrip(const char* name, const char* src, int n,
                           int chunkSize, elh_params_t params) {
    elh_frame_params_t fp = { params, chunkSize, 1 };
    int bound = elh_frame_compress_bound(n, chunkSize);
    char* frame = (char*)malloc((size_t)bound);
    char* out = (char*)malloc((size_t)n ? (size_t)n : 1);
    int fLen = elh_frame_compress(src, n, frame, bound, fp);
    int original = elh_frame_get_original_size(frame, fLen);
    int dLen = elh_frame_decompress(frame, fLen, out, n);
    int ok = (fLen >= 0 && original == n && dLen == n &&
              memcmp(src, out, (size_t)n) == 0);
    passfail(name, ok);
    free(frame);
    free(out);
}

int main(void) {
    printf("ELH Frame API Tests\n");
    printf("===================\n\n");

    {
        elh_params_t p = ELH_PARAMS_DEFAULT;
        test_roundtrip("empty frame", "", 0, 0, p);
    }

    {
        static const unsigned char abc_frame[] = {
            'E','L','H','1', 1, 0, 40, 0,
            3, 0, 0, 0, 0, 0, 0, 0,
            3, 0, 0, 0,
            4, 0, 0, 0,
            1, 0, 0, 0,
            1, 0, 0, 0,
            0, 0, 0, 0,
            0, 0, 0, 0,
            3, 0, 0, 0,
            3, 0, 0, 0,
            1, 0, 0, 0,
            'a', 'b', 'c'
        };
        char out[3];
        int original = elh_frame_get_original_size(abc_frame,
                                                   (int)sizeof(abc_frame));
        int dLen = elh_frame_decompress(abc_frame, (int)sizeof(abc_frame),
                                        out, (int)sizeof(out));
        passfail("compat fixture raw abc",
                 original == 3 && dLen == 3 && memcmp(out, "abc", 3) == 0);
    }

    {
        elh_params_t p = ELH_PARAMS_DEFAULT;
        const char* text =
            "The quick brown fox jumps over the lazy dog. "
            "The quick brown fox jumps over the lazy dog. ";
        int n = (int)strlen(text) * 1000;
        char* s = (char*)malloc((size_t)n);
        for (int i = 0; i < n; i++) s[i] = text[i % (int)strlen(text)];
        test_roundtrip("repeated text default chunks", s, n, 0, p);
        test_roundtrip("repeated text small chunks", s, n, 4096, p);
        free(s);
    }

    {
        elh_params_t p = ELH_PARAMS_MAX;
        int n = 65537 * 2 + 123;
        char* s = (char*)malloc((size_t)n);
        for (int i = 0; i < n; i++)
            s[i] = (char)("frame-boundary-data:"[i % 20] + (i / 8192) % 7);
        test_roundtrip("frame window boundary chunks", s, n, 65535, p);
        free(s);
    }

    {
        elh_params_t p = ELH_PARAMS_DEFAULT;
        int n = 10000;
        char* s = (char*)malloc((size_t)n);
        unsigned x = 12345;
        for (int i = 0; i < n; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            s[i] = (char)x;
        }
        test_roundtrip("random data raw fallback", s, n, 1024, p);
        free(s);
    }

    {
        elh_frame_params_t fp = ELH_FRAME_PARAMS_DEFAULT;
        char src[64];
        char out[64];
        int bound = elh_frame_compress_bound((int)sizeof(src), fp.chunk_size);
        char* frame = (char*)malloc((size_t)bound);
        memset(src, 'A', sizeof(src));
        int fLen = elh_frame_compress(src, (int)sizeof(src),
                                      frame, bound, fp);
        int badMagic = 0, badTruncate = 0, badCapacity = 0;
        if (fLen > 0) {
            frame[0] = 'X';
            badMagic = elh_frame_decompress(frame, fLen, out, sizeof(out)) < 0;
            frame[0] = 'E';
            badTruncate = elh_frame_decompress(frame, fLen - 1,
                                               out, sizeof(out)) < 0;
            badCapacity = elh_frame_decompress(frame, fLen,
                                               out, sizeof(out) - 1) < 0;
        }
        passfail("malformed frame rejection",
                 fLen > 0 && badMagic && badTruncate && badCapacity);
        free(frame);
    }

    printf("\nResults: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
