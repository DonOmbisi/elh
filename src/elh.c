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
#ifdef __AVX2__
#include <immintrin.h>
#endif

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

static inline int elh_put(elh_htable_t* ht, U32 seq, U32 pos, int k) {
    U32 h1 = elh_hash_a1(seq);
    U32 b  = h1 * ELH_BUCKET_MAX;
    U32 evicted = ht->a1[b + k - 1];
    if (k >= 4) ht->a1[b+3] = ht->a1[b+2];
    if (k >= 3) ht->a1[b+2] = ht->a1[b+1];
    if (k >= 2) ht->a1[b+1] = ht->a1[b+0];
    ht->a1[b] = pos;
    if (evicted != 0) {
        ht->a2[elh_hash_a2(seq)] = evicted;
        return 1;  /* eviction occurred — bucket was full */
    }
    return 0;  /* no eviction — bucket had free slot */
}

static inline U32 elh_get(const elh_htable_t* ht, U32 seq,
                           const U8* base, const U8* ip,
                           const U8* mlimit, U32 curPos,
                           int k, int ovf, U32 window)
{
    U32 h1   = elh_hash_a1(seq);
    U32 b    = h1 * ELH_BUCKET_MAX;
    U32 best = 0, blen = 0;

#ifdef __AVX2__
    /* SIMD path: load all 4 candidates, check distances in parallel */
    if (k == ELH_BUCKET_MAX) {
        /* Load 4 candidate positions as 128-bit vector */
        __m128i cands  = _mm_loadu_si128((const __m128i*)(ht->a1 + b));
        __m128i vcur   = _mm_set1_epi32((int)curPos);
        __m128i vmax   = _mm_set1_epi32((int)ELH_DISTANCE_MAX);
        __m128i vzero  = _mm_setzero_si128();

        /* Compute distances: curPos - pos for each candidate */
        __m128i dists  = _mm_sub_epi32(vcur, cands);

        /* Valid mask: pos != 0 AND dist <= DISTANCE_MAX */
        __m128i nonzero = _mm_cmpgt_epi32(cands, vzero);
        __m128i inrange = _mm_cmpgt_epi32(vmax, _mm_sub_epi32(dists,
                          _mm_set1_epi32(1)));  /* dist-1 < max → dist <= max */
        __m128i valid   = _mm_and_si128(nonzero, inrange);

        /* Apply window mask if set */
        if (window) {
            __m128i vwin  = _mm_set1_epi32((int)window);
            __m128i winok = _mm_cmpgt_epi32(vwin, _mm_sub_epi32(dists,
                            _mm_set1_epi32(1)));
            valid = _mm_and_si128(valid, winok);
        }

        int mask = _mm_movemask_epi8(valid);  /* 4 bits set per valid lane */

        /* Check each valid candidate */
        U32 slots[4];
        _mm_storeu_si128((__m128i*)slots, cands);

        int i;
        for (i = 0; i < 4; i++) {
            if (!(mask & (0xF << (i*4)))) continue;
            U32 pos = slots[i];
            const U8* m = base + pos;
            if (elh_read32(m) != elh_read32(ip)) continue;
            const U8* a = ip+4, *c = m+4;
            while (a < mlimit && *a == *c) { a++; c++; }
            U32 len = (U32)(a - ip);
            if (len > blen) { blen = len; best = pos; }
        }
    } else {
#endif
    /* Scalar path: used when k < ELH_BUCKET_MAX or no AVX2 */
    {
        int i;
        for (i = 0; i < k; i++) {
            U32 pos = ht->a1[b+i];
            if (!pos) continue;
            if (curPos - pos > ELH_DISTANCE_MAX) continue;
            if (window && curPos - pos > window) continue;
            const U8* m = base + pos;
            if (elh_read32(m) != elh_read32(ip)) continue;
            const U8* a = ip+4, *c = m+4;
            while (a < mlimit && *a == *c) { a++; c++; }
            U32 len = (U32)(a - ip);
            if (len > blen) { blen = len; best = pos; }
        }
    }
#ifdef __AVX2__
    }
#endif

    if (!best && ovf) {
        U32 pos = ht->a2[elh_hash_a2(seq)];
        if (pos && curPos - pos <= ELH_DISTANCE_MAX &&
            (!window || curPos - pos <= window) &&
            elh_read32(base+pos) == elh_read32(ip)) best = pos;
    }
    return best;
}

