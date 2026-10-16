#!/usr/bin/env python3
"""Offline checks for the bounded reference; fake scores are not quality evidence."""

from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from run_cross_encoder_reference import POLICY_PATH, checked_candidates, gate, key, reorder, score_case, verify_model
from run_local_semantic_pilot import fingerprint

POLICY = json.loads(POLICY_PATH.read_bytes())


def row(path, start=1):
    return {"project": "fixture", "path": path, "start_line": start, "end_line": start + 2,
            "source_sha256": "a" * 64, "record_ids": [f"{path}:{start}"],
            "signals": {"lexical": {"evidence": [{"verification_level": "source-observed"}]}},
            "current_source_verified": False, "status": "navigation-candidate-not-an-answer"}


class ReferenceTests(unittest.TestCase):
    def test_reordering_preserves_provenance_and_distinct_path_first(self):
        rows = [row("one"), row("one", 10), row("two"), row("three")]
        before = deepcopy(rows)
        selected, order = reorder(rows, [1., 4., 2., 3.], POLICY)
        self.assertEqual(order, [1, 3, 2, 0])
        self.assertEqual(rows, before)
        self.assertEqual({key(r) for r in selected}, {key(r) for r in rows})
        for item, index in zip(selected, order):
            self.assertEqual(item, rows[index])
        selected[0]["record_ids"].append("mutated")
        self.assertEqual(rows, before)

    def test_ties_are_deterministic_and_bad_scores_rejected(self):
        rows = [row("one"), row("two")]
        self.assertEqual(reorder(rows, [1., 1.], POLICY)[1], [0, 1])
        for scores in ([1.], [1., float("nan")], [float("inf"), 1.]):
            with self.assertRaises(ValueError):
                reorder(rows, scores, POLICY)

    def test_no_cross_project_duplicates_or_wider_pool(self):
        for rows in ([row("one"), row("one")], [row("one", i) for i in range(3)],
                     [row(str(i)) for i in range(21)], [row("one"), {**row("two"), "project": "other"}]):
            with self.assertRaises(ValueError):
                checked_candidates(rows, POLICY)

    def test_native_bypass_does_not_call_model_or_use_eval_kind(self):
        model = Mock()
        for mode in ("owner-scoped", "identifier-scoped"):
            case = {"query": "name", "candidates": [row("one")], "texts": ["body"], "kind": "russian",
                    "core_retrieval_mode": mode}
            self.assertIsNone(score_case(case, model, POLICY)[2])
        model.rerank.assert_not_called()

    def test_ordinary_inference_only_gets_query_and_bound_text(self):
        model = Mock()
        model.rerank.return_value = [2., 1.]
        case = {"query": "question", "candidates": [row("one"), row("two")], "texts": ["first", "second"],
                "expected_paths": ["two"], "kind": "exact", "core_retrieval_mode": "strict"}
        score_case(case, model, POLICY)
        model.rerank.assert_called_once_with("question", ["first", "second"], batch_size=2)
        case["texts"] = []
        with self.assertRaises(ValueError):
            score_case(case, model, POLICY)

    def test_gate_cannot_cancel_a_regression_with_gains(self):
        rows = [{"id": "better", "cohort": "old", "before_rank": 6, "after_rank": 1},
                {"id": "lost", "cohort": "old", "before_rank": 3, "after_rank": 6}]
        result = gate(rows)
        self.assertFalse(result["passed"])
        self.assertEqual(result["regressions"], ["lost"])
        self.assertEqual(result["old_misses_improved"], ["better"])
        self.assertFalse(gate([{**rows[0], "cohort": "new", "after_rank": 7}])["passed"])

    def test_model_pins_checked_before_loading_optional_runtime(self):
        with tempfile.TemporaryDirectory() as base:
            directory = Path(base)
            path = directory / "model.onnx"
            path.write_bytes(b"not model weights")
            with self.assertRaisesRegex(ValueError, "pin"):
                verify_model(directory, POLICY)

    def test_model_budget_symlink_and_runtime_are_not_silently_accepted(self):
        with tempfile.TemporaryDirectory() as base:
            directory = Path(base)
            path = directory / "model.onnx"
            path.write_bytes(b"fixture")
            policy = {**POLICY, "files": {"model.onnx": fingerprint(path)}, "max_model_bytes": 1}
            with self.assertRaisesRegex(ValueError, "budget"):
                verify_model(directory, policy)
            target = directory / "target"
            path.rename(target)
            path.symlink_to(target)
            with self.assertRaisesRegex(ValueError, "Unsafe"):
                verify_model(directory, policy)

    def test_runtime_and_tokenizer_pins_checked(self):
        with tempfile.TemporaryDirectory() as base:
            directory = Path(base)
            weights = directory / "model.onnx"
            weights.write_bytes(b"fixture")
            config = directory / "tokenizer_config.json"
            config.write_text('{"model_max_length": 513}')
            policy = {**POLICY, "files": {"model.onnx": fingerprint(weights)}}
            with self.assertRaisesRegex(ValueError, "Tokenizer"):
                verify_model(directory, policy)
            config.write_text('{"model_max_length": 512}')
            with patch("importlib.metadata.version", return_value="different"):
                with self.assertRaisesRegex(ValueError, "pinned optional runtime"):
                    verify_model(directory, policy)


if __name__ == "__main__":
    unittest.main()
