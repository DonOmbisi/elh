import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from fastapi.testclient import TestClient

import app as app_module


client = TestClient(app_module.app)


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
        )
        self.assertEqual(compressed.status_code, 200)
        self.assertTrue(
            compressed.headers["content-type"].startswith("application/vnd.elh.frame")
        )
        self.assertEqual(int(compressed.headers["x-original-size"]), len(data))

        restored = client.post("/decompress", content=compressed.content)
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
                )
                self.assertEqual(created.status_code, 200)
                metadata = created.json()
                self.assertEqual(metadata["event_count"], 2)
                self.assertTrue(metadata["verified"])
                self.assertGreater(metadata["compressed_bytes"], 0)

                listed = client.get("/ingest/batches")
                self.assertEqual(listed.status_code, 200)
                self.assertEqual(listed.json()["count"], 1)

                replay = client.get(f"/ingest/batches/{metadata['batch_id']}/decompress")
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
                response = client.post("/ingest/event", json=[{"a": 1}, {"a": 2}])
                self.assertEqual(response.status_code, 400)
        finally:
            app_module.INGEST_DIR = old_dir

    def test_invalid_frame_rejected(self):
        response = client.post("/decompress", content=b"not an elh frame")
        self.assertEqual(response.status_code, 400)

    def test_request_limit(self):
        old_limit = app_module.MAX_REQUEST_BYTES
        try:
            app_module.MAX_REQUEST_BYTES = 8
            response = client.post("/compress", content=b"0123456789")
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


if __name__ == "__main__":
    unittest.main()