/* Adaptive k constants */
#define ELH_ADAPT_WINDOW    1024   /* measure eviction rate every N inserts */
#define ELH_ADAPT_UP        768    /* 75% eviction rate -> increase k */
#define ELH_ADAPT_DOWN      256    /* 25% eviction rate -> decrease k */

/* Adaptive k constants */
#define ELH_ADAPT_WINDOW    1024   /* measure eviction rate every N inserts */
#define ELH_ADAPT_UP        768    /* 75% eviction rate -> increase k */
#define ELH_ADAPT_DOWN      256    /* 25% eviction rate -> decrease k */

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

    int k_init = params.bucket_k < 1 ? 1 :
               params.bucket_k > ELH_BUCKET_MAX ? ELH_BUCKET_MAX : params.bucket_k;
    int ovf    = params.use_overflow;
    int acc    = params.acceleration < 1 ? 1 : params.acceleration;
    U32 window = (params.window_size <= 0 || params.window_size > ELH_DISTANCE_MAX)
                 ? 0 : (U32)params.window_size;

    /* Adaptive k state — elastic hashing batch protocol */
    int k           = k_init;          /* current probe depth */
    int adapt_count = 0;               /* inserts since last adaptation */
    int evict_count = 0;               /* evictions in current window */
    const int do_adapt = (params.bucket_k == 0); /* 0 = fully adaptive mode */

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
            U32 pos = elh_get(ht, seq, base, search, mlimit, cur, k, ovf, window);
            evict_count += elh_put(ht, seq, cur, k);
            if (do_adapt && ++adapt_count >= ELH_ADAPT_WINDOW) {
                /* Elastic hashing batch adaptation:
                 * High eviction rate = table dense = increase k for better history
                 * Low eviction rate  = table sparse = decrease k to save time
                 * No evictions at all = incompressible data = stop adapting */
                if (evict_count == 0) {
                    /* Table never fills — incompressible data.
                     * Lock k=1 and disable further adaptation. */
                    k = 1;
                } else {
                    if (evict_count > ELH_ADAPT_UP && k < ELH_BUCKET_MAX)
                        k++;
                    else if (evict_count < ELH_ADAPT_DOWN && k > 1)
                        k--;
                }
                adapt_count = 0;
                evict_count = 0;
            }
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
                evict_count += elh_put(ht, elh_read32(hp), pp, k);
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
/* ═══════════════════════════════════════════════════════════
 * Streaming API implementation
 * ═══════════════════════════════════════════════════════════ */

struct elh_stream_s {
    elh_htable_t* ht;          /* persistent hash table */
    elh_params_t  params;      /* compression parameters */
    U32           curPos;      /* absolute position counter */
    /* Compressor: ring buffer of last 65536 input bytes for cross-chunk matches */
    U8            window_buf[65536];
    U32           wfill;       /* bytes filled so far (caps at 65536) */
    /* Decompressor history: last 64KB of output for cross-chunk matches */
    U8            history[65536];
    U32           histPos;     /* write position in history ring buffer */
    U32           histFill;    /* bytes written so far (caps at 65536) */
};

elh_stream_t* elh_stream_new(elh_params_t params) {
    elh_stream_t* s = (elh_stream_t*)calloc(1, sizeof(elh_stream_t));
    if (!s) return NULL;
    s->ht = (elh_htable_t*)calloc(1, sizeof(elh_htable_t));
    if (!s->ht) { free(s); return NULL; }
    s->params  = params;
    s->curPos  = 0;
    s->histPos = 0;
    s->histFill = 0;
    return s;
}

