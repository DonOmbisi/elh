FROM python:3.12-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target elh_shared lz4_clean_shared --parallel

FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    ELH_LIBRARY=/app/libelh.so \
    LZ4_LIBRARY=/app/liblz4_clean.so \
    PYTHONPATH=/app/python \
    ELH_API_MAX_BYTES=10485760 \
    ELH_API_KEY=""

WORKDIR /app
COPY api/requirements.txt /app/api/requirements.txt
RUN pip install --no-cache-dir -r /app/api/requirements.txt

COPY --from=build /src/build/libelh.so /app/libelh.so
COPY --from=build /src/build/liblz4_clean.so /app/liblz4_clean.so
COPY python /app/python
COPY api /app/api

EXPOSE 8000
CMD ["sh", "-c", "uvicorn api.app:app --host 0.0.0.0 --port ${PORT:-8000}"]
