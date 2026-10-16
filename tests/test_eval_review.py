#!/usr/bin/env python3
"""Eval review stays advisory, bounded and independent of Core admission."""

import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("eval_review", ROOT / "tools/shin_eval_review.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def query(identity="fixture", candidates=802, cap=850, passed=True):
    return {"id": identity, "variant_index": -1, "query": "why empty documents are hidden", "query_passed": passed,
            "passed": passed, "query_diagnostics": {"candidate_rows": candidates}, "matched_sections": 85,
            "matched_facts": 0, "max_candidate_rows": cap, "max_matched_sections": 100, "max_matched_facts": -1,
            "query_coverage": {"covered_groups": 4}, "min_covered_groups": 4,
            "missing_paths": [], "missing_top_paths": [], "unexpected_paths": [], "missing_record_types": [],
            "rank_violations": [], "ceiling_violations": []}


def full(rows=None, ok=True):
    rows = [query()] if rows is None else rows
    queries = [item for row in rows for item in [row, *row.get("query_variants", [])]]
    passed = sum(row["query_passed"] for row in queries)
    return {"schema_version": "shin.eval.v1", "tool": "shin-context", "command": "eval", "ok": ok,
            "profile": "fixture", "snapshot_id": "fnv1a64:0123456789abcdef",
            "index_freshness": {"current": True, "fully_verified": True, "status": "current"},
            "query_summary": {"total": len(queries), "passed": passed, "failed": len(queries) - passed}, "cases": rows}


def failed_only():
    row = {"id": "fixture", "variant_index": 2, "query": "why empty documents are hidden", "detail_level": "full",
           "failure_reasons": ["retrieval-ceiling-violated"], "missing_paths": [], "missing_top_paths": [],
           "unexpected_paths": [], "missing_record_types": [], "rank_violations": [],
           "ceiling_violations": ["candidate_rows"], "ceiling_violation_details": [
               {"metric": "candidate_rows", "actual": 802, "allowed": 800, "comparator": "<=", "stage": "candidate-discovery"}]}
    report = full(ok=False)
    report.pop("cases")
    report.update(schema_version="shin.eval-failures.v1", failures=[row], query_summary={"total": 89, "passed": 88, "failed": 1})
    return report


class ReviewTests(unittest.TestCase):
    def test_explicit_advisory_with_independent_resource_result(self):
        value = full([query(candidates=871)])
        value["resource_check"] = {"policy": "advisory", "ok": False,
                                   "failed_queries": 1, "warning_queries": 1}
        result = self.review(value)
        self.assertTrue(result["ok"])
        self.assertFalse(result["resource_check"]["ok"])
        self.assertEqual(result["queries"][0]["classification"], "resource-warning")
        value["cases"][0]["min_covered_groups"] = 5
        with self.assertRaises(ValueError):
            self.review(value)

    def test_invalid_resource_policy_or_counters_rejected(self):
        for check in ({"policy": "auto", "ok": True, "failed_queries": 0, "warning_queries": 0},
                      {"policy": "advisory", "ok": True, "failed_queries": 1, "warning_queries": 1},
                      {"policy": "strict", "ok": True, "failed_queries": 0, "warning_queries": 1}):
            value = full()
            value["resource_check"] = check
            with self.assertRaises(ValueError):
                self.review(value)

    def review(self, value, limit=8):
        before = copy.deepcopy(value)
        result = module.review(value, "a" * 64, limit)
        self.assertEqual(value, before)
        return result

    def test_headroom_and_units(self):
        result = self.review(full())
        limit = result["queries"][0]["resource_limits"][0]
        self.assertEqual((limit["actual"], limit["allowed"], limit["headroom_percent"]), (802, 850, 5.65))
        self.assertEqual(result["queries"][0]["classification"], "passed")
        self.assertIn("not-live-readiness", result["scope"])

    def test_resource_only_historical_failure(self):
        result = self.review(failed_only())
        self.assertFalse(result["ok"])
        row = result["queries"][0]
        self.assertEqual(row["classification"], "resource-only-failure")
        self.assertEqual(row["next_action"], "compare-explicit-pinned-publications")
        self.assertEqual(row["resource_limits"][0]["headroom"], -2)
        self.assertFalse(row["limits_complete"])
        self.assertEqual(result["source_successful_queries_omitted"], 88)

    def test_variants_use_query_not_aggregate_pass(self):
        base, variant = query(), query(candidates=851, passed=False)
        variant["variant_index"] = 0
        base.update(passed=False, query_variants=[variant])
        result = self.review(full([base], ok=False))
        self.assertEqual(result["observed_classifications"], {"passed": 1, "resource-only-failure": 1})
        self.assertEqual(result["queries"][0]["variant_index"], 0)

    def test_quality_negatives_cannot_be_reclassified_as_resources(self):
        for field, failure in (("missing_paths", "missing.md"), ("missing_top_paths", "required.md"),
                               ("unexpected_paths", "forbidden.md"), ("missing_record_types", "contract"),
                               ("rank_violations", {"reason": "order-violated"})):
            with self.subTest(field=field):
                row = query(passed=False)
                row[field] = [failure]
                self.assertEqual(self.review(full([row], ok=False))["queries"][0]["classification"], "quality-failure")
                row["max_candidate_rows"] = 800
                self.assertEqual(self.review(full([row], ok=False))["queries"][0]["classification"], "quality-and-resource-failure")

    def test_minimum_coverage_is_quality_not_resource_budget(self):
        row = query(passed=False)
        row["min_covered_groups"] = 5
        result = self.review(full([row], ok=False))
        self.assertEqual(result["queries"][0]["classification"], "quality-failure")
        historical = failed_only()
        historical["failures"][0]["ceiling_violation_details"] = [
            {"metric": "covered_groups", "actual": 4, "allowed": 5, "comparator": ">=", "stage": "coverage"}]
        historical["failures"][0]["ceiling_violations"] = ["covered_groups"]
        self.assertEqual(self.review(historical)["queries"][0]["classification"], "quality-failure")

    def test_headers_and_unknown_reasons_are_not_quality_passes(self):
        for change in ({"detail_level": "header"}, {"failure_reasons": ["future-failure"]},
                       {"ceiling_violation_details": []}):
            report = failed_only()
            report["failures"][0].update(change)
            row = self.review(report)["queries"][0]
            self.assertEqual(row["classification"], "incomplete-or-unknown")
            self.assertEqual(row["quality_status"], "unknown")

    def test_omitted_failures_remain_visible(self):
        report = failed_only()
        report["failures"] = []
        result = self.review(report)
        self.assertFalse(result["ok"])
        self.assertEqual(result["source_failed_queries_omitted"], 1)

    def test_stale_result_remains_failed_despite_passed_queries(self):
        report = full(ok=False)
        report["index_freshness"].update(current=False, fully_verified=False, status="stale")
        result = self.review(report)
        self.assertFalse(result["ok"])
        self.assertFalse(result["freshness_at_capture"]["current"])
        self.assertFalse(result["automatic_actions"])

    def test_wrong_command_and_corrupt_schema_rejected(self):
        for patch in ({"schema_version": "shin.validation-summary.v2"}, {"command": "doctor"}, {"tool": "other"}, {"ok": 1}):
            report = full()
            report.update(patch)
            with self.assertRaises(ValueError):
                self.review(report)

    def test_inconsistent_success_and_duplicate_queries_rejected(self):
        with self.assertRaises(ValueError):
            self.review(full([query(cap=800, passed=False)], ok=True))
        with self.assertRaises(ValueError):
            self.review(full([query(), query()]))
        report = full()
        report.update(strict=True)
        report["index_freshness"]["current"] = False
        with self.assertRaises(ValueError):
            self.review(report)

    def test_negative_disabled_and_zero_caps(self):
        row = query(candidates=0, cap=0)
        row["max_matched_sections"] = -1
        result = self.review(full([row]))["queries"][0]["resource_limits"]
        self.assertEqual(len(result), 1)
        self.assertIsNone(result[0]["headroom_percent"])
        self.assertFalse(result[0]["exceeded"])
        for invalid in (-2, True, 1.5):
            row["max_candidate_rows"] = invalid
            with self.assertRaises(ValueError):
                self.review(full([row]))

    def test_whole_record_output_budget_and_truncated_query(self):
        rows = [query(identity=str(i) + "x" * 1000) for i in range(128)]
        for row in rows:
            row["query"] = "\u042f" * 16384
        result = self.review(full(rows), limit=32)
        self.assertLessEqual(len(module.encode(result)) + 1, module.MAX_OUTPUT)
        self.assertLess(result["query_budget"]["returned"], 32)
        self.assertEqual(result["query_budget"]["returned"] + result["query_budget"]["omitted"], 128)
        self.assertTrue(result["queries"][0]["query_truncated"])

    def test_cli_read_only_exit_and_malformed_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "eval.json"
            for value, expected in ((full(), 0), (failed_only(), 1), ({"not": "an eval"}, 2)):
                path.write_text(json.dumps(value))
                original = path.read_bytes()
                result = subprocess.run([sys.executable, str(ROOT / "tools/shin_eval_review.py"), "--eval", str(path)], capture_output=True, timeout=10)
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertEqual(result.stderr, b"")
                self.assertEqual(path.read_bytes(), original)
                self.assertEqual(sorted(p.name for p in path.parent.iterdir()), ["eval.json"])
            for raw in ('{"ok":true,"ok":false}', '{"x":NaN}', '{'):
                path.write_text(raw)
                with self.assertRaises(ValueError):
                    module.load(path)
            link = path.parent / "symlink.json"
            link.symlink_to(path)
            with self.assertRaises(OSError):
                module.load(link)


if __name__ == "__main__":
    unittest.main()
