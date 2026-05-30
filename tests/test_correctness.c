/*
 * ELH correctness test suite
 * Tests all parameter combinations on varied input types
 */
#include "elh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

static void test_empty(void) {
    elh_params_t p = ELH_PARAMS_DEFAULT;
    char comp[16];
    char decomp[16];
    int cLen = elh_compress("", 0, comp, sizeof(comp), p);
    int dLen = elh_decompress(comp, cLen, decomp, sizeof(decomp));
    int ok = (cLen == 0 && dLen == 0);

    tests_run++;
    if (ok) {
        tests_passed++;
        printf("  PASS  %-40s  0->0\n", "empty");
    } else {
        printf("  FAIL  %-40s  cLen=%d dLen=%d\n", "empty", cLen, dLen);
    }
}

static void test(const char* name, const void* src, int n,
                 int k, int ovf, int acc) {
    int bound = elh_compress_bound(n);
    char* comp   = malloc(bound);
    char* decomp = malloc(n + 1);

    elh_params_t p = {k, ovf, acc};
    int cLen = elh_compress(src, n, comp, bound, p);
    int dLen = elh_decompress(comp, cLen, decomp, n);
    int ok   = (dLen == n) && (memcmp(src, decomp, n) == 0);

    tests_run++;
    if (ok) {
        tests_passed++;
        double ratio = n ? 100.0*cLen/n : 0.0;
        printf("  PASS  %-40s k=%d ovf=%d  %d->%d (%.1f%%)\n",
               name, k, ovf, n, cLen, ratio);
    } else {
        printf("  FAIL  %-40s k=%d ovf=%d  dLen=%d expected=%d\n",
               name, k, ovf, dLen, n);
    }

    free(comp);
    free(decomp);
}

static void test_all_params(const char* name, const void* src, int n) {
    test(name, src, n, 1, 0, 1);
    test(name, src, n, 2, 0, 1);
    test(name, src, n, 2, 1, 1);
    test(name, src, n, 4, 0, 1);
    test(name, src, n, 4, 1, 1);
}

int main() {
    printf("ELH Correctness Tests\n");
    printf("=====================\n\n");

    /* Empty input */
    test_empty();

    /* Tiny inputs around the minimum match length */
    {
        const char* tiny = "abcdef";
        for (int n = 1; n <= 6; n++) {
            char name[32];
            snprintf(name, sizeof(name), "tiny input (%d bytes)", n);
            test(name, tiny, n, 4, 1, 1);
        }
    }

    /* Highly repetitive */
    {
        int n = 65536;
        char* s = malloc(n);
        memset(s, 'A', n);
        test_all_params("all same byte (64KB)", s, n);
        free(s);
    }

    /* Pattern repetition */
    {
        int n = 32768;
        char* s = malloc(n);
        for (int i = 0; i < n; i++) s[i] = "abcdefgh"[i%8];
        test_all_params("8-byte pattern (32KB)", s, n);
        free(s);
    }

    /* Cross-boundary matches */
    {
        int n = 131072;  /* 128KB — crosses two 64KB boundaries */
        char* s = malloc(n);
        for (int i = 0; i < n; i++) s[i] = "hello world "[i%12];
        test_all_params("cross-boundary (128KB)", s, n);
        free(s);
    }

    /* Window boundary sizes */
    {
        int sizes[] = {65535, 65536, 65537};
        for (int si = 0; si < 3; si++) {
            int n = sizes[si];
            char* s = malloc(n);
            for (int i = 0; i < n; i++)
                s[i] = (char)("ELH-window-boundary!"[i % 20] + (i / 4096) % 3);
            char name[48];
            snprintf(name, sizeof(name), "window boundary (%d bytes)", n);
            test_all_params(name, s, n);
            free(s);
        }
    }

    /* Near-random (high entropy) */
    {
        int n = 4096;
        char* s = malloc(n);
        unsigned x = 12345;
        for (int i = 0; i < n; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            s[i] = (char)x;
        }
        test_all_params("pseudo-random (4KB)", s, n);
        free(s);
    }

    /* English text sample */
    {
        const char* text =
            "The quick brown fox jumps over the lazy dog. "
            "The quick brown fox jumps over the lazy dog. "
            "Pack my box with five dozen liquor jugs. "
            "How vexingly quick daft zebras jump. ";
        int n = (int)strlen(text) * 100;
        char* s = malloc(n);
        for (int i = 0; i < n; i++) s[i] = text[i % strlen(text)];
        test_all_params("english text (repeated)", s, n);
        free(s);
    }

    /* Acceleration parameter sweep */
    {
        int n = 16384;
        char* s = malloc(n);
        for (int i = 0; i < n; i++) s[i] = "abcd"[i%4];
        for (int acc = 1; acc <= 5; acc++) {
            char name[32];
            snprintf(name, sizeof(name), "acceleration=%d", acc);
            test(name, s, n, 4, 1, acc);
        }
        free(s);
    }

    printf("\n");
    printf("Results: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
