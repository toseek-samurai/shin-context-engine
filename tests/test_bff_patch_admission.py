#!/usr/bin/env python3
"""BFF admission distinguishes behavioral failures from broken environments."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from probe_bff_patch_admission import CLASS, CONTROLS, behavior_oracle, calibration_ok, test_results


class AdmissionTests(unittest.TestCase):
    def setUp(self):
        passed = {"execution": {"exit_code": 0, "timed_out": False},
                  "results": {"tests": 8, "errors": 0, "failures": 0, "skipped": 0,
                              "cases": [{"skipped": False, "failures": []} for _ in range(8)]}}
        failed = copy.deepcopy(passed)
        failed["execution"]["exit_code"] = 1
        failed["results"]["failures"] = 1
        failed["results"]["cases"][0]["failures"] = [{"type": "java.lang.AssertionError"}]
        self.variants = {name: copy.deepcopy(passed if name in {"fixed", "alternative-validation-and-wording"} else failed)
                         for name in {"parent", "fixed", *CONTROLS}}

    def test_reviewed_controls_pass(self):
        self.assertTrue(calibration_ok(self.variants))

    def test_missing_extra_or_undetected_control_refused(self):
        missing = copy.deepcopy(self.variants)
        del missing["storage-limit-removed"]
        self.assertFalse(calibration_ok(missing))
        extra = copy.deepcopy(self.variants)
        extra["unknown"] = extra["fixed"]
        self.assertFalse(calibration_ok(extra))
        self.variants["storage-limit-removed"] = copy.deepcopy(self.variants["fixed"])
        self.assertFalse(calibration_ok(self.variants))

    def test_alternative_must_pass(self):
        self.variants["alternative-validation-and-wording"] = copy.deepcopy(self.variants["parent"])
        self.assertFalse(calibration_ok(self.variants))

    def test_compilation_timeout_or_context_failure_is_not_bug_detection(self):
        for value in [None, {"tests": 1, "errors": 0, "failures": 1, "skipped": 0, "cases": []}]:
            altered = copy.deepcopy(self.variants)
            altered["parent"]["results"] = value
            self.assertFalse(calibration_ok(altered))
        self.variants["parent"]["results"]["cases"][0]["failures"][0]["type"] = "java.lang.IllegalStateException"
        self.assertFalse(calibration_ok(self.variants))

    def test_timeout_cannot_use_old_xml_as_success(self):
        self.variants["fixed"]["execution"]["timed_out"] = True
        self.assertFalse(calibration_ok(self.variants))

    def test_missing_skipped_or_inconsistent_tests_refused(self):
        for field, value in [("tests", 7), ("skipped", 1), ("failures", 2)]:
            altered = copy.deepcopy(self.variants)
            altered["parent"]["results"][field] = value
            self.assertFalse(calibration_ok(altered))

    def test_cosmetic_oracle_change_preserves_error_code(self):
        original = b'before.contains("PERMISSION_DESCRIPTION", "source permission");after'
        self.assertEqual(behavior_oracle(original), b'before.contains("PERMISSION_DESCRIPTION");after')
        for malformed in [b"missing", original + original]:
            with self.assertRaises(ValueError):
                behavior_oracle(malformed)

    def test_junit_results_are_structured_and_missing_is_not_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.assertIsNone(test_results(root))
            report = root / "build/test-results/test" / ("TEST-" + CLASS + ".xml")
            report.parent.mkdir(parents=True)
            report.write_text('<testsuite tests="2" failures="1" errors="0" skipped="0">'
                              '<testcase name="good"/><testcase name="bad"><failure type="java.lang.AssertionError" '
                              'message="expected &lt;blocked&gt;"/></testcase></testsuite>')
            result = test_results(root)
            self.assertEqual(result["failures"], 1)
            self.assertEqual(result["cases"][1]["failures"][0]["message"], "expected <blocked>")

    def test_public_candidate_excludes_private_reference(self):
        folder = Path(__file__).parent / "fixtures/agent-task-candidates/bff-permission-release-v1"
        candidate = json.loads((folder / "candidate.json").read_text())
        prompt = (folder / candidate["agent_prompt"]).read_text()
        private = candidate["private_evaluation"]
        for value in [private["reference_fix"], private["source_parent"], private["oracle_source"], "QueryReleaseService.java"]:
            self.assertNotIn(value, prompt)
        self.assertEqual(private["required_behavior_cases"], 8)
        self.assertFalse(candidate["model_run_authorized"])


if __name__ == "__main__":
    unittest.main()
