#!/usr/bin/env python3
"""Structural guarantees for the source-owned candidate pipeline, not model quality."""

from copy import deepcopy
import json
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import test_semantic_runtime
from shin_semantic.artifact import Rejected, sha
from shin_semantic.hierarchical import execute, make_plan
from shin_semantic.selection import candidate

try:
    import numpy as np
except ImportError:
    np = None


@unittest.skipIf(np is None, "Optional NumPy runtime is absent")
class HierarchicalTests(unittest.TestCase):
    def fixture(self, files=110, members=18):
        units, by_path, lookup, texts = [], {}, {}, {}
        for file in range(files):
            path = f"src/file-{file}.cpp"
            for member in range(members):
                index = len(units)
                text = f"body {file} {member}"
                unit = {"project": "fixture", "path": path, "start_line": member * 10 + 1,
                        "end_line": member * 10 + 8, "record_ids": [f"{file}:{member}"],
                        "source_sha256": "a" * 64, "text_sha256": sha(text.encode())}
                units.append(unit)
                by_path.setdefault(path, []).append(index)
                lookup[path, unit["start_line"], unit["end_line"]] = index
                texts[unit["text_sha256"]] = text
        artifact = SimpleNamespace(units=units, by_path=by_path, lookup=lookup,
            manifest={"binding": {"project": "fixture", "snapshot_id": "snapshot"}})
        return artifact, np.linspace(1., 0., len(units)), texts

    def test_pool_counts_files_not_twenty_rows_and_keeps_baseline(self):
        artifact, scores, _ = self.fixture()
        baseline = {"candidates": [candidate(artifact.units[-1])]}
        plan = make_plan(artifact, scores, baseline)
        self.assertEqual(len(plan["files"]), 100)
        self.assertIn("src/file-109.cpp", [f["path"] for f in plan["files"]])
        self.assertTrue(all(len(f["members"]) == 16 and f["members_truncated"] for f in plan["files"]))

    def test_two_stages_obey_separate_budgets_without_mutation(self):
        artifact, scores, texts = self.fixture()
        plan = make_plan(artifact, scores, {"candidates": []})
        before = deepcopy(plan)
        model = Mock()
        model.rerank.side_effect = lambda query, passages, batch_size: [-float(text.split()[-1]) for text in passages]
        result = execute(plan, "question", lambda unit: texts[unit["text_sha256"]], model)
        self.assertEqual(result["work"]["reranked_passages"], 460)
        self.assertEqual(len(result["navigation"]["candidates"]), 20)
        self.assertEqual(len({r["path"] for r in result["navigation"]["candidates"]}), 20)
        self.assertEqual(len(result["collection"]["files"]), 5)
        self.assertTrue(all(len(f["members"]) == 3 for f in result["collection"]["files"]))
        self.assertEqual(plan, before)
        self.assertFalse(result["navigation"]["pack_completion_asserted"])

    def test_scoped_lookup_bypasses_text_and_inference(self):
        artifact, scores, _ = self.fixture(1, 1)
        baseline = {"candidates": [candidate(artifact.units[0])]}
        plan = make_plan(artifact, scores, baseline)
        text, model = Mock(), Mock()
        result = execute(plan, "symbol", text, model, scoped=True)
        self.assertEqual(result["navigation"], baseline)
        text.assert_not_called()
        model.rerank.assert_not_called()

    def test_wrong_text_cross_file_and_invalid_scores_rejected(self):
        artifact, scores, texts = self.fixture(2, 2)
        plan = make_plan(artifact, scores, {"candidates": []})
        model = Mock()
        with self.assertRaisesRegex(Rejected, "text-binding"):
            execute(plan, "q", lambda _: "wrong", model)
        model.rerank.assert_not_called()
        model.rerank.return_value = [float("nan")] * 4
        with self.assertRaisesRegex(Rejected, "reranker-scores"):
            execute(plan, "q", lambda unit: texts[unit["text_sha256"]], model)
        plan["units"]["0"]["project"] = "other"
        with self.assertRaisesRegex(Rejected, "member-file"):
            execute(plan, "q", lambda _: "wrong", model)

    def test_reference_witness_survives_member_change(self):
        artifact, scores, texts = self.fixture(1, 4)
        original = candidate(artifact.units[0])
        original["signals"] = {"source_reference": {"origin": {"path": "caller"}, "resolved_path": original["path"]}}
        plan = make_plan(artifact, scores, {"candidates": [original]})
        model = Mock()
        model.rerank.return_value = [0., 1., 2., 3.]
        result = execute(plan, "q", lambda unit: texts[unit["text_sha256"]], model)
        top = result["navigation"]["candidates"][0]
        self.assertEqual(top["start_line"], 31)
        self.assertEqual(top["signals"]["file_reference"]["basis"], original)
        self.assertEqual(top["signals"]["file_reference"]["scope"], "file-binding-not-member-selection")

    def test_member_selection_can_reach_beyond_file_representatives(self):
        artifact, scores, texts = self.fixture(2, 18)
        plan = make_plan(artifact, scores, {"candidates": []})
        model = Mock()
        model.rerank.side_effect = lambda query, passages, batch_size: [float(text.split()[-1]) for text in passages]
        result = execute(plan, "q", lambda unit: texts[unit["text_sha256"]], model)
        first = result["collection"]["files"][0]["members"][0]
        self.assertEqual(first["start_line"], 151)
        self.assertNotIn(15, plan["files"][0]["representatives"])
        self.assertEqual(result["work"]["reranked_passages"], 32)

    def test_original_binding_and_nonfinite_similarity_are_not_admitted(self):
        artifact, scores, _ = self.fixture(2, 2)
        baseline = {"candidates": [{**candidate(artifact.units[0]), "source_sha256": "b" * 64}]}
        with self.assertRaisesRegex(Rejected, "binding"):
            make_plan(artifact, scores, baseline)
        scores[0] = np.nan
        with self.assertRaisesRegex(Rejected, "query-scores"):
            make_plan(artifact, scores, {"candidates": []})

    def test_output_limit_rejects_instead_of_silently_dropping_records(self):
        artifact, scores, texts = self.fixture(1, 4)
        plan = make_plan(artifact, scores, {"candidates": []})
        model = Mock()
        model.rerank.return_value = [0., 1., 2., 3.]
        with patch("shin_semantic.hierarchical.MAX_OUTPUT", 100):
            with self.assertRaisesRegex(Rejected, "output-budget"):
                execute(plan, "q", lambda unit: texts[unit["text_sha256"]], model)


if __name__ == "__main__":
    unittest.main()
