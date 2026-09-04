from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
import threading

from fastapi import FastAPI, HTTPException, Request, Response, Security
from fastapi.responses import FileResponse
from fastapi.security import APIKeyHeader
from fastapi.middleware.cors import CORSMiddleware


ROOT = Path(__file__).resolve().parents[1]
PYTHON_DIR = ROOT / "python"
STATIC_DIR = Path(__file__).resolve().parent / "static"
if str(PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(PYTHON_DIR))

import elh  # noqa: E402
import lz4_baseline  # noqa: E402


MAX_REQUEST_BYTES = int(os.environ.get("ELH_API_MAX_BYTES", str(10 * 1024 * 1024)))
DEFAULT_CHUNK_SIZE = int(os.environ.get("ELH_API_CHUNK_SIZE", str(elh.DEFAULT_CHUNK_SIZE)))
INGEST_DIR = Path(os.environ.get("ELH_INGEST_DIR", str(ROOT / "data" / "ingest")))
API_KEY = os.environ.get("ELH_API_KEY") or os.environ.get("ELH_INGEST_API_KEY") or ""
api_key_header = APIKeyHeader(name="X-API-Key", auto_error=False)

# Thread-safe lock for metadata.jsonl writes to prevent race conditions
_metadata_lock = threading.Lock()

# Simple in-memory rate limiter to prevent API abuse
class RateLimiter:
    def __init__(self, max_requests: int = 100, window_seconds: int = 60):
        self.max_requests = max_requests
        self.window_seconds = window_seconds
        self.requests = {}  # {client_id: [timestamp1, timestamp2, ...]}
        self.lock = threading.Lock()
    
    def is_allowed(self, client_id: str) -> bool:
        now = time.time()
        with self.lock:
            # Clean up old requests outside the time window
            if client_id in self.requests:
                self.requests[client_id] = [
                    ts for ts in self.requests[client_id]
                    if now - ts < self.window_seconds
                ]
            else:
                self.requests[client_id] = []
            
            # Check if under the limit
            if len(self.requests[client_id]) < self.max_requests:
                self.requests[client_id].append(now)
                return True
            return False

# Per-IP rate limiter (primary protection)
_per_ip_limiter = RateLimiter(max_requests=600, window_seconds=60)

# Global circuit-breaker rate limiter (backup protection)
_global_limiter = RateLimiter(max_requests=3000, window_seconds=60)

app = FastAPI(
    title="ELH Compression API",
    version="0.1.0",
    description="HTTP API for ELH frame compression and decompression.",
)

# Add CORS middleware for broader API accessibility
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # Configure appropriately for production
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Rate limiting middleware
@app.middleware("http")
async def rate_limit_middleware(request: Request, call_next):
    # Skip rate limiting for health endpoint
    if request.url.path == "/health":
        return await call_next(request)
    
    client_id = request.client.host if request.client else "unknown"
    
    # Primary protection: per-IP rate limiting
    if not _per_ip_limiter.is_allowed(client_id):
        raise HTTPException(status_code=429, detail="Rate limit exceeded. Maximum 600 requests per minute per IP.")
    
    # Backup protection: global circuit-breaker rate limiting
    if not _global_limiter.is_allowed("global"):
        raise HTTPException(status_code=429, detail="Global rate limit exceeded. Maximum 3000 requests per minute.")
    
    return await call_next(request)


@app.get("/", include_in_schema=False)
def index() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html")


def require_api_key(api_key: str | None = Security(api_key_header)) -> None:
    if not API_KEY:
        return
    if not api_key or not hmac.compare_digest(api_key, API_KEY):
        raise HTTPException(status_code=401, detail="invalid or missing API key")


async def read_limited_body(request: Request) -> bytes:
    content_length = request.headers.get("content-length")
    if content_length is not None:
        try:
            declared = int(content_length)
        except ValueError as exc:
            raise HTTPException(status_code=400, detail="invalid Content-Length") from exc
        if declared > MAX_REQUEST_BYTES:
            raise HTTPException(status_code=413, detail="request body too large")

    body = bytearray()
    async for chunk in request.stream():
        body.extend(chunk)
        if len(body) > MAX_REQUEST_BYTES:
            raise HTTPException(status_code=413, detail="request body too large")
    return bytes(body)