void elh_stream_free(elh_stream_t* s) {
    if (!s) return;
    free(s->ht);
    free(s);
}

void elh_stream_reset(elh_stream_t* s) {
    if (!s) return;
    memset(s->ht, 0, sizeof(elh_htable_t));
    s->curPos   = 0;
    s->histPos  = 0;
    s->histFill = 0;
}

int elh_stream_bound(int srcSize) {
    return elh_compress_bound(srcSize);
}

int elh_stream_compress(elh_stream_t* s,
                        const void* src, int srcSize,
                        void* dst, int dstCapacity)
{
    if (!s || srcSize <= 0 || !src || !dst) return -1;
    if (dstCapacity < elh_stream_bound(srcSize)) return -1;

    int k   = s->params.bucket_k < 1 ? 1 :
              s->params.bucket_k > ELH_BUCKET_MAX ? ELH_BUCKET_MAX :
              s->params.bucket_k;
    int ovf = s->params.use_overflow;
    int acc = s->params.acceleration < 1 ? 1 : s->params.acceleration;
    U32 wlim = (s->params.window_size <= 0 ||
                s->params.window_size > ELH_DISTANCE_MAX)
               ? 0 : (U32)s->params.window_size;
    const int do_adapt = (s->params.bucket_k == 0);
    if (do_adapt) k = 1;
    int adapt_count = 0, evict_count = 0;

    const U8* src8  = (const U8*)src;
    U8*  op   = (U8*)dst;
    U8*  oend = op + dstCapacity;
    U32  cstart = s->curPos;  /* stream position of src8[0] */

    /* Copy chunk into ring buffer so previous-chunk matches remain accessible */
    for (int i = 0; i < srcSize; i++)
        s->window_buf[(cstart + i) & 65535] = src8[i];
    s->wfill = (cstart + srcSize) < 65536 ? (cstart + srcSize) : 65536;

#define WB(P)     (s->window_buf[(P) & 65535])
#define WR32(P)   ((U32)WB(P)|((U32)WB((P)+1)<<8)|((U32)WB((P)+2)<<16)|((U32)WB((P)+3)<<24))
#define SPOS(ptr) (cstart + (U32)((ptr) - src8))

    const U8* anchor  = src8;
    const U8* ip      = src8;
    const U8* iend    = src8 + srcSize;
    const U8* mflimit = iend - ELH_MFLIMIT;
    const U8* mlimit  = iend - ELH_LASTLITERALS;

    if (srcSize < ELH_MINMATCH + 1) goto _last;
    ip++;

    for (;;) {
        const U8* search = ip;
        U32 match_spos = 0;
        U32 match_len  = 0;
        int skips = 0;

        /* Search for a match */
        while (search <= mflimit) {
            U32 seq = elh_read32(search);
            U32 cur = SPOS(search);
            U32 h1  = elh_hash_a1(seq);
            U32 b   = h1 * ELH_BUCKET_MAX;
            U32 bestlen = 0, bestpos = 0;
            int j;

            for (j = 0; j < k; j++) {
                U32 p = s->ht->a1[b+j];
                if (!p || cur - p > ELH_DISTANCE_MAX) continue;
                if (wlim && cur - p > wlim) continue;
                if (WR32(p) != seq) continue;
                U32 ml = ELH_MINMATCH;
                while (search+ml < mlimit && ml < 65530 &&
                       src8[(search-src8)+ml] == WB(p+ml)) ml++;
                if (ml > bestlen) { bestlen = ml; bestpos = p; }
            }
            if (!bestpos && ovf) {
                U32 p = s->ht->a2[elh_hash_a2(seq)];
                if (p && cur-p <= ELH_DISTANCE_MAX &&
                    (!wlim || cur-p <= wlim) && WR32(p) == seq) {
                    bestpos = p; bestlen = ELH_MINMATCH;
                }
            }

            /* Insert */
            {
                U32 ev = s->ht->a1[b+k-1];
                if (k>=4) s->ht->a1[b+3]=s->ht->a1[b+2];
                if (k>=3) s->ht->a1[b+2]=s->ht->a1[b+1];
                if (k>=2) s->ht->a1[b+1]=s->ht->a1[b+0];
                s->ht->a1[b] = cur;
                if (ev) { s->ht->a2[elh_hash_a2(seq)]=ev; evict_count++; }
            }

            if (bestpos) {
                ip = search; match_spos = bestpos; match_len = bestlen;
                goto _found;
            }
            if (do_adapt && ++adapt_count >= ELH_ADAPT_WINDOW) {
                if (!evict_count) k=1;
                else if (evict_count > ELH_ADAPT_UP && k < ELH_BUCKET_MAX) k++;
                else if (evict_count < ELH_ADAPT_DOWN && k > 1) k--;
                adapt_count = 0; evict_count = 0;
            }
            skips++;
            search += 1 + (skips >> (acc+2));
        }
        goto _last;

    _found:;
        /* Extend backward */
        {
            U32 ip_s = SPOS(ip);
            while (ip > anchor && ip_s > cstart &&
                   match_spos > 0 && ip[-1] == WB(match_spos-1)) {
                ip--; ip_s--; match_spos--;
            }
            /* Re-measure match length from adjusted ip */
            match_len = ELH_MINMATCH;
            while ((ip+match_len) < mlimit &&
                   src8[(ip-src8)+match_len] == WB(match_spos+match_len))
                match_len++;
        }

        /* Emit token + literals + offset + match length */
        {
            U32 litLen  = (U32)(ip - anchor);
            U32 ip_s    = SPOS(ip);
            U32 offset  = ip_s - match_spos;
            U32 mlCode  = match_len - ELH_MINMATCH;
            U8* token   = op++;

            if (op + litLen + (litLen/255) + 10 > oend) return -1;

            if (litLen >= ELH_RUN_MASK) {
                *token = (U8)(ELH_RUN_MASK << 4);
                U32 r = litLen - ELH_RUN_MASK;
                while (r >= 255) { *op++ = 255; r -= 255; }
                *op++ = (U8)r;
            } else {
                *token = (U8)(litLen << 4);
            }
            memcpy(op, anchor, litLen); op += litLen;

            elh_write16(op, (U16)offset); op += 2;

            if (mlCode < ELH_ML_MASK) {
                *token |= (U8)mlCode;
            } else {
                *token |= ELH_ML_MASK;
                U32 r = mlCode - ELH_ML_MASK;
                if (op + (r/255) + 2 > oend) return -1;
                while (r >= 255) { *op++ = 255; r -= 255; }
                *op++ = (U8)r;
            }

            ip     += match_len;
            anchor  = ip;

            /* Hash match interior */
            const U8* hp = ip - match_len + 2;
            while (hp < ip-1 && hp <= mflimit) {
                U32 seq2 = elh_read32(hp);
                U32 cur2 = SPOS(hp);
                U32 h2   = elh_hash_a1(seq2);
                U32 b2   = h2 * ELH_BUCKET_MAX;
                U32 ev2  = s->ht->a1[b2+k-1];
                if (k>=4) s->ht->a1[b2+3]=s->ht->a1[b2+2];
                if (k>=3) s->ht->a1[b2+2]=s->ht->a1[b2+1];
                if (k>=2) s->ht->a1[b2+1]=s->ht->a1[b2+0];
                s->ht->a1[b2] = cur2;
                if (ev2) s->ht->a2[elh_hash_a2(seq2)] = ev2;
                hp++;
            }

            if (ip > mflimit) goto _last;
        }
    }

_last:;
    {
        U32 litLen = (U32)(iend - anchor);
        if (op + litLen + (litLen/255) + 2 > oend) return -1;
        U8* token = op++;
        if (litLen >= ELH_RUN_MASK) {
            *token = (U8)(ELH_RUN_MASK << 4);
            U32 r = litLen - ELH_RUN_MASK;
            while (r >= 255) { *op++ = 255; r -= 255; }
            *op++ = (U8)r;
        } else {
            *token = (U8)(litLen << 4);
        }
        memcpy(op, anchor, litLen); op += litLen;
    }

#undef WB
#undef WR32
#undef SPOS

    s->curPos += (U32)srcSize;
    return (int)(op - (U8*)dst);
}

