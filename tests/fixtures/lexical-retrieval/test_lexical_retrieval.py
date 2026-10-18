"""Portable candidate/scorer and document-boundary regressions."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = str(Path(sys.argv.pop(1)).resolve())


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compact_lexical import term_postings


class LexicalRetrievalTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-lexical-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "root"
        self.out = self.base / "index"
        self.profiles = self.base / "profiles"
        self.profiles.mkdir()
        (self.root / "docs").mkdir(parents=True)
        (self.root / "README.md").write_text("# Neutral retrieval fixture\n")
        (self.profiles / "lexical.json").write_text(json.dumps({
            "schemaVersion": "shin.profile.v1", "id": "lexical",
            "sourceLabel": "Neutral retrieval", "rootFiles": ["README.md"],
            "sourceExtensions": [".md"], "agentStartPaths": ["README.md"],
            "defaultTopic": "fixture", "evalCases": [], "topicRules": [],
        }))

    def invoke(self, *args, expected=0):
        result = subprocess.run([
            TOOL, "--profile", "lexical", "--profile-dir", str(self.profiles),
            "--root", str(self.root), "--out-dir", str(self.out), *args,
        ], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, expected, result.stderr or result.stdout)
        return json.loads(result.stdout or result.stderr)

    def document(self, name, text):
        (self.root / "docs" / name).write_text(text)

    def context(self, query, source_scan=False):
        prefix = ["--source-scan"] if source_scan else []
        return self.invoke(*prefix, "context", query, "--require-current", "--limit", "100", "--per-doc", "8")

    def test_owner_lifecycle_qualifiers_keep_matching_sections_and_provenance(self):
        self.document("mixed-contract.md", (
            "# Mixed contract\n\nStatus: accepted\n\n"
            "## Original decision\n\nStatus: history\n\n"
            "Legacy storage used the retired transport.\n\n"
            "## Replacement decision\n\nStatus: proposed\n\n"
            "A future transport would replace storage.\n\n"
            "## Current contract\n\nStatus: active\n\n"
            "The running transport preserves existing storage.\n"
        ))
        self.invoke("build")
        expected = {
            "history": (5, "Original decision", "history"),
            "proposed": (11, "Replacement decision", "advisory"),
            "active": (17, "Current contract", "source-of-truth"),
        }
        for status, (line, heading, authority) in expected.items():
            with self.subTest(status=status):
                query = f"docs/mixed-contract.md {status}"
                result = self.invoke("brief", query, "--limit", "3", "--per-doc", "3", "--require-current")
                self.assertEqual(result["retrieval_mode"], "owner-scoped")
                self.assertEqual(result["ranked_paths"], [{
                    "path": "docs/mixed-contract.md", "unique_path_rank": 1,
                    "first_evidence_rank": 1,
                }])
                self.assertEqual(result["query_coverage"]["covered_groups"], 2)
                self.assertEqual(result["query_coverage"]["uncovered_groups"], 0)
                sections = result["sections"]
                typed = result["contracts"] + result["decisions"] + result["runbooks"] + result["facts"]
                self.assertTrue(sections)
                self.assertTrue(typed)
                for section in sections:
                    self.assertEqual((section["line"], section["heading"]), (line, heading))
                for row in sections + typed:
                    self.assertEqual(row["path"], "docs/mixed-contract.md")
                    self.assertEqual(row["status"], status)
                    self.assertEqual(row["authority_level"], authority)
                    self.assertEqual(row["verification_level"], "declared")
                    self.assertEqual(row["matched_group_indices"], [0, 1])
                compact = self.invoke("brief", query, "--limit", "3", "--per-doc", "3",
                                      "--projection", "compact", "--require-current")
                self.assertEqual(compact["ranked_paths"], result["ranked_paths"])
                self.assertEqual(compact["snapshot_id"], result["snapshot_id"])
                self.assertTrue(compact["evidence"])
                self.assertTrue(all(row["status"] == status for row in compact["evidence"]))

    def test_working_profile_ignores_archive_drift_but_not_required_failures(self):
        self.document("current.md", "# Current contract\n\nquasarx uses the active transport.\n")
        archive = self.root / "docs/history"
        archive.mkdir()
        historical = archive / "retired.md"
        historical.write_text("# Retired contract\n\nStatus: history\n\nquasarx used another transport.\n")
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile["documentExcludePrefixes"] = ["docs/history"]
        profile["topicRules"] = [{"id": "fixture", "description": "Neutral fixture", "keywords": ["fixture"]}]
        profile["evalCases"] = [{
            "id": "required-current", "query": "quasarx", "mustPaths": ["docs/current.md"],
            "mustNotPaths": ["docs/history/retired.md"], "limit": 4, "perDocLimit": 2,
        }]
        profile["packEvalCases"] = [{
            "id": "working-pack", "expectedComplete": True, "mustPaths": ["docs/current.md"],
            "request": {
                "schema_version": "shin.task-request.v1", "request_id": "working-pack",
                "task": {"objective": "Read current transport.",
                         "must": [{"id": "current", "query": "quasarx"}]},
                "budget": {"max_evidence_bytes": 6000, "max_records": 4,
                           "per_query_records": 4, "per_document_records": 2},
            },
        }]
        profile_path.write_text(json.dumps(profile))
        history_profile = {
            **profile, "id": "history", "rootFiles": ["docs/history/retired.md"],
            "agentStartPaths": ["docs/history/retired.md"], "documentExcludePrefixes": [],
            "documentIncludePrefixes": ["docs/history"], "packEvalCases": [],
            "evalCases": [{"id": "historical-miss", "query": "quasarx",
                           "mustPaths": ["docs/current.md"], "limit": 4, "perDocLimit": 2}],
        }
        (self.profiles / "history.json").write_text(json.dumps(history_profile))
        history_out = self.base / "history-index"

        def history(*args, expected=0):
            process = subprocess.run([
                TOOL, "--profile", "history", "--profile-dir", str(self.profiles),
                "--root", str(self.root), "--out-dir", str(history_out), *args,
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(process.returncode, expected, process.stderr or process.stdout)
            return json.loads(process.stdout or process.stderr)

        self.invoke("build")
        history("build")
        baseline = self.invoke("brief", "quasarx", "--require-current")
        self.assertEqual([row["path"] for row in baseline["ranked_paths"]], ["docs/current.md"])
        self.assertTrue(self.invoke("ready", "--strict")["ready"])
        missed = history("eval", "--strict", expected=1)
        self.assertFalse(missed["ok"])
        self.assertTrue(self.invoke("ready", "--strict")["ready"])
        historical.write_text(historical.read_text() + "\nMore retired detail.\n")
        self.assertTrue(self.invoke("worktree-diff")["current"])
        self.assertFalse(history("worktree-diff")["current"])
        self.invoke("build")
        unchanged = self.invoke("brief", "quasarx", "--require-current")
        self.assertEqual(unchanged["snapshot_id"], baseline["snapshot_id"])
        self.assertTrue(self.invoke("ready", "--strict")["ready"])
        self.document("current.md", "# Current contract\n\nThe required topic was removed.\n")
        self.assertFalse(self.invoke("ready", "--strict", expected=1)["ready"])
        self.invoke("build")
        self.assertFalse(self.invoke("ready", "--strict", expected=1)["ready"])

    def test_registered_mdc_uses_markdown_without_rewriting_provenance(self):
        policy_dir = self.root / ".agent"
        policy_dir.mkdir()
        content = ("---\ndescription: Neutral policy\nalwaysApply: true\n---\n"
                   "# Neutral policy\n\n## Publication\n\n" + "ordinary context " * 100 +
                   "\nquasarx belongs to the complete section body.\n\n"
                   "## References\n\n[Target](../docs/target.md)\n")
        self.document("target.md", "# Target\n\nLinked evidence.\n")
        registered = [".agent/policy.mdc", ".agent/policy-upper.MDC", ".agent/control.txt"]
        for name in registered:
            (self.root / name).write_text(content)
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile["rootFiles"].extend(registered)
        profile_path.write_text(json.dumps(profile))
        self.invoke("build")
        generated = self.out / "docs/kb/generated"
        manifest = json.loads((generated / "manifest.json").read_text())
        sources = {row["path"]: row for row in manifest["sources"]}
        for name in registered[:2]:
            self.assertEqual(sources[name]["kind"], "markdown")
        self.assertEqual(sources[registered[2]]["kind"], "text")
        sections = [json.loads(line) for line in (generated / "sections.jsonl").read_text().splitlines()]
        for name in registered[:2]:
            section = next(row for row in sections if row["path"] == name and row["heading"] == "Publication")
            self.assertEqual((section["start_line"], section["end_line"]), (7, 10))
            self.assertNotIn("quasarx", section["preview"])
            self.assertIn("quasarx", section["indexed_text"])
        indexed = self.context("quasarx")
        scanned = self.context("quasarx", True)
        self.assertEqual(indexed["sections"], scanned["sections"])
        self.assertTrue(set(registered[:2]).issubset(indexed["related_documents"]))

    def test_topics_boost_without_admitting_candidates(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile["defaultTopic"] = "rareanchor"
        profile_path.write_text(json.dumps(profile))
        self.document("control.md", "# Control\n\nalpha content.\n")
        self.invoke("build")
        diagnosis = self.invoke("diagnose", "rareanchor", "--expected-path", "docs/control.md")
        self.assertEqual(diagnosis["ordinary_row_candidate_probe"]["strict"]["candidate_rows"], 0)
        self.assertEqual(diagnosis["matching"]["best_matched_groups"], 0)
        topics = [json.loads(line) for line in
                  (self.out / "docs/kb/generated/topics.jsonl").read_text().splitlines()]
        self.assertIn("rareanchor", [topic["id"] for topic in topics])
        profile["defaultTopic"] = "alpha"
        profile_path.write_text(json.dumps(profile))
        self.invoke("build")
        result = self.context("alpha")
        rows = [row for row in result["sections"] if row["path"] == "docs/control.md"]
        self.assertTrue(rows)
        self.assertTrue(all(row["score_components"].get("topics", 0) > 0 for row in rows))

    def test_retool_layout_mode_is_context_not_the_layout_title(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceFactRoots=["apps"], sourceFactExtensions=[".rsx", ".json"],
                       sourceFactExtractors=["retool"], compoundQueryFirstTermScope=True,
                       compoundQueryPreferredStems=["main"],
                       aliases=[{"term": "page", "expands": ["screen", "multi-page"]}])
        profile_path.write_text(json.dumps(profile))
        app = self.root / "apps" / "Atlas"
        (app / "src").mkdir(parents=True)
        (app / ".positions").mkdir()
        (app / "metadata.json").write_text("{}\n")
        (app / "main.rsx").write_text('<App><Include src="./src/overview.rsx" /></App>\n')
        (app / "src/overview.rsx").write_text('<Screen id="overview" />\n')
        for index in range(12):
            (app / ".positions" / f".other-{index}.positions.json").write_text("{}\n")
        self.invoke("build")
        result = self.invoke("brief", "apps/Atlas pages", "--limit", "2", "--per-doc", "1", "--require-current")
        self.assertEqual({row["path"] for row in result["ranked_paths"]},
                         {"apps/Atlas/main.rsx", "apps/Atlas/src/overview.rsx"})
        facts = [json.loads(line) for line in (self.out / "docs/kb/generated/facts.jsonl").read_text().splitlines()]
        layouts = [row for row in facts if row["record_type"] == "retool-layout"]
        self.assertEqual(len(layouts), 12)
        for layout in layouts:
            self.assertNotIn("multi-page", layout["title"])
            self.assertIn("layout=multi-page", layout["summary"])
        layout_query = self.context("layout multi-page")
        self.assertTrue(any(row["record_type"] == "retool-layout" for row in layout_query["facts"]))

    def test_candidate_overflow_preserves_alias_phrase_matches(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceExtensions=[".md", ".jsonl"],
                       aliases=[{"term": "recovery", "expands": ["retry window"]}])
        profile_path.write_text(json.dumps(profile))
        for count in (1, 10001):
            with self.subTest(rows=count):
                self.document("catalog.jsonl", "".join(json.dumps({
                    "id": f"entry-{index}", "type": "decision", "title": "RetryWindow",
                    "summary": "RetryWindow has bounded storage.",
                }) + "\n" for index in range(count)))
                self.invoke("build")
                result = self.context("recovery")
                self.assertIn("docs/catalog.jsonl", result["related_documents"])
                self.assertTrue(result["facts"])
                self.assertTrue(all(row["matched_group_indices"] == [0] for row in result["facts"]))
                self.assertEqual(result["query_diagnostics"]["candidate_index_used"], count == 1)
                self.assertEqual(result["query_diagnostics"]["fallback_used"], False)

    def test_internal_substrings_are_not_evidence(self):
        self.document("one.md", "# Stable Memory\n\nImmutable portable memory.\n")
        self.document("two.md", "# Table Memory\n\nTable memory.\n")
        self.invoke("build")
        for source_scan in (False, True):
            with self.subTest(source_scan=source_scan):
                result = self.context("table memory", source_scan)
                self.assertEqual(result["related_documents"], ["docs/two.md"])
                self.assertEqual(result["query_coverage"]["covered_groups"], 2)
        diagnosis = self.invoke("diagnose", "table memory", "--expected-path", "docs/one.md", "--require-current")
        self.assertEqual(diagnosis["matching"]["best_matched_groups"], 1)
        self.assertEqual(diagnosis["outcome"], "below-required-threshold")
        self.assertEqual(diagnosis["matching"]["document_matched_groups"], 1)
        self.assertEqual(diagnosis["ordinary_row_candidate_probe"]["strict"]["expected_path_admitted_rows"], 0)

    def test_query_length_does_not_enable_internal_prefix_matching(self):
        query = "loader packet author result policy request resume stream"
        self.document("reference.md", "# Reference\n\nloader packet author ResultEnvelope PolicyGuard RequestBroker ResumeToken StreamReader.\n")
        self.document("compressed.md", "# Compressed\n\nloader packet author resultenvelope policyguard requestbroker resumetoken streamreader.\n")
        self.document("vocabulary.md", "# Vocabulary\n\npolicy request resume stream.\n")
        for distractors in (0, 8):
            for index in range(distractors):
                self.document(f"other-{index}.md", "# Other\n\nloader packet author result.\n")
            self.invoke("build")
            indexed = self.context(query)
            scanned = self.context(query, True)
            with self.subTest(distractors=distractors):
                self.assertIn("docs/reference.md", indexed["related_documents"])
                self.assertNotIn("docs/compressed.md", indexed["related_documents"])
                self.assertEqual(indexed["related_documents"], scanned["related_documents"])
                reference = [row for row in indexed["sections"] if row["path"] == "docs/reference.md"]
                self.assertTrue(reference)
                self.assertTrue(any(row["matched_group_indices"] == list(range(8)) for row in reference))
                self.assertTrue(all("prefix_completion" not in row["matched_fields"] for row in reference))

    def test_compact_aliases_do_not_join_independent_prose_words(self):
        self.document("prose.md", "# Explanation\n\nThe worker is not able to proceed.\n")
        self.document("symbol.md", "# Reference\n\nCustomerSideBar handles navigation.\n")
        self.invoke("build")
        for source_scan in (False, True):
            with self.subTest(source_scan=source_scan):
                result = self.context("notable", source_scan)
                self.assertEqual(result["sections"], [])
                self.assertEqual(result["facts"], [])
                result = self.context("sidebar", source_scan)
                self.assertIn("docs/symbol.md", result["related_documents"])

    def test_repeated_query_words_do_not_manufacture_coverage(self):
        self.document("one.md", "# One\n\nconnector revision.\n")
        self.document("two.md", "# Two\n\nconnector only.\n")
        self.invoke("build")
        for source_scan in (False, True):
            once = self.context("connector revision", source_scan)
            repeated = self.context("connector connector revision", source_scan)
            self.assertEqual(once["sections"], repeated["sections"])
            self.assertEqual(repeated["query_coverage"]["total_groups"], 2)

    def test_preposition_through_does_not_gate_prose_retrieval(self):
        topic = "alpha beta gamma delta epsilon zeta eta theta"
        self.document("target.md", "# Reference\n\n" + topic + ".\n")
        self.document("peer.md", "# Peer\n\n" + topic + " through mediation.\n")
        self.document("partial.md", "# Partial\n\nalpha beta gamma delta epsilon zeta eta.\n")
        self.invoke("build")
        for source_scan in (False, True):
            with self.subTest(source_scan=source_scan):
                plain = self.context(topic, source_scan)
                connected = self.context("alpha through beta gamma delta epsilon zeta eta theta", source_scan)
                self.assertEqual(connected["query_coverage"]["total_groups"], 8)
                self.assertEqual(connected["sections"], plain["sections"])
                self.assertEqual(connected["facts"], plain["facts"])
                self.assertIn("docs/target.md", connected["related_documents"])
                self.assertNotIn("docs/partial.md", connected["related_documents"])

    def test_standalone_through_remains_searchable(self):
        self.document("target.md", "# Reference\n\nthrough mediation.\n")
        self.document("other.md", "# Other\n\nlocal mediation.\n")
        self.invoke("build")
        for source_scan in (False, True):
            with self.subTest(source_scan=source_scan):
                result = self.context("through", source_scan)
                self.assertEqual(result["query_coverage"]["total_groups"], 1)
                self.assertEqual(result["related_documents"], ["docs/target.md"])
                self.assertTrue(result["sections"])
                self.assertTrue(all(row["matched_group_indices"] == [0] for row in result["sections"]))

    def test_through_compound_identifier_and_path_remain_searchable(self):
        self.document("through-gate.md", "# ThroughGate\n\nThe route is active.\n")
        self.document("other.md", "# OtherGate\n\nThe route is active.\n")
        self.invoke("build")
        for query in ("ThroughGate", "through_gate", "through-gate", "docs/through-gate.md"):
            for source_scan in (False, True):
                with self.subTest(query=query, source_scan=source_scan):
                    result = self.context(query, source_scan)
                    self.assertEqual(result["query_coverage"]["total_groups"], 1)
                    self.assertEqual(result["related_documents"], ["docs/through-gate.md"])
                    self.assertTrue(result["sections"])

    def test_document_coverage_survives_section_split(self):
        query = "alpha beta gamma delta epsilon zeta eta theta"
        self.document("left.md", "# Left\n\nalpha beta gamma delta.\n")
        self.document("right.md", "# Right\n\nepsilon zeta eta theta.\n")
        for split in (False, True):
            text = ("# Manual\n\n## First\n\nalpha beta.\n\n"
                    "## Second\n\ngamma delta.\n\n## Third\n\nepsilon zeta.\n\n"
                    "## Fourth\n\neta theta.\n" if split else
                    "# Manual\n\nalpha beta gamma delta epsilon zeta eta theta.\n")
            self.document("manual.md", text)
            self.invoke("build")
            result = self.invoke("brief", query, "--require-current", "--limit", "8", "--per-doc", "2")
            with self.subTest(split=split):
                self.assertIn("docs/manual.md", [row["path"] for row in result["ranked_paths"]])
                self.assertEqual(result["query_coverage"]["covered_groups"], 8)
                if split:
                    self.assertEqual(result["retrieval_mode"], "planned")
                    self.assertEqual(result["query_coverage"]["selected_document_covered_groups"], 8)
                    for group in result["query_coverage"]["groups"]:
                        self.assertIn("docs/manual.md", group["selected_document_paths"])
                    for row in result["sections"]:
                        if row["path"] == "docs/manual.md":
                            self.assertLess(len(row["matched_group_indices"]), 8)
                    bounded = self.invoke("brief", query, "--require-current", "--limit", "1", "--per-doc", "1")
                    selected = {row["path"] for row in bounded["ranked_paths"]}
                    for group in bounded["query_coverage"]["groups"]:
                        self.assertLessEqual(set(group["selected_document_paths"]), selected)
                    self.assertLess(bounded["query_coverage"]["covered_groups"], 8)

    def test_supplemental_planner_accounts_for_all_qualified_documents(self):
        query = "alpha beta gamma delta epsilon zeta eta theta"
        self.document("left.md", "# Left\n\nalpha beta gamma delta.\n")
        self.document("right.md", "# Right\n\nepsilon zeta eta theta.\n")
        self.document(
            "manual.md",
            "# Manual\n\n"
            "## First\n\nalpha beta.\n\n"
            "## Second\n\ngamma delta.\n\n"
            "## Third\n\nepsilon zeta.\n\n"
            "## Fourth\n\neta theta.\n",
        )
        for document_index in range(12):
            sections = []
            for section_index in range(20):
                words = "alpha beta" if section_index % 2 == 0 else "gamma delta"
                sections.append(f"## Noise {document_index} Section {section_index}\n\n{words}.\n")
            self.document(f"noise-{document_index}.md", "# Noise\n\n" + "\n".join(sections))

        self.invoke("build")
        result = self.invoke("brief", query, "--require-current", "--limit", "8", "--per-doc", "2")
        ranked_paths = {row["path"] for row in result["ranked_paths"]}
        self.assertTrue(result["query_diagnostics"]["query_planner_used"])
        self.assertTrue({"docs/manual.md", "docs/left.md", "docs/right.md"}.issubset(ranked_paths))
        self.assertEqual(result["query_coverage"]["covered_groups"], 8)
        self.assertEqual(result["query_coverage"]["selected_document_covered_groups"], 8)
        # Every fixture owner covers at least four groups. Rarity must not hide work.
        stores = ("sections", "facts", "decisions", "contracts", "runbooks")
        admitted = {store: set() for store in stores}
        for posting in term_postings(self.out / "docs/kb/generated/terms.lex"):
            if posting["term"] in query.split():
                for store in stores:
                    admitted[store].update(posting[store])
        self.assertEqual(result["query_diagnostics"]["candidate_rows"],
                         sum(len(rows) for rows in admitted.values()))

    def test_document_query_survives_unrelated_strict_rows(self):
        words = "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi".split()
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile["sourceExtensions"] = [".md", ".jsonl"]
        profile_path.write_text(json.dumps(profile))
        for count in (4, 5, 7, 8, 14):
            with self.subTest(groups=count):
                query = " ".join(words[:count])
                self.document("other.md", f"# Other\n\n{query}.\n")
                self.document("another.md", f"# Another\n\n{query}.\n")
                first, second = " ".join(words[:2]), " ".join(words[2:count])
                self.document("manual.md", f"# Manual\n\n## First\n\n{first}.\n\n"
                              f"## Second\n\n{second}.\n")
                self.document("partial.md", "# Partial\n\n" + " ".join(words[:count - 1]) + ".\n")
                self.document("rows.jsonl", "\n".join(json.dumps({
                    "id": str(index), "type": "contract", "title": value,
                }) for index, value in enumerate((first, second))) + "\n")
                self.invoke("build")
                result = self.invoke("brief", query, "--require-current", "--limit", "8", "--per-doc", "2")
                paths = [row["path"] for row in result["ranked_paths"]]
                self.assertIn("docs/manual.md", paths)
                self.assertNotIn("docs/partial.md", paths)
                self.assertNotIn("docs/rows.jsonl", paths)
                self.assertTrue(result["query_diagnostics"]["query_planner_used"])
                self.assertEqual(result["query_coverage"]["selected_document_covered_groups"], count)
                for row in result["sections"]:
                    if row["path"] == "docs/manual.md":
                        self.assertLess(len(row["matched_group_indices"]), count)

    def test_short_supplement_preserves_terse_and_multiple_strict_owners(self):
        for query, owners, limit in (("alpha beta gamma", 2, 8),
                                     ("alpha beta gamma delta epsilon", 2, 2),
                                     ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi", 2, 2)):
            with self.subTest(query=query, owners=owners, limit=limit):
                for path in (self.root / "docs").iterdir():
                    path.unlink()
                for index in range(owners):
                    self.document(f"other-{index}.md", f"# Other\n\n{query}.\n")
                words = query.split()
                self.document("manual.md", "# Manual\n\n## First\n\n" + " ".join(words[:2]) +
                              ".\n\n## Second\n\n" + " ".join(words[2:]) + ".\n")
                self.invoke("build")
                result = self.invoke("brief", query, "--require-current", "--limit", str(limit), "--per-doc", "2")
                self.assertFalse(result["query_diagnostics"]["planner_attempted"])
                self.assertEqual({row["path"] for row in result["ranked_paths"]},
                                 {f"docs/other-{index}.md" for index in range(owners)})

    def test_single_strict_document_does_not_hide_relaxed_candidates(self):
        query = "alpha beta gamma delta epsilon"
        self.document("focused.md", "# Alpha beta gamma delta\n\nA bounded policy.\n")
        self.document("broad.md", "# Overview\n\n" + "ordinary filler " * 100 + query + ".\n")
        self.invoke("build")
        admitted = []
        for limit in (1, 4, 8):
            result = self.invoke("brief", query, "--limit", str(limit), "--per-doc", "1")
            self.assertEqual(result["ranked_paths"][0]["path"], "docs/focused.md")
            self.assertTrue(result["query_diagnostics"]["fallback_used"])
            self.assertFalse(result["query_diagnostics"]["query_planner_used"])
            admitted.append(result["query_diagnostics"]["candidate_rows"])
            row = next(row for row in result["sections"] if row["path"] == "docs/focused.md")
            self.assertEqual(row["matched_group_indices"], [0, 1, 2, 3])
        self.assertEqual(admitted, [2, 2, 2])

    def test_sparse_prose_expansion_rejects_high_score_minority_matches(self):
        query = "alpha beta gamma delta epsilon zeta eta"
        self.document("broad.md", "# Overview\n\n" + query + ".\n")
        self.document("policy.md", "# Policy\n\nalpha beta gamma delta epsilon.\n")
        for index in range(4):
            self.document(f"noise-{index}.md", "# Alpha beta gamma\n\nUnrelated material.\n")
        self.invoke("build")
        for limit in (2, 8):
            result = self.invoke("brief", query, "--limit", str(limit), "--per-doc", "1")
            self.assertEqual({row["path"] for row in result["ranked_paths"]}, {"docs/broad.md", "docs/policy.md"})
            self.assertTrue(result["query_diagnostics"]["fallback_used"])
            self.assertEqual(result["query_diagnostics"]["required_group_matches"], 4)

    def test_brief_selection_preserves_unique_paths_before_duplicate_evidence(self):
        query = "oak birch cedar maple willow copper quartz"
        expected_paths = {f"docs/distractor-{index}.md" for index in range(6)}
        expected_paths.add("docs/target.md")
        for index in range(6):
            sections = "\n\n".join(
                f"## {query} Detail {section}\n\nSupporting detail {section}."
                for section in range(3)
            )
            self.document(f"distractor-{index}.md", "# Reference\n\n" + sections + "\n")
        self.document("target.md", f"# Target\n\nThe reference covers {query}.\n")
        self.invoke("build")

        legacy_context = self.invoke("context", query, "--limit", "8", "--per-doc", "3")
        self.assertEqual(
            [(row["path"], row["line"]) for row in legacy_context["sections"]],
            [(f"docs/distractor-{index}.md", line)
             for index, line in ((0, 3), (0, 7), (0, 11), (1, 3), (1, 7), (1, 11), (2, 3), (2, 7))],
        )

        expanded = self.invoke("brief", query, "--limit", "100", "--per-doc", "3")
        target_rank = next(row for row in expanded["ranked_paths"] if row["path"] == "docs/target.md")
        self.assertEqual(target_rank["unique_path_rank"], 7)
        self.assertLessEqual(target_rank["first_evidence_rank"], 8)

        bounded = self.invoke("brief", query, "--limit", "8", "--per-doc", "3")
        bounded_paths = [row["path"] for row in bounded["ranked_paths"]]
        self.assertEqual(set(bounded_paths), expected_paths)
        self.assertEqual(bounded["evidence_budget"]["returned_records"], 8)
        identities = [(row["path"], row["line"], row["heading"]) for row in bounded["sections"]]
        self.assertEqual(len(identities), len(set(identities)))
        self.assertEqual(identities[-1], (
            "docs/distractor-0.md", 7,
            "oak birch cedar maple willow copper quartz Detail 1",
        ))

        self.document("single.md", (
            "# Single\n\n## Solo amber violet\n\nFirst.\n\n"
            "## Solo amber silver\n\nSecond.\n\n"
            "## Solo amber gold\n\nThird.\n"
        ))
        self.invoke("build")
        single = self.invoke("brief", "solo amber", "--limit", "3", "--per-doc", "3")
        self.assertEqual([row["path"] for row in single["ranked_paths"]], ["docs/single.md"])
        self.assertEqual([row["heading"] for row in single["sections"]],
                         ["Solo amber violet", "Solo amber silver", "Solo amber gold"])

    def test_strict_owner_count_is_measured_before_fact_output_limit(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile["sourceExtensions"] = [".md", ".jsonl"]
        profile_path.write_text(json.dumps(profile))
        query = "alpha beta gamma delta epsilon"
        for name in ("one", "two"):
            self.document(name + ".jsonl", json.dumps({"id": name, "type": "fact",
                "title": query}) + "\n")
        self.document("partial.md", "# Alpha beta gamma delta\n\nPartial coverage.\n")
        self.invoke("build")
        for limit in (1, 8):
            result = self.invoke("brief", query, "--limit", str(limit), "--per-doc", "1")
            self.assertFalse(result["query_diagnostics"]["fallback_used"])
            self.assertNotIn("docs/partial.md", [row["path"] for row in result["ranked_paths"]])

    def test_authority_bonus_is_not_a_primary_field_match(self):
        query = "alpha beta gamma delta epsilon"
        self.document("focused.md", "# Alpha beta gamma delta\n\nBounded evidence.\n")
        self.document("broad-contract.md", "# Overview\n\n" + "ordinary filler " * 100 + query + ".\n")
        self.invoke("build")
        strict = self.context(query)
        self.assertEqual(len(strict["sections"]), 1)
        self.assertEqual(strict["sections"][0]["authority_level"], "source-of-truth")
        self.assertIn("authority_level", strict["sections"][0]["matched_fields"])
        for limit in (1, 8):
            result = self.invoke("brief", query, "--limit", str(limit), "--per-doc", "1")
            self.assertTrue(result["query_diagnostics"]["fallback_used"])
            self.assertEqual(result["ranked_paths"][0]["path"], "docs/focused.md")

    def test_lower_weight_status_match_preserves_focused_retrieval(self):
        query = "active alpha beta gamma epsilon"
        self.document("broad.md", "# Overview\n\n" + "ordinary filler " * 100 + query + ".\n")
        self.document("review-focused.md", "# Alpha beta gamma epsilon\n\nBounded evidence.\n")
        self.invoke("build")
        strict = self.context(query)
        self.assertEqual(len(strict["sections"]), 1)
        self.assertEqual(strict["sections"][0]["status"], "active")
        self.assertNotIn("status", strict["sections"][0]["matched_fields"])
        result = self.invoke("brief", query, "--limit", "8", "--per-doc", "1")
        self.assertFalse(result["query_diagnostics"]["fallback_used"])
        self.assertEqual([row["path"] for row in result["ranked_paths"]], ["docs/broad.md"])

    def test_multiple_body_only_owners_admit_only_near_complete_rows(self):
        words = "alpha beta gamma delta epsilon zeta eta theta iota kappa".split()
        words.extend(f"term{index:02}" for index in range(54))
        for count in (5, 7, 8, 10, 64):
            with self.subTest(groups=count):
                query = " ".join(words[:count])
                for name in ("one", "two"):
                    self.document(name + ".md", "# Overview\n\n" + "ordinary filler " * 50 + query + ".\n")
                self.document("focused.md", "# " + " ".join(words[:count - 1]) + "\n\nBounded evidence.\n")
                self.document("partial.md", "# " + " ".join(words[:count - 2]) + "\n\nWeak partial evidence.\n")
                self.invoke("build")
                result = self.invoke("brief", query, "--limit", "8", "--per-doc", "1")
                self.assertTrue(result["query_diagnostics"]["fallback_used"])
                self.assertTrue(result["query_diagnostics"]["query_planner_used"])
                self.assertEqual(result["query_diagnostics"]["candidate_rows"], 3)
                self.assertEqual(result["ranked_paths"][0]["path"], "docs/focused.md")
                self.assertNotIn("docs/partial.md", [row["path"] for row in result["ranked_paths"]])
                focused = next(row for row in result["sections"] if row["path"] == "docs/focused.md")
                self.assertEqual(focused["matched_group_indices"], list(range(count - 1)))
                missing_group = result["query_coverage"]["groups"][count - 1]
                self.assertNotIn("docs/focused.md", missing_group["returned_evidence_paths"])
                self.assertNotIn("docs/focused.md", missing_group["selected_document_paths"])

    def test_near_complete_body_keeps_same_document_heading_corroboration(self):
        query = "alpha beta gamma delta epsilon"
        for name in ("one", "two"):
            self.document(name + ".md", "# Overview\n\n" + query + ".\n")
        self.document("alpha-beta-policy.md",
                      "# Alpha beta policy\n\n## Decision\n\ngamma delta.\n")
        self.document("alpha-beta-noise.md",
                      "# Overview\n\n## Decision\n\ngamma delta.\n")
        self.invoke("build")
        result = self.invoke("brief", query, "--limit", "8", "--per-doc", "3")
        paths = [row["path"] for row in result["ranked_paths"]]
        self.assertIn("docs/alpha-beta-policy.md", paths)
        self.assertLessEqual(paths.index("docs/alpha-beta-policy.md"), 1)
        self.assertNotIn("docs/alpha-beta-noise.md", paths)
        row = next(row for row in result["sections"]
                   if row["path"] == "docs/alpha-beta-policy.md" and row["heading"] == "Decision")
        self.assertEqual(row["matched_group_indices"], [0, 1, 2, 3])
        self.assertNotIn("docs/alpha-beta-policy.md",
                         result["query_coverage"]["groups"][4]["returned_evidence_paths"])

    def test_multiple_body_owner_expansion_preserves_primary_heading_match(self):
        words = "alpha beta gamma delta epsilon zeta eta theta iota kappa".split()
        for count in (5, 8, 10):
            with self.subTest(groups=count):
                query = " ".join(words[:count])
                self.document("one.md", "# Alpha\n\n" + query + ".\n")
                self.document("two.md", "# Overview\n\n" + query + ".\n")
                self.document("focused.md", "# " + " ".join(words[:count - 1]) + "\n\nBounded evidence.\n")
                self.invoke("build")
                result = self.invoke("brief", query, "--limit", "8", "--per-doc", "1")
                self.assertFalse(result["query_diagnostics"]["fallback_used"])
                self.assertNotIn("docs/focused.md", [row["path"] for row in result["ranked_paths"]])

    def test_incidental_owner_words_do_not_suppress_near_complete_sections(self):
        words = "alpha beta gamma delta epsilon zeta eta theta iota kappa".split()
        for count in (5, 8, 10):
            with self.subTest(groups=count):
                query = " ".join(words[:count])
                for name in ("one", "two"):
                    self.document("alpha-" + name + ".md", "# Overview\n\n" + query + ".\n")
                self.document("focused.md", "# " + " ".join(words[:count - 1]) + "\n\nBounded evidence.\n")
                self.document("alpha-owner-only.md", "# Overview\n\n" + " ".join(words[:count - 1]) + ".\n")
                self.invoke("build")
                result = self.invoke("brief", query, "--limit", "8", "--per-doc", "1")
                self.assertTrue(result["query_diagnostics"]["query_planner_used"])
                self.assertEqual(result["query_diagnostics"]["candidate_rows"], 4)
                self.assertEqual(result["ranked_paths"][0]["path"], "docs/focused.md")
                self.assertNotIn("docs/alpha-owner-only.md", [row["path"] for row in result["ranked_paths"]])
                focused = next(row for row in result["sections"] if row["path"] == "docs/focused.md")
                self.assertEqual(focused["matched_group_indices"], list(range(count - 1)))
                missing = result["query_coverage"]["groups"][count - 1]
                self.assertNotIn("docs/focused.md", missing["returned_evidence_paths"])
                self.assertNotIn("docs/focused.md", missing["selected_document_paths"])

    def test_multiple_body_owner_expansion_overflow_retains_strict_result(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile["sourceExtensions"] = [".md", ".jsonl"]
        profile_path.write_text(json.dumps(profile))
        words = "alpha beta gamma delta epsilon zeta eta theta iota kappa".split()
        for count in (5, 10):
            with self.subTest(groups=count):
                query = " ".join(words[:count])
                for name in ("one", "two"):
                    self.document(name + ".md", "# Overview\n\n" + query + ".\n")
                self.document("noise.jsonl", "\n".join(json.dumps({"id": str(i), "type": "fact",
                    "title": " ".join(words[:count - 1])}) for i in range(20001)) + "\n")
                self.invoke("build")
                result = self.invoke("brief", query, "--limit", "8", "--per-doc", "1")
                self.assertFalse(result["query_diagnostics"]["fallback_used"])
                # An overflowing individual group does not prevent the bounded
                # strict intersection from using the index. The incomplete
                # document planner must still be rejected below.
                self.assertTrue(result["query_diagnostics"]["candidate_index_used"])
                self.assertTrue(result["query_diagnostics"]["planner_attempted"])
                self.assertEqual(result["query_diagnostics"]["planner_rejection_reason"],
                                 "document-postings-incomplete")
                self.assertEqual({row["path"] for row in result["ranked_paths"]}, {"docs/one.md", "docs/two.md"})

    def test_sparse_expansion_overflow_retains_strict_result(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile["sourceExtensions"] = [".md", ".jsonl"]
        profile_path.write_text(json.dumps(profile))
        query = "alpha beta gamma delta epsilon"
        self.document("strict.md", "# Strict\n\n" + query + ".\n")
        self.document("noise.jsonl", "\n".join(json.dumps({"id": str(i), "type": "fact",
            "title": "alpha beta gamma"}) for i in range(20001)) + "\n")
        self.invoke("build")
        result = self.invoke("brief", query, "--limit", "8", "--per-doc", "1")
        self.assertFalse(result["query_diagnostics"]["fallback_used"])
        self.assertTrue(result["query_diagnostics"]["candidate_index_used"])
        self.assertGreater(result["query_diagnostics"]["candidate_rows"], 20000)
        self.assertEqual([row["path"] for row in result["ranked_paths"]], ["docs/strict.md"])

    def test_single_strict_fact_preserves_focused_retrieval(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile["sourceExtensions"] = [".md", ".jsonl"]
        profile_path.write_text(json.dumps(profile))
        query = "alpha beta gamma delta epsilon"
        self.document("exact.jsonl", json.dumps({"id": "exact", "type": "fact",
            "title": query}) + "\n")
        self.document("partial.md", "# Alpha beta gamma delta\n\nPartial coverage.\n")
        self.invoke("build")
        result = self.invoke("brief", query, "--limit", "8", "--per-doc", "1")
        self.assertFalse(result["query_diagnostics"]["fallback_used"])
        self.assertEqual([row["path"] for row in result["ranked_paths"]], ["docs/exact.jsonl"])

    def test_long_complete_json_match_is_not_replaced_by_partial_titles(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceExtensions=[".md", ".json", ".jsonl"], jsonKeySections=True)
        profile_path.write_text(json.dumps(profile))
        query = "alpha beta gamma delta epsilon zeta eta theta"
        self.document("complete.json", json.dumps({
            "document": "complete-policy", "summary": query,
        }, indent=2))
        for index in range(4):
            self.document(f"partial-{index}.jsonl", json.dumps({
                "id": f"partial-{index}", "type": "fact",
                "title": "alpha beta gamma delta epsilon",
                "summary": "A separate partial policy.",
            }) + "\n")
        self.invoke("build")
        for limit in (1, 8):
            result = self.invoke("brief", query, "--limit", str(limit), "--per-doc", "2")
            self.assertTrue(result["query_diagnostics"]["fallback_used"])
            self.assertEqual(result["ranked_paths"][0]["path"], "docs/complete.json")
            if limit > 1:
                self.assertGreater(len(result["ranked_paths"]), 1)
            self.assertEqual(result["query_coverage"]["returned_evidence_covered_groups"], 8)

        self.document("complete.json", json.dumps({
            "document": "complete-policy", "status": "history", "summary": query,
        }, indent=2))
        self.invoke("build")
        historical = self.invoke("brief", query, "--limit", "8", "--per-doc", "2")
        self.assertNotEqual(historical["ranked_paths"][0]["path"], "docs/complete.json")

    def test_strict_heading_or_path_match_preserves_focused_retrieval(self):
        query = "alpha beta gamma delta epsilon"
        for name, heading in (("named.md", "Alpha"), ("alpha.md", "Overview")):
            with self.subTest(name=name):
                for path in (self.root / "docs").iterdir():
                    path.unlink()
                self.document(name, "# " + heading + "\n\n" + query + ".\n")
                self.document("partial.md", "# Beta gamma delta epsilon\n\nPartial evidence.\n")
                self.invoke("build")
                result = self.invoke("brief", query, "--limit", "8", "--per-doc", "1")
                self.assertFalse(result["query_diagnostics"]["fallback_used"])
                self.assertEqual([row["path"] for row in result["ranked_paths"]], ["docs/" + name])

    def test_supplemental_planner_waits_for_ordinary_budget(self):
        query = "alpha beta gamma delta epsilon zeta eta theta"
        self.document("left.md", "# Left\n\nalpha beta gamma delta.\n")
        self.document("right.md", "# Right\n\nepsilon zeta eta theta.\n")
        self.document(
            "manual.md",
            "# Manual\n\n"
            "## First\n\nalpha beta.\n\n"
            "## Second\n\ngamma delta.\n\n"
            "## Third\n\nepsilon zeta.\n\n"
            "## Fourth\n\neta theta.\n",
        )
        self.invoke("build")

        ordinary = self.invoke("brief", query, "--require-current", "--limit", "2", "--per-doc", "2")
        ordinary_paths = {row["path"] for row in ordinary["ranked_paths"]}
        self.assertEqual(ordinary_paths, {"docs/left.md", "docs/right.md"})
        self.assertFalse(ordinary["query_diagnostics"]["query_planner_used"])

        expanded = self.invoke("brief", query, "--require-current", "--limit", "8", "--per-doc", "2")
        expanded_paths = {row["path"] for row in expanded["ranked_paths"]}
        self.assertTrue(expanded["query_diagnostics"]["query_planner_used"])
        self.assertIn("docs/manual.md", expanded_paths)
        self.assertEqual(expanded["query_coverage"]["covered_groups"], 8)

    def test_json_key_rows_own_scalar_and_container_spans(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile.update(
            sourceExtensions=[".md", ".json"],
            jsonKeySections=True,
            jsonKeyMaxDepth=0,
            jsonKeyMaxSectionsPerFile=20,
        )
        profile_path.write_text(json.dumps(profile))
        settings = {
            "settings": {
                "alpha": "alpha ownvalue",
                "container": {
                    "nestedvalue": [
                        "quoted } brace",
                        {"deep": 'escaped "quoted } brace"'},
                    ],
                },
                "beta": "beta otherfield",
            }
        }
        self.document("settings.json", json.dumps(settings, indent=2) + "\n")
        self.invoke("build")

        sections_path = self.out / "docs/kb/generated/sections.jsonl"
        rows = [json.loads(line) for line in sections_path.read_text().splitlines()]
        rows = [row for row in rows if row["path"] == "docs/settings.json"]
        self.assertTrue(rows)
        self.assertTrue(all(row["row_type"] == "json-key" for row in rows))
        by_heading = {row["heading"]: row for row in rows}

        alpha = by_heading["alpha"]
        self.assertEqual((alpha["start_line"], alpha["end_line"]), (3, 3))
        self.assertIn("alpha ownvalue", alpha["preview"])
        self.assertNotIn("beta otherfield", alpha["preview"])

        container = by_heading["container"]
        self.assertEqual((container["start_line"], container["end_line"]), (4, 11))
        self.assertIn("nestedvalue", container["preview"])
        self.assertIn("quoted } brace", container["preview"])
        self.assertNotIn("beta otherfield", container["preview"])

        nested = by_heading["nestedvalue"]
        self.assertEqual((nested["start_line"], nested["end_line"]), (5, 10))
        self.assertIn("quoted } brace", nested["preview"])
        self.assertIn("escaped", nested["preview"])

        deep = by_heading["deep"]
        self.assertEqual((deep["start_line"], deep["end_line"]), (8, 8))
        self.assertIn('escaped \\"quoted } brace\\"', deep["preview"])

        beta = by_heading["beta"]
        self.assertEqual((beta["start_line"], beta["end_line"]), (12, 12))
        self.assertIn("beta otherfield", beta["preview"])

    def test_exact_filename_owner_is_preserved(self):
        self.document("CURRENT-CONTRACT.md", "# Operations\n\nUnrelated descriptive content.\n")
        self.invoke("build")
        result = self.invoke("brief", "CURRENT-CONTRACT.md", "--require-current")
        self.assertEqual(result["retrieval_mode"], "owner-scoped")
        self.assertEqual(result["ranked_paths"][0]["path"], "docs/CURRENT-CONTRACT.md")
        indexed = self.context("CURRENT-CONTRACT.md")
        self.assertEqual(indexed["related_documents"], ["docs/CURRENT-CONTRACT.md"])
        self.assertEqual(indexed["related_documents"], self.context("CURRENT-CONTRACT.md", True)["related_documents"])

    def test_explicit_workflow_filename_beats_thematic_plan(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile["rootFiles"].append(".github/workflows/ci.yml")
        profile_path.write_text(json.dumps(profile))
        workflow = self.root / ".github/workflows/ci.yml"
        workflow.parent.mkdir(parents=True)
        workflow.write_text(
            "name: CI\n\non: [push]\n\njobs:\n  image:\n    steps:\n"
            "      - uses: google-github-actions/auth@v3\n"
            "        with:\n          workload_identity_provider: provider-reference\n"
        )
        self.document("access-plan.md", (
            "# Contracts: new CI and local workflow\n\n## CI identity\n\n"
            "Both image-publication workflows use Google Workload Identity Federation.\n"
            "Prefer a separate service account for a contracts job.\n"
        ))
        self.invoke("build")
        query = "CI workflow ci.yml Workload Identity image"
        for spelling in (query, "Workload Identity image CI workflow ci.yml",
                         "CI workflow .github/workflows/ci.yml Workload Identity image"):
            with self.subTest(query=spelling):
                result = self.invoke("brief", spelling, "--require-current", "--limit", "8", "--per-doc", "3")
                self.assertEqual(result["retrieval_mode"], "owner-scoped")
                self.assertEqual(result["ranked_paths"][0]["path"], ".github/workflows/ci.yml")
                diagnostic = self.invoke("diagnose", spelling, "--expected-path", ".github/workflows/ci.yml",
                                         "--limit", "8", "--per-doc", "3", "--require-current")
                self.assertEqual(diagnostic["returned_path_rank"], 1)
        for spelling in ("CI workflow Workload Identity image", "CI workflow ci.yaml Workload Identity image"):
            with self.subTest(query=spelling):
                result = self.invoke("brief", spelling, "--require-current")
                self.assertNotEqual(result["retrieval_mode"], "owner-scoped")
                self.assertEqual(result["ranked_paths"][0]["path"], "docs/access-plan.md")

    def test_embedded_filename_preserves_prefix_qualifiers_and_coverage(self):
        owner = self.owner_key_document()
        result = self.invoke("brief", "beta " + owner + " alpha", "--per-doc", "2", "--require-current")
        self.assertEqual(result["retrieval_mode"], "owner-scoped")
        self.assertEqual(result["query_diagnostics"]["scope_group_index"], 1)
        groups = {row["heading"]: row["matched_group_indices"] for row in result["sections"]}
        self.assertEqual(groups, {"alpha": [1, 2], "beta": [0, 1]})
        self.assertEqual(result["query_coverage"]["covered_groups"], 3)
        query = "absentword " + owner + " alpha"
        result = self.invoke("brief", query, "--per-doc", "1", "--require-current")
        self.assertEqual([row["heading"] for row in result["sections"]], ["alpha"])
        self.assertFalse(result["query_coverage"]["groups"][0]["covered"])
        self.assertEqual(result["sections"][0]["matched_group_indices"], [1, 2])
        compact = self.invoke("brief", query, "--per-doc", "1", "--require-current", "--projection", "compact")
        self.assertEqual(compact["ranked_paths"], result["ranked_paths"])
        self.assertEqual(compact["query_coverage"]["covered_groups"], result["query_coverage"]["covered_groups"])
        self.assertEqual(compact["query_coverage"]["uncovered_groups"], [{"index": 0, "term": "absentword"}])

    def test_embedded_filename_does_not_choose_ambiguous_or_multiple_owners(self):
        for directory in ("alpha", "beta"):
            (self.root / "docs" / directory).mkdir()
            self.document(directory + "/settings.md", "# Deployment\n\nRegistry configuration changes.\n")
        self.document("left.md", "# Changes\n\nLeft configuration changes.\n")
        self.document("right.md", "# Changes\n\nRight configuration changes.\n")
        self.invoke("build")
        for query in ("deployment settings.md registry", "compare docs/left.md docs/right.md changes",
                      "docs/left.md docs/right.md changes"):
            with self.subTest(query=query):
                result = self.invoke("brief", query, "--require-current")
                self.assertNotEqual(result["retrieval_mode"], "owner-scoped")
        result = self.invoke("brief", "deployment docs/alpha/settings.md registry", "--require-current")
        self.assertEqual(result["retrieval_mode"], "owner-scoped")
        self.assertEqual(result["ranked_paths"][0]["path"], "docs/alpha/settings.md")

    def test_embedded_filename_preserves_leading_literal_and_directory_scope(self):
        self.document("release notes.md", "# Release\n\nDeployment guide.\n")
        self.document("notes.md", "# Notes\n\nDeployment guide.\n")
        (self.root / "docs/alpha").mkdir()
        self.document("alpha/local.md", "# Local registry\n\nDeployment guide.\n")
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile["compoundQueryFirstTermScope"] = True
        profile_path.write_text(json.dumps(profile))
        self.invoke("build")
        for query in ("release notes.md deployment", "docs/release notes.md deployment"):
            result = self.invoke("brief", query, "--require-current")
            self.assertEqual(result["retrieval_mode"], "owner-scoped")
            self.assertEqual(result["ranked_paths"][0]["path"], "docs/release notes.md")
        # Embedded multiword names are not new owner anchors, but their final
        # token must not select a different, shorter indexed filename either.
        result = self.invoke("brief", "topic release notes.md deployment", "--require-current")
        self.assertNotEqual(result["retrieval_mode"], "owner-scoped")
        result = self.invoke("brief", "docs/alpha notes.md registry deployment guide", "--require-current")
        self.assertNotEqual(result["retrieval_mode"], "owner-scoped")
        self.assertTrue(all(row["path"].startswith("docs/alpha/") for row in result["ranked_paths"]))
        self.assertTrue(result["ranked_paths"])

    def test_embedded_filename_requires_exact_manifest_identity(self):
        self.document("record-one.md", "# Record\n\nPolicy decisions.\n")
        self.document("record_one.md", "# Record\n\nOther policy decisions.\n")
        self.document("guide.md", "# Operations\n\nInstructions refer to missing.md.\n")
        (self.root / "private.bin").write_text("unpublished operational file")
        self.invoke("build")
        for query, expected in (("policy record-one.md decisions", "docs/record-one.md"),
                                ("policy record_one.md decisions", "docs/record_one.md")):
            with self.subTest(query=query):
                result = self.invoke("brief", query, "--require-current")
                self.assertEqual(result["retrieval_mode"], "owner-scoped")
                self.assertEqual(result["ranked_paths"][0]["path"], expected)
        for query in ("operations missing.md instructions", "operations private.bin instructions",
                      "policy record.one.md decisions"):
            with self.subTest(query=query):
                result = self.invoke("brief", query, "--require-current")
                self.assertNotEqual(result["retrieval_mode"], "owner-scoped")
                self.assertNotIn("private.bin", [row["path"] for row in result["ranked_paths"]])

    def test_humanized_component_owner_beats_single_word_document(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".tsx"])
        profile_path.write_text(json.dumps(profile))
        (self.root / "src").mkdir()
        (self.root / "src/RelayBoardPage.tsx").write_text(
            "export function RelayBoardPage() { return hostView; }\n")
        self.document("relay.md", "# Relay\n\nThe relay board host page is described here.\n")
        self.invoke("build")
        for query, owner in [
            ("relay board host page", "src/RelayBoardPage.tsx"),
            ("RelayBoardPage host", "src/RelayBoardPage.tsx"),
            ("relay.md board host page", "docs/relay.md"),
            ("docs/relay.md board host page", "docs/relay.md"),
        ]:
            with self.subTest(query=query):
                result = self.invoke("brief", query, "--require-current")
                self.assertEqual(result["retrieval_mode"], "owner-scoped")
                self.assertEqual(result["ranked_paths"][0]["path"], owner)

    def test_ambiguous_humanized_owner_does_not_select_single_word_document(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".tsx"])
        profile_path.write_text(json.dumps(profile))
        for directory in ("alpha", "beta"):
            parent = self.root / "src" / directory
            parent.mkdir(parents=True)
            (parent / "RelayBoardPage.tsx").write_text(
                "export function RelayBoardPage() { return hostView; }\n")
        self.document("relay.md", "# Relay\n\nThe relay board host page is described here.\n")
        self.invoke("build")
        result = self.invoke("brief", "relay board host page", "--require-current")
        self.assertNotEqual(result["retrieval_mode"], "owner-scoped")
        paths = {item["path"] for item in result["ranked_paths"]}
        self.assertTrue({"src/alpha/RelayBoardPage.tsx", "src/beta/RelayBoardPage.tsx"} <= paths)

    def owner_key_document(self, summary="Shared document entry."):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceExtensions=[".md", ".json"], jsonKeySections=True,
                       jsonKeyMaxDepth=1)
        profile_path.write_text(json.dumps(profile))
        (self.root / "docs/catalog").mkdir()
        self.document("catalog/atlas.json", json.dumps({
            "schemaVersion": "fixture.atlas.v1", "document": "atlas",
            "status": "active", "title": "Atlas contract", "summary": summary,
            "alpha": ["waiting", "complete"],
            "beta": ["add", "remove"],
        }, indent=2) + "\n")
        self.invoke("build")
        return "docs/catalog/atlas.json"

    def test_owner_qualifiers_keep_both_keys_before_generic_type_seed(self):
        owner = self.owner_key_document()
        query = owner + " catalog alpha beta"
        args = ("--limit", "6", "--per-doc", "2", "--require-current")

        result = self.invoke("brief", query, *args)

        self.assertEqual(result["retrieval_mode"], "owner-scoped")
        self.assertEqual([row["heading"] for row in result["sections"]], ["alpha", "beta"])
        self.assertEqual(result["contracts"], [])
        self.assertIn('"waiting"', result["sections"][0]["preview"])
        self.assertIn('"remove"', result["sections"][1]["preview"])
        self.assertEqual(result["query_coverage"]["returned_evidence_covered_groups"], 4)
        self.assertEqual(result["matched_sections"], 7)
        self.assertEqual(result["matched_facts"], 1)
        self.assertEqual(result["query_diagnostics"]["candidate_rows"], 8)
        compact = self.invoke("brief", query, *args, "--projection", "compact")
        self.assertEqual([(row["type"], row["heading"]) for row in compact["evidence"]],
                         [("section", "alpha"), ("section", "beta")])
        self.assertEqual(compact["verification_paths"][0]["ranges"], [
            {"start_line": 7, "end_line": 10}, {"start_line": 11, "end_line": 14},
        ])
        bounded = self.invoke("brief", query, "--limit", "6", "--per-doc", "1", "--require-current")
        self.assertEqual([row["heading"] for row in bounded["sections"]], ["alpha"])
        self.assertEqual(bounded["contracts"], [])
        self.assertEqual(bounded["query_coverage"]["covered_groups"], 3)
        self.assertFalse(bounded["query_coverage"]["groups"][-1]["covered"])

    def test_owner_without_matching_qualifier_keeps_entry_fallback(self):
        owner = self.owner_key_document()
        for query in (owner, owner + " catalog", owner + " catalog absentword"):
            with self.subTest(query=query):
                result = self.invoke("brief", query, "--limit", "6", "--per-doc", "2", "--require-current")
                self.assertEqual(result["retrieval_mode"], "owner-scoped")
                self.assertEqual([row["path"] for row in result["ranked_paths"]], [owner])
                self.assertEqual([row["summary"] for row in result["contracts"]], ["Shared document entry."])
                if "absentword" in query:
                    self.assertFalse(result["query_coverage"]["groups"][-1]["covered"])

    def test_owner_qualifier_filter_respects_allowed_evidence_types(self):
        owner = self.owner_key_document()
        request = self.base / "request.json"
        request.write_text(json.dumps({
            "schema_version": "shin.task-request.v1", "request_id": "owner-contract-only",
            "task": {"objective": "Read permitted owner entry.",
                     "must": [{"id": "owner", "query": owner + " catalog alpha beta"}]},
            "evidence_types": ["contract"],
            "budget": {"max_evidence_bytes": 8000, "max_records": 2,
                       "per_query_records": 6, "per_document_records": 2},
        }))

        result = self.invoke("pack", "--input", str(request), "--require-current")

        self.assertEqual(result["requirements"][0]["retrieval_mode"], "owner-scoped")
        self.assertEqual(len(result["evidence"]), 1)
        self.assertEqual(result["evidence"][0]["evidence_type"], "contract")
        self.assertEqual(result["evidence"][0]["record"]["path"], owner)
        self.assertFalse(result["completion"]["coverage_complete"])

    def test_owner_keeps_typed_fact_with_a_real_qualifier_match(self):
        owner = self.owner_key_document(summary="Alpha governs this boundary.")
        result = self.invoke("brief", owner + " catalog alpha beta", "--limit", "6", "--per-doc", "2",
                             "--require-current")
        self.assertEqual([row["summary"] for row in result["contracts"]], ["Alpha governs this boundary."])
        self.assertIn(2, result["contracts"][0]["matched_group_indices"])
        self.assertEqual(result["retrieval_mode"], "owner-scoped")

    def test_camel_components_are_lexical_boundaries(self):
        self.document("version.md", "# Configuration\n\ncurrentReleaseVersion is available.\n")
        self.document("noise.md", "# Noise\n\nprerelease conversion settings.\n")
        self.invoke("build")
        for source_scan in (False, True):
            result = self.context("release version", source_scan)
            self.assertEqual(result["related_documents"], ["docs/version.md"])
            self.assertEqual(result["query_coverage"]["covered_groups"], 2)

    def test_compound_phrase_requires_ordered_whole_words(self):
        self.document("agreement.md", "# Reference\n\nreference.knowledge-contract.v1 defines the boundary.\n")
        self.document("reversed.md", "# Other\n\ncontract precedes knowledge.\n")
        self.document("partial.md", "# Other\n\nknowledge contractual wording.\n")
        self.invoke("build")
        for source_scan in (False, True):
            result = self.context("knowledge-contract", source_scan)
            self.assertEqual(result["related_documents"], ["docs/agreement.md"])

    def test_source_context_and_identifier_identity_are_distinct(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".ts"],
                       sourceFactExtractors=["language"], sourceFactMaxFiles=32, sourceFactMaxFileBytes=131072)
        profile_path.write_text(json.dumps(profile))
        (self.root / "src").mkdir()
        (self.root / "src/controller.ts").write_text(
            "export function PrefixReleaseVersionController(input: string) {\n  return input;\n}\n")
        self.document("manual.md", "# Manual\n\nrelease version is explicit.\n")
        self.invoke("build")
        for source_scan in (False, True):
            result = self.context("release version", source_scan)
            self.assertIn("src/controller.ts", [row["path"] for row in result["facts"]])
            unrelated = self.context("lease vers", source_scan)
            self.assertNotIn("src/controller.ts", [row["path"] for row in unrelated["facts"]])
            full = self.context("PrefixReleaseVersionController", source_scan)
            self.assertIn("src/controller.ts", [row["path"] for row in full["facts"]], full)
        full = self.invoke("brief", "prefix release version controller", "--require-current")
        self.assertEqual(full["ranked_paths"][0]["path"], "src/controller.ts")
        self.assertEqual(full["retrieval_mode"], "identifier-scoped")
        topical = self.invoke("brief", "release version", "--require-current")
        self.assertNotEqual(topical["retrieval_mode"], "identifier-scoped")

    def test_source_field_vocabulary_matches_index_and_scan(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".ts"],
                       sourceFactExtractors=["language"], sourceFactMaxFiles=32, sourceFactMaxFileBytes=131072)
        profile_path.write_text(json.dumps(profile))
        (self.root / "src").mkdir()
        (self.root / "src/PackageRulesTest.ts").write_text(
            "export class PackageRulesTest {\n"
            "  dryRunPreview(input: BoundedPreview) {\n"
            "    const allowed = 'bounded preview';\n"
            "    return allowed;\n"
            "  }\n"
            "}\n")
        self.invoke("build")
        for query in ("PackageRulesTest dry", "bounded preview", "dry run preview",
                      "PackageRulesTest", "preview allowed", "ackaged revious"):
            with self.subTest(query=query):
                indexed = self.context(query)
                scanned = self.context(query, True)
                project = lambda result: [(row["id"], row["score"], row["matched_group_indices"])
                                          for row in result["facts"]]
                self.assertEqual(project(indexed), project(scanned))
                paths = [row["path"] for row in indexed["facts"]]
                if query != "ackaged revious":
                    self.assertIn("src/PackageRulesTest.ts", paths)
                else:
                    self.assertNotIn("src/PackageRulesTest.ts", paths)

    def test_reference_components_do_not_receive_complete_identity_weight(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".ts"],
                       sourceFactExtractors=["language"])
        profile_path.write_text(json.dumps(profile))
        (self.root / "src").mkdir()
        (self.root / "src/caller.ts").write_text(
            "export function apply(value: unknown) {\n"
            "  return PrefixReleaseVersionController(value);\n}\n")
        self.invoke("build")
        queries = [("release", 16), ("release-version", 16),
                   ("PrefixReleaseVersionController", 32),
                   ("prefixreleaseversioncontroller", 32),
                   ("prefix-release-version-controller", 32)]
        for query, expected_weight in queries:
            for source_scan in (False, True):
                with self.subTest(query=query, source_scan=source_scan):
                    result = self.context(query, source_scan)
                    rows = [row for row in result["facts"] if row["title"] == "ts function apply"]
                    self.assertEqual(len(rows), 1)
                    self.assertEqual(rows[0]["score_components"]["identifier_references"], expected_weight)
                    self.assertEqual(rows[0]["matched_group_indices"], [0])
        # A later alias may be a complete identity after an earlier component match.
        profile["aliases"] = [{"term": "release", "expands": ["PrefixReleaseVersionController"]}]
        profile_path.write_text(json.dumps(profile))
        self.invoke("build")
        for source_scan in (False, True):
            rows = [row for row in self.context("release", source_scan)["facts"]
                    if row["title"] == "ts function apply"]
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["score_components"]["identifier_references"], 32)

    def test_prose_references_do_not_outvote_matching_declaration(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".ts"],
                       sourceFactExtractors=["language"])
        profile_path.write_text(json.dumps(profile))
        (self.root / "src").mkdir()
        (self.root / "src/focused.ts").write_text(
            "export function refreshStableCacheSnapshot() {\n"
            "  return consume(operationName, requestPath, currentValue, documentRoot);\n}\n")
        broad = self.root / "src/broad.ts"
        query = "stable cache snapshot operation request current document"
        for arguments in ("stable, cache, snapshot, operation, request, current",
                          "stable, cache, snapshot, operation, request, current, document"):
            with self.subTest(arguments=arguments):
                broad.write_text(f"export function apply() {{\n  return consume({arguments});\n}}\n")
                self.invoke("build")
                result = self.invoke("brief", query, "--limit", "1", "--require-current")
                self.assertEqual(result["ranked_paths"][0]["path"], "src/focused.ts")
                self.assertEqual(result["facts"][0]["matched_group_indices"], list(range(7)))
                indexed, scanned = self.context(query), self.context(query, True)
                projection = lambda value: [(row["path"], row["score"], row["matched_group_indices"])
                                            for row in value["facts"]]
                self.assertEqual(projection(indexed), projection(scanned))

    def test_prose_reference_scores_preserve_targeted_identifier_lookups(self):
        profile_path = self.profiles / "lexical.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".ts"],
                       sourceFactExtractors=["language"])
        profile_path.write_text(json.dumps(profile))
        (self.root / "src").mkdir()
        (self.root / "src/caller.ts").write_text(
            "export function apply() {\n"
            "  return PrefixReleaseVersionController(marker);\n}\n")
        self.invoke("build")
        for query, weight in (("marker", 32), ("apply marker", 16),
                              ("apply PrefixReleaseVersionController", 32),
                              ("apply prefixreleaseversioncontroller", 32),
                              ("apply release-version", 16)):
            for source_scan in (False, True):
                with self.subTest(query=query, source_scan=source_scan):
                    rows = [row for row in self.context(query, source_scan)["facts"]
                            if row["title"] == "ts function apply"]
                    self.assertEqual(len(rows), 1)
                    self.assertEqual(rows[0]["score_components"]["identifier_references"], weight)
        profile["aliases"] = [{"term": "policy", "expands": ["PrefixReleaseVersionController"]}]
        profile_path.write_text(json.dumps(profile))
        self.invoke("build")
        for source_scan in (False, True):
            rows = [row for row in self.context("apply policy", source_scan)["facts"]
                    if row["title"] == "ts function apply"]
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["score_components"]["identifier_references"], 32)
            self.assertEqual(self.context("apply mark", source_scan)["facts"], [])

    def test_numeric_and_compound_components_are_lexical_not_substrings(self):
        self.document("rules.md", "# Rules 0017\n\nHTTP 401 403 and CustomerSideBar navigation.\n")
        self.document("other.md", "# Rules 0018\n\nHTTP 1401 and stable navigation.\n")
        self.invoke("build")
        for query in ("0017", "401", "sidebar navigation", "SideBar"):
            with self.subTest(query=query):
                indexed = self.context(query)
                scanned = self.context(query, True)
                self.assertEqual(indexed["related_documents"], ["docs/rules.md"])
                self.assertEqual(indexed["related_documents"], scanned["related_documents"])
        self.assertEqual(self.context("table navigation")["related_documents"], [])

    def test_document_owner_corruption_fails_closed(self):
        query = "alpha beta gamma delta epsilon zeta eta theta"
        self.document("left.md", "# Left\n\nalpha beta gamma delta.\n")
        self.document("right.md", "# Right\n\nepsilon zeta eta theta.\n")
        index_path = self.out / "docs/kb/generated/document-rows.json"
        for corruption in ("gap", "path", "total", "range-length", "unknown", "syntax"):
            with self.subTest(corruption=corruption):
                self.invoke("build")
                data = json.loads(index_path.read_text())
                owner = next(row for row in data["owners"] if "sections" in row)
                if corruption == "gap":
                    owner["sections"][0] += 1
                elif corruption == "path":
                    owner["path"] = "../outside.md"
                elif corruption == "total":
                    data["totals"]["sections"] += 1
                elif corruption == "range-length":
                    owner["sections"].append(999)
                elif corruption == "unknown":
                    owner["authority"] = "trusted"
                serialized = "{" if corruption == "syntax" else json.dumps(data)
                index_path.write_text(serialized)
                result = self.invoke("brief", query, expected=1)
                self.assertEqual(result["error"]["code"], "index-corrupt")
                self.assertEqual(index_path.read_text(), serialized)


if __name__ == "__main__":
    unittest.main()
