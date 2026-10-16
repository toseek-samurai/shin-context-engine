"""Neutral CLI contracts for document planner admission and scoring."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = str(Path(sys.argv.pop(1)).resolve())
QUERY = "alpha beta gamma delta epsilon zeta eta theta"


class PlannerContractTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-planner-contract-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "root"
        self.out = self.base / "index"
        self.profiles = self.base / "profiles"
        self.profiles.mkdir()
        (self.root / "docs").mkdir(parents=True)
        (self.root / "README.md").write_text("# Planner contract fixture\n")
        (self.profiles / "planner.json").write_text(json.dumps({
            "schemaVersion": "shin.profile.v1", "id": "planner",
            "sourceLabel": "Planner contract fixture", "rootFiles": ["README.md"],
            "sourceExtensions": [".md"], "agentStartPaths": ["README.md"],
            "defaultTopic": "fixture", "evalCases": [], "topicRules": [],
        }))

    def invoke(self, *args):
        result = subprocess.run([
            TOOL, "--profile", "planner", "--profile-dir", str(self.profiles),
            "--root", str(self.root), "--out-dir", str(self.out), *args,
        ], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        return json.loads(result.stdout)

    def document(self, name, text):
        (self.root / "docs" / name).write_text(text)

    def brief(self, limit=4):
        return self.invoke("brief", QUERY, "--require-current", "--limit", str(limit), "--per-doc", "2")

    def test_document_with_required_groups_is_not_excluded_by_rare_anchor(self):
        self.document("anchor.md", "# Anchor\n\nalpha beta gamma delta\n")
        self.document("ordinary.md", "# Ordinary\n\nepsilon zeta eta theta\n")
        self.document(
            "bridge.md",
            "# Bridge\n\n## First\n\nbeta gamma\n\n## Second\n\ndelta epsilon\n",
        )
        self.invoke("build")

        result = self.brief()

        self.assertTrue(result["query_diagnostics"]["query_planner_used"])
        self.assertIn("docs/bridge.md", [path["path"] for path in result["ranked_paths"]])

    def test_directory_scope_retains_only_explicit_preferred_root_entries(self):
        profile_path = self.profiles / "planner.json"
        profile = json.loads(profile_path.read_text())
        profile.update(compoundQueryFirstTermScope=True,
                       compoundQueryPreferredStems=["entry"],
                       compoundQueryPreferredStemSuffixes=["entry"])
        profile_path.write_text(json.dumps(profile))
        for directory in ("Atlas", "Atlas/nested", "Atlas2"):
            (self.root / "docs" / directory).mkdir(parents=True, exist_ok=True)
        self.document("Atlas/entry.md", "# Bootstrap\n\nApplication wiring.\n\n## Configuration\n\nBoot settings.\n")
        self.document("Atlas/notes.md", "# Notes\n\nUnrelated material.\n")
        self.document("Atlas/secondaryentry.md", "# Secondary\n\nApplication wiring.\n")
        self.document("Atlas/nested/entry.md", "# Nested\n\nApplication wiring.\n")
        self.document("Atlas2/entry.md", "# Sibling\n\nApplication wiring.\n")
        self.document("Atlas/worker.md", "# Worker\n\nalpha beta gamma delta epsilon zeta eta theta.\n")
        self.document("Atlas/nested/feature.md", "# Feature\n\nalpha behavior.\n")
        self.invoke("build")
        query = "docs/Atlas " + QUERY + " absentword"
        result = self.invoke("brief", query, "--limit", "8", "--per-doc", "2", "--require-current")
        paths = {row["path"] for row in result["ranked_paths"]}
        self.assertEqual(paths, {"docs/Atlas/entry.md", "docs/Atlas/worker.md", "docs/Atlas/nested/feature.md"})
        entry = next(row for row in result["sections"] if row["path"] == "docs/Atlas/entry.md")
        self.assertEqual(sum(row["path"] == "docs/Atlas/entry.md" for row in result["sections"]), 1)
        self.assertEqual(entry["matched_group_indices"], [0])
        self.assertFalse(result["query_coverage"]["groups"][-1]["covered"])
        profile["compoundQueryPreferredStems"] = []
        profile_path.write_text(json.dumps(profile))
        self.invoke("build")
        unpreferred = self.invoke("brief", query, "--limit", "8", "--per-doc", "1")
        self.assertNotIn("docs/Atlas/entry.md", [row["path"] for row in unpreferred["ranked_paths"]])

    def test_planner_keeps_raw_lexical_scores(self):
        self.document(
            "split.md",
            "# Split\n\n## One\n\nalpha beta\n\n## Two\n\ngamma delta\n\n"
            "## Three\n\nepsilon zeta\n\n## Four\n\neta theta\n",
        )
        self.invoke("build")

        result = self.brief(limit=1)

        self.assertTrue(result["query_diagnostics"]["query_planner_used"])
        hits = (
            result["sections"] + result["decisions"] + result["contracts"] +
            result["runbooks"] + result["facts"]
        )
        self.assertTrue(hits)
        for hit in hits:
            with self.subTest(hit=hit.get("path")):
                self.assertNotIn("document_coverage_boost", hit["score_components"])
                self.assertNotIn("new_group_coverage_boost", hit["score_components"])
                self.assertNotIn("document_coverage", hit["matched_fields"])
        self.assertEqual(result["query_coverage"]["covered_groups"], 8)
        self.assertEqual(result["query_coverage"]["selected_document_covered_groups"], 8)
        self.assertLess(result["query_coverage"]["returned_evidence_covered_groups"], 8)

    def test_directory_entry_context_respects_pack_evidence_types(self):
        profile_path = self.profiles / "planner.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceExtensions=[".md", ".jsonl"], compoundQueryFirstTermScope=True,
                       compoundQueryPreferredStems=["entry"])
        profile["packEvalCases"] = [{
            "id": "entry-context-type-filter",
            "request": {
                "schema_version": "shin.task-request.v1", "request_id": "entry-context-type-filter",
                "task": {"objective": "Read permitted directory entry context.",
                         "must": [{"id": "entry", "query": "docs/Atlas pulse absentword"}]},
                "evidence_types": ["decision"],
                "budget": {"max_evidence_bytes": 8000, "max_records": 1,
                           "per_query_records": 4, "per_document_records": 2},
            },
            "mustPaths": ["docs/Atlas/entry.jsonl"], "expectedComplete": True,
            "maxRecords": 1, "maxEvidenceBytes": 8000, "maxCompactBytes": 12000,
            "requireSerialParallelIdentity": True,
        }]
        profile_path.write_text(json.dumps(profile))
        (self.root / "docs/Atlas").mkdir()
        self.document("Atlas/entry.jsonl", "\n".join(json.dumps(row) for row in (
            {"type": "contract", "id": "higher", "title": "Atlas bootstrap"},
            {"type": "decision", "id": "permitted", "title": "Bootstrap decision"},
        )) + "\n")
        self.document("Atlas/worker.md", "# Worker\n\npulse behavior.\n")
        self.invoke("build")
        result = self.invoke("pack-eval", "--strict", "--require-current")
        self.assertEqual(result["summary"], {"total": 1, "passed": 1, "failed": 0})
        self.assertTrue(result["cases"][0]["serial_parallel"]["identical"])

    def assert_sparse_plan_retains_ordinary(self, name, contents):
        self.document(name, contents)
        self.document("bridge.md", "# Bridge\n\n## One\n\nalpha beta\n\n"
                      "## Two\n\ngamma delta\n\n## Three\n\nepsilon zeta\n\n"
                      "## Four\n\neta theta\n")
        self.invoke("build")
        result = self.brief(limit=6)
        self.assertTrue(result["query_diagnostics"]["query_planner_used"])
        self.assertEqual({row["path"] for row in result["ranked_paths"]},
                         {f"docs/{name}", "docs/bridge.md"})
        hits = result["sections"] + result["facts"] + result["contracts"]
        ordinary = [hit for hit in hits if hit["path"] == f"docs/{name}"]
        self.assertTrue(ordinary)
        self.assertTrue(all(len(hit["matched_group_indices"]) == 4 for hit in ordinary))
        self.assertEqual(result["query_coverage"]["covered_groups"], 8)
        self.assertEqual(len(hits), len({json.dumps(hit, sort_keys=True) for hit in hits}))
        reference = self.invoke("context", QUERY, "--limit", "20")
        for hit in ordinary:
            self.assertIn(hit, reference["sections"] + reference["facts"])
        bounded = self.brief(limit=1)
        self.assertEqual([row["path"] for row in bounded["ranked_paths"]], [f"docs/{name}"])
        self.assertEqual(bounded["query_coverage"]["covered_groups"], 4)

    def test_sparse_planner_retains_ordinary_section(self):
        self.assert_sparse_plan_retains_ordinary("ordinary.md", "# alpha beta gamma delta\n")

    def test_sparse_planner_retains_ordinary_fact(self):
        profile_path = self.profiles / "planner.json"
        profile = json.loads(profile_path.read_text())
        profile["sourceExtensions"] = [".md", ".jsonl"]
        profile_path.write_text(json.dumps(profile))
        self.assert_sparse_plan_retains_ordinary("ordinary.jsonl", json.dumps({
            "type": "contract", "id": "policy", "title": "alpha beta gamma delta",
            "summary": "alpha beta gamma delta", "status": "accepted",
        }) + "\n")

    def test_sparse_planner_does_not_duplicate_retained_sections(self):
        self.document("ordinary.md", f"# {QUERY}\n")
        self.invoke("build")
        result = self.brief()
        self.assertTrue(result["query_diagnostics"]["query_planner_used"])
        self.assertEqual(len(result["sections"]), 1)
        self.assertEqual(result["sections"][0]["path"], "docs/ordinary.md")

    def test_diagnose_separates_expanded_budget_from_route_change(self):
        self.document("left.md", "# Left\n\nalpha beta gamma delta\n")
        self.document("right.md", "# Right\n\nepsilon zeta eta theta\n")
        self.document("bridge.md", "# Bridge\n\n## First\n\nalpha beta\n\n"
                      "## Second\n\ngamma delta\n\n## Third\n\nepsilon zeta\n\n"
                      "## Fourth\n\neta theta\n")
        self.invoke("build")
        result = self.invoke("diagnose", QUERY, "--expected-path", "docs/bridge.md",
                             "--limit", "2", "--per-doc", "2")
        self.assertFalse(result["ok"])
        self.assertEqual(result["retrieval"]["mode"], "relaxed")
        self.assertIn("returned-with-expanded-budget", result["expectation"]["reason_codes"])
        self.assertIn("retrieval-route-changed", result["expectation"]["reason_codes"])
        self.assertNotIn("ranked-out-global-budget", result["expectation"]["reason_codes"])
        self.assertTrue(result["budget_probe"]["executed"])
        self.assertTrue(result["budget_probe"]["route_changed"])
        self.assertEqual(result["budget_probe"]["retrieval_mode"], "planned")
        self.assertGreater(result["budget_probe"]["returned_path_rank"], 0)

    def test_independent_source_members_do_not_borrow_document_coverage(self):
        profile_path = self.profiles / "planner.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".ts"],
                       sourceFactExtractors=["language"])
        profile_path.write_text(json.dumps(profile))
        (self.root / "src").mkdir()
        (self.root / "src" / "Units.ts").write_text("\n" * 30 + ("\n" * 30).join(
            f"export function {name}() {{ return 1; }}" for name in QUERY.split()
        ))
        self.invoke("build")
        result = self.brief(limit=1)
        self.assertEqual(result["query_coverage"]["selected_document_covered_groups"], 0)
        self.assertEqual(result["query_coverage"]["covered_groups"],
                         result["query_coverage"]["returned_evidence_covered_groups"])
        self.assertLess(result["query_coverage"]["covered_groups"], 4)

        expanded = self.invoke("brief", QUERY, "--limit", "8", "--per-doc", "8", "--require-current")
        self.assertEqual(expanded["query_coverage"]["selected_document_covered_groups"], 0)
        self.assertEqual(expanded["query_coverage"]["covered_groups"], 8)
        self.assertEqual(expanded["query_coverage"]["returned_evidence_covered_groups"], 8)

    def test_jsonl_records_are_not_a_composite_document(self):
        profile_path = self.profiles / "planner.json"
        profile = json.loads(profile_path.read_text())
        profile["sourceExtensions"] = [".md", ".jsonl"]
        profile_path.write_text(json.dumps(profile))
        self.document("records.jsonl", "\n".join(json.dumps({
            "type": "contract", "id": name, "title": name, "summary": name,
        }) for name in QUERY.split()) + "\n")
        self.invoke("build")
        result = self.brief(limit=1)
        self.assertEqual(result["query_coverage"]["selected_document_covered_groups"], 0)
        self.assertEqual(result["query_coverage"]["covered_groups"],
                         result["query_coverage"]["returned_evidence_covered_groups"])
        self.assertLess(result["query_coverage"]["covered_groups"], 4)

        expanded = self.invoke("brief", QUERY, "--limit", "8", "--per-doc", "8", "--require-current")
        self.assertEqual(expanded["query_coverage"]["selected_document_covered_groups"], 0)
        self.assertEqual(expanded["query_coverage"]["returned_evidence_covered_groups"], 8)
        self.assertEqual({hit["title"] for hit in expanded["facts"] + expanded["contracts"]},
                         set(QUERY.split()))

    def test_selected_source_unit_retains_its_own_partial_coverage(self):
        profile_path = self.profiles / "planner.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".ts"],
                       sourceFactExtractors=["language"])
        profile_path.write_text(json.dumps(profile))
        (self.root / "src").mkdir()
        (self.root / "src" / "Units.ts").write_text(
            "export function Left() { return alpha + beta + gamma + delta; }\n" +
            "\n" * 30 +
            "export function Right() { return epsilon + zeta + eta + theta; }\n")
        self.invoke("build")
        result = self.brief(limit=1)
        self.assertTrue(result["query_diagnostics"]["query_planner_used"])
        self.assertEqual(len(result["facts"]), 1)
        self.assertEqual(result["facts"][0]["path"], "src/Units.ts")
        self.assertEqual(len(result["facts"][0]["matched_group_indices"]), 4)
        self.assertEqual(result["query_coverage"]["covered_groups"], 4)
        self.assertEqual(result["query_coverage"]["returned_evidence_covered_groups"], 4)
        self.assertEqual(result["query_coverage"]["selected_document_covered_groups"], 0)

    def test_ordinary_brief_preserves_distinct_records_with_shared_id(self):
        profile_path = self.profiles / "planner.json"
        profile = json.loads(profile_path.read_text())
        profile["sourceExtensions"] = [".md", ".jsonl"]
        profile["compoundQueryFirstTermScope"] = True
        profile_path.write_text(json.dumps(profile))
        record = {"type": "contract", "id": "shared", "title": "alpha policy",
                  "summary": "alpha boundary", "status": "accepted"}
        # Distinct document/source-id components can yield the same readable ID.
        (self.root / "docs" / "data").mkdir()
        self.document("data/left.jsonl", json.dumps({**record, "id": "part-jsonl-shared"}) + "\n")
        self.document("data/left-jsonl-part.jsonl", "\n".join(json.dumps(value) for value in (
            record, {**record, "summary": "alpha alternative"},
            {**record, "status": "proposed"},
        )) + "\n")
        self.invoke("build")
        expected = {
            ("docs/data/left.jsonl", 1, "alpha boundary", "accepted"),
            ("docs/data/left-jsonl-part.jsonl", 1, "alpha boundary", "accepted"),
            ("docs/data/left-jsonl-part.jsonl", 2, "alpha alternative", "accepted"),
            ("docs/data/left-jsonl-part.jsonl", 3, "alpha boundary", "proposed"),
        }
        for source_args in ((), ("--source-scan",)):
            with self.subTest(source_scan=bool(source_args)):
                result = self.invoke("brief", "alpha", "--limit", "12", "--per-doc", "8", *source_args)
                self.assertFalse(result["query_diagnostics"]["query_planner_used"])
                hits = result["contracts"]
                self.assertEqual(len(hits), 4)
                self.assertEqual({(hit["path"], hit["line"], hit["summary"], hit["status"])
                                  for hit in hits}, expected)
                colliding = [hit for hit in hits if hit["line"] == 1]
                self.assertEqual(len({hit["id"] for hit in colliding}), 1)

        scoped = self.invoke("brief", "docs/data alpha", "--limit", "12", "--per-doc", "8")
        self.assertTrue(scoped["query_diagnostics"]["directory_owner_scope_used"])
        self.assertEqual({(hit["path"], hit["line"], hit["summary"], hit["status"])
                          for hit in scoped["contracts"]}, expected)


if __name__ == "__main__":
    unittest.main()
