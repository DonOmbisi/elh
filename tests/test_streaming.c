/*
 * ELH streaming API correctness tests
 * Tests cross-chunk match recovery and round-trip correctness
 */
#include "elh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

static int roundtrip_chunk(elh_stream_t* cs, elh_stream_t* ds,
                          const char* src, int n,
                          char* comp, int bound,
                          char* out) {
    int clen = elh_stream_compress(cs, src, n, comp, bound);
    if (clen < 0) return -1;
    int dlen = elh_stream_decompress(ds, comp, clen, out, n);
    if (dlen != n) return -1;
    return clen;
}

static void test_stream(const char* name, const void* src, int n,
                        int chunk_size, elh_params_t params) {
    int bound = elh_stream_bound(chunk_size + 16);
    char* comp   = malloc(bound * (n / chunk_size + 2));
    char* decomp = malloc(n + 1);
    int*  clens  = malloc(sizeof(int) * (n / chunk_size + 2));

    elh_stream_t* cs = elh_stream_new(params);
    elh_stream_t* ds = elh_stream_new(params);

    /* Compress in chunks */
    int pos = 0, nchunks = 0, total_comp = 0;
    char* cp = comp;
    while (pos < n) {
        int csz = (pos + chunk_size <= n) ? chunk_size : n - pos;
        int clen = elh_stream_compress(cs, (char*)src + pos, csz,
                                       cp, bound);
        if (clen < 0) {
            printf("  FAIL  %-40s chunk_size=%d: compress error at pos %d\n",
                   name, chunk_size, pos);
            goto done;
        }
        clens[nchunks++] = clen;
        cp += clen;
        total_comp += clen;
        pos += csz;
    }

    /* Decompress in chunks */
    pos = 0;
    cp = comp;
    char* dp = decomp;
    for (int i = 0; i < nchunks; i++) {
        int chunk_orig = (i < nchunks-1) ? chunk_size : n - (nchunks-1)*chunk_size;
        int dlen = elh_stream_decompress(ds, cp, clens[i], dp, chunk_orig);
        if (dlen < 0) {
            printf("  FAIL  %-40s chunk_size=%d: decompress error chunk %d\n",
                   name, chunk_size, i);
            goto done;
        }
        cp += clens[i];
        dp += dlen;
    }

    /* Verify */
    int ok = (memcmp(src, decomp, n) == 0);
    tests_run++;
    if (ok) {
        tests_passed++;
        printf("  PASS  %-40s chunk=%5d  %d->%d (%.1f%%)\n",
               name, chunk_size, n, total_comp, 100.0*total_comp/n);
    } else {
        printf("  FAIL  %-40s chunk=%5d  mismatch\n", name, chunk_size);
    }

done:
    elh_stream_free(cs);
    elh_stream_free(ds);
    free(comp); free(decomp); free(clens);
}