def parse_int_query(value: str | None, default: int, name: str) -> int:
    if value is None:
        return default
    try:
        return int(value)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=f"{name} must be an integer") from exc


def parse_int_list_query(
    value: str | None,
    default: list[int],
    name: str,
    *,
    max_items: int = 8,
) -> list[int]:
    if value is None or not value.strip():
        return default

    items: list[int] = []
    try:
        for part in value.split(","):
            part = part.strip()
            if not part:
                continue
            items.append(int(part))
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=f"{name} must be comma-separated integers") from exc

    if not items:
        raise HTTPException(status_code=400, detail=f"{name} cannot be empty")
    if len(items) > max_items:
        raise HTTPException(status_code=400, detail=f"{name} has too many values")
    return items


def positive_int(value: int, name: str) -> int:
    if value <= 0:
        raise HTTPException(status_code=400, detail=f"{name} must be positive")
    return value


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def ratio_percent(compressed_size: int, input_size: int) -> float:
    if input_size == 0:
        return 0.0
    return round((compressed_size / input_size) * 100.0, 2)


def bench_units(
    units: list[bytes],
    *,
    bucket_k: int,
    use_overflow: int,
    acceleration: int,
    chunk_size: int,
) -> dict[str, object]:
    def compress_unit(unit: bytes) -> bytes:
        return elh.compress(
            unit,
            bucket_k=bucket_k,
            use_overflow=use_overflow,
            acceleration=acceleration,
            chunk_size=chunk_size,
        )

    def decompress_unit(frame: bytes, original_size: int) -> bytes:
        _ = original_size
        return elh.decompress(frame)

    return bench_codec_units(units, compress_unit, decompress_unit)


def bench_lz4_units(units: list[bytes]) -> dict[str, object]:
    return bench_codec_units(units, lz4_baseline.compress, lz4_baseline.decompress)


def bench_codec_units(
    units: list[bytes],
    compress_unit,
    decompress_unit,
) -> dict[str, object]:
    started = time.perf_counter()
    frames = [compress_unit(unit) for unit in units]
    compressed_at = time.perf_counter()
    restored = b"".join(
        decompress_unit(frame, len(unit)) for frame, unit in zip(frames, units)
    )
    finished = time.perf_counter()

    input_size = sum(len(unit) for unit in units)
    compressed_size = sum(len(frame) for frame in frames)
    return {
        "units": len(units),
        "input_bytes": input_size,
        "compressed_bytes": compressed_size,
        "ratio_percent": ratio_percent(compressed_size, input_size),
        "compress_ms": round((compressed_at - started) * 1000.0, 3),
        "decompress_ms": round((finished - compressed_at) * 1000.0, 3),
        "verified": restored == b"".join(units),
    }


def fixed_batches(data: bytes, batch_size: int) -> list[bytes]:
    if not data:
        return [b""]
    return [data[i : i + batch_size] for i in range(0, len(data), batch_size)]


def records(data: bytes) -> list[bytes]:
    if not data:
        return [b""]
    split = data.splitlines(keepends=True)
    return split if split else [data]


def ingest_paths() -> tuple[Path, Path, Path]:
    root = INGEST_DIR
    batches = root / "batches"
    metadata = root / "metadata.jsonl"
    batches.mkdir(parents=True, exist_ok=True)
    return root, batches, metadata


