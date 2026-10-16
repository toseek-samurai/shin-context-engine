#!/usr/bin/env python3
"""Check contract-summary navigation against a competing procedural guide."""

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = Path(sys.argv.pop(1)).resolve(strict=True)
ROOT = Path(sys.argv.pop(1)).resolve(strict=True)
VERIFY_ORIGINAL_MISS = "--verify-original-miss" in sys.argv
if VERIFY_ORIGINAL_MISS:
    sys.argv.remove("--verify-original-miss")
OLD_SUMMARY = (
    "Core provides a deterministic read-only lifecycle plan from cold-start discovery "
    "to explicit permanent adoption without generating consumer policy or mutating the consumer repository."
)


class AdoptionSummaryTests(unittest.TestCase):
    def test_summary_exposes_the_actual_read_only_output_contract(self):
        current = json.loads((ROOT / "docs/machine/adoption-planner-v1.json").read_text())
        old = copy.deepcopy(current)
        old.pop("title", None)
        old["summary"] = OLD_SUMMARY
        self.assertEqual(current["output"]["projectMutationPerformed"], False)
        self.assertTrue({"gaps", "commands"} <= set(current["output"]["sections"]))
        with tempfile.TemporaryDirectory(prefix="shin-adoption-summary-") as temporary:
            base = Path(temporary)
            source = base / "source"
            (source / "docs").mkdir(parents=True)
            registry = base / "profiles"
            registry.mkdir()
            profile = {"schemaVersion": "shin.profile.v1", "id": "probe",
                       "sourceLabel": "Contract and procedure navigation", "rootFiles": [],
                       "sourceExtensions": [".md", ".json"], "jsonKeySections": True,
                       "jsonKeyMaxDepth": 1, "jsonKeyMaxSectionsPerFile": 200,
                       "jsonSchemaTitle": True, "jsonSummaryKeys": True,
                       "markdownChunkLines": 0, "evalCases": [], "topicRules": []}
            (registry / "probe.json").write_text(json.dumps(profile))
            (source / "docs/guide.md").write_text(
                "# Read-Only Adoption Planner\n\n"
                "The adoption planner reports lifecycle gaps and recommended commands without writes.\n")
            prefix = [str(TOOL), "--profile", "probe", "--profile-dir", str(registry),
                      "--root", str(source), "--out-dir", str(base / "index")]

            def invoke(*arguments):
                result = subprocess.run([*prefix, *arguments], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return json.loads(result.stdout)

            cases = [("current", current, True)]
            if VERIFY_ORIGINAL_MISS:
                cases.insert(0, ("original", old, False))
            for label, contract, expected in cases:
                with self.subTest(label=label):
                    (source / "docs/owner.json").write_text(json.dumps(contract, indent=2) + "\n")
                    self.assertTrue(invoke("build")["ok"])
                    result = invoke("brief", "read only adoption planner gaps commands",
                                    "--projection", "compact", "--limit", "8", "--per-doc", "2",
                                    "--require-current")
                    paths = [row["path"] for row in result["ranked_paths"][:3]]
                    self.assertEqual("docs/owner.json" in paths, expected, result)


if __name__ == "__main__":
    unittest.main()
