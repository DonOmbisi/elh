import unittest
import base64
from pathlib import Path
from tempfile import TemporaryDirectory

from fastapi.testclient import TestClient

import app as app_module

# Get API key from environment if configured
API_KEY = app_module.API_KEY
client = TestClient(app_module.app)

def with_api_key(headers=None):
    """Add API key to headers if configured"""
    if API_KEY:
        headers = headers or {}
        headers["X-API-Key"] = API_KEY
    return headers


class ApiTests(unittest.TestCase):
    def test_home_page(self):
        response = client.get("/")
        self.assertEqual(response.status_code, 200)
        self.assertIn("ELH Compression Tester", response.text)

    def test_health_and_version(self):
        health = client.get("/health")
        self.assertEqual(health.status_code, 200)
        self.assertIs(health.json()["ok"], True)

        version = client.get("/version")
        self.assertEqual(version.status_code, 200)
        self.assertEqual(version.json()["frame_version"], 1)

    def test_compress_decompress_roundtrip(self):
        data = (b'{"service":"api","message":"hello elastic hashing"}\n' * 1000)
        compressed = client.post(
            "/compress?bucket_k=4&overflow=1&chunk=4096",
            content=data,
            headers=with_api_key(),
        )
        self.assertEqual(compressed.status_code, 200)
        self.assertTrue(
            compressed.headers["content-type"].startswith("application/vnd.elh.frame")
        )
        self.assertEqual(int(compressed.headers["x-original-size"]), len(data))

        restored = client.post("/decompress", content=compressed.content, headers=with_api_key())
        self.assertEqual(restored.status_code, 200)
        self.assertEqual(restored.content, data)

    def test_benchmark_ai_kafka_modes(self):
        data = (
            b'{"service":"inference","model":"elh-demo","latency_ms":42,"ok":true}\n'
            * 256
        )
        response = client.post(
            "/benchmark?bucket_k=4&overflow=1&chunk=4096&batch=2048",
            content=data,
            headers=with_api_key(),
        )
        self.assertEqual(response.status_code, 200)
        report = response.json()
        self.assertEqual(report["input_bytes"], len(data))
        self.assertIn("continuous", report["modes"])
        self.assertIn("kafka_batch", report["modes"])
        self.assertIn("per_record", report["modes"])
        for mode in report["modes"].values():
            self.assertTrue(mode["verified"])
            self.assertGreater(mode["compressed_bytes"], 0)
            self.assertIn("lz4", mode)
            self.assertTrue(mode["lz4"]["verified"])
            self.assertGreater(mode["lz4"]["compressed_bytes"], 0)
            self.assertIn("elh_size_savings_vs_lz4_percent", mode)

    def test_benchmark_sweep(self):
        data = (
            b'{"service":"events","route":"/v1/chat/completions","status":"ok"}\n'
            * 256
        )
        response = client.post(
            "/benchmark/sweep?buckets=1,4&overflows=0,1&batches=2048,4096&chunk=4096",
            content=data,
            headers=with_api_key(),
        )
        self.assertEqual(response.status_code, 200)
        report = response.json()
        self.assertEqual(report["input_bytes"], len(data))
        self.assertEqual(report["sweep"]["configs_tested"], 8)
        self.assertIn("best_elh", report["sweep"])
        self.assertIn("best_lz4", report["sweep"])
        self.assertTrue(report["sweep"]["best_elh"]["verified"])
        self.assertTrue(report["sweep"]["best_lz4"]["verified"])

    def test_ingest_batch_store_and_replay(self):
        old_dir = app_module.INGEST_DIR
        try:
            with TemporaryDirectory() as tmp:
                app_module.INGEST_DIR = Path(tmp)
                events = [
                    {"source": "whatsapp", "event": "trade_intent", "text": "Buy KCB"},
                    {"source": "hedera", "event": "hcs_receipt", "status": "ok"},
                ]
                created = client.post(
                    "/ingest/batch?event_type=autovest.audit&source=test",
                    json=events,
                    headers=with_api_key(),
                )
                self.assertEqual(created.status_code, 200)
                metadata = created.json()
                self.assertEqual(metadata["event_count"], 2)
                self.assertTrue(metadata["verified"])
                self.assertGreater(metadata["compressed_bytes"], 0)

                listed = client.get("/ingest/batches", headers=with_api_key())
                self.assertEqual(listed.status_code, 200)
                self.assertEqual(listed.json()["count"], 1)

                replay = client.get(f"/ingest/batches/{metadata['batch_id']}/decompress", headers=with_api_key())
                self.assertEqual(replay.status_code, 200)
                self.assertEqual(replay.headers["x-verified"], "true")
                self.assertIn(b'"event":"trade_intent"', replay.content)
        finally:
            app_module.INGEST_DIR = old_dir

    def test_ingest_event_requires_single_event(self):
        old_dir = app_module.INGEST_DIR
        try:
            with TemporaryDirectory() as tmp:
                app_module.INGEST_DIR = Path(tmp)
                response = client.post("/ingest/event", json=[{"a": 1}, {"a": 2}], headers=with_api_key())
                self.assertEqual(response.status_code, 400)
        finally:
            app_module.INGEST_DIR = old_dir

    def test_invalid_frame_rejected(self):
        response = client.post("/decompress", content=b"not an elh frame", headers=with_api_key())
        self.assertEqual(response.status_code, 400)

    def test_request_limit(self):
        old_limit = app_module.MAX_REQUEST_BYTES
        try:
            app_module.MAX_REQUEST_BYTES = 8
            response = client.post("/compress", content=b"0123456789", headers=with_api_key())
            self.assertEqual(response.status_code, 413)
        finally:
            app_module.MAX_REQUEST_BYTES = old_limit

    def test_api_key_required_when_configured(self):
        old_key = app_module.API_KEY
        try:
            app_module.API_KEY = "secret-test-key"
            denied = client.post("/compress", content=b"hello")
            self.assertEqual(denied.status_code, 401)

            allowed = client.post(
                "/compress",
                content=b"hello",
                headers={"X-API-Key": "secret-test-key"},
            )
            self.assertEqual(allowed.status_code, 200)

            health = client.get("/health")
            self.assertEqual(health.status_code, 200)
        finally:
            app_module.API_KEY = old_key

    def test_rate_limiting_present(self):
        # Verify that rate limiting is configured
        self.assertIsNotNone(app_module._per_ip_limiter)
        self.assertIsNotNone(app_module._global_limiter)
        self.assertEqual(app_module._per_ip_limiter.max_requests, 600)
        self.assertEqual(app_module._global_limiter.max_requests, 3000)

    def test_compress_json_endpoint(self):
        data = b'{"service":"api","message":"hello elastic hashing"}\n' * 100
        encoded_data = base64.b64encode(data).decode("utf-8")
        
        request_json = {
            "data": encoded_data,
            "bucket_k": 4,
            "overflow": 1,
            "chunk": 4096
        }
        
        response = client.post("/compress.json", json=request_json, headers=with_api_key())
        self.assertEqual(response.status_code, 200)
        
        result = response.json()
        self.assertIn("compressed", result)
        self.assertIn("original_size", result)
        self.assertIn("compressed_size", result)
        self.assertIn("ratio_percent", result)
        self.assertEqual(result["original_size"], len(data))
        self.assertGreater(result["compressed_size"], 0)
        
        # Verify the compressed data can be decoded
        compressed = base64.b64decode(result["compressed"])
        self.assertGreater(len(compressed), 0)

    def test_decompress_json_endpoint(self):
        data = b'{"service":"api","message":"hello elastic hashing"}\n' * 100
        encoded_data = base64.b64encode(data).decode("utf-8")
        
        # First compress
        compress_request = {
            "data": encoded_data,
            "bucket_k": 4,
            "overflow": 1,
            "chunk": 4096
        }
        
        compress_response = client.post("/compress.json", json=compress_request, headers=with_api_key())
        self.assertEqual(compress_response.status_code, 200)
        compressed_result = compress_response.json()
        
        # Then decompress
        decompress_request = {
            "data": compressed_result["compressed"]
        }
        
        decompress_response = client.post("/decompress.json", json=decompress_request, headers=with_api_key())
        self.assertEqual(decompress_response.status_code, 200)
        
        decompress_result = decompress_response.json()
        self.assertIn("decompressed", decompress_result)
        self.assertIn("original_size", decompress_result)
        self.assertIn("compressed_size", decompress_result)
        
        # Verify round-trip
        restored = base64.b64decode(decompress_result["decompressed"])
        self.assertEqual(restored, data)
        self.assertEqual(decompress_result["original_size"], len(data))

    def test_compress_json_defaults(self):
        data = b'{"service":"api","message":"hello"}\n'
        encoded_data = base64.b64encode(data).decode("utf-8")
        
        # Test with minimal required fields (use defaults)
        request_json = {"data": encoded_data}
        
        response = client.post("/compress.json", json=request_json, headers=with_api_key())
        self.assertEqual(response.status_code, 200)
        
        result = response.json()
        self.assertIn("compressed", result)
        self.assertEqual(result["original_size"], len(data))

    def test_compress_json_invalid_base64(self):
        request_json = {"data": "not-valid-base64!!"}
        
        response = client.post("/compress.json", json=request_json, headers=with_api_key())
        self.assertEqual(response.status_code, 400)
        self.assertIn("base64", response.json()["detail"].lower())

    def test_compress_json_missing_data_field(self):
        request_json = {"bucket_k": 4}
        
        response = client.post("/compress.json", json=request_json, headers=with_api_key())
        self.assertEqual(response.status_code, 400)
        self.assertIn("data", response.json()["detail"].lower())

    def test_decompress_json_invalid_base64(self):
        request_json = {"data": "not-valid-base64!!"}
        
        response = client.post("/decompress.json", json=request_json, headers=with_api_key())
        self.assertEqual(response.status_code, 400)
        self.assertIn("base64", response.json()["detail"].lower())


if __name__ == "__main__":
    unittest.main()
