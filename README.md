# ELH — Elastic Hashing Lossless Compression

ELH is a fast lossless compression library implementing a two-level elastic
bucket hash table, motivated by the elastic hashing construction of
Farach-Colton, Krapivin, and Kuszmaul (2025).

## Key Results

### Block Compression (Silesia Corpus)

| File     | LZ4    | ELH k=1 | ELH k=2 | ELH k=4 | Gain vs LZ4 |
|----------|--------|---------|---------|---------|-------------|
| dickens  | 76.17% | 63.34%  | 58.99%  | 55.73%  | +20.4%      |
| webster  | 56.49% | 49.16%  | 44.95%  | 42.14%  | +14.4%      |
| xml      | 25.92% | 28.49%  | 22.80%  | 20.23%  | +5.7%       |
| osdb     | 77.55% | 52.71%  | 44.06%  | 42.23%  | +35.3%      |
| reymont  | 54.60% | 53.30%  | 49.25%  | 45.59%  | +9.0%       |
| samba    | 40.67% | 36.72%  | 34.52%  | 33.01%  | +7.7%       |

Lower ratio = better compression. ELH k=4 beats LZ4 on all files.
Decompression speed: 470–1440 MB/s. Compression speed: 100–310 MB/s.

### Streaming Compression (LLM Inference Logs)

| Lines   | Raw KB   | Per-line block | ELH streaming | Gain   |
|---------|----------|---------------|---------------|--------|
| 100     | 12.7     | 99.2%         | 17.9%         | +81%   |
| 1,000   | 127.6    | 99.9%         | 15.5%         | +84%   |
| 10,000  | 1,278.6  | 100.6%        | 15.2%         | +85%   |
| 100,000 | 12,785.8 | 101.4%        | 15.2%         | +86%   |

Per-line block compression (LZ4 style) is useless for short log lines —
each 129-byte line is too short to find internal matches. ELH streaming
accumulates cross-line history and achieves 15% ratio across 100K lines.

## How It Works

ELH replaces the direct-mapped hash table used in LZ4 with a two-level
elastic bucket structure:

- **A1**: primary bucket table, k slots per hash bucket (k=1,2,4)
- **A2**: overflow table, receives genuine evictions from A1
- **Sliding window**: hash table persists across the full input
- **AVX2 SIMD**: parallel distance check across all k candidates

Each hash position stores the k most recent match candidates. When A1's
oldest slot is evicted, it moves to A2 for overflow recovery. This directly
implements the collision history retention insight from elastic hashing theory.

## API

### Block API

```c
#include "elh.h"

elh_params_t params = {
    .bucket_k     = 4,  /* 1, 2, or 4 — higher = better ratio, slower */
    .use_overflow = 1,  /* enable A2 overflow table */
    .acceleration = 1,  /* 1-9, higher = faster, worse ratio */
    .window_size  = 0   /* 0 = max (65535) */
};

int bound = elh_compress_bound(srcSize);
char* dst = malloc(bound);
int compressedSize = elh_compress(src, srcSize, dst, bound, params);
int originalSize   = elh_decompress(dst, compressedSize, out, outCapacity);
```

### Streaming API

Compresses data incrementally. Each chunk is compressed against the full
history of all previous chunks. Matches span chunk boundaries up to 65535
bytes back. Ideal for log compression, streaming pipelines, and any
workload where adjacent chunks share structure.

```c
#include "elh.h"

elh_params_t params = {4, 1, 1, 0};

/* Create streaming state */
elh_stream_t* cs = elh_stream_new(params);
elh_stream_t* ds = elh_stream_new(params);

/* Compress line by line */
while (has_more_data) {
    int clen = elh_stream_compress(cs, line, lineLen,
                                   cbuf, sizeof(cbuf));
    /* store clen bytes of cbuf */
}

/* Decompress chunk by chunk */
for each stored chunk:
    int dlen = elh_stream_decompress(ds, cbuf, clen,
                                     out, origLen);

elh_stream_free(cs);
elh_stream_free(ds);
```

## Presets

```c
elh_params_t fast     = {1, 0, 1, 0};  /* fastest, ~LZ4 k=4 ratio */
elh_params_t balanced = {2, 1, 1, 0};  /* 5-20% better than LZ4    */
elh_params_t max      = {4, 1, 1, 0};  /* 7-35% better than LZ4    */
elh_params_t adaptive = {0, 1, 1, 0};  /* eviction-adaptive k      */
```

## Building

```bash
mkdir build && cd build
cmake ..
make
```

Or single-file:
```bash
gcc -O2 -mavx2 -Iinclude src/elh.c your_program.c -o your_program
```

## Recommended Use Cases

**Block API:**
- Storage systems where compressed size matters more than write speed
- Network transfer of repetitive structured data (logs, database dumps)
- Any workload currently using LZ4 where 10-35% size reduction justifies
  ~50% compression speed cost

**Streaming API:**
- LLM inference log compression (86% improvement over per-line LZ4)
- Any streaming text data with repetitive structure across chunks
- Log aggregation pipelines (Kafka, ClickHouse) where per-message
  compression loses cross-message redundancy

## Not Recommended For

- Latency-critical paths where LZ4's 400-600 MB/s is required
- Near-random binary data (executables, encrypted data, images)
- KV cache compression where chunk size ≥ 64KB (exceeds window limit)

## Known Behaviour

- ELH k=1 performs slightly worse than LZ4 on xml (28.49% vs 25.92%)
  due to different hash resolution at k=1. Use k=2 or k=4 for better
  results — ELH k=4 beats LZ4 on xml (20.23% vs 25.92%).
- Streaming SIMD (AVX2) does not improve speed because the bottleneck
  is the GETB ring-buffer read, not the distance check. AVX2 is used
  in the block compressor's elh_get function.
- Streaming window is capped at 65535 bytes (LZ4 format constraint).
  Chunks larger than 65535 bytes see no cross-chunk benefit.

## Theoretical Background

Motivated by: Farach-Colton, M., Krapivin, A., and Kuszmaul, W. (2025).
*Optimal Bounds for Open Addressing Without Reordering*. arXiv:2501.02305v2.

The elastic hashing construction achieves O(1) amortized probe complexity
by retaining collision history. ELH implements this insight in compression:
retaining k match candidates per hash slot improves compression ratio at
the cost of k compression-time probes, with decompression unaffected.

See the accompanying research paper for full analysis across five
experimental phases including two honest negative results (adaptive
probing and adaptive k) and cross-compressor validation on zstd.

## Research Paper

arXiv preprint: coming soon
TICON Africa 2026 submission: September 2026, Livingstone, Zambia

## License

BSD 2-Clause. See LICENSE.
