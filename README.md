# ELH — Elastic Hashing Lossless Compression

ELH is a fast lossless compression library implementing a two-level elastic
bucket hash table, motivated by the elastic hashing construction of
Farach-Colton, Krapivin, and Kuszmaul (2025).

## Key Results (Silesia Corpus)

| File     | LZ4    | ELH k=1 | ELH k=2 | ELH k=4 | Gain vs LZ4 |
|----------|--------|---------|---------|---------|-------------|
| dickens  | 76.17% | 63.34%  | 58.99%  | 55.73%  | +20.4%      |
| webster  | 56.49% | 49.16%  | 44.95%  | 42.14%  | +14.4%      |
| xml      | 25.92% | 28.49%  | 22.80%  | 20.23%  | +5.7%       |
| osdb     | 77.55% | 52.71%  | 44.06%  | 42.23%  | +35.3%      |
| reymont  | 54.60% | 53.30%  | 49.25%  | 45.59%  | +9.0%       |
| samba    | 40.67% | 36.72%  | 34.52%  | 33.01%  | +7.7%       |

Lower ratio = better compression. ELH k=4 beats LZ4 baseline on all files.
Decompression speed: 470–1440 MB/s. Compression speed: 100–310 MB/s.

## How It Works

ELH replaces the direct-mapped hash table used in LZ4 with a two-level
elastic bucket structure:

- **A1**: primary bucket table, k slots per hash bucket (k=1,2,4)
- **A2**: overflow table, receives genuine evictions from A1
- **Sliding window**: hash table persists across the full input

Each hash position stores the k most recent match candidates. When A1's
oldest slot is evicted, it moves to A2 for overflow recovery. This directly
implements the collision history retention insight from elastic hashing theory.

## API

```c
#include "elh.h"

/* Configure compression */
elh_params_t params = {
    .bucket_k    = 4,  /* 1, 2, or 4 — higher = better ratio, slower */
    .use_overflow = 1, /* enable A2 overflow table */
    .acceleration = 1  /* 1-9, higher = faster but worse ratio */
};

/* Compress */
int bound = elh_compress_bound(srcSize);
char* dst = malloc(bound);
int compressedSize = elh_compress(src, srcSize, dst, bound, params);

/* Decompress */
int originalSize = elh_decompress(dst, compressedSize, out, outCapacity);
```

## Presets

```c
elh_params_t fast    = {1, 0, 1};  /* ~LZ4 k=4 ratio, fastest */
elh_params_t balanced = {2, 1, 1}; /* 5-20% better than LZ4, moderate speed */
elh_params_t max     = {4, 1, 1};  /* 7-35% better than LZ4, slowest */
```

## Building

```bash
mkdir build && cd build
cmake ..
make
```

Or single-file:
```bash
gcc -O2 -Iinclude src/elh.c your_program.c -o your_program
```

## Recommended Use Cases

- **Storage systems** where compressed size matters more than write speed
- **Network transfer** of repetitive structured data (logs, database dumps)
- **KV cache compression** in LLM inference (fast decompress, good ratio)
- Any workload currently using LZ4 where ratio improvement justifies ~50% speed cost

## Not Recommended For

- Latency-critical paths where LZ4's 400-600 MB/s is required
- Near-random binary data (executables, encrypted data, images)
- Real-time compression where compression speed matters

## Known Issues

- ELH k=1 performs slightly worse than LZ4 on xml (28.49% vs 25.92%) due to
  different hash resolution. ELH allocates 4 slots per bucket regardless of k,
  giving fewer buckets at k=1 than LZ4's direct-mapped table. Use k=2 or k=4
  for better results — ELH k=4 beats LZ4 on xml (20.23% vs 25.92%).
- A tunable window_size parameter is available in elh_params_t for workloads
  where limiting match distance improves performance.
- Compression speed is 35-65% slower than LZ4 baseline due to larger hash table
  (128KB vs 64KB). SIMD bucket scan optimisation is planned.

## Theoretical Background

Motivated by: Farach-Colton, M., Krapivin, A., and Kuszmaul, W. (2025).
*Optimal Bounds for Open Addressing Without Reordering*. arXiv:2501.02305v2.

The elastic hashing construction achieves O(1) amortized probe complexity by
retaining collision history — precisely what bucket slots and the A2 overflow
table implement in ELH. See the accompanying research paper for full analysis.

## License

BSD 2-Clause. See LICENSE.