int main() {
    printf("ELH Streaming API Tests\n");
    printf("=======================\n\n");

    elh_params_t p4 = {4, 1, 1, 0};

    /* Test 1: cross-chunk matches — pattern repeats across boundary */
    {
        int n = 131072;
        char* s = malloc(n);
        for (int i = 0; i < n; i++) s[i] = "hello world "[i % 12];
        test_stream("cross-chunk pattern", s, n, 4096, p4);
        test_stream("cross-chunk pattern", s, n, 1024, p4);
        test_stream("cross-chunk pattern", s, n, 256,  p4);
        free(s);
    }

    /* Test 2: single byte chunks */
    {
        int n = 1024;
        char* s = malloc(n);
        for (int i = 0; i < n; i++) s[i] = "abcd"[i%4];
        test_stream("single-byte chunks", s, n, 1, p4);
        free(s);
    }

    /* Test 3: chunk size larger than input */
    {
        const char* s = "hello world";
        test_stream("chunk > input", s, 11, 4096, p4);
    }

    /* Test 4: KV cache simulation — repeated token embeddings */
    {
        int n = 65536;
        char* s = malloc(n);
        /* Simulate 512-dim float32 embeddings, 32 tokens */
        /* Each embedding has structured repeated values */
        for (int tok = 0; tok < 32; tok++) {
            for (int dim = 0; dim < 512*4; dim++) {
                s[tok*512*4 + dim] = (char)(tok * 7 + dim % 13);
            }
        }
        test_stream("kv-cache simulation", s, n, 2048, p4);
        test_stream("kv-cache simulation", s, n, 512,  p4);
        free(s);
    }

    /* Test 5: all parameter combinations */
    {
        int n = 32768;
        char* s = malloc(n);
        for (int i = 0; i < n; i++) s[i] = "abcdefgh"[i%8];
        elh_params_t params[] = {
            {1, 0, 1, 0},
            {2, 1, 1, 0},
            {4, 1, 1, 0},
            {0, 1, 1, 0},  /* adaptive */
        };
        const char* names[] = {"k=1", "k=2 ovf", "k=4 ovf", "adaptive"};
        for (int i = 0; i < 4; i++)
            test_stream(names[i], s, n, 1024, params[i]);
        free(s);
    }

    /* Test 6: reset clears history */
    {
        printf("\n  Testing reset clears state...\n");
        int n = 8192;
        char* s = malloc(n);
        for (int i = 0; i < n; i++) s[i] = "abcd"[i%4];
        int bound = elh_stream_bound(n);
        char* c1 = malloc(bound); char* c2 = malloc(bound);
        char* d1 = malloc(n);

        elh_stream_t* cs = elh_stream_new(p4);
        int len1 = elh_stream_compress(cs, s, n, c1, bound);
        elh_stream_reset(cs);
        int len2 = elh_stream_compress(cs, s, n, c2, bound);
        int same = (len1 == len2 && memcmp(c1, c2, len1) == 0);
        tests_run++;
        if (same) { tests_passed++; printf("  PASS  reset produces identical output\n"); }
        else printf("  FAIL  reset did not clear state\n");

        elh_stream_free(cs);
        free(s); free(c1); free(c2); free(d1);
    }

    /* Test 7: offset 65535 is a valid narrow offset and a wide escape */
    {
        printf("\n  Testing exact 65535-byte streaming offset...\n");
        elh_params_t params[] = {
            {4, 1, 1, 0, 0},
            {4, 1, 1, 0, 1},
        };
        const char* names[] = {"narrow", "wide"};
        int finalLens[2] = {0, 0};

        for (int mode = 0; mode < 2; mode++) {
            int total = 65535 + 64;
            char* expected = malloc(total);
            char* actual = malloc(total);
            int bound = elh_stream_bound(80);
            char* comp = malloc(bound);
            int outPos = 0;
            int ok = 1;

            elh_stream_t* cs = elh_stream_new(params[mode]);
            elh_stream_t* ds = elh_stream_new(params[mode]);

            char first[16] = {
                '_', 'E', 'L', 'H', '6', '5', 'K', '_',
                'm', 'a', 't', 'c', 'h', '!', '!', '!'
            };
            memcpy(expected + outPos, first, sizeof(first));
            if (roundtrip_chunk(cs, ds, first, sizeof(first), comp, bound,
                                actual + outPos) < 0) ok = 0;
            outPos += (int)sizeof(first);

            for (int i = 0; ok && i < 65519; i++) {
                char one = (char)('a' + (i % 23));
                expected[outPos] = one;
                if (roundtrip_chunk(cs, ds, &one, 1, comp, bound,
                                    actual + outPos) < 0) ok = 0;
                outPos++;
            }

            char final[64];
            memset(final, 'q', sizeof(final));
            final[0] = '!';
            memcpy(final + 1, first + 1, 15);
            for (int i = 16; i < 64; i++) final[i] = (char)('A' + (i % 17));
            memcpy(expected + outPos, final, sizeof(final));
            int finalClen = roundtrip_chunk(cs, ds, final, sizeof(final),
                                            comp, bound, actual + outPos);
            if (finalClen < 0) ok = 0;
            finalLens[mode] = finalClen;
            outPos += (int)sizeof(final);

            tests_run++;
            if (ok && outPos == total && memcmp(expected, actual, total) == 0) {
                tests_passed++;
                printf("  PASS  exact offset 65535 (%s) final chunk=%d bytes\n",
                       names[mode], finalClen);
            } else {
                printf("  FAIL  exact offset 65535 (%s)\n", names[mode]);
            }

            elh_stream_free(cs);
            elh_stream_free(ds);
            free(expected); free(actual); free(comp);
        }

        tests_run++;
        if (finalLens[1] == finalLens[0] + 4) {
            tests_passed++;
            printf("  PASS  wide escape adds 4 bytes at offset 65535\n");
        } else {
            printf("  FAIL  wide escape size: narrow=%d wide=%d\n",
                   finalLens[0], finalLens[1]);
        }
    }

    /* Test 8: chunk sizes around the streaming window limit */
    {
        printf("\n  Testing streaming chunk sizes around 64KB...\n");
        int chunk_sizes[] = {65535, 65536, 65537};
        int n = 65537 * 2 + 31;
        char* s = malloc(n);
        for (int i = 0; i < n; i++)
            s[i] = (char)("stream-window-boundary:"[i % 23] + (i / 8192) % 5);

        for (int i = 0; i < 3; i++) {
            char name[48];
            snprintf(name, sizeof(name), "stream window chunk=%d", chunk_sizes[i]);
            test_stream(name, s, n, chunk_sizes[i], p4);
        }
        free(s);
    }

    printf("\nResults: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
