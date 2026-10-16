import concurrent.futures
import hashlib
import json
from pathlib import Path
import sys
import subprocess
import unittest

from native_json_codec import NativeJsonCodec

BINARY = Path(sys.argv.pop(1)).resolve()


class NativeJsonCodecTests(unittest.TestCase):
    def setUp(self) -> None:
        self.codec = NativeJsonCodec(BINARY)

    def tearDown(self) -> None:
        self.codec.close()

    def test_double_representation_binds_the_native_bytes(self) -> None:
        payload = {"duration_ms": 10.723719}
        native = self.codec.encode(payload)
        self.assertEqual(native, b'{"duration_ms":10.723718999999999}')
        python = json.dumps(payload, separators=(",", ":")).encode()
        self.assertEqual(json.loads(native), json.loads(python))
        self.assertNotEqual(hashlib.sha256(native).digest(), hashlib.sha256(python).digest())

    def test_unicode_and_escaped_controls(self) -> None:
        payload = {"text": "\u044f\n\x00\\u0000", "number": 9223372036854775807}
        native = self.codec.encode(payload)
        self.assertIn("\u044f".encode(), native)
        self.assertIn(b"\\u0000", native)
        self.assertEqual(json.loads(native), payload)

    def test_concurrent_requests_keep_their_frames(self) -> None:
        values = [{"id": index, "duration_ms": 2.046292, "body": "x" * 10000} for index in range(64)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            actual = list(pool.map(self.codec.encode, values))
        self.assertEqual([json.loads(item) for item in actual], values)

    def test_input_bounds_and_invalid_numbers(self) -> None:
        with self.assertRaises(ValueError):
            self.codec.encode({"body": "x" * (1024 * 1024)})
        with self.assertRaises(ValueError):
            self.codec.encode({"number": float("nan")})
        self.assertIsNone(self.codec.process)

    def test_explicit_close_allows_a_new_fixture(self) -> None:
        self.assertEqual(self.codec.encode({"first": True}), b'{"first":true}')
        self.codec.close()
        self.assertIsNone(self.codec.process)
        self.assertEqual(self.codec.encode([1, None]), b'[1,null]')

    def test_native_parser_rejects_invalid_json(self) -> None:
        result = subprocess.run([str(BINARY)], input=b"{\n", capture_output=True, timeout=5, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, b"")
        self.assertEqual(result.stderr, b"Invalid JSON codec fixture input.\n")


if __name__ == "__main__":
    unittest.main()
