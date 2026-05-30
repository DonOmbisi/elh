#include "elh_frame.h"
#include <stdint.h>
#include <string.h>

typedef uint8_t  U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef uint64_t U64;

#define ELH_FRAME_MAGIC0 'E'
#define ELH_FRAME_MAGIC1 'L'
#define ELH_FRAME_MAGIC2 'H'
#define ELH_FRAME_MAGIC3 '1'
#define ELH_FRAME_HEADER_SIZE 40
#define ELH_FRAME_CHUNK_HEADER_SIZE 12
#define ELH_FRAME_FLAG_RAW 1

static void write16(U8* p, U16 v) {
    p[0] = (U8)v;
    p[1] = (U8)(v >> 8);
}

static void write32(U8* p, U32 v) {
    p[0] = (U8)v;
    p[1] = (U8)(v >> 8);
    p[2] = (U8)(v >> 16);
    p[3] = (U8)(v >> 24);
}

static void write64(U8* p, U64 v) {
    for (int i = 0; i < 8; i++) p[i] = (U8)(v >> (8 * i));
}

static U16 read16(const U8* p) {
    return (U16)p[0] | ((U16)p[1] << 8);
}

static U32 read32(const U8* p) {
    return (U32)p[0] | ((U32)p[1] << 8) |
           ((U32)p[2] << 16) | ((U32)p[3] << 24);
}

static U64 read64(const U8* p) {
    U64 v = 0;
    for (int i = 0; i < 8; i++) v |= (U64)p[i] << (8 * i);
    return v;
}

static int norm_chunk_size(int chunkSize) {
    if (chunkSize <= 0) return ELH_FRAME_DEFAULT_CHUNK_SIZE;
    if (chunkSize > ELH_FRAME_DEFAULT_CHUNK_SIZE)
        return ELH_FRAME_DEFAULT_CHUNK_SIZE;
    return chunkSize;
}

static int valid_header(const U8* p, int srcSize) {
    if (srcSize < ELH_FRAME_HEADER_SIZE) return 0;
    if (p[0] != ELH_FRAME_MAGIC0 || p[1] != ELH_FRAME_MAGIC1 ||
        p[2] != ELH_FRAME_MAGIC2 || p[3] != ELH_FRAME_MAGIC3) return 0;
    if (p[4] != ELH_FRAME_VERSION) return 0;
    if (read16(p + 6) != ELH_FRAME_HEADER_SIZE) return 0;
    return 1;
}

int elh_frame_compress_bound(int srcSize, int chunkSize) {
    int chunk, chunks;
    if (srcSize < 0) return -1;
    chunk = norm_chunk_size(chunkSize);
    chunks = srcSize ? (srcSize + chunk - 1) / chunk : 0;
    return ELH_FRAME_HEADER_SIZE +
           chunks * (ELH_FRAME_CHUNK_HEADER_SIZE + elh_compress_bound(chunk));
}

int elh_frame_compress(const void* src, int srcSize,
                       void* dst, int dstCapacity,
                       elh_frame_params_t params) {
    const U8* ip = (const U8*)src;
    U8* op = (U8*)dst;
    U8* oend = op + dstCapacity;
    int chunkSize;
    int pos = 0;

    if (srcSize < 0 || !src || !dst) return -1;
    chunkSize = norm_chunk_size(params.chunk_size);
    if (dstCapacity < elh_frame_compress_bound(srcSize, chunkSize)) return -1;

    op[0] = ELH_FRAME_MAGIC0;
    op[1] = ELH_FRAME_MAGIC1;
    op[2] = ELH_FRAME_MAGIC2;
    op[3] = ELH_FRAME_MAGIC3;
    op[4] = ELH_FRAME_VERSION;
    op[5] = 0; /* frame flags */
    write16(op + 6, ELH_FRAME_HEADER_SIZE);
    write64(op + 8, (U64)srcSize);
    write32(op + 16, (U32)chunkSize);
    write32(op + 20, (U32)params.params.bucket_k);
    write32(op + 24, (U32)params.params.use_overflow);
    write32(op + 28, (U32)params.params.acceleration);
    write32(op + 32, (U32)params.params.window_size);
    write32(op + 36, (U32)params.params.use_wide_offsets);
    op += ELH_FRAME_HEADER_SIZE;

    while (pos < srcSize) {
        int rawSize = srcSize - pos;
        int compSize;
        U8* chunkHeader = op;
        U8* payload;
        int payloadCap;

        if (rawSize > chunkSize) rawSize = chunkSize;
        if (op + ELH_FRAME_CHUNK_HEADER_SIZE > oend) return -1;
        op += ELH_FRAME_CHUNK_HEADER_SIZE;
        payload = op;
        payloadCap = (int)(oend - op);

        compSize = elh_compress(ip + pos, rawSize, payload, payloadCap,
                                params.params);
        if (compSize < 0 ||
            (params.store_uncompressed && compSize >= rawSize)) {
            if (payloadCap < rawSize) return -1;
            memcpy(payload, ip + pos, (size_t)rawSize);
            compSize = rawSize;
            chunkHeader[8] = ELH_FRAME_FLAG_RAW;
        } else {
            chunkHeader[8] = 0;
        }

        write32(chunkHeader + 0, (U32)rawSize);
        write32(chunkHeader + 4, (U32)compSize);
        chunkHeader[9] = chunkHeader[10] = chunkHeader[11] = 0;
        op += compSize;
        pos += rawSize;
    }

    return (int)(op - (U8*)dst);
}

int elh_frame_decompress(const void* src, int srcSize,
                         void* dst, int dstCapacity) {
    const U8* ip = (const U8*)src;
    const U8* iend = ip + srcSize;
    U8* op = (U8*)dst;
    U8* oend = op + dstCapacity;
    U64 originalSize;
    int written = 0;

    if (!src || !dst || srcSize < 0 || dstCapacity < 0) return -1;
    if (!valid_header(ip, srcSize)) return -1;

    originalSize = read64(ip + 8);
    if (originalSize > (U64)dstCapacity || originalSize > 0x7fffffffU)
        return -1;
    ip += ELH_FRAME_HEADER_SIZE;

    while (written < (int)originalSize) {
        U32 rawSize, compSize;
        U8 flags;
        int dLen;

        if (ip + ELH_FRAME_CHUNK_HEADER_SIZE > iend) return -1;
        rawSize = read32(ip + 0);
        compSize = read32(ip + 4);
        flags = ip[8];
        if (ip[9] || ip[10] || ip[11]) return -1;
        ip += ELH_FRAME_CHUNK_HEADER_SIZE;

        if (rawSize == 0 || rawSize > ELH_FRAME_DEFAULT_CHUNK_SIZE)
            return -1;
        if (compSize == 0 || ip + compSize > iend) return -1;
        if (op + rawSize > oend) return -1;

        if (flags & ELH_FRAME_FLAG_RAW) {
            if (compSize != rawSize) return -1;
            memcpy(op, ip, rawSize);
            dLen = (int)rawSize;
        } else {
            dLen = elh_decompress(ip, (int)compSize, op, (int)rawSize);
        }
        if (dLen != (int)rawSize) return -1;

        ip += compSize;
        op += rawSize;
        written += (int)rawSize;
    }

    if (ip != iend) return -1;
    return written;
}

int elh_frame_get_original_size(const void* src, int srcSize) {
    const U8* p = (const U8*)src;
    U64 n;
    if (!src || !valid_header(p, srcSize)) return -1;
    n = read64(p + 8);
    if (n > 0x7fffffffU) return -1;
    return (int)n;
}
