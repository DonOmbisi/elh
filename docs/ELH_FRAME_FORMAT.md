# ELH Frame Format

ELH frames are self-describing containers for storing or transmitting ELH-compressed data.
The raw `elh_compress()` API emits one LZ4-compatible block and requires the caller to
track the original size externally. The frame API adds metadata, chunking, and raw fallback.

All integer fields are little-endian.

## Header

The frame starts with a fixed 40-byte header:

| Offset | Size | Field | Description |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `ELH1` |
| 4 | 1 | version | `1` |
| 5 | 1 | frame_flags | Reserved, currently `0` |
| 6 | 2 | header_size | `40` |
| 8 | 8 | original_size | Total uncompressed byte size |
| 16 | 4 | chunk_size | Configured chunk size, max `65536` |
| 20 | 4 | bucket_k | ELH bucket depth: `1`, `2`, `4`, or `0` adaptive |
| 24 | 4 | use_overflow | `0` or `1` |
| 28 | 4 | acceleration | Compression acceleration parameter |
| 32 | 4 | window_size | `0` means default max distance |
| 36 | 4 | use_wide_offsets | Reserved frame metadata for wide-offset experiments |

## Chunks

After the header, the frame stores zero or more chunks. Chunks continue until
`original_size` bytes have been reconstructed. Empty inputs contain only the 40-byte header.

Each chunk has a 12-byte chunk header:

| Offset | Size | Field | Description |
|---:|---:|---|---|
| 0 | 4 | raw_size | Uncompressed chunk size |
| 4 | 4 | payload_size | Stored payload byte size |
| 8 | 1 | chunk_flags | Bit 0 set means payload is raw/uncompressed |
| 9 | 3 | reserved | Must be zero |

If `chunk_flags & 1` is set, the payload is copied directly and `payload_size` must equal
`raw_size`. Otherwise the payload is a raw ELH/LZ4-style block and is decompressed with
`elh_decompress(payload, payload_size, dst, raw_size)`.

## Raw Fallback

The default frame compressor stores a chunk raw when compression expands that chunk. This
keeps frames robust for high-entropy inputs and bounds worst-case expansion. Raw fallback can
be disabled with `elh_frame_params_t.store_uncompressed = 0` or `elh_cli --no-raw`.

## Compatibility Rules

Decoders must reject:

- Unknown magic or version.
- Header sizes other than `40`.
- Non-zero reserved chunk bytes.
- Raw chunks where `payload_size != raw_size`.
- Chunks larger than `65536` bytes.
- Trailing bytes after the declared `original_size` has been reconstructed.

The current frame format is chunked/block-oriented. It does not use cross-chunk streaming
history; use the lower-level streaming API when that behavior is required.