int elh_stream_decompress(elh_stream_t* s,
                          const void* src, int srcSize,
                          void* dst, int dstCapacity)
{
    if (!s || !src || !dst) return -1;

    const U8* ip   = (const U8*)src;
    const U8* iend = ip + srcSize;
    U8* op   = (U8*)dst;
    U8* op0  = op;
    U8* oend = op + dstCapacity;

    while (ip < iend) {
        U8  token  = *ip++;
        U32 litLen = token >> 4;
        if (litLen == ELH_RUN_MASK) {
            U8 sv; do { sv = *ip++; litLen += sv; } while (sv == 255 && ip < iend);
        }
        if (op + litLen > oend || ip + litLen > iend) return -1;
        memcpy(op, ip, litLen); op += litLen; ip += litLen;
        if (ip >= iend) break;
        if (ip + 2 > iend) return -1;
        U16 offset; memcpy(&offset, ip, 2); ip += 2;
        if (!offset) return -1;

        U32 matchLen = (token & ELH_ML_MASK) + ELH_MINMATCH;
        if ((token & ELH_ML_MASK) == ELH_ML_MASK) {
            U8 sv; do { sv = *ip++; matchLen += sv; } while (sv == 255 && ip < iend);
        }
        if (op + matchLen > oend) return -1;

        U32 cur_out = (U32)(op - op0);
        if (offset <= cur_out) {
            /* Match entirely within current chunk */
            const U8* match = op - offset;
            U32 i; for (i = 0; i < matchLen; i++) op[i] = match[i];
        } else {
            /* Match spans into history from previous chunks */
            U32 back_in_history = offset - cur_out;
            if (back_in_history > s->histFill) return -1;
            U32 hist_idx = s->histFill - back_in_history;
            U32 i;
            for (i = 0; i < matchLen; i++) {
                U32 idx = hist_idx + i;
                if (idx < s->histFill) {
                    op[i] = s->history[idx];
                } else {
                    /* Crossed into current chunk output */
                    op[i] = op0[idx - s->histFill];
                }
            }
        }
        op += matchLen;
    }

    /* Update history: keep last 65536 bytes of all output so far */
    U32 outLen = (U32)(op - op0);
    if (s->histFill + outLen <= 65536) {
        memcpy(s->history + s->histFill, op0, outLen);
        s->histFill += outLen;
    } else if (outLen >= 65536) {
        memcpy(s->history, op0 + outLen - 65536, 65536);
        s->histFill = 65536;
    } else {
        U32 keep = 65536 - outLen;
        memmove(s->history, s->history + (s->histFill - keep), keep);
        memcpy(s->history + keep, op0, outLen);
        s->histFill = 65536;
    }

    return (int)(op - op0);
}

/* ── Block decompressor ─────────────────────────────────── */
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
            U8 sv; do { sv = *ip++; litLen += sv; } while (sv == 255 && ip < iend);
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
            U8 sv; do { sv = *ip++; matchLen += sv; } while (sv == 255 && ip < iend);
        }
        if (op + matchLen > oend) return -1;
        U32 i; for (i = 0; i < matchLen; i++) op[i] = match[i];
        op += matchLen;
    }
    return (int)(op - op0);
}
