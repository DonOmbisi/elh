/*
 * ELH — Elastic Hashing Lossless Compression
 * src/elh.c — compressor and decompressor
 *
 * Output format: LZ4 block format (compatible with LZ4_decompress_safe)
 * Hash table:    two-level elastic bucket structure (Farach-Colton et al. 2025)
 * Window:        sliding window, matches cross block boundaries
 *
 * Don Morrigan, 2026
 */

#include "elh.h"
#include <string.h>
#include <stdlib.h>

typedef uint8_t  U8;
typedef uint16_t U16;
typedef uint32_t U32;

static inline U32 elh_read32(const void* p) { U32 v; memcpy(&v, p, 4); return v; }
static inline void elh_write16(void* p, U16 v) { memcpy(p, &v, 2); }

/* LZ4 block format constants */
#define ELH_MINMATCH     4
#define ELH_LASTLITERALS 5
#define ELH_MFLIMIT      13
#define ELH_DISTANCE_MAX 65535
#define ELH_ML_MASK      ((1U << 4) - 1)
#define ELH_RUN_MASK     ((1U << 4) - 1)

/* Hash table — U32 positions, sliding window */
#define ELH_BUCKET_MAX   4
#define ELH_A1_HASHLOG   12              /* 4096 buckets */
#define ELH_A1_BUCKETS   (1 << ELH_A1_HASHLOG)
#define ELH_A1_SLOTS     (ELH_A1_BUCKETS * ELH_BUCKET_MAX)  /* 16384 U32 = 65536 bytes */
#define ELH_A2_HASHLOG   14
#define ELH_A2_BUCKETS   (1 << ELH_A2_HASHLOG)              /* 16384 U32 = 65536 bytes */

static inline U32 elh_hash_a1(U32 seq) {
    return (seq * 2654435761u) >> (32 - ELH_A1_HASHLOG);
}
static inline U32 elh_hash_a2(U32 seq) {
    return (seq * 2246822519u) >> (32 - ELH_A2_HASHLOG);
}

typedef struct {
    U32 a1[ELH_A1_SLOTS];   /* 65536 bytes */
    U32 a2[ELH_A2_BUCKETS]; /* 65536 bytes */
} elh_htable_t;             /* 131072 bytes total */

static inline void elh_put(elh_htable_t* ht, U32 seq, U32 pos, int k) {
    U32 h1 = elh_hash_a1(seq);
    U32 b  = h1 * ELH_BUCKET_MAX;
    U32 evicted = ht->a1[b + k - 1];
    if (k >= 4) ht->a1[b+3] = ht->a1[b+2];
    if (k >= 3) ht->a1[b+2] = ht->a1[b+1];
    if (k >= 2) ht->a1[b+1] = ht->a1[b+0];
    ht->a1[b] = pos;
    if (evicted != 0) {
        ht->a2[elh_hash_a2(seq)] = evicted;
    }
}

static inline U32 elh_get(const elh_htable_t* ht, U32 seq,
                           const U8* base, const U8* ip,
                           const U8* mlimit, U32 curPos,
                           int k, int ovf)
{
    U32 h1   = elh_hash_a1(seq);
    U32 b    = h1 * ELH_BUCKET_MAX;
    U32 best = 0, blen = 0;
    int i;
    for (i = 0; i < k; i++) {
        U32 pos = ht->a1[b+i];
        if (!pos) continue;
        if (curPos - pos > ELH_DISTANCE_MAX) continue;
        const U8* m = base + pos;
        if (elh_read32(m) != elh_read32(ip)) continue;
        const U8* a = ip+4, *c = m+4;
        while (a < mlimit && *a == *c) { a++; c++; }
        U32 len = (U32)(a - ip);
        if (len > blen) { blen = len; best = pos; }
    }
    if (!best && ovf) {
        U32 pos = ht->a2[elh_hash_a2(seq)];
        if (pos && curPos - pos <= ELH_DISTANCE_MAX &&
            elh_read32(base+pos) == elh_read32(ip)) best = pos;
    }
    return best;
}

int elh_compress_bound(int n) {
    /* 8 bytes per 64KB block header + LZ4 overhead */
    return n + (n / 255) + (n / 65536 + 1) * 8 + 32;
}

