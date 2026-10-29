"""Resource policy changes admission, never retrieval or quality assertions."""

import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOL = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[2]


class ResourcePolicyTests(unittest.TestCase):
    def test_quality_verdict_is_separate_from_current_retrieval_readiness(self):
        (self.root / 'docs/quality-target.md').write_text('# Other\nUnrelated material.\n')
        self.profile['evalCases'][0]['mustPaths'] = ['docs/quality-target.md']
        self.save(rebuild=True)
        for args in (('eval', '--strict'), ('eval', '--failed-only', '--strict')):
            result = self.call(*args, code=1)
            self.assertFalse(result['ok'])
            self.assertTrue(result['quality_failure_only'])
        self.assertTrue(self.call('validate', '--summary', '--strict', code=1)['retrieval_ready'])
        ready = self.call('ready', '--strict', code=1)
        self.assertFalse(ready['ready'])
        self.assertTrue(ready['retrieval_ready'])
        with (self.root / 'README.md').open('a') as stream:
            stream.write('\nchanged source\n')
        self.assertFalse(self.call('ready', '--strict', code=1)['retrieval_ready'])

    def test_truncated_quality_diagnostics_cannot_hide_negative_path_failure(self):
        case = copy.deepcopy(self.profile['evalCases'][0])
        (self.root / 'docs/quality-target.md').write_text('# Other\nUnrelated material.\n')
        case.update(mustPaths=['docs/quality-target.md'], queryVariants=[])
        self.profile['evalCases'] = [{**case, 'id': f'missing-{i}'} for i in range(40)]
        self.save(rebuild=True)
        result = self.call('eval', '--strict', '--failed-only', code=1)
        self.assertTrue(result['failure_budget']['truncated'])
        self.assertTrue(result['quality_failure_only'])
        self.profile['evalCases'][-1].update(mustPaths=[], mustTopPaths=[], mustNotPaths=['docs/architecture.md'])
        self.save(rebuild=True)
        self.assertFalse(self.call('eval', '--strict', '--failed-only', code=1)['quality_failure_only'])
        self.assertFalse(self.call('validate', '--strict', '--summary', code=1)['retrieval_ready'])

    def test_pack_quality_miss_is_advisory_but_payload_bounds_are_not(self):
        (self.root / 'docs/quality-target.md').write_text('# Other\nUnrelated material.\n')
        self.profile['packEvalCases'][0]['mustPaths'] = ['docs/quality-target.md']
        self.save(rebuild=True)
        self.assertTrue(self.call('pack-eval', '--strict', '--failed-only', code=1)['quality_failure_only'])
        self.assertTrue(self.call('ready', '--strict', code=1)['retrieval_ready'])
        self.profile['packEvalCases'][0]['maxEvidenceBytes'] = 1
        self.save(rebuild=True)
        self.assertFalse(self.call('pack-eval', '--strict', '--failed-only', code=1)['quality_failure_only'])
        self.assertFalse(self.call('ready', '--strict', code=1)['retrieval_ready'])

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="shin-resource-policy-")
        self.addCleanup(temp.cleanup)
        self.base = Path(temp.name)
        self.root = self.base / "root"
        shutil.copytree(ROOT / "tests/fixtures/basic/root", self.root)
        (self.root / "docs/kb").mkdir(exist_ok=True)
        self.registry = self.base / "profiles"
        self.registry.mkdir()
        self.profile_path = self.registry / "fixture.json"
        self.profile = json.loads((ROOT / "tests/fixtures/basic/profiles/fixture.json").read_text())
        self.profile["evalCases"] = [row for row in self.profile["evalCases"]
                                     if row["id"] in ("fixture-capability-contract", "fixture-widget-source-fact")]
        self.prefix = [TOOL, "--root", str(self.root), "--profile", "fixture",
                       "--profile-dir", str(self.registry), "--out-dir", str(self.base / "index")]
        self.save()
        self.call("build")

    def call(self, *args, code=0):
        run = subprocess.run([*self.prefix, *args], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, code, run.stdout + run.stderr)
        return json.loads(run.stdout or run.stderr)

    def save(self, rebuild=False):
        self.profile_path.write_text(json.dumps(self.profile))
        if rebuild:
            self.call("build")

    def exceed(self, policy=None):
        if policy is not None:
            self.profile["evalResourcePolicy"] = policy
        self.profile["evalCases"][0].update(maxCandidateRows=0, maxMatchedSections=0)
        self.profile["evalCases"][1]["maxMatchedFacts"] = 0
        self.save(rebuild=True)

    def test_default_and_explicit_strict_preserve_failures(self):
        self.exceed()
        result = self.call("eval", "--strict", code=1)
        self.assertTrue(result["quality_ok"])
        self.assertFalse(result['quality_failure_only'])
        self.assertFalse(self.call('ready', '--strict', code=1)['retrieval_ready'])
        self.assertEqual(result["resource_check"], {
            "policy": "strict", "ok": False, "failed_queries": 4, "warning_queries": 0})
        self.assertEqual(result["warnings"], [])
        self.assertEqual(result["query_summary"], {"total": 4, "passed": 0, "failed": 4})
        self.profile["evalResourcePolicy"] = "strict"
        self.save(rebuild=True)
        explicit = self.call("eval", "--strict", code=1)
        self.assertEqual(result["cases"], explicit["cases"])

    def test_advisory_preserves_evidence_and_surfaces_all_projections(self):
        baseline = self.call("eval", "--strict")
        self.exceed("advisory")
        result = self.call("eval", "--strict")
        self.assertTrue(result["ok"] and result["quality_ok"])
        self.assertFalse(result["resource_check"]["ok"])
        self.assertEqual(result["resource_check"]["warning_queries"], 4)
        for old, new in zip(baseline["cases"], result["cases"]):
            for field in ("returned_paths", "ranked_paths", "path_ranks", "query_diagnostics", "query_coverage"):
                self.assertEqual(old[field], new[field], field)
            self.assertTrue(new["ceiling_violations"])
            self.assertFalse(new["resources_passed"])
        failed = self.call("eval", "--failed-only", "--strict")
        self.assertEqual(failed["failures"], [])
        self.assertEqual(failed["warnings"], result["warnings"])
        validation = self.call("validate", "--summary", "--strict")
        self.assertEqual(validation["warnings"], result["warnings"])
        ready = self.call("ready", "--strict")
        check = next(step["result"] for step in ready["steps"] if step["id"] == "validate")
        self.assertEqual(check["warnings"], result["warnings"])
        self.assertFalse(check["resource_check"]["ok"])

    def test_quality_coverage_rank_forbidden_and_type_still_block(self):
        self.exceed("advisory")
        original = copy.deepcopy(self.profile)
        for patch in ({"minCoveredGroups": 100}, {"mustPaths": ["missing.md"]},
                      {"mustTopPaths": ["missing.md"]},
                      {"mustNotPaths": ["docs/architecture.md"], "mustPaths": [], "mustTopPaths": []},
                      {"expectedRecordTypes": ["absent-type"]},
                      {"mustRankBefore": [{"higher": "missing.md", "lower": "docs/architecture.md"}]}):
            with self.subTest(patch=patch):
                self.profile = copy.deepcopy(original)
                self.profile["evalCases"][0].update(patch)
                self.save(rebuild=True)
                result = self.call("eval", "--strict", code=1)
                self.assertFalse(result["quality_ok"])
                self.assertEqual(result['quality_failure_only'], 'mustNotPaths' not in patch)
                self.assertFalse(result["cases"][0]["query_passed"])
                self.assertTrue(result["warnings"])

    def test_profile_change_and_source_drift_still_block(self):
        self.profile["evalResourcePolicy"] = "advisory"
        self.save()
        self.call("ready", "--strict", code=1)
        self.exceed("advisory")
        self.call("ready", "--strict")
        with (self.root / "README.md").open("a") as stream:
            stream.write("\nchanged source\n")
        self.call("ready", "--strict", code=1)

    def test_invalid_policy_is_not_silent_opt_in(self):
        for value in (None, True, 1, {}, [], "warn", "ADVISORY"):
            with self.subTest(value=value):
                self.profile["evalResourcePolicy"] = value
                self.save()
                self.call("profile-check", "--strict", code=1)
                self.call("eval", "--strict", code=2)

    def test_safety_and_pack_still_block(self):
        self.exceed("advisory")
        self.profile["packEvalCases"][0]["maxEvidenceBytes"] = 1
        self.save(rebuild=True)
        result = self.call("ready", "--strict", code=1)
        validation = next(step["result"] for step in result["steps"] if step["id"] == "validate")
        self.assertIn("pack-eval", [gate["id"] for gate in validation["failed_gates"]])
        self.assertEqual(validation["resource_check"]["warning_queries"], 4)
        (self.root / "docs/unsafe.md").write_text("# Credential\n\nsecret = sk-" + "Q7x9" * 16 + "\n")
        doctor = self.call("doctor", "--strict", code=1)
        self.assertGreater(doctor["counts"]["errors"], 0, "unpublished finding must not be accepted")
        self.call("build")
        result = self.call("ready", "--strict", code=1)
        validation = next(step["result"] for step in result["steps"] if step["id"] == "validate")
        self.assertEqual(validation["summary"]["failure_classes"]["safety_findings"], 0)
        self.assertIn("pack-eval", [gate["id"] for gate in validation["failed_gates"]])
        self.assertEqual(self.call("doctor", "--strict")["counts"]["quarantined_files"], 1)

    def test_warnings_are_bounded_with_explicit_omissions(self):
        self.exceed("advisory")
        case = self.profile["evalCases"][0]
        self.profile["evalCases"] = [{**case, "id": f"resource-{i}"} for i in range(12)]
        self.save(rebuild=True)
        result = self.call("eval", "--failed-only", "--strict")
        self.assertEqual(result["resource_check"]["warning_queries"], 36)
        self.assertEqual(result["warning_budget"]["returned"], 8)
        self.assertEqual(result["warning_budget"]["omitted"], 28)
        self.assertLessEqual(len(json.dumps(result["warnings"], separators=(",", ":")).encode()), 4098)


if __name__ == "__main__":
    unittest.main()
