/*
 * ELH frame API
 * Self-describing container for integrating ELH with other projects.
 */
#ifndef ELH_FRAME_H
#define ELH_FRAME_H

#include "elh.h"

#define ELH_FRAME_VERSION 1
#define ELH_FRAME_DEFAULT_CHUNK_SIZE 65536

typedef struct {
    elh_params_t params;
    int chunk_size;      /* 0 = default 64KB */
    int store_uncompressed; /* allow raw chunks when compression expands */
} elh_frame_params_t;

#define ELH_FRAME_PARAMS_DEFAULT { ELH_PARAMS_DEFAULT, ELH_FRAME_DEFAULT_CHUNK_SIZE, 1 }

/* Worst-case frame size for srcSize bytes using chunked block compression. */
int elh_frame_compress_bound(int srcSize, int chunkSize);

/* Compress into an ELH frame. Returns frame size or negative on error. */
int elh_frame_compress(const void* src, int srcSize,
                       void* dst, int dstCapacity,
                       elh_frame_params_t params);

/* Decompress an ELH frame. Returns decompressed size or negative on error. */
int elh_frame_decompress(const void* src, int srcSize,
                         void* dst, int dstCapacity);

/* Read original size from a frame without decompressing. */
int elh_frame_get_original_size(const void* src, int srcSize);

#endif /* ELH_FRAME_H */
