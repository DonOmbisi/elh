/*
 * ELH - Elastic Hashing Lossless Compression
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
    int bucket_k;        /* hash bucket depth: 1, 2, 4 (default 4)
                            * set to 0 for fully adaptive mode (elastic hashing) */
    int use_overflow;    /* enable A2 overflow table (default 1) */
    int acceleration;    /* speed bias 1-9, higher=faster/worse (default 1) */
    int window_size;     /* max match distance 1-65535, 0=max (default 0) */
    int use_wide_offsets;/* 0=16-bit offsets (default), 1=32-bit offset escape
                            * format. Current streaming history is still 64KB. */
} elh_params_t;

#define ELH_PARAMS_DEFAULT  { 4, 1, 1, 0, 0 }
#define ELH_PARAMS_FAST     { 2, 0, 1, 0, 0 }
#define ELH_PARAMS_MAX      { 4, 1, 1, 0, 0 }
#define ELH_PARAMS_SHORT    { 4, 1, 1, 16384, 0 } /* for xml/json/markup */
#define ELH_PARAMS_ADAPTIVE { 0, 1, 1, 0, 0 }     /* fully adaptive k */
#define ELH_PARAMS_WIDE     { 4, 1, 1, 0, 1 }     /* 32-bit offset escape format */

/* Returns max compressed size for srcSize input */
int elh_compress_bound(int srcSize);

/* Streaming API
 * Compresses data incrementally. Each chunk is compressed against the retained
 * stream history. Current matches span chunk boundaries up to 65535 bytes back.
 * Ideal for log streaming and incremental backup of append-only data structures.
 */

/* Opaque streaming state */
typedef struct elh_stream_s elh_stream_t;

/* Create streaming compressor. Returns NULL on allocation failure. */
elh_stream_t* elh_stream_new(elh_params_t params);

/* Destroy streaming compressor. */
void elh_stream_free(elh_stream_t* s);

/* Reset to initial state (clears hash table, resets position). */
void elh_stream_reset(elh_stream_t* s);

/* Max compressed output size for a chunk of srcSize bytes. */
int elh_stream_bound(int srcSize);

/* Compress one chunk. Hash table persists across calls.
 * Returns compressed bytes written, or negative on error. */
int elh_stream_compress(elh_stream_t* s,
                        const void* src, int srcSize,
                        void* dst, int dstCapacity);

/* Decompress one chunk produced by elh_stream_compress.
 * Returns decompressed bytes written, or negative on error. */
int elh_stream_decompress(elh_stream_t* s,
                          const void* src, int srcSize,
                          void* dst, int dstCapacity);

/* Compress src into dst. Returns compressed size or negative on error.
 * Output is valid LZ4 block format - decompress with LZ4_decompress_safe. */
int elh_compress(const void* src, int srcSize,
                 void* dst, int dstCapacity,
                 elh_params_t params);

/* Decompress LZ4 block format. Returns decompressed size or negative. */
int elh_decompress(const void* src, int srcSize,
                   void* dst, int dstCapacity);

#endif /* ELH_H */