def normalize_event_body(body: bytes, content_type: str | None) -> tuple[bytes, int, str]:
    content_type = content_type or ""
    should_try_json = "json" in content_type or body.strip().startswith((b"{", b"["))
    if should_try_json:
        try:
            parsed = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            parsed = None

        if isinstance(parsed, list):
            lines = [
                json.dumps(item, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
                for item in parsed
            ]
            return ("\n".join(lines) + ("\n" if lines else "")).encode("utf-8"), len(lines), "json"

        if isinstance(parsed, dict):
            line = json.dumps(parsed, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
            return (line + "\n").encode("utf-8"), 1, "json"

    line_count = len(body.splitlines()) if body else 0
    return body, line_count, "bytes"


def append_metadata(metadata: dict[str, object]) -> None:
    _, _, metadata_path = ingest_paths()
    with _metadata_lock:
        with metadata_path.open("a", encoding="utf-8") as f:
            f.write(json.dumps(metadata, sort_keys=True, separators=(",", ":")) + "\n")


def read_metadata(limit: int | None = None) -> list[dict[str, object]]:
    _, _, metadata_path = ingest_paths()
    if not metadata_path.exists():
        return []

    entries = []
    with metadata_path.open("r", encoding="utf-8") as f:
        for line in f:
            if line.strip():
                entries.append(json.loads(line))
    if limit is not None:
        entries = entries[-limit:]
    return entries


def find_metadata(batch_id: str) -> dict[str, object]:
    for item in read_metadata():
        if item.get("batch_id") == batch_id:
            return item
    raise HTTPException(status_code=404, detail="batch not found")


def store_ingest_batch(
    data: bytes,
    *,
    event_count: int,
    event_type: str,
    source: str,
    payload_format: str,
    bucket_k: int,
    use_overflow: int,
    acceleration: int,
    chunk_size: int,
) -> dict[str, object]:
    try:
        started = time.perf_counter()
        frame = elh.compress(
            data,
            bucket_k=bucket_k,
            use_overflow=use_overflow,
            acceleration=acceleration,
            chunk_size=chunk_size,
        )
        compressed_at = time.perf_counter()
        restored = elh.decompress(frame)
        finished = time.perf_counter()
    except (elh.Error, ValueError) as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    if restored != data:
        raise HTTPException(status_code=500, detail="stored batch failed verification")

    batch_id = f"{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S')}-{uuid.uuid4().hex[:12]}"
    _, batches_dir, _ = ingest_paths()
    frame_path = batches_dir / f"{batch_id}.elh"
    frame_path.write_bytes(frame)

    metadata = {
        "batch_id": batch_id,
        "codec": "elh-frame",
        "event_type": event_type,
        "source": source,
        "payload_format": payload_format,
        "event_count": event_count,
        "original_bytes": len(data),
        "compressed_bytes": len(frame),
        "ratio_percent": ratio_percent(len(frame), len(data)),
        "sha256_original": sha256_hex(data),
        "sha256_compressed": sha256_hex(frame),
        "bucket_k": bucket_k,
        "overflow": bool(use_overflow),
        "acceleration": acceleration,
        "chunk_size": chunk_size,
        "compress_ms": round((compressed_at - started) * 1000.0, 3),
        "decompress_ms": round((finished - compressed_at) * 1000.0, 3),
        "verified": True,
        "storage_path": str(frame_path),
        "created_at": now_iso(),
    }
    append_metadata(metadata)
    return metadata


def with_lz4_baseline(elh_result: dict[str, object], lz4_result: dict[str, object]) -> dict[str, object]:
    lz4_size = int(lz4_result["compressed_bytes"])
    elh_size = int(elh_result["compressed_bytes"])
    if lz4_size > 0:
        improvement = round(((lz4_size - elh_size) / lz4_size) * 100.0, 2)
    else:
        improvement = 0.0

    result = dict(elh_result)
    result["codec"] = "elh"
    result["lz4"] = {"codec": "lz4_clean_v1.10.0", **lz4_result}
    result["elh_size_savings_vs_lz4_percent"] = improvement
    return result


def best_result(results: list[dict[str, object]]) -> dict[str, object]:
    return min(results, key=lambda item: int(item["compressed_bytes"]))


def sweep_batch_mode(
    data: bytes,
    *,
    bucket_values: list[int],
    overflow_values: list[int],
    batch_values: list[int],
    acceleration: int,
    chunk_size: int,
) -> dict[str, object]:
    configs: list[dict[str, object]] = []
    lz4_by_batch: dict[int, dict[str, object]] = {}

    for batch_size in batch_values:
        units = fixed_batches(data, batch_size)
        lz4_result = bench_lz4_units(units)
        lz4_by_batch[batch_size] = lz4_result

        for bucket_k in bucket_values:
            for use_overflow in overflow_values:
                elh_result = bench_units(
                    units,
                    bucket_k=bucket_k,
                    use_overflow=use_overflow,
                    acceleration=acceleration,
                    chunk_size=chunk_size,
                )
                compared = with_lz4_baseline(elh_result, lz4_result)
                compared["bucket_k"] = bucket_k
                compared["overflow"] = bool(use_overflow)
                compared["batch_size"] = batch_size
                configs.append(compared)

    best_elh = best_result(configs)
    best_lz4_batch = min(
        (
            {"batch_size": batch_size, **result}
            for batch_size, result in lz4_by_batch.items()
        ),
        key=lambda item: int(item["compressed_bytes"]),
    )
    return {
        "mode": "kafka_batch",
        "configs_tested": len(configs),
        "best_elh": best_elh,
        "best_lz4": {"codec": "lz4_clean_v1.10.0", **best_lz4_batch},
        "configs": sorted(configs, key=lambda item: int(item["compressed_bytes"])),
    }


@app.get("/health")
def health() -> dict[str, object]:
    return {"ok": True, "max_request_bytes": MAX_REQUEST_BYTES}


@app.get("/version")
def version() -> dict[str, object]:
    return {
        "api": "0.1.0",
        "frame_version": 1,
        "default_chunk_size": DEFAULT_CHUNK_SIZE,
    }


@app.post("/benchmark")
async def benchmark(request: Request, _: None = Security(require_api_key)) -> dict[str, object]:
    data = await read_limited_body(request)
    bucket_k = parse_int_query(request.query_params.get("bucket_k"), 4, "bucket_k")
    use_overflow = parse_int_query(request.query_params.get("overflow"), 1, "overflow")
    acceleration = parse_int_query(request.query_params.get("accel"), 1, "accel")
    chunk_size = positive_int(
        parse_int_query(request.query_params.get("chunk"), DEFAULT_CHUNK_SIZE, "chunk"),
        "chunk",
    )
    batch_size = positive_int(
        parse_int_query(request.query_params.get("batch"), 256 * 1024, "batch"),
        "batch",
    )

    try:
        continuous_units = [data]
        batch_units = fixed_batches(data, batch_size)
        record_units = records(data)

        continuous = bench_units(
            continuous_units,
            bucket_k=bucket_k,
            use_overflow=use_overflow,
            acceleration=acceleration,
            chunk_size=chunk_size,
        )
        batched = bench_units(
            batch_units,
            bucket_k=bucket_k,
            use_overflow=use_overflow,
            acceleration=acceleration,
            chunk_size=chunk_size,
        )
        per_record = bench_units(
            record_units,
            bucket_k=bucket_k,
            use_overflow=use_overflow,
            acceleration=acceleration,
            chunk_size=chunk_size,
        )
        continuous = with_lz4_baseline(continuous, bench_lz4_units(continuous_units))
        batched = with_lz4_baseline(batched, bench_lz4_units(batch_units))
        per_record = with_lz4_baseline(per_record, bench_lz4_units(record_units))
    except (elh.Error, lz4_baseline.Error, ValueError) as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    return {
        "input_bytes": len(data),
        "batch_size": batch_size,
        "chunk_size": chunk_size,
        "bucket_k": bucket_k,
        "overflow": bool(use_overflow),
        "baselines": {
            "lz4": "clean upstream LZ4 v1.10.0 from third_party/lz4",
        },
        "modes": {
            "continuous": continuous,
            "kafka_batch": batched,
            "per_record": per_record,
        },
    }


@app.post("/benchmark/sweep")
async def benchmark_sweep(request: Request, _: None = Security(require_api_key)) -> dict[str, object]:
    data = await read_limited_body(request)
    bucket_values = parse_int_list_query(
        request.query_params.get("buckets"),
        [1, 2, 4, 8],
        "buckets",
    )
    overflow_values = parse_int_list_query(
        request.query_params.get("overflows"),
        [0, 1],
        "overflows",
        max_items=2,
    )
    batch_values = [
        positive_int(value, "batches")
        for value in parse_int_list_query(
            request.query_params.get("batches"),
            [16 * 1024, 64 * 1024, 256 * 1024, 1024 * 1024],
            "batches",
        )
    ]
    acceleration = parse_int_query(request.query_params.get("accel"), 1, "accel")
    chunk_size = positive_int(
        parse_int_query(request.query_params.get("chunk"), DEFAULT_CHUNK_SIZE, "chunk"),
        "chunk",
    )

    for bucket_k in bucket_values:
        positive_int(bucket_k, "buckets")
    for use_overflow in overflow_values:
        if use_overflow not in (0, 1):
            raise HTTPException(status_code=400, detail="overflows must contain only 0 or 1")

    try:
        sweep = sweep_batch_mode(
            data,
            bucket_values=bucket_values,
            overflow_values=overflow_values,
            batch_values=batch_values,
            acceleration=acceleration,
            chunk_size=chunk_size,
        )
    except (elh.Error, lz4_baseline.Error, ValueError) as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    return {
        "input_bytes": len(data),
        "chunk_size": chunk_size,
        "buckets": bucket_values,
        "overflows": overflow_values,
        "batches": batch_values,
        "baselines": {
            "lz4": "clean upstream LZ4 v1.10.0 from third_party/lz4",
        },
        "sweep": sweep,
    }


@app.post("/ingest/event")
async def ingest_event(request: Request, _: None = Security(require_api_key)) -> dict[str, object]:
    body = await read_limited_body(request)
    if not body:
        raise HTTPException(status_code=400, detail="request body cannot be empty")

    data, event_count, payload_format = normalize_event_body(
        body,
        request.headers.get("content-type"),
    )
    if event_count != 1:
        raise HTTPException(status_code=400, detail="expected exactly one event")

    event_type = request.query_params.get("event_type", "event")
    source = request.query_params.get("source", "api")
    bucket_k = parse_int_query(request.query_params.get("bucket_k"), 4, "bucket_k")
    use_overflow = parse_int_query(request.query_params.get("overflow"), 1, "overflow")
    acceleration = parse_int_query(request.query_params.get("accel"), 1, "accel")
    chunk_size = positive_int(
        parse_int_query(request.query_params.get("chunk"), DEFAULT_CHUNK_SIZE, "chunk"),
        "chunk",
    )

    return store_ingest_batch(
        data,
        event_count=event_count,
        event_type=event_type,
        source=source,
        payload_format=payload_format,
        bucket_k=bucket_k,
        use_overflow=use_overflow,
        acceleration=acceleration,
        chunk_size=chunk_size,
    )


@app.post("/ingest/batch")
async def ingest_batch(request: Request, _: None = Security(require_api_key)) -> dict[str, object]:
    body = await read_limited_body(request)
    if not body:
        raise HTTPException(status_code=400, detail="request body cannot be empty")

    data, event_count, payload_format = normalize_event_body(
        body,
        request.headers.get("content-type"),
    )
    event_type = request.query_params.get("event_type", "event.batch")
    source = request.query_params.get("source", "api")
    bucket_k = parse_int_query(request.query_params.get("bucket_k"), 4, "bucket_k")
    use_overflow = parse_int_query(request.query_params.get("overflow"), 1, "overflow")
    acceleration = parse_int_query(request.query_params.get("accel"), 1, "accel")
    chunk_size = positive_int(
        parse_int_query(request.query_params.get("chunk"), DEFAULT_CHUNK_SIZE, "chunk"),
        "chunk",
    )

    return store_ingest_batch(
        data,
        event_count=event_count,
        event_type=event_type,
        source=source,
        payload_format=payload_format,
        bucket_k=bucket_k,
        use_overflow=use_overflow,
        acceleration=acceleration,
        chunk_size=chunk_size,
    )


@app.get("/ingest/batches")
def ingest_batches(limit: int = 100, _: None = Security(require_api_key)) -> dict[str, object]:
    limit = positive_int(limit, "limit")
    if limit > 1000:
        raise HTTPException(status_code=400, detail="limit cannot exceed 1000")
    batches = read_metadata(limit=limit)
    return {"count": len(batches), "batches": batches}


@app.get("/ingest/batches/{batch_id}")
def ingest_batch_metadata(batch_id: str, _: None = Security(require_api_key)) -> dict[str, object]:
    return find_metadata(batch_id)


@app.get("/ingest/batches/{batch_id}/compressed")
def ingest_batch_compressed(batch_id: str, _: None = Security(require_api_key)) -> Response:
    metadata = find_metadata(batch_id)
    path = Path(str(metadata["storage_path"]))
    if not path.exists():
        raise HTTPException(status_code=404, detail="compressed batch file not found")
    frame = path.read_bytes()
    return Response(
        content=frame,
        media_type="application/vnd.elh.frame",
        headers={
            "X-Batch-Id": batch_id,
            "X-Original-Size": str(metadata["original_bytes"]),
            "X-Compressed-Size": str(metadata["compressed_bytes"]),
        },
    )


@app.get("/ingest/batches/{batch_id}/decompress")
def ingest_batch_decompress(batch_id: str, _: None = Security(require_api_key)) -> Response:
    metadata = find_metadata(batch_id)
    path = Path(str(metadata["storage_path"]))
    if not path.exists():
        raise HTTPException(status_code=404, detail="compressed batch file not found")
    try:
        data = elh.decompress(path.read_bytes())
    except elh.Error as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    digest = sha256_hex(data)
    verified = digest == metadata["sha256_original"]
    return Response(
        content=data,
        media_type="application/x-ndjson" if metadata["payload_format"] == "json" else "application/octet-stream",
        headers={
            "X-Batch-Id": batch_id,
            "X-Verified": "true" if verified else "false",
            "X-Sha256": digest,
        },
    )


@app.post("/compress")
async def compress(request: Request, _: None = Security(require_api_key)) -> Response:
    data = await read_limited_body(request)
    bucket_k = parse_int_query(request.query_params.get("bucket_k"), 4, "bucket_k")
    use_overflow = parse_int_query(request.query_params.get("overflow"), 1, "overflow")
    acceleration = parse_int_query(request.query_params.get("accel"), 1, "accel")
    chunk_size = parse_int_query(request.query_params.get("chunk"), DEFAULT_CHUNK_SIZE, "chunk")

    try:
        frame = elh.compress(
            data,
            bucket_k=bucket_k,
            use_overflow=use_overflow,
            acceleration=acceleration,
            chunk_size=chunk_size,
        )
    except (elh.Error, ValueError) as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    return Response(
        content=frame,
        media_type="application/vnd.elh.frame",
        headers={
            "X-Original-Size": str(len(data)),
            "X-Compressed-Size": str(len(frame)),
        },
    )


@app.post("/decompress")
async def decompress(request: Request, _: None = Security(require_api_key)) -> Response:
    frame = await read_limited_body(request)
    try:
        data = elh.decompress(frame)
    except elh.Error as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    return Response(
        content=data,
        media_type="application/octet-stream",
        headers={
            "X-Original-Size": str(len(data)),
            "X-Compressed-Size": str(len(frame)),
        },
    )


@app.post("/compress.json")
async def compress_json(request: Request, _: None = Security(require_api_key)) -> dict[str, object]:
    body = await read_limited_body(request)
    try:
        request_json = json.loads(body.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise HTTPException(status_code=400, detail="Invalid JSON request body") from exc

    # Validate and extract required field
    if "data" not in request_json:
        raise HTTPException(status_code=400, detail="Missing required field: data")
    
    try:
        data = base64.b64decode(request_json["data"])
    except Exception as exc:
        raise HTTPException(status_code=400, detail="Invalid base64 encoding in data field") from exc

    # Extract optional parameters with defaults
    bucket_k = parse_int_query(request_json.get("bucket_k"), 4, "bucket_k")
    use_overflow = parse_int_query(request_json.get("overflow"), 1, "overflow")
    chunk_size = positive_int(
        parse_int_query(request_json.get("chunk"), DEFAULT_CHUNK_SIZE, "chunk"),
        "chunk",
    )

    try:
        frame = elh.compress(
            data,
            bucket_k=bucket_k,
            use_overflow=use_overflow,
            acceleration=1,
            chunk_size=chunk_size,
        )
    except (elh.Error, ValueError) as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    return {
        "compressed": base64.b64encode(frame).decode("utf-8"),
        "original_size": len(data),
        "compressed_size": len(frame),
        "ratio_percent": ratio_percent(len(frame), len(data)),
    }


@app.post("/decompress.json")
async def decompress_json(request: Request, _: None = Security(require_api_key)) -> dict[str, object]:
    body = await read_limited_body(request)
    try:
        request_json = json.loads(body.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise HTTPException(status_code=400, detail="Invalid JSON request body") from exc

    # Validate and extract required field
    if "data" not in request_json:
        raise HTTPException(status_code=400, detail="Missing required field: data")
    
    try:
        frame = base64.b64decode(request_json["data"])
    except Exception as exc:
        raise HTTPException(status_code=400, detail="Invalid base64 encoding in data field") from exc

    try:
        data = elh.decompress(frame)
    except elh.Error as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    return {
        "decompressed": base64.b64encode(data).decode("utf-8"),
        "original_size": len(data),
        "compressed_size": len(frame),
    }
