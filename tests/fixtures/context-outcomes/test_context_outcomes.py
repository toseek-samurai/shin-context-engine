"""Selected context assertions and document lifecycle, independent of consumer layout."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = str(Path(sys.argv.pop(1)).resolve())


class ContextOutcomeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-context-outcomes-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "root"
        (self.root / "docs").mkdir(parents=True)
        (self.root / "README.md").write_text("# Neutral project\n")
        self.registry = self.base / "profiles"
        self.registry.mkdir()
        self.profile = {"schemaVersion": "shin.profile.v1", "id": "outcomes",
                        "sourceLabel": "Neutral context outcomes",
                        "rootFiles": ["README.md"], "sourceExtensions": [".md"],
                        "agentStartPaths": ["README.md"], "defaultTopic": "fixture",
                        "topicRules": [{"id": "fixture", "description": "Neutral fixture", "keywords": ["fixture"]}],
                        "evalCases": [], "packEvalCases": [{"id": "entry-context", "expectedComplete": True,
                            "request": {"schema_version": "shin.task-request.v1", "request_id": "entry",
                                        "task": {"objective": "Find project entry", "must": [{"id": "entry", "query": "neutral project"}]},
                                        "budget": {"max_evidence_bytes": 4000, "max_records": 2,
                                                   "per_query_records": 2, "per_document_records": 1}}}]}

    def call(self, *args, code=0):
        result = subprocess.run([TOOL, "--profile", "outcomes", "--profile-dir", str(self.registry),
                                 "--root", str(self.root), "--out-dir", str(self.base / "index"), *args],
                                capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        return json.loads(result.stdout or result.stderr)

    def build(self):
        (self.registry / "outcomes.json").write_text(json.dumps(self.profile))
        return self.call("build")

    def policy(self):
        (self.root / "docs/policy.md").write_text(
            "# Reference\n\nStatus: accepted\n\n## Cache timeout\n\nStatus: history\n\n"
            "The cache timeout was 900 seconds.\n\n## Current behavior\n\nStatus: active\n\n"
            "Entries expire after 60 seconds.\n")
        return {"id": "current-timeout", "query": "cache timeout", "mustPaths": ["docs/policy.md"],
                "limit": 1, "perDocLimit": 1, "mustTopN": 1,
                "mustEvidence": [{"path": "docs/policy.md", "contains": "60 seconds", "status": "active"}]}

    def test_correct_path_with_wrong_fragment_fails_and_variants_are_checked(self):
        case = self.policy()
        case["queryVariants"] = ["entries expire"]
        self.profile["evalCases"] = [case]
        self.build()
        result = self.call("eval", "--strict", code=1)
        self.assertFalse(result["quality_ok"])
        self.assertEqual(result["cases"][0]["missing_paths"], [])
        self.assertEqual(result["cases"][0]["missing_evidence"], case["mustEvidence"])
        failed = self.call("eval", "--strict", "--failed-only", code=1)
        self.assertIn("missing-required-evidence", failed["failures"][0]["failure_reasons"])
        self.assertTrue(result["quality_failure_only"])

    def test_current_display_fragment_passes(self):
        case = self.policy()
        case["query"] = "entries expire"
        self.profile["evalCases"] = [case]
        self.build()
        result = self.call("eval", "--strict")
        self.assertTrue(result["quality_ok"])
        self.assertEqual(result["cases"][0]["missing_evidence"], [])

    def test_forbidden_statement_stays_a_visible_quality_failure_under_advisory(self):
        case = self.policy()
        case["mustEvidence"] = []
        case["mustNotEvidence"] = [{"contains": "900 seconds", "status": "history"}]
        self.profile.update(evalCases=[case], evalResourcePolicy="advisory")
        self.build()
        result = self.call("eval", "--strict", code=1)
        self.assertFalse(result["quality_ok"])
        self.assertTrue(result["quality_failure_only"])
        self.assertTrue(result["cases"][0]["forbidden_evidence"])
        # Statement relevance is not source authority; one bad scenario must not
        # disable all safe project context. Explicit forbidden paths stay hard.
        self.assertTrue(self.call("ready", "--strict", code=1)["retrieval_ready"])

    def test_search_only_text_cannot_satisfy_a_display_assertion(self):
        (self.root / "docs/manual.md").write_text("# Manual\n\n" + "neutral material " * 80 +
                                                "required-outcome-beyond-preview.\n")
        self.profile["evalCases"] = [{"id": "visible-only", "query": "docs/manual.md", "limit": 1, "mustTopN": 1,
                                     "mustEvidence": [{"contains": "required-outcome-beyond-preview"}]}]
        self.build()
        result = self.call("eval", "--strict", code=1)
        self.assertTrue(result["cases"][0]["missing_evidence"])

    def test_pack_checks_its_final_budgeted_display(self):
        self.policy()
        self.profile["packEvalCases"] = [{"id": "pack-current", "expectedComplete": True,
            "request": {"schema_version": "shin.task-request.v1", "request_id": "outcome",
                        "task": {"objective": "Find the current cache rule", "must": [{"id": "policy", "query": "cache timeout"}]},
                        "budget": {"max_evidence_bytes": 4000, "max_records": 1, "per_query_records": 1, "per_document_records": 1}},
            "mustPaths": ["docs/policy.md"], "mustEvidence": [{"contains": "60 seconds"}]}]
        self.build()
        result = self.call("pack-eval", "--strict", code=1)
        self.assertEqual(result["cases"][0]["missing_paths"], [])
        self.assertIn("missing-required-evidence", result["cases"][0]["failure_reasons"])
        self.assertTrue(result["quality_failure_only"])
        self.profile["packEvalCases"][0]["request"]["task"]["must"][0]["query"] = "entries expire"
        self.build()
        self.assertTrue(self.call("pack-eval", "--strict")["ok"])

    def linked_documents(self, kind="Amends", status="accepted"):
        (self.root / "docs/old.md").write_text("# Cache retention policy\n\nStatus: accepted\n\n## Retention\n\nThe cache retains entries for 900 seconds.\n")
        (self.root / "docs/new.md").write_text(f"# Cache amendment\n\nStatus: {status}\n\n{kind}: [Original policy](old.md)\n\n## Replacement rule\n\nThe retention period is now 60 seconds instead of 900.\n")

    def test_amendment_is_delivered_without_reorganizing_either_document(self):
        self.linked_documents()
        before = {p.name: p.read_bytes() for p in (self.root / "docs").iterdir()}
        self.build()
        result = self.call("brief", "cache retention policy", "--limit", "4", "--per-doc", "2", "--projection", "compact")
        self.assertIn("docs/new.md", {r["path"] for r in result["evidence"]})
        self.assertIn("docs/old.md", {r["path"] for r in result["evidence"]})
        self.assertEqual(result["document_lifecycle"]["relations"][0]["kind"], "amends")
        self.assertTrue(any("60 seconds" in r.get("excerpt", "") for r in result["evidence"]))
        self.assertEqual(before, {p.name: p.read_bytes() for p in (self.root / "docs").iterdir()})

    def test_superseded_fragments_are_historical_and_replacement_is_available(self):
        self.linked_documents("Supersedes")
        self.build()
        result = self.call("brief", "docs/old.md retention", "--limit", "4", "--per-doc", "2", "--projection", "compact")
        old = [r for r in result["evidence"] if r["path"] == "docs/old.md"]
        self.assertTrue(old)
        self.assertTrue(all(r["status"] == "superseded" and r["authority_level"] == "history" for r in old))
        self.assertIn("docs/new.md", {r["path"] for r in result["evidence"]})
        # A companion's owner lookup term never becomes original query coverage.
        self.assertEqual(result["query_coverage"]["total_groups"], 2)
        self.assertTrue(all(all(i < 2 for i in r["matched_group_indices"]) for r in result["evidence"]))

    def test_draft_or_cyclic_replacement_does_not_retire_accepted_evidence(self):
        for status in ("proposed", "accepted"):
            with self.subTest(status=status):
                self.linked_documents("Supersedes", status)
                if status == "accepted":
                    (self.root / "docs/old.md").write_text("# Old\n\nStatus: accepted\n\nSupersedes: [New](new.md)\n\n## Cache retention policy\n\n900 seconds.\n")
                self.build()
                result = self.call("brief", "docs/old.md retention", "--limit", "4", "--projection", "compact")
                self.assertTrue(all(r["status"] != "superseded" for r in result["evidence"]))
                self.assertTrue(result["document_lifecycle"]["relations"])

    def test_relation_to_excluded_document_does_not_expand_scope(self):
        self.linked_documents("Supersedes")
        self.profile["documentExcludePrefixes"] = ["docs/new.md"]
        self.build()
        result = self.call("brief", "docs/old.md", "--projection", "compact")
        self.assertNotIn("docs/new.md", {r["path"] for r in result["evidence"]})
        self.assertNotIn("document_lifecycle", result)
        self.assertTrue(all(r["status"] != "superseded" for r in result["evidence"]))

    def test_small_fallback_keeps_all_previous_owners(self):
        for name, body in {"primary": "alpha beta gamma delta epsilon", "companion": "delta epsilon zeta",
                           "exception": "alpha beta"}.items():
            (self.root / f"docs/{name}.md").write_text(f"# {name}\n\n{body}.\n")
        self.build()
        result = self.call("brief", "alpha beta gamma delta epsilon zeta", "--limit", "8")
        self.assertEqual({r["path"] for r in result["ranked_paths"]}, {"docs/primary.md", "docs/companion.md", "docs/exception.md"})
        plan = result["query_plan"]
        self.assertEqual(plan["candidate_row_visits"], sum(s["candidate_rows"] for s in plan["stages"]))
        self.assertGreaterEqual(plan["candidate_row_visits"], result["query_diagnostics"]["candidate_rows"])
        self.assertIn("strict", [s["stage"] for s in plan["stages"]])
        self.assertIn("relaxed", [s["stage"] for s in plan["stages"]])

    def test_unicode_statement_assertion_uses_visible_normalized_text(self):
        (self.root / "docs/policy.md").write_text("# Правило\n\nStatus: active\n\nЗаписи истекают через 60 секунд.\n", encoding="utf-8")
        self.profile["evalCases"] = [{"id": "unicode-current", "query": "истекают", "mustTopN": 1,
                                     "mustEvidence": [{"heading": "ПРАВИЛО", "contains": "ИСТЕКАЮТ  ЧЕРЕЗ 60 СЕКУНД", "status": "active"}]}]
        self.build()
        self.assertTrue(self.call("eval", "--strict")["quality_ok"])

    def test_reverse_replacement_declaration_has_the_same_effect(self):
        self.linked_documents()
        (self.root / "docs/old.md").write_text("# Cache retention policy\n\nStatus: accepted\n\nSuperseded by: [Current](new.md)\n\n## Retention\n\n900 seconds.\n")
        (self.root / "docs/new.md").write_text("# Current retention\n\nStatus: accepted\n\nThe cache retains entries for 60 seconds.\n")
        self.build()
        result = self.call("brief", "docs/old.md retention", "--limit", "4", "--projection", "compact")
        self.assertTrue(any(r["path"] == "docs/new.md" and "60 seconds" in r.get("excerpt", "") for r in result["evidence"]))
        self.assertTrue(all(r["status"] == "superseded" for r in result["evidence"] if r["path"] == "docs/old.md"))

    def test_section_replacement_does_not_retire_or_supplement_unrelated_sections(self):
        (self.root / "docs/old.md").write_text("# Policies\n\nStatus: accepted\n\n## Timeouts\n\nEntries expire after 900 seconds.\n\n## Requests\n\nRequests use named parameters.\n")
        (self.root / "docs/new.md").write_text("# Timeout amendment\n\nStatus: accepted\n\nSupersedes: [Timeouts](old.md#timeouts)\n\n## Current timeout\n\nEntries expire after 60 seconds.\n")
        self.build()
        changed = self.call("brief", "docs/old.md timeouts", "--limit", "4", "--projection", "compact")
        self.assertTrue(any(r["path"] == "docs/old.md" and r["heading"] == "Timeouts" and r["status"] == "superseded" for r in changed["evidence"]))
        other = self.call("brief", "docs/old.md requests", "--projection", "compact")
        self.assertTrue(all(r["status"] != "superseded" for r in other["evidence"]))
        self.assertNotIn("docs/new.md", {r["path"] for r in other["evidence"]})
        self.assertNotIn("document_lifecycle", other)

    def test_code_examples_and_narrative_links_do_not_retire_documents(self):
        self.linked_documents()
        for declaration in ("~~~text\nSupersedes: [Old](old.md)\n~~~", "## Example\n\nSupersedes: [Old](old.md)", "This note supersedes [Old](old.md) only as a hypothetical example."):
            with self.subTest(declaration=declaration):
                (self.root / "docs/new.md").write_text("# Example\n\nStatus: accepted\n\n" + declaration + "\n")
                self.build()
                result = self.call("brief", "docs/old.md", "--projection", "compact")
                self.assertTrue(all(r["status"] != "superseded" for r in result["evidence"]))
                self.assertNotIn("document_lifecycle", result)

    def test_ordinary_wrapped_adr_preface_needs_no_special_metadata(self):
        self.linked_documents()
        (self.root / "docs/old.md").write_text("# Cache retention policy\n\nStatus: accepted\n\nAmended by\n[Current](new.md) and\n[Other](other.md).\n\n## Original rule\n\nThe cache retains entries for 900 seconds.\n")
        (self.root / "docs/new.md").write_text("# Current retention\n\nStatus: accepted, 2030-01-01\n\n## Current rule\n\nThe cache retains entries for 60 seconds.\n")
        (self.root / "docs/other.md").write_text("# Other amendment\n\nStatus: active; explanatory note\n\n## Current scope\n\nCache retention is per request.\n")
        self.build()
        result = self.call("brief", "docs/old.md retention", "--limit", "8", "--per-doc", "2", "--projection", "compact")
        self.assertTrue({"docs/old.md", "docs/new.md", "docs/other.md"}.issubset({r["path"] for r in result["evidence"]}))
        self.assertTrue(all(r["state"] == "confirmed" for r in result["document_lifecycle"]["relations"]))

    def test_partial_prose_replacement_keeps_the_other_operative_rules(self):
        self.linked_documents()
        (self.root / "docs/old.md").write_text("# Cache retention policy\n\nStatus: accepted\n\nRequest timeout rules are superseded by [Current](new.md).\nOther retention rules remain operative.\n\n## Retention\n\nThe cache retains entries for 900 seconds.\n")
        self.build()
        result = self.call("brief", "docs/old.md retention", "--limit", "4", "--projection", "compact")
        replacements = [r for r in result["document_lifecycle"]["relations"] if r["kind"] == "supersedes"]
        self.assertTrue(replacements)
        self.assertTrue(all(r["scope"] == "partial" for r in replacements))
        self.assertTrue(all(r["status"] != "superseded" for r in result["evidence"] if r["path"] == "docs/old.md"))
        self.assertIn("docs/new.md", {r["path"] for r in result["evidence"]})

    def test_amendment_notices_are_bounded_with_explicit_omissions(self):
        links = []
        for i in range(12):
            (self.root / f"docs/new-{i}.md").write_text(f"# Amendment {i}\n\nStatus: accepted\n\nCurrent cache rule {i}.\n")
            links.append(f"[Current {i}](new-{i}.md)")
        (self.root / "docs/old.md").write_text("# Cache rule\n\nStatus: accepted\n\nAmended by " + ", ".join(links) + ".\n")
        self.build()
        result = self.call("brief", "docs/old.md", "--limit", "1", "--projection", "compact")
        notes = result["document_lifecycle"]
        self.assertEqual(len(notes["relations"]), 8)
        self.assertEqual(notes["relations_omitted"], 4)
        self.assertEqual(notes["inventory_omitted"], 0)
        self.assertLessEqual(len(json.dumps(notes["relations"], separators=(",", ":")).encode()), 4096)
        self.assertEqual(result["evidence"][0]["path"], "docs/old.md")
        self.assertNotIn("query_plan", result)

    def test_replacement_status_covers_chunks_and_keeps_its_verification_dependency(self):
        body = "\n".join(f"The timeout policy retains record {i} for 900 seconds under a bounded cache rule." for i in range(16))
        (self.root / "docs/old.md").write_text("# Policies\n\nStatus: accepted\n\n## Timeouts\n\n" + body + "\n\n## Requests\n\nRequests use named parameters.\n")
        (self.root / "docs/new.md").write_text("# Current timeout\n\nStatus: accepted\n\nSupersedes [Previous](old.md#timeouts).\n\n## Rule\n\nThe timeout is 60 seconds.\n")
        self.profile.update(markdownChunkLines=3, markdownChunkMinLines=2, markdownChunkChars=120)
        self.build()
        result = self.call("brief", "docs/old.md timeout", "--limit", "8", "--per-doc", "6")
        old = [r for r in result["sections"] if r["path"] == "docs/old.md"]
        self.assertGreater(len(old), 1)
        self.assertTrue(any(r.get("row_type") == "chunk" for r in old))
        self.assertTrue(all(r["status"] == "superseded" for r in old))
        dependencies = result["document_lifecycle"]["verification_dependencies"]
        self.assertIn("docs/new.md", {r["path"] for r in dependencies})

    def test_invalid_predicates_fail_closed(self):
        case = self.policy()
        for predicate in ({}, {"typo": "active"}, {"path": "../outside.md"},
                          {"contains": ""}, {"contains": True}, {"contains": "x" * 4097}):
            with self.subTest(predicate=str(predicate)[:70]):
                case["mustEvidence"] = [predicate]
                self.profile["evalCases"] = [case]
                self.build()
                self.assertFalse(self.call("eval", "--strict", code=2)["ok"])


if __name__ == "__main__":
    unittest.main()
