# ELH - Elastic Hashing Lossless Compression

ELH is a fast experimental lossless compression library that improves
LZ4-style match finding by retaining multiple candidates per hash slot. It is
motivated by the elastic hashing construction of Farach-Colton, Krapivin, and
Kuszmaul (2025).

## Key Results

### Block Compression (Silesia Corpus)

| File | LZ4 | ELH k=1 | ELH k=2 | ELH k=4 | Gain vs LZ4 |
|---|---:|---:|---:|---:|---:|
| dickens | 76.17% | 63.34% | 58.99% | 55.73% | +20.4% |
| webster | 56.49% | 49.16% | 44.95% | 42.14% | +14.4% |
| xml | 25.92% | 28.49% | 22.80% | 20.23% | +5.7% |
| osdb | 77.55% | 52.71% | 44.06% | 42.23% | +35.3% |
| reymont | 54.60% | 53.30% | 49.25% | 45.59% | +9.0% |
| samba | 40.67% | 36.72% | 34.52% | 33.01% | +7.7% |

Lower ratio is better. ELH k=4 beats LZ4 on these files. The extra work is on
the compression side; decompression remains LZ4-style.

### Streaming Compression (LLM Inference Logs)

| Lines | Raw KB | Per-line block | ELH streaming | Gain |
|---:|---:|---:|---:|---:|
| 100 | 12.7 | 99.2% | 17.9% | +81% |
| 1,000 | 127.6 | 99.9% | 15.5% | +84% |
| 10,000 | 1,278.6 | 100.6% | 15.2% | +85% |
| 100,000 | 12,785.8 | 101.4% | 15.2% | +86% |

Per-line block compression is ineffective for short structured log lines.
ELH streaming accumulates cross-line history and recovers repeated JSON fields,
model names, timestamps, and token strings.

## How It Works

ELH replaces the direct-mapped hash table used in LZ4 with a two-level elastic
bucket structure:

- A1: primary bucket table, k slots per hash bucket.
- A2: overflow table for evicted A1 entries.
- Sliding window: candidates are retained across the input.
- Optional AVX2: parallel distance checks in the block compressor.

Each hash position stores the k most recent match candidates. The compressor
tests the retained candidates and emits the best match. The compressed block
format remains LZ4-compatible for the block API.

## APIs

### Frame API

The frame API is the recommended integration surface for other projects. It
wraps ELH blocks in a self-describing container with magic/version metadata,
original size, chunk size, compression parameters, per-chunk sizes, and raw
fallback for incompressible chunks.

```c
#include "elh_frame.h"

elh_frame_params_t params = ELH_FRAME_PARAMS_DEFAULT;

int bound = elh_frame_compress_bound(srcSize, params.chunk_size);
char* frame = malloc(bound);
int frameSize = elh_frame_compress(src, srcSize, frame, bound, params);

int originalSize = elh_frame_get_original_size(frame, frameSize);
char* restored = malloc(originalSize);
int decodedSize = elh_frame_decompress(frame, frameSize,
                                       restored, originalSize);
```

See `docs/ELH_FRAME_FORMAT.md` for the binary layout and
`examples/frame_roundtrip.c` for a complete C example.

### CLI

`elh_cli` provides a script-friendly compression pipeline using the frame
format:

```bash
elh_cli -c input.log input.elh
elh_cli -d input.elh restored.log
elh_cli -c --chunk 4096 -k 4 --overflow 1 input.log input.elh
```

### Python

The lightweight Python binding uses `ctypes` over the shared frame library:

```python
import elh

frame = elh.compress(b"hello hello hello", chunk_size=4096)
restored = elh.decompress(frame)
assert restored == b"hello hello hello"
```

From a source checkout:

```bash
cmake --build build
PYTHONPATH=python ELH_LIBRARY=build/libelh.so python python/test_elh.py
```

### Block API

The block API emits a raw LZ4-compatible block. Callers must track the original
size out-of-band.

```c
#include "elh.h"

elh_params_t params = ELH_PARAMS_DEFAULT;

int bound = elh_compress_bound(srcSize);
char* dst = malloc(bound);
int compressedSize = elh_compress(src, srcSize, dst, bound, params);
int originalSize = elh_decompress(dst, compressedSize, out, outCapacity);
```

### Streaming API

The streaming API compresses chunks incrementally against retained history.
Matches span chunk boundaries up to 65535 bytes back.

```c
#include "elh.h"

elh_params_t params = ELH_PARAMS_MAX;
elh_stream_t* cs = elh_stream_new(params);
elh_stream_t* ds = elh_stream_new(params);

int clen = elh_stream_compress(cs, chunk, chunkLen, cbuf, cbufCap);
int dlen = elh_stream_decompress(ds, cbuf, clen, out, chunkLen);

elh_stream_free(cs);
elh_stream_free(ds);
```

## Presets

```c
elh_params_t fast     = ELH_PARAMS_FAST;
elh_params_t balanced = {2, 1, 1, 0, 0};
elh_params_t max      = ELH_PARAMS_MAX;
elh_params_t adaptive = ELH_PARAMS_ADAPTIVE;
```

## Building

```bash
mkdir build && cd build
cmake ..
make
```

Single-file-style embedding with the frame API:

```bash
gcc -O2 -mavx2 -Iinclude src/elh.c src/elh_frame.c your_program.c -o your_program
```

## Recommended Use Cases

Frame/block API:

- Storage systems where compressed size matters more than write speed.
- Network transfer of repetitive structured data such as logs or dumps.
- LZ4-like deployments where a compression-speed reduction is acceptable for
  better ratio.

Streaming API:

- LLM inference log compression.
- Structured event streams with repeated fields across adjacent records.
- Log aggregation pipelines where per-message compression loses cross-message
  redundancy.

## Not Recommended For

- Latency-critical compression paths where stock LZ4 compression speed is required.
- Near-random binary data, encrypted data, and already-compressed media.
- KV-cache-style chunks at or above the 65535-byte window when cross-chunk matches
  are required.

## Known Behavior

- ELH k=1 can be worse than LZ4 on some XML-like inputs. Use k=2 or k=4 for
  better compression.
- Streaming SIMD did not improve speed in experiments because the bottleneck is
  ring-buffer history reads.
- The current frame format is chunked/block-oriented and does not use cross-chunk
  streaming history.

## Research Paper

The accompanying paper describes the elastic hashing motivation, experimental
phases, and benchmark results.

## License

BSD 2-Clause. See `LICENSE`.
