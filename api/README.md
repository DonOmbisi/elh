# ELH Public HTTP API

This service exposes ELH frame compression over HTTP for demos, integrations,
and remote experiments.

## Endpoints

```text
GET  /
GET  /health
GET  /version
POST /benchmark
POST /benchmark/sweep
POST /compress
POST /decompress
POST /ingest/event
POST /ingest/batch
GET  /ingest/batches
GET  /ingest/batches/{batch_id}
GET  /ingest/batches/{batch_id}/compressed
GET  /ingest/batches/{batch_id}/decompress
```

Open `/` in a browser to use the public tester UI. It lets users upload a
file, compress it, decompress it, verify SHA-256 equality, and download both
the `.elh` frame and restored file.

`/benchmark` is intended for AI/ML logs and Kafka-like event streams. It
compresses the same body three ways: one continuous log, fixed-size batches,
and individual newline-delimited records. Each mode includes an ELH result and
a clean upstream LZ4 v1.10.0 baseline built from `third_party/lz4`.

```bash
curl -X POST --data-binary @inference.jsonl \
  "http://localhost:8000/benchmark?bucket_k=4&overflow=1&chunk=65536&batch=262144"
```

`/benchmark/sweep` searches Kafka-style batch configurations and returns the
best ELH result against the best clean LZ4 result:

```bash
curl -X POST --data-binary @events.jsonl \
  "http://localhost:8000/benchmark/sweep?buckets=1,2,4,8&overflows=0,1&batches=16384,65536,262144,1048576"
```

`/compress` accepts raw bytes and returns an ELH frame:

```bash
curl -X POST --data-binary @input.log \
  "http://localhost:8000/compress?bucket_k=4&overflow=1&chunk=65536" \
  -o input.elh
```

`/decompress` accepts an ELH frame and returns original bytes:

```bash
curl -X POST --data-binary @input.elh \
  http://localhost:8000/decompress \
  -o restored.log
```

## Local Run

Build the shared library first:

```bash
cmake -S . -B build
cmake --build build --target elh_shared lz4_clean_shared
```

Run the API:

```bash
PYTHONPATH=python ELH_LIBRARY=build/libelh.so LZ4_LIBRARY=build/liblz4_clean.so \
  ELH_API_KEY=change-me \
  uvicorn api.app:app --host 0.0.0.0 --port 8000
```

Then open:

```text
http://localhost:8000/
```

On Windows-native Python, build Windows DLLs and set `ELH_LIBRARY` and
`LZ4_LIBRARY` to those DLLs instead.

## Ingestion Store

The ingestion endpoints provide a simple integration layer for systems such as
AutoVest, Kafka-like producers, and AI log collectors. JSON objects and arrays
are normalized to canonical JSONL, compressed as ELH frames, stored under
`ELH_INGEST_DIR`, and recorded in `metadata.jsonl` with hashes and size metrics.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  --data-binary @events.json \
  "http://localhost:8000/ingest/batch?event_type=autovest.audit&source=whatsapp"
```

List stored batches:

```bash
curl http://localhost:8000/ingest/batches
```

Replay a stored batch exactly:

```bash
curl http://localhost:8000/ingest/batches/BATCH_ID/decompress -o replay.jsonl
```

For private data, compress before encryption:

```text
normalize JSON -> compress with ELH -> encrypt -> store
```

## Docker

Build from the repository root:

```bash
docker build -f api/Dockerfile -t elh-api .
docker run --rm -p 8000:8000 -e ELH_API_MAX_BYTES=10485760 -e ELH_API_KEY=change-me elh-api
```

## Railway

This repository includes `railway.json`, which tells Railway to build
`api/Dockerfile` and health-check `/health`.

Set these Railway environment variables:

```text
ELH_API_KEY=<long-random-secret>
ELH_API_MAX_BYTES=10485760
ELH_INGEST_DIR=/app/data/ingest
```

Railway injects `PORT`; the Docker image starts uvicorn on that port.

After deployment:

```bash
curl https://YOUR-RAILWAY-DOMAIN/health
curl -X POST -H "X-API-Key: <long-random-secret>" \
  --data-binary "hello" \
  https://YOUR-RAILWAY-DOMAIN/compress \
  -o hello.elh
```

For durable ingestion storage, attach a Railway volume mounted at `/app/data`.
Without a volume, compressed ingest batches are suitable for demos but may not
survive restarts or redeploys.

## Limits

The API rejects request bodies larger than `ELH_API_MAX_BYTES`, defaulting to
10 MiB. Keep this limit enabled on public deployments.

## Cloudflare Tunnel

For temporary remote integration, run the API with an API key and expose it
through Cloudflare Tunnel:

```bash
PYTHONPATH=python ELH_LIBRARY=build/libelh.so LZ4_LIBRARY=build/liblz4_clean.so \
  ELH_API_KEY=change-me \
  uvicorn api.app:app --host 127.0.0.1 --port 8000

cloudflared tunnel --url http://127.0.0.1:8000
```

Use the generated HTTPS URL as `ELH_API_URL` on the AutoVest server and send
the same key as `X-API-Key`.
