#!/usr/bin/env python3
"""Fail-closed mutation audit checks; no model or external service required."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
import unittest

from prepare_agent_workflow_probe import RULE, audit, inventory, write


class WorkflowAuditTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shin-probe-audit-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.output = self.base / "report"
        self.case = self.base / "outside-project"
        self.project = self.case / "project"
        self.working = self.case / "cache/working"
        self.profile_path = self.project / "docs/machine/shin-profiles/workflow-probe.json"
        self.original = {"rootFiles": ["AGENTS.md"], "documentIncludePrefixes": ["AGENTS.md"],
                         "evalCases": [{"mustPaths": ["docs/access.md"]}], "packEvalCases": [{"mustPaths": ["docs/access.md"]}]}
        write(self.profile_path, self.original)
        write(self.project / RULE, "# Untouched Canonical Rule\n")
        write(self.working / "docs/kb/generated/manifest.json", {"generation": "before"})
        write(self.case / "cache/history/docs/kb/generated/manifest.json", {"generation": "history"})
        write(self.case / "profile-before.json", self.original)
        write(self.output / "cases.json", {"mdc-fixture": {
            "case_dir": str(self.case), "root": str(self.project), "working": str(self.working),
            "registry": str(self.profile_path.parent), "permission": "enroll-mdc"}})
        write(self.output / "before.json", inventory([self.project, self.case / "cache"]))

    def run_audit(self):
        with redirect_stdout(io.StringIO()):
            code = audit(self.output)
        return code, json.loads((self.output / "readonly-audit.json").read_text())

    def enroll(self):
        profile = json.loads(self.profile_path.read_text())
        for key in ("rootFiles", "documentIncludePrefixes"):
            profile[key].append(RULE)
        write(self.profile_path, profile)

    def test_unmodified_fixture_passes(self):
        self.assertEqual(self.run_audit()[0], 0)

    def test_explicit_enrollment_and_known_publication_files_pass(self):
        self.enroll()
        write(self.working / "docs/kb/generated/manifest.json", {"generation": "after"})
        write(self.working / "docs/kb/.shin-context-freshness.json", {"cache": "after"})
        code, result = self.run_audit()
        self.assertEqual(code, 0, result)
        self.assertEqual(result["enrollment"], {"mdc-fixture": "approved-fields-only"})

    def test_rule_mutation_is_not_enrollment(self):
        self.enroll()
        write(self.project / RULE, "# Rewritten Rule\n")
        self.assertEqual(self.run_audit()[0], 1)

    def test_changed_eval_cannot_grant_publication_permission(self):
        self.enroll()
        profile = json.loads(self.profile_path.read_text())
        profile["evalCases"] = []
        write(self.profile_path, profile)
        write(self.working / "docs/kb/generated/manifest.json", {"generation": "after"})
        code, result = self.run_audit()
        self.assertEqual(code, 1)
        self.assertEqual(result["enrollment"]["mdc-fixture"], "unexpected-profile-change")

    def test_new_unrecognized_output_and_history_changes_fail(self):
        self.enroll()
        write(self.working / "docs/kb/generated/extra.json", {})
        write(self.case / "cache/history/docs/kb/generated/manifest.json", {"generation": "changed"})
        code, result = self.run_audit()
        self.assertEqual(code, 1)
        self.assertEqual(len(result["unexpected_changes"]), 2)

    def test_freshness_write_without_enrollment_fails(self):
        write(self.working / "docs/kb/.shin-context-freshness.json", {})
        self.assertEqual(self.run_audit()[0], 1)


if __name__ == "__main__":
    unittest.main()
