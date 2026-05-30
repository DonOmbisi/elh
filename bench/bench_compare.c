#include "elh.h"
#include "lz4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int (*compress_fn)(const char*, int, char*, int, void*);
typedef int (*decompress_fn)(const char*, int, char*, int, void*);

typedef struct {
    const char* name;
    compress_fn compress;
    decompress_fn decompress;
    void* ctx;
} codec_t;

typedef struct {
    elh_params_t params;
} elh_ctx_t;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int read_file(const char* path, char** data, int* size) {
    FILE* f = fopen(path, "rb");
    long n;
    char* buf;

    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    n = ftell(f);
    if (n < 0 || n > 0x7fffffff) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    buf = (char*)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return -1; }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *data = buf;
    *size = (int)n;
    return 0;
}

static const char* base_name(const char* path) {
    const char* a = strrchr(path, '/');
    const char* b = strrchr(path, '\\');
    const char* p = a > b ? a : b;
    return p ? p + 1 : path;
}

static int lz4_compress_wrap(const char* src, int srcSize,
                             char* dst, int dstCapacity, void* ctx) {
    (void)ctx;
    return LZ4_compress_default(src, dst, srcSize, dstCapacity);
}

static int lz4_decompress_wrap(const char* src, int srcSize,
                               char* dst, int dstCapacity, void* ctx) {
    (void)ctx;
    return LZ4_decompress_safe(src, dst, srcSize, dstCapacity);
}

static int elh_compress_wrap(const char* src, int srcSize,
                             char* dst, int dstCapacity, void* ctx) {
    elh_ctx_t* e = (elh_ctx_t*)ctx;
    return elh_compress(src, srcSize, dst, dstCapacity, e->params);
}

static int elh_decompress_wrap(const char* src, int srcSize,
                               char* dst, int dstCapacity, void* ctx) {
    (void)ctx;
    return elh_decompress(src, srcSize, dst, dstCapacity);
}

static void bench_codec(const char* file_name, const char* src, int srcSize,
                        codec_t codec, int bound, int runs) {
    char* comp = (char*)malloc((size_t)bound);
    char* decomp = (char*)malloc((size_t)srcSize ? (size_t)srcSize : 1);
    int cLen, dLen, verified = 0;
    double best_comp = 1e99, best_decomp = 1e99;

    if (!comp || !decomp) {
        fprintf(stderr, "allocation failed for %s\n", codec.name);
        free(comp);
        free(decomp);
        return;
    }

    cLen = codec.compress(src, srcSize, comp, bound, codec.ctx);
    dLen = codec.decompress(comp, cLen, decomp, srcSize, codec.ctx);
    verified = (cLen >= 0 && dLen == srcSize &&
                memcmp(src, decomp, (size_t)srcSize) == 0);

    for (int r = 0; r < runs; r++) {
        double t0 = now_sec();
        cLen = codec.compress(src, srcSize, comp, bound, codec.ctx);
        double t1 = now_sec();
        if (cLen < 0) {
            verified = 0;
            break;
        }
        if (t1 - t0 < best_comp) best_comp = t1 - t0;
    }

    for (int r = 0; r < runs; r++) {
        double t0 = now_sec();
        dLen = codec.decompress(comp, cLen, decomp, srcSize, codec.ctx);
        double t1 = now_sec();
        if (dLen != srcSize) {
            verified = 0;
            break;
        }
        if (t1 - t0 < best_decomp) best_decomp = t1 - t0;
    }

    if (dLen == srcSize && memcmp(src, decomp, (size_t)srcSize) != 0)
        verified = 0;

    printf("%s,%s,%d,%d,%.4f,%.2f,%.2f,%s\n",
           file_name,
           codec.name,
           srcSize,
           cLen,
           srcSize ? (100.0 * cLen / srcSize) : 0.0,
           best_comp > 0.0 && best_comp < 1e90 ? (srcSize / 1e6) / best_comp : 0.0,
           best_decomp > 0.0 && best_decomp < 1e90 ? (srcSize / 1e6) / best_decomp : 0.0,
           verified ? "yes" : "no");

    free(comp);
    free(decomp);
}

int main(int argc, char** argv) {
    char* src = NULL;
    int srcSize = 0;
    int runs = 3;
    int bound;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: bench_compare <file> [runs]\n");
        return 1;
    }
    if (argc == 3) {
        runs = atoi(argv[2]);
        if (runs < 1) runs = 1;
    }

    if (read_file(argv[1], &src, &srcSize) != 0) {
        perror(argv[1]);
        return 1;
    }

    bound = elh_compress_bound(srcSize);
    if (LZ4_compressBound(srcSize) > bound)
        bound = LZ4_compressBound(srcSize);

    elh_ctx_t e1 = {{1, 0, 1, 0, 0}};
    elh_ctx_t e2 = {{2, 0, 1, 0, 0}};
    elh_ctx_t e2o = {{2, 1, 1, 0, 0}};
    elh_ctx_t e4 = {{4, 0, 1, 0, 0}};
    elh_ctx_t e4o = {{4, 1, 1, 0, 0}};

    codec_t codecs[] = {
        {"lz4_default", lz4_compress_wrap, lz4_decompress_wrap, NULL},
        {"elh_k1_ovf0", elh_compress_wrap, elh_decompress_wrap, &e1},
        {"elh_k2_ovf0", elh_compress_wrap, elh_decompress_wrap, &e2},
        {"elh_k2_ovf1", elh_compress_wrap, elh_decompress_wrap, &e2o},
        {"elh_k4_ovf0", elh_compress_wrap, elh_decompress_wrap, &e4},
        {"elh_k4_ovf1", elh_compress_wrap, elh_decompress_wrap, &e4o},
    };

    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++)
        bench_codec(base_name(argv[1]), src, srcSize, codecs[i], bound, runs);

    free(src);
    return 0;
}