int elh_compress(const void* src, int srcSize,
                 void* dst, int dstCapacity,
                 elh_params_t params)
{
    if (srcSize <= 0 || !src || !dst) return -1;
    if (dstCapacity < elh_compress_bound(srcSize)) return -1;

    int k   = params.bucket_k < 1 ? 1 :
              params.bucket_k > ELH_BUCKET_MAX ? ELH_BUCKET_MAX : params.bucket_k;
    int ovf = params.use_overflow;
    int acc = params.acceleration < 1 ? 1 : params.acceleration;

    const U8* ip      = (const U8*)src;
    const U8* iend    = ip + srcSize;
    const U8* base    = ip;   /* absolute base — never changes */
    const U8* anchor  = ip;
    U8* op   = (U8*)dst;
    U8* oend = op + dstCapacity;

    /* Hash table persists across the entire input */
    elh_htable_t* ht = (elh_htable_t*)calloc(1, sizeof(elh_htable_t));
    if (!ht) return -1;

    if (srcSize < ELH_MINMATCH + 1) goto _last_lits;
    ip++;

    for (;;) {
        const U8* search = ip;
        const U8* mptr   = NULL;
        int skips = 0;
        /* mflimit and mlimit are relative to end of input */
        const U8* mflimit = iend - ELH_MFLIMIT;
        const U8* mlimit  = iend - ELH_LASTLITERALS;

        while (search <= mflimit) {
            U32 seq = elh_read32(search);
            U32 cur = (U32)(search - base);
            U32 pos = elh_get(ht, seq, base, search, mlimit, cur, k, ovf);
            elh_put(ht, seq, cur, k);
            if (pos) {
                /* pos is valid — distance check already done in elh_get */
                mptr = base + pos; ip = search; goto _found;
            }
            skips++;
            search += 1 + (skips >> (acc + 2));
        }
        goto _last_lits;

    _found:
        while (ip > anchor && mptr > base && ip[-1] == mptr[-1]) { ip--; mptr--; }

        {
            U32 litLen = (U32)(ip - anchor);
            U8* token  = op++;
            if (op + litLen + (litLen/255) + 10 > oend) { free(ht); return -1; }

            if (litLen >= ELH_RUN_MASK) {
                *token = (U8)(ELH_RUN_MASK << 4);
                U32 r = litLen - ELH_RUN_MASK;
                while (r >= 255) { *op++ = 255; r -= 255; }
                *op++ = (U8)r;
            } else {
                *token = (U8)(litLen << 4);
            }
            memcpy(op, anchor, litLen);
            op += litLen;

            const U8* mlimit = iend - ELH_LASTLITERALS;
            const U8* ma = ip+4, *mc = mptr+4;
            while (ma < mlimit && *ma == *mc) { ma++; mc++; }
            U32 matchLen = (U32)(ma - ip);
            U32 mlCode   = matchLen - ELH_MINMATCH;

            elh_write16(op, (U16)(ip - mptr)); op += 2;

            if (mlCode < ELH_ML_MASK) {
                *token |= (U8)mlCode;
            } else {
                *token |= ELH_ML_MASK;
                U32 r = mlCode - ELH_ML_MASK;
                if (op + (r/255) + 2 > oend) { free(ht); return -1; }
                while (r >= 255) { *op++ = 255; r -= 255; }
                *op++ = (U8)r;
            }

            ip += matchLen;
            anchor = ip;

            const U8* mflimit2 = iend - ELH_MFLIMIT;
            const U8* hp = ip - matchLen + 2;
            while (hp < ip - 1 && hp <= mflimit2) {
                U32 pp = (U32)(hp - base);
                elh_put(ht, elh_read32(hp), pp, k);
                hp++;
            }

            if (ip > mflimit2) goto _last_lits;
        }
    }

_last_lits:;
    {
        U32 litLen = (U32)(iend - anchor);
        if (op + litLen + (litLen/255) + 2 > oend) { free(ht); return -1; }
        U8* token = op++;
        if (litLen >= ELH_RUN_MASK) {
            *token = (U8)(ELH_RUN_MASK << 4);
            U32 r = litLen - ELH_RUN_MASK;
            while (r >= 255) { *op++ = 255; r -= 255; }
            *op++ = (U8)r;
        } else {
            *token = (U8)(litLen << 4);
        }
        memcpy(op, anchor, litLen);
        op += litLen;
    }

    free(ht);
    return (int)(op - (U8*)dst);
}

/* Decompressor — single contiguous stream, no block headers */
int elh_decompress(const void* src, int srcSize,
                   void* dst, int dstCapacity)
{
    const U8* ip   = (const U8*)src;
    const U8* iend = ip + srcSize;
    U8* op   = (U8*)dst;
    U8* op0  = op;
    U8* oend = op + dstCapacity;

    while (ip < iend) {
        U8  token  = *ip++;
        U32 litLen = token >> 4;
        if (litLen == ELH_RUN_MASK) {
            U8 s; do { s = *ip++; litLen += s; } while (s == 255 && ip < iend);
        }
        if (op + litLen > oend || ip + litLen > iend) return -1;
        memcpy(op, ip, litLen); op += litLen; ip += litLen;
        if (ip >= iend) break;
        if (ip + 2 > iend) return -1;
        U16 offset; memcpy(&offset, ip, 2); ip += 2;
        if (!offset) return -1;
        const U8* match = op - offset;
        if (match < op0) return -1;
        U32 matchLen = (token & ELH_ML_MASK) + ELH_MINMATCH;
        if ((token & ELH_ML_MASK) == ELH_ML_MASK) {
            U8 s; do { s = *ip++; matchLen += s; } while (s == 255 && ip < iend);
        }
        if (op + matchLen > oend) return -1;
        U32 i; for (i = 0; i < matchLen; i++) op[i] = match[i];
        op += matchLen;
    }
    return (int)(op - op0);
}
