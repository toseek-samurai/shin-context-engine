"""Neutral indexed-Brief identifier interpretation and fallback regressions."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = str(Path(sys.argv.pop(1)).resolve())


class IdentifierQueryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-identifier-query-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "root"
        self.out = self.base / "index"
        self.profiles = self.base / "profiles"
        self.profiles.mkdir()
        (self.root / "src").mkdir(parents=True)
        (self.root / "docs").mkdir()
        (self.root / "README.md").write_text("# Reference lookup fixture\n")
        self.profile = {
            "schemaVersion": "shin.profile.v1", "id": "identifier-query",
            "sourceLabel": "Neutral reference fixture", "rootFiles": ["README.md"],
            "sourceExtensions": [".md"], "sourceFactRoots": ["src"],
            "sourceFactExtensions": [".ts"], "sourceFactExtractors": ["language"],
            "sourceFactMaxFiles": 128, "sourceFactMaxFileBytes": 131072,
            "agentStartPaths": ["README.md"], "defaultTopic": "fixture",
            "evalCases": [], "topicRules": [],
        }
        self.reference = self.root / "src/reference.ts"
        self.reference.write_text(
            "export function resolveMode(input: string) {\n"
            "  const retentionWindow = input;\n  return retentionWindow;\n}\n")
        (self.root / "src/separate.ts").write_text("\n".join(
            f"export function unrelated{index}(retention: number, window: number) {{\n"
            "  return retention + window;\n}\n" for index in range(32)))
        self.build()

    def invoke(self, *args, expected=0):
        argv = [TOOL, "--profile", self.profile["id"], "--profile-dir", str(self.profiles),
                "--root", str(self.root), "--out-dir", str(self.out), *args]
        process = subprocess.run(argv, capture_output=True, text=True, timeout=60)
        self.assertEqual(process.returncode, expected, process.stderr or process.stdout)
        return json.loads(process.stdout)

    def build(self):
        (self.profiles / "identifier-query.json").write_text(json.dumps(self.profile))
        self.invoke("build")

    def brief(self, query, *args):
        return self.invoke("brief", query, "--require-current", "--limit", "8", "--per-doc", "2", *args)

    def test_confirmed_identifier_spelling_and_provenance(self):
        exact = self.brief("retentionWindow")
        spaced = self.brief("retention window")
        self.assertEqual(spaced["retrieval_mode"], "identifier-scoped")
        self.assertEqual(spaced["query_diagnostics"]["matching_policy"], "confirmed-identifier-scope-v1")
        self.assertEqual(spaced["query_diagnostics"]["candidate_rows"], exact["query_diagnostics"]["candidate_rows"])
        self.assertLess(spaced["query_diagnostics"]["candidate_rows"], 16)
        self.assertEqual(spaced["query_diagnostics"]["identifier_probe_rows"], spaced["query_diagnostics"]["candidate_rows"])
        self.assertEqual(spaced["query_coverage"]["total_groups"], 2)
        self.assertEqual(spaced["query_coverage"]["uncovered_groups"], 0)
        self.assertEqual(spaced["ranked_paths"][0]["path"], "src/reference.ts")
        self.assertNotIn("src/separate.ts", [row["path"] for row in spaced["ranked_paths"]])
        compact = self.brief("retention window", "--projection", "compact")
        self.assertEqual(compact["retrieval_mode"], "identifier-scoped")
        self.assertEqual(compact["identifier_scope"]["key"], "retention window")
        self.assertTrue(compact["evidence"])
        for row in compact["evidence"]:
            self.assertEqual(row["matched_group_indices"], [0, 1])
            self.assertEqual(row["verification_level"], "source-observed")
        for query in ("retention_window", "retention-window"):
            self.assertEqual(self.brief(query)["ranked_paths"][0]["path"], "src/reference.ts")

    def test_prose_does_not_confirm_identifier_scope(self):
        self.reference.unlink()
        (self.root / "docs/phrase.md").write_text("# Retention Window\n\nDocumentation only.\n")
        self.build()
        result = self.brief("retention window")
        self.assertNotEqual(result["retrieval_mode"], "identifier-scoped")
        self.assertIn("src/separate.ts", [row["path"] for row in result["ranked_paths"]])
        self.assertGreaterEqual(result["query_diagnostics"]["candidate_rows"], 32)
        diagnosed = self.invoke("diagnose", "retention window", "--expected-path", "src/separate.ts",
                                "--require-current", "--strict")
        self.assertFalse(diagnosed["retrieval"]["identifier_scope_used"])
        self.assertEqual(diagnosed["retrieval"]["identifier_probe_rows"], result["query_diagnostics"]["identifier_probe_rows"])
        self.assertEqual(diagnosed["retrieval"]["candidate_rows"], result["query_diagnostics"]["candidate_rows"])

    def test_identifier_boundaries_are_not_compact_collisions(self):
        self.reference.write_text("export function resolveMode(retentionwindow: string) {\n  return retentionwindow;\n}\n")
        self.build()
        result = self.brief("retention window")
        self.assertNotEqual(result["retrieval_mode"], "identifier-scoped")
        self.assertIn("src/separate.ts", [row["path"] for row in result["ranked_paths"]])

    def test_explicit_aliases_retain_their_meaning(self):
        self.profile["aliases"] = [{"term": "retention", "expands": ["timeout"]}]
        (self.root / "src/alias.ts").write_text(
            "export function aliased(timeout: number, window: number) {\n  return timeout + window;\n}\n")
        self.build()
        result = self.brief("retention window")
        self.assertNotEqual(result["retrieval_mode"], "identifier-scoped")
        self.assertIn("src/alias.ts", [row["path"] for row in result["ranked_paths"]])

    def test_allowed_evidence_types_can_fall_back(self):
        (self.root / "docs/manual.md").write_text(
            "# Operations\n\nRetention is independently configured for each workload. "
            "Several other settings are explicitly reviewed before choosing a window.\n")
        self.build()
        task = {
            "schema_version": "shin.task-request.v1", "request_id": "section-fallback",
            "task": {"objective": "Read the approved evidence type", "must": [
                {"id": "manual", "query": "retention window", "require_full_coverage": True}]},
            "evidence_types": ["section"],
            "budget": {"max_evidence_bytes": 8192, "max_records": 4,
                       "per_query_records": 8, "per_document_records": 2},
        }
        request = self.base / "task.json"
        request.write_text(json.dumps(task))
        pack = self.invoke("pack", "--input", str(request), "--require-current", "--strict")
        self.assertTrue(pack["completion"]["complete"])
        self.assertIn("docs/manual.md", [row["path"] for row in pack["verification_paths"]])
        self.assertTrue(all(row["retrieval_mode"] != "identifier-scoped" for row in pack["requirements"]))

    def test_probe_limit_falls_back_without_truncation(self):
        self.reference.write_text("\n".join(
            f"export function repeated{index}(retentionWindow: number) {{\n  return retentionWindow;\n}}\n"
            for index in range(270)))
        self.build()
        result = self.brief("retention window")
        self.assertNotEqual(result["retrieval_mode"], "identifier-scoped")
        self.assertGreater(result["query_diagnostics"]["identifier_probe_rows"], 256)
        self.assertEqual(result["query_coverage"]["uncovered_groups"], 0)
        self.assertIn("src/reference.ts", [row["path"] for row in result["ranked_paths"]])

    def test_eval_diagnose_and_read_only_state(self):
        self.profile["evalCases"] = [{
            "id": "identifier-forms", "query": "retentionWindow", "queryVariants": ["retention window"],
            "mustTopPaths": ["src/reference.ts"], "mustTopN": 1, "maxCandidateRows": 16, "limit": 8,
        }]
        self.build()
        before = {str(path): path.read_bytes() for parent in (self.root, self.out, self.profiles)
                  for path in parent.rglob("*") if path.is_file()}
        evaluated = self.invoke("eval", "--require-current", "--strict")
        self.assertEqual(evaluated["summary"], {"total": 1, "passed": 1, "failed": 0})
        diagnosed = self.invoke("diagnose", "retention window", "--expected-path", "src/reference.ts",
                                "--require-current", "--strict")
        self.assertEqual(diagnosed["returned_path_rank"], 1)
        self.assertEqual(diagnosed["retrieval"]["mode"], "identifier-scoped")
        self.assertTrue(diagnosed["retrieval"]["identifier_scope_used"])
        self.assertEqual(diagnosed["retrieval"]["identifier_scope_key"], "retention window")
        self.assertEqual(diagnosed["retrieval"]["identifier_probe_rows"], diagnosed["retrieval"]["candidate_rows"])
        after = {str(path): path.read_bytes() for parent in (self.root, self.out, self.profiles)
                 for path in parent.rglob("*") if path.is_file()}
        self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()
