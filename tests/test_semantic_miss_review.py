#!/usr/bin/env python3
"""Diagnostic logic only; no model, network, source mutation or consumer index."""

from types import SimpleNamespace
import unittest

from check_semantic_answer_ranges import attainability, covered, inspect
from probe_semantic_token_windows import MODEL, windows
from review_semantic_misses import classify, input_budget, rank


class CharacterTokenizer:
    """Offsets and special-token budgeting independent of a provisioned model."""

    def __init__(self):
        self.limit = None

    def no_truncation(self):
        self.limit = None

    def no_padding(self):
        pass

    def enable_truncation(self, max_length):
        self.limit = max_length

    def encode(self, text, add_special_tokens=True):
        offsets = [(i, i + 1) for i in range(len(text))]
        if add_special_tokens:
            offsets = [(0, 0), *offsets, (0, 0)]
        if self.limit is not None and len(offsets) > self.limit:
            offsets = [*offsets[:self.limit - 1], (0, 0)]
        return SimpleNamespace(ids=list(range(len(offsets))), offsets=offsets)


class MissReviewTests(unittest.TestCase):
    def test_rank_uses_first_expected_file_and_is_one_based(self):
        self.assertEqual(rank([{"path": "a"}, {"path": "b"}, {"path": "c"}], {"b", "c"}), 2)
        self.assertIsNone(rank([{"path": "a"}], {"b"}))
        self.assertIsNone(rank([], set()))

    def test_stages_do_not_confuse_enrollment_recall_and_selection(self):
        self.assertEqual(classify(0, None, None, None), "expected-source-not-enrolled")
        self.assertEqual(classify(3, None, None, None), "candidate-recall-miss")
        self.assertEqual(classify(3, None, 5, 6), "first-five-lost-in-selection")
        self.assertEqual(classify(3, 1, None, None), "first-five-lost-in-selection")
        self.assertEqual(classify(3, None, 7, 6), "candidate-present-outside-first-five")

    def test_budget_counts_prefix_special_tokens_and_visible_characters(self):
        tokenizer = CharacterTokenizer()
        unit = {"text": "abcdef", "path": "a", "start_line": 1, "end_line": 2,
                "source_sha256": "a" * 64, "record_ids": ["source-a"]}
        result = input_budget(unit, tokenizer, 12)
        self.assertEqual(result["input_tokens"], 17)
        self.assertEqual(result["visible_tokens"], 12)
        self.assertEqual(result["visible_characters"], 10)
        self.assertTrue(result["token_truncated"])
        self.assertNotIn("text", result)
        self.assertEqual(input_budget(unit, tokenizer, 17)["token_truncated"], False)


class WindowTests(unittest.TestCase):
    policy = {"max_tokens": 128, "max_header_tokens": 64, "overlap_tokens": 8, "max_windows_per_parent": 3}
    prefix = "file.cpp\ncpp function\nSource range: "

    def project(self, body, **overrides):
        tokenizer = CharacterTokenizer()
        return windows(self.prefix + body, tokenizer, {**self.policy, **overrides})

    def test_short_parent_is_not_duplicated(self):
        self.assertEqual(self.project("short body"), ([], "fits"))

    def test_long_input_complete_and_every_window_fits(self):
        body = "".join(chr(0x400 + i) for i in range(180))
        rows, status = self.project(body)
        self.assertEqual(status, "complete-captured-body")
        covered = set()
        for row in rows:
            self.assertEqual(row["text"], self.prefix + body[row["body_start"]:row["body_end"]])
            self.assertLessEqual(len(MODEL["passage_prefix"] + row["text"]) + 2, self.policy["max_tokens"])
            covered.update(range(row["body_start"], row["body_end"]))
        self.assertEqual(covered, set(range(len(body))))
        self.assertLess(rows[1]["body_start"], rows[0]["body_end"])

    def test_cap_reports_incomplete_coverage(self):
        rows, status = self.project("x" * 400, max_windows_per_parent=1)
        self.assertEqual(status, "window-budget")
        self.assertEqual(len(rows), 1)
        self.assertLess(rows[-1]["body_end"], 400)

    def test_large_header_is_not_silently_rewritten(self):
        rows, status = windows("x" * 200 + "\nSource range: body", CharacterTokenizer(), self.policy)
        self.assertEqual((rows, status), ([], "header-budget"))

    def test_missing_body_is_explicit(self):
        self.assertEqual(windows("x" * 200, CharacterTokenizer(), self.policy), ([], "no-captured-body"))

    def test_invalid_overlap_rejected(self):
        with self.assertRaisesRegex(ValueError, "Invalid window policy"):
            self.project("x" * 400, overlap_tokens=100)


