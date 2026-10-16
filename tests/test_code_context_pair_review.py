#!/usr/bin/env python3
"""P4 accounting tests; no inference or semantic-judgment automation."""

from copy import deepcopy
from pathlib import Path
import unittest

from check_code_context_pair import thresholds, answer_available
from shin_semantic.artifact import decode


class ReviewTests(unittest.TestCase):
    def setUp(self):
        self.policy = decode((Path(__file__).parent / "fixtures/code-context-pilot-policy.json").read_bytes())
        self.rows = []
        for topic in [row["id"] for row in self.policy["topics"]] + ["writer"]:
            for language in ("english", "russian"):
                for variant in ("baseline", "candidate"):
                    self.rows.append({"id": topic + "-" + language, "variant": variant,
                        "group": "writer" if topic == "writer" else "new", "language": language,
                        "passed": True, "supported_clauses": 4 if topic == "writer" else 3,
                        "unsupported_claims": [], "bytes": 1000 if variant == "baseline" else 800,
                        "tool_seconds": 1.0, "seconds": 30.0, "native_seconds": [0.2]})

    def candidate(self):
        return [row for row in self.rows if row["variant"] == "candidate"]

    def test_twenty_percent_is_exact_and_no_token_saving_is_inferred(self):
        result = thresholds(self.rows, self.policy)
        self.assertTrue(result["passed"])
        self.assertEqual(result["common_completed_bytes"], {"baseline": 10000, "candidate": 8000})
        self.candidate()[0]["bytes"] += 1
        self.assertFalse(thresholds(self.rows, self.policy)["checks"]["additional_task_or_twenty_percent_bytes"])

    def test_one_additional_task_is_alternative_not_relabelled_old_failure(self):
        for row in self.candidate():
            row["bytes"] = 1100
        self.rows[0]["passed"] = False
        self.rows[0]["supported_clauses"] = 2
        result = thresholds(self.rows, self.policy)
        self.assertTrue(result["passed"])
        self.assertEqual(result["completed"], {"baseline": 9, "candidate": 10})
        self.assertEqual(len(result["common_completed_tasks"]), 9)

    def test_clause_regression_and_unsupported_claim_are_independent_failures(self):
        row = self.candidate()[0]
        row.update(passed=False, supported_clauses=2, unsupported_claims=["unread behavior"])
        result = thresholds(self.rows, self.policy)
        self.assertEqual(result["regressions"], [row["id"]])
        self.assertFalse(result["checks"]["no_unsupported_candidate_claims"])
        self.assertFalse(result["passed"])

    def test_writer_and_per_language_gates_cannot_be_hidden_by_total(self):
        row = next(row for row in self.candidate() if row["group"] == "writer")
        row.update(passed=False, supported_clauses=3)
        self.assertFalse(thresholds(self.rows, self.policy)["checks"]["candidate_writer"])
        for row in [row for row in self.candidate() if row["group"] == "new" and row["language"] == "russian"][:2]:
            row["passed"] = False
        self.assertFalse(thresholds(self.rows, self.policy)["checks"]["candidate_each_language"])

    def test_cost_limits_remain_separate_from_correctness(self):
        for field, value, check in (("native_seconds", [5.01], "candidate_native_max"),
                                    ("tool_seconds", 30.01, "task_tool_budget"),
                                    ("seconds", 180.01, "task_deadline"),
                                    ("bytes", 65537, "response_budget")):
            with self.subTest(field=field):
                rows = deepcopy(self.rows)
                rows[1][field] = value
                result = thresholds(rows, self.policy)
                self.assertFalse(result["checks"][check])
                self.assertEqual(result["completed"]["candidate"], 10)

    def test_median_ratio_does_not_use_only_faster_cases(self):
        for row in self.candidate():
            row["tool_seconds"] = 1.26
        self.assertFalse(thresholds(self.rows, self.policy)["checks"]["tool_median_ratio"])

    def test_quota_refusal_and_supported_final_answer_are_separate_from_admission_failure(self):
        receipt = {"completed": True, "failure": None, "stop_reason": "pilot-response-budget"}
        self.assertTrue(answer_available(receipt))
        for stopped in ("pilot-admission-rejected", "pilot-snapshot-changed", "pilot-task-deadline"):
            receipt["stop_reason"] = stopped
            self.assertFalse(answer_available(receipt))
        receipt.update(stop_reason="pilot-response-budget", failure="pilot-agent-called-after-stop")
        self.assertFalse(answer_available(receipt))


if __name__ == "__main__":
    unittest.main()
