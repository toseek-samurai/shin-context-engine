"""The consumer replay must not silently inherit Core's unique-path rank semantics."""

import copy
import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location(
    "consumer_golden_replay", Path(__file__).resolve().parents[1] / "tools/consumer_golden_replay.py")
replay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(replay)


class ConsumerGoldenReplayTests(unittest.TestCase):
    def setUp(self):
        self.case = {"id": "owner", "query": "owner protocol", "limit": 4, "perDocLimit": 2,
                     "mustPaths": ["owner.md"], "mustTopPaths": ["owner.md"],
                     "mustNotPaths": ["generated.jsonl"], "mustTopN": 2}
        self.result = {"ok": True, "profile": "fixture", "projection": "compact",
                       "query": self.case["query"], "snapshot_id": "snapshot",
                       "index_freshness": {"current": True, "fully_verified": True,
                                           "snapshot_id": "snapshot"},
                       "evidence": [{"path": "peer.md"}, {"path": "owner.md"}]}

    def check(self, result=None):
        return replay.check_observation(self.case, self.result if result is None else result, "fixture")

    def test_rank_boundary_is_inclusive(self):
        self.assertTrue(self.check()["passed"])

    def test_duplicate_evidence_uses_record_rank_not_unique_path_rank(self):
        self.result["evidence"].insert(0, {"path": "peer.md"})
        self.result["ranked_paths"] = [{"path": "owner.md", "unique_path_rank": 2}]
        checked = self.check()
        self.assertFalse(checked["passed"])
        self.assertEqual(checked["path_ranks"]["owner.md"], 3)
        self.assertEqual(checked["missing_top"], ["owner.md"])

    def test_unique_path_rank_requires_explicit_selection_and_uses_actual_evidence(self):
        self.result["evidence"].insert(0, {"path": "peer.md"})
        self.result["ranked_paths"] = [{"path": "owner.md", "unique_path_rank": 99}]
        checked = replay.check_observation(self.case, self.result, "fixture", "unique-path-rank")
        self.assertTrue(checked["passed"])
        self.assertEqual(checked["path_ranks"], {"peer.md": 1, "owner.md": 2})
        self.assertEqual(checked["rank_basis"], "unique-path-rank")
        self.assertFalse(self.check()["passed"])
        self.case["rankBasis"] = "unique-path-rank"
        self.assertTrue(self.check()["passed"])

    def test_unique_rank_does_not_hide_missing_forbidden_or_budget_failures(self):
        self.result["evidence"] = [{"path": "generated.jsonl"}]
        checked = replay.check_observation(self.case, self.result, "fixture", "unique-path-rank")
        self.assertFalse(checked["passed"])
        self.assertEqual(checked["missing"], ["owner.md"])
        self.assertEqual(checked["forbidden"], ["generated.jsonl"])
        self.result["evidence"] = [{"path": "owner.md"}] * 3
        with self.assertRaises(ValueError):
            replay.check_observation(self.case, self.result, "fixture", "unique-path-rank")

    def test_rank_basis_defaults_and_unknown_values_fail_closed(self):
        self.assertEqual(replay.rank_basis({}), "first-evidence-rank")
        self.assertEqual(replay.rank_basis({"rankBasis": "unique-path-rank"}), "unique-path-rank")
        for basis in (None, True, [], "unique", "score"):
            with self.subTest(basis=basis), self.assertRaises(ValueError):
                replay.validate_cases({"goldenRetrievalCases": [{**self.case, "rankBasis": basis}]})

    def test_rank_basis_must_be_case_level_and_definition_must_be_an_object(self):
        replay.validate_cases({"goldenRetrievalCases": [{**self.case, "rankBasis": "unique-path-rank"}]})
        for definition in ([], None, {"rankBasis": "unique-path-rank", "goldenRetrievalCases": [self.case]}):
            with self.subTest(definition=definition), self.assertRaises(ValueError):
                replay.validate_cases(definition)

    def test_missing_and_forbidden_paths_fail(self):
        self.result["evidence"] = [{"path": "generated.jsonl"}]
        checked = self.check()
        self.assertFalse(checked["passed"])
        self.assertEqual(checked["missing"], ["owner.md"])
        self.assertEqual(checked["forbidden"], ["generated.jsonl"])

    def test_invalid_receipts_fail_closed(self):
        for key, value in (("ok", False), ("profile", "other"), ("query", "rewritten"),
                           ("projection", "full"), ("snapshot_id", "other"),
                           ("evidence", [{}])):
            with self.subTest(key=key):
                result = copy.deepcopy(self.result)
                result[key] = value
                with self.assertRaises(ValueError):
                    self.check(result)
        for key in ("current", "fully_verified"):
            with self.subTest(key=key):
                result = copy.deepcopy(self.result)
                result["index_freshness"][key] = False
                with self.assertRaises(ValueError):
                    self.check(result)

    def test_output_budgets_cannot_be_widened(self):
        for paths in (["owner.md"] * 3, ["owner.md", "a", "b", "c", "d"]):
            with self.subTest(paths=paths), self.assertRaises(ValueError):
                self.check({**self.result, "evidence": [{"path": path} for path in paths]})

    def test_definitions_reject_unknown_assertions_and_empty_suites(self):
        replay.validate_cases({"goldenRetrievalCases": [self.case]})
        for cases in ([], [self.case, self.case], [{**self.case, "mustRankBefore": []}],
                      [{**self.case, "limit": True}], [{**self.case, "mustPaths": None}]):
            with self.subTest(cases=cases), self.assertRaises(ValueError):
                replay.validate_cases({"goldenRetrievalCases": cases})


if __name__ == "__main__":
    unittest.main()