class AnswerRangeTests(unittest.TestCase):
    review = {"project": "shin", "path": "source.cpp", "source_sha256": "a" * 64,
              "clauses": [{"id": "failure", "spans": [[10, 20], [30, 40]]}]}

    def row(self, start, end, **extra):
        return {"project": "shin", "path": "source.cpp", "source_sha256": "a" * 64,
                "start_line": start, "end_line": end, **extra}

    def test_expected_file_is_not_an_answer_range(self):
        result = inspect([self.row(1, 3)], self.review, 5)
        self.assertEqual(result["first_expected_path_rank"], 1)
        self.assertEqual(result["covered_clauses"], 0)
        self.assertFalse(result["all_required_ranges_covered"])

    def test_union_requires_no_missing_line(self):
        self.assertTrue(covered([(15, 20), (10, 14)], 10, 20))
        self.assertFalse(covered([(10, 14), (16, 20)], 10, 20))
        self.assertFalse(covered([], 10, 20))
        self.assertTrue(covered([(1, 30), (3, 5)], 10, 20))

    def test_all_spans_needed_and_output_limit_preserved(self):
        rows = [self.row(10, 20), self.row(30, 40)]
        self.assertFalse(inspect(rows, self.review, 1)["all_required_ranges_covered"])
        result = inspect(rows, self.review, 2)
        self.assertTrue(result["all_required_ranges_covered"])
        self.assertEqual(result["supplied_snippet_sufficiency"], "not-assessed-navigation-does-not-deliver-these-source-bodies")
        self.assertFalse(result["runtime_proof"])

    def test_wrong_generation_rejected(self):
        with self.assertRaisesRegex(ValueError, "source differs"):
            inspect([self.row(1, 40, source_sha256="b" * 64)], self.review, 5)

    def test_other_project_and_test_do_not_supply_implementation(self):
        rows = [self.row(1, 40, project="other"), self.row(1, 40, path="source_test.cpp")]
        result = inspect(rows, self.review, 5)
        self.assertIsNone(result["first_expected_path_rank"])
        self.assertFalse(result["all_required_ranges_covered"])

    def test_capacity_is_independent_of_retrieved_rank(self):
        review = {**self.review, "clauses": [{"id": str(i), "spans": [[i, i + 4]]} for i in (10, 20, 30)]}
        units = [self.row(i, i + 4) for i in (10, 20, 30)]
        result = attainability(units, review, 2)
        self.assertEqual(result["minimum_parent_ranges"], 3)
        self.assertEqual(result["maximum_covered_clauses_with_cap"], 2)
        self.assertFalse(result["complete_navigation_attainable"])
        self.assertTrue(attainability(units, review, 3)["complete_navigation_attainable"])

    def test_existing_enclosing_range_can_satisfy_multiple_clauses(self):
        result = attainability([self.row(10, 20), self.row(1, 40), self.row(30, 40)], self.review, 1)
        self.assertEqual(result["minimum_parent_ranges"], 1)
        self.assertEqual(result["minimum_range_witness"], [[1, 40]])
        self.assertTrue(result["complete_navigation_attainable"])

    def test_missing_enrollment_is_not_a_capacity_failure(self):
        result = attainability([self.row(10, 20)], self.review, 2)
        self.assertEqual(result["status"], "required-range-not-enrolled")
        self.assertEqual(result["first_uncovered_line"], 30)
        self.assertIsNone(result["minimum_parent_ranges"])

    def test_attainability_remains_bounded(self):
        with self.assertRaisesRegex(ValueError, "capacity"):
            attainability([], self.review, 5)
        with self.assertRaisesRegex(ValueError, "unit budget"):
            attainability([self.row(1, 40) for _ in range(25)], self.review, 2)


if __name__ == "__main__":
    unittest.main()
