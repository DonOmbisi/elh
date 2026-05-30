import unittest

import elh


class ElhPythonBindingTests(unittest.TestCase):
    def test_empty_roundtrip(self):
        frame = elh.compress(b"")
        self.assertEqual(elh.original_size(frame), 0)
        self.assertEqual(elh.decompress(frame), b"")

    def test_repeated_text_roundtrip(self):
        data = (b"ts=1 service=api message=hello elastic hashing\n" * 2000)
        frame = elh.compress(data, chunk_size=4096, bucket_k=4)
        self.assertEqual(elh.original_size(frame), len(data))
        self.assertEqual(elh.decompress(frame), data)

    def test_bytes_like_inputs(self):
        data = bytearray(b"abcdef" * 100)
        frame = elh.compress(memoryview(data), bucket_k=2)
        self.assertEqual(elh.decompress(bytearray(frame)), bytes(data))

    def test_invalid_frame_rejected(self):
        with self.assertRaises(elh.Error):
            elh.decompress(b"not an elh frame")


if __name__ == "__main__":
    unittest.main()
