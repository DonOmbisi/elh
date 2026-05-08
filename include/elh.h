/*
 * ELH — Elastic Hashing Lossless Compression
 * LZ4-compatible block format, tunable bucket hash table
 * Don Morrigan, 2026
 */
#ifndef ELH_H
#define ELH_H

#include <stddef.h>
#include <stdint.h>

#define ELH_VERSION_MAJOR 0
#define ELH_VERSION_MINOR 1

/* Compression parameters */
typedef struct {
    int bucket_k;      /* hash bucket depth: 1, 2, 4, 8 (default 4) */
    int use_overflow;  /* enable A2 overflow table (default 1) */
    int acceleration;  /* speed bias 1-9, higher=faster/worse (default 1) */
    int window_size;   /* max match distance 1-65535, 0=max (default 0) */
} elh_params_t;

#define ELH_PARAMS_DEFAULT  { 4, 1, 1, 0 }
#define ELH_PARAMS_FAST     { 2, 0, 1, 0 }
#define ELH_PARAMS_MAX      { 4, 1, 1, 0 }
#define ELH_PARAMS_SHORT    { 4, 1, 1, 16384 } /* for xml/json/markup */

/* Returns max compressed size for srcSize input */
int elh_compress_bound(int srcSize);

/* Compress src into dst. Returns compressed size or negative on error.
 * Output is valid LZ4 block format — decompress with LZ4_decompress_safe. */
int elh_compress(const void* src, int srcSize,
                 void* dst, int dstCapacity,
                 elh_params_t params);

/* Decompress LZ4 block format. Returns decompressed size or negative. */
int elh_decompress(const void* src, int srcSize,
                   void* dst, int dstCapacity);

#endif /* ELH_H */
