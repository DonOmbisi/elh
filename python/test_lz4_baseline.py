import unittest

import lz4_baseline


class Lz4BaselineTests(unittest.TestCase):
    def test_roundtrip(self):
        data = (b'{"service":"events","message":"clean upstream lz4"}\n' * 1000)
        compressed = lz4_baseline.compress(data)
        restored = lz4_baseline.decompress(compressed, len(data))
        self.assertEqual(restored, data)
        self.assertLess(len(compressed), len(data))


if __name__ == "__main__":
    unittest.main()
