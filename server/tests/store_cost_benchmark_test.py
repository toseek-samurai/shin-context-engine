#!/usr/bin/env python3
"""No-DB validation for store cost benchmark parser and numeric bounds."""

from __future__ import annotations

import argparse
import json
import sys
import unittest


from store_cost_benchmark import (
    BenchmarkFailure,
    MAX_PROBE_OUTPUT_BYTES,
    bounded_count,
    execute_probe,
    parse_probe_output,
)


def valid_probe_payload() -> bytes:
    return json.dumps({
        "schema_version": "shin.server-store-cost-probe.v1",
        "tool": "shin-server-store-cost-probe",
        "ok": True,
        "iterations": 2,
        "warmup": 1,
        "cold_first_readiness_ms": 1.0,
        "warm_pooled_readiness_ms": [2.0, 3.0],
        "raw_select1_lower_bound_ms": [0.5, 0.6],
    }).encode("utf-8")


class StoreCostBenchmarkParserTests(unittest.TestCase):
    def test_executes_process_with_byte_output(self) -> None:
        command = [sys.executable, "-c", "import sys; sys.stdout.buffer.write(" + repr(valid_probe_payload()) + ")"]
        self.assertEqual(execute_probe(command, iterations=2, warmup=1), (1.0, [2.0, 3.0], [0.5, 0.6]))

    def test_accepts_bounded_finite_probe_json(self) -> None:
        self.assertEqual(parse_probe_output(valid_probe_payload(), iterations=2, warmup=1),
                         (1.0, [2.0, 3.0], [0.5, 0.6]))

    def test_rejects_duplicate_keys(self) -> None:
        payload = valid_probe_payload().decode("utf-8").replace('"iterations": 2', '"iterations": 2, "iterations": 2')
        with self.assertRaises(BenchmarkFailure):
            parse_probe_output(payload.encode("utf-8"), iterations=2, warmup=1)

    def test_rejects_nonfinite_and_boolean_counters(self) -> None:
        nonfinite = valid_probe_payload().decode("utf-8").replace("1.0", "NaN", 1)
        boolean_counter = valid_probe_payload().decode("utf-8").replace('"iterations": 2', '"iterations": true')
        with self.assertRaises(BenchmarkFailure):
            parse_probe_output(nonfinite.encode("utf-8"), iterations=2, warmup=1)
        with self.assertRaises(BenchmarkFailure):
            parse_probe_output(boolean_counter.encode("utf-8"), iterations=2, warmup=1)

    def test_rejects_oversized_output_and_invalid_counts(self) -> None:
        with self.assertRaises(BenchmarkFailure):
            parse_probe_output(b" " * (MAX_PROBE_OUTPUT_BYTES + 1), iterations=2, warmup=1)
        for value in ("0", "10001", "one"):
            with self.assertRaises(argparse.ArgumentTypeError):
                bounded_count(value, name="--iterations", minimum=1, maximum=10000)


if __name__ == "__main__":
    unittest.main()
