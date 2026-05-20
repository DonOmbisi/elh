#include "elh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Scaled-down KV cache */
#define KV_LAYERS    32
#define KV_HEADS     8
#define KV_HEAD_DIM  64
#define KV_DTYPE     2
#define KV_PER_TOKEN (KV_LAYERS * KV_HEADS * KV_HEAD_DIM * KV_DTYPE * 2)

static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Three data scenarios:
 * 1. HIGH SIMILARITY: adjacent tokens very similar (repetitive prompt)
 * 2. MEDIUM SIMILARITY: moderate variation (typical conversation)
 * 3. LOW SIMILARITY: near-random (code or binary data) */
static void gen_kv_high(unsigned char* buf, int tok) {
    /* Tokens share 90% of their values — long repetitive context */
    for (int i = 0; i < KV_PER_TOKEN; i++) {
        int base  = (i * 7 + 42) & 0xFF;
        int delta = ((tok + i/256) * 2) & 0x0F;  /* slow variation */
        buf[i] = (unsigned char)(base + delta);
    }
}

static void gen_kv_medium(unsigned char* buf, int tok) {
    /* Tokens share ~60% of values */
    for (int i = 0; i < KV_PER_TOKEN; i++) {
        int layer = i / (KV_HEADS * KV_HEAD_DIM * KV_DTYPE * 2);
        int base  = (layer * 31 + (i%64) * 5) & 0xFF;
        int delta = (tok * 7 + i/64) & 0x3F;
        buf[i] = (unsigned char)(base + delta);
    }
}

static void gen_kv_low(unsigned char* buf, int tok) {
    /* Near-random — worst case for compression */
    unsigned x = tok * 2654435761u + 1;
    for (int i = 0; i < KV_PER_TOKEN; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        buf[i] = (unsigned char)x;
    }
}

static void run_bench(const char* label,
                      void (*gen)(unsigned char*, int),
                      int n_tokens)
{
    int total_size = n_tokens * KV_PER_TOKEN;
    unsigned char* kv   = malloc(total_size);
    int bound = elh_stream_bound(KV_PER_TOKEN + 64) * n_tokens + 1024;
    unsigned char* c_blk = malloc(bound);
    unsigned char* c_str = malloc(bound);

    for (int t = 0; t < n_tokens; t++)
        gen(kv + t * KV_PER_TOKEN, t);

    /* Block compression (per-token, no history) */
    elh_params_t pb = {4, 1, 1, 0};
    int blk_total = 0;
    double t0 = now_sec();
    for (int t = 0; t < n_tokens; t++) {
        blk_total += elh_compress(kv + t*KV_PER_TOKEN, KV_PER_TOKEN,
                                  c_blk + blk_total, bound - blk_total, pb);
    }
    double blk_ms = (now_sec() - t0) * 1000;

    /* Streaming compression (cross-token history) */
    elh_stream_t* cs = elh_stream_new(pb);
    int str_total = 0;
    t0 = now_sec();
    for (int t = 0; t < n_tokens; t++) {
        str_total += elh_stream_compress(cs,
                                         kv + t*KV_PER_TOKEN, KV_PER_TOKEN,
                                         c_str + str_total, bound - str_total);
    }
    double str_ms = (now_sec() - t0) * 1000;
    elh_stream_free(cs);

    double blk_r = 100.0 * blk_total / total_size;
    double str_r = 100.0 * str_total / total_size;
    double gain  = blk_r - str_r;

    printf("%-22s %4d tok %6.0f KB  block:%5.1f%%  stream:%5.1f%%  gain:%+.1f%%\n",
           label, n_tokens, total_size/1024.0,
           blk_r, str_r, gain);

    free(kv); free(c_blk); free(c_str);
}

int main() {
    printf("ELH KV Cache Compression Benchmark\n");
    printf("KV per token: %d bytes (%.0f KB)\n\n", KV_PER_TOKEN, KV_PER_TOKEN/1024.0);
    printf("%-22s %8s %8s  %10s  %11s  %8s\n",
           "Scenario", "Tokens", "Size", "Block", "Stream", "Gain");
    printf("%s\n", "------------------------------------------------------------");

    int tokens[] = {4, 16, 64, 128};
    int nt = 4;

    for (int i = 0; i < nt; i++)
        run_bench("high similarity", gen_kv_high, tokens[i]);
    printf("\n");
    for (int i = 0; i < nt; i++)
        run_bench("medium similarity", gen_kv_medium, tokens[i]);
    printf("\n");
    for (int i = 0; i < nt; i++)
        run_bench("low similarity", gen_kv_low, tokens[i]);

    printf("\nblock  = per-token compression, no cross-token history\n");
    printf("stream = ELH streaming, full cross-token history\n");
    printf("gain   = stream improvement over block\n");
    return 0;
}
