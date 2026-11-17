"""Natural document layouts must not require project-specific search tuning."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = str(Path(sys.argv.pop(1)).resolve())


class ProjectFlexibilityTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-project-flexibility-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "project"
        self.out = self.base / "index"
        self.profiles = self.base / "profiles"
        (self.root / "docs").mkdir(parents=True)
        self.profiles.mkdir()
        (self.root / "README.md").write_text("# Operator handbook\n", encoding="utf-8")
        self.profile = self.profiles / "natural.json"
        self.profile.write_text(json.dumps({
            "schemaVersion": "shin.profile.v1", "id": "natural",
            "sourceLabel": "Natural documentation", "rootFiles": ["README.md"],
            "sourceExtensions": [".md"], "agentStartPaths": ["README.md"],
            "defaultTopic": "handbook", "evalCases": [], "topicRules": [],
        }), encoding="utf-8")
        self.profile_hash = hashlib.sha256(self.profile.read_bytes()).hexdigest()

    def invoke(self, *args, expected_code=0):
        process = subprocess.run([
            TOOL, "--profile", "natural", "--profile-dir", str(self.profiles),
            "--root", str(self.root), "--out-dir", str(self.out), *args,
        ], capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(process.returncode, expected_code, process.stderr or process.stdout)
        return json.loads(process.stdout)

    def write_layout(self, language, layout):
        docs = self.root / "docs"
        for path in docs.rglob("*.md"):
            path.unlink()
        if language == "en":
            first = "An operator changes an alert threshold."
            second = "Manual review is required before applying the update. Audit history records the previous and new value."
            unrelated = "The workstation guide describes display brightness, keyboard shortcuts and application startup."
            queries = ["operator changes alert threshold manual review audit history",
                       "operator alert threshold manual review audit history"]
        else:
            first = "Оператор меняет порог уведомлений."
            second = "Ручная проверка обязательна перед сохранением. Журнал изменений хранит старое и новое значение."
            unrelated = "Руководство рабочего места описывает яркость дисплея, сочетания клавиш и запуск приложения."
            queries = ["оператор меняет порог уведомлений ручная проверка журнал изменений",
                       "оператор порог уведомлений ручная проверка журнал изменений"]
        name = "handbook.md" if layout != "renamed" else "reference/operations.md"
        owner = docs / name
        owner.parent.mkdir(exist_ok=True)
        if layout == "compact":
            content = f"# Operations\n\n{first} {second}\n"
        elif layout == "sections":
            content = f"# Operations\n\n## Editing\n\n{first}\n\n## Recording\n\n{second}\n"
        elif layout == "deep-headings":
            content = f"# Operations\n\n### Editing\n\n{first}\n\n###### Recording\n\n{second}\n"
        elif layout == "formatted":
            content = f"# Operations\n\n> {first}\n\n- **{second.split('.')[0]}.**\n- {'.'.join(second.split('.')[1:]).strip()}\n"
        else:
            # Both rules remain in ordinary prose. No repeated query heading,
            # hand-written aliases or profile changes help the engine find them.
            content = f"# Operations\n\n{first}\n\n" + (unrelated + "\n\n") * 250 + second + "\n"
        owner.write_text(content, encoding="utf-8")
        (docs / "workstation.md").write_text("# Workstation\n\n" + unrelated, encoding="utf-8")
        self.invoke("build")
        queries.extend([queries[0].upper(), " ".join(reversed(queries[0].split()))])
        return owner.relative_to(self.root).as_posix(), queries

    def test_layout_language_and_output_budget(self):
        # Seven/eight groups and seven/eight records cross former planner cliffs.
        # Layouts and paths change; the project's profile remains byte-identical.
        for language in ("en", "ru"):
            for layout in ("compact", "sections", "deep-headings", "formatted", "long", "renamed"):
                owner, queries = self.write_layout(language, layout)
                for query in queries:
                    for limit in (3, 7, 8):
                        with self.subTest(language=language, layout=layout, query=query, limit=limit):
                            result = self.invoke("brief", query, "--require-current", "--limit", str(limit), "--per-doc", "1")
                            self.assertIn(owner, [p["path"] for p in result["ranked_paths"]])
                            coverage = result["query_coverage"]
                            self.assertEqual(coverage["covered_groups"], coverage["total_groups"])
                            self.assertLessEqual(result["evidence_budget"]["returned_records"], limit)
                            self.assertFalse(result["query_plan"]["optional_work_limited"])
                self.assertEqual(hashlib.sha256(self.profile.read_bytes()).hexdigest(), self.profile_hash)

    def test_independent_requests_keep_bounded_scoring_work(self):
        owner, queries = self.write_layout("en", "long")
        for _ in range(2):
            result = self.invoke("brief", queries[0], "--require-current", "--limit", "7", "--per-doc", "2")
            self.assertIn(owner, [p["path"] for p in result["ranked_paths"]])
            work = result["query_plan"].get("scoring_work")
            self.assertIsNotNone(work)
            self.assertEqual(work["scope"], "single-query")
            self.assertEqual(work["score_calls"], work["score_computations"] + work["score_reuses"])
            self.assertLessEqual(work["retained_entries"], work["entry_limit"])
            self.assertLessEqual(work["retained_payload_bytes_estimate"], work["payload_budget_bytes"])

    def test_directory_ownership_needs_no_body_keyword(self):
        profile = json.loads(self.profile.read_text())
        profile["compoundQueryFirstTermScope"] = True
        self.profile.write_text(json.dumps(profile), encoding="utf-8")
        for directory in ("AtlasPanel", "Atlas-Panel_2", "Кабинет_Оператора-2"):
            folder = self.root / "docs" / directory
            folder.mkdir()
            (folder / "policy.md").write_text(
                "# Policy\n\nManual review precedes changes; audit history records the decision.\n", encoding="utf-8")
        self.invoke("build")
        for directory in ("AtlasPanel", "Atlas-Panel_2", "Кабинет_Оператора-2"):
            with self.subTest(directory=directory):
                expected = f"docs/{directory}/policy.md"
                result = self.invoke("brief", f"docs/{directory} manual review audit history",
                                     "--require-current", "--limit", "3", "--per-doc", "1")
                self.assertTrue(result["query_diagnostics"]["directory_owner_scope_used"])
                work = result["query_plan"]["scoring_work"]
                self.assertGreater(work["uncacheable_calls"], 0)
                self.assertGreaterEqual(work["score_calls"], result["matched_sections"] + result["matched_facts"])
                self.assertEqual([p["path"] for p in result["ranked_paths"]], [expected])

    def test_typed_only_request_does_not_borrow_section_coverage(self):
        _, queries = self.write_layout("en", "sections")
        request = self.base / "task.json"
        request.write_text(json.dumps({
            "schema_version": "shin.task-request.v1", "request_id": "typed-only-layout",
            "task": {"objective": "Read declared operational evidence", "must": [{"id": "policy", "query": queries[0]}]},
            "evidence_types": ["runbook"],
            "budget": {"max_evidence_bytes": 8000, "max_records": 3,
                       "per_query_records": 3, "per_document_records": 1},
        }), encoding="utf-8")
        result = self.invoke("pack", "--input", str(request), "--require-current")
        # The summary has four groups. A disallowed section must never lend its
        # eight-group document coverage or be returned to make this look complete.
        self.assertTrue(result["evidence"])
        for record in result["evidence"]:
            self.assertEqual(record["evidence_type"], "runbook")
        requirement = result["requirements"][0]
        self.assertEqual(requirement["query_coverage"]["covered_groups"], 4)
        self.assertEqual(requirement["query_coverage"]["selected_document_covered_groups"], 0)
        self.assertEqual(requirement["selected_query_coverage"]["covered_groups"], 4)

    def test_split_rule_is_delivered_not_only_counted(self):
        for language in ("en", "ru"):
            for layout in ("sections", "long"):
                with self.subTest(language=language, layout=layout):
                    owner, queries = self.write_layout(language, layout)
                    result = self.invoke("brief", queries[0], "--require-current", "--limit", "4", "--per-doc", "2")
                    text = " ".join(str(hit.get(field, ""))
                                    for family in ("sections", "facts", "contracts", "decisions", "runbooks")
                                    for hit in result[family] if hit["path"] == owner
                                    for field in ("summary", "preview", "indexed_text")).casefold()
                    phrases = ("alert threshold", "manual review", "audit history") if language == "en" else (
                        "порог уведомлений", "ручная проверка", "журнал изменений")
                    for phrase in phrases:
                        self.assertTrue(phrase in text, f"Missing delivered rule: {phrase}")
                    compact = self.invoke("brief", queries[0], "--require-current", "--limit", "4", "--per-doc", "2",
                                          "--projection", "compact")
                    displayed = " ".join(hit["excerpt"] for hit in compact["evidence"] if hit["path"] == owner).casefold()
                    for phrase in phrases:
                        self.assertTrue(phrase in displayed, f"Missing compact rule: {phrase}")
                    self.assertEqual(compact["ranked_paths"], result["ranked_paths"])
                    for hit in compact["evidence"]:
                        if hit["type"] == "section": self.assertLessEqual(len(hit["excerpt"].encode()), 900)
                    request = self.base / "complete-task.json"
                    request.write_text(json.dumps({
                        "schema_version": "shin.task-request.v1", "request_id": "delivered-rule",
                        "task": {"objective": "Read the complete operational rule", "must": [
                            {"id": "rule", "query": queries[0], "require_full_coverage": True}]},
                        "budget": {"max_evidence_bytes": 40000, "max_records": 4,
                                   "per_query_records": 4, "per_document_records": 2},
                    }), encoding="utf-8")
                    packed = self.invoke("pack", "--input", str(request), "--require-current", "--strict")
                    self.assertTrue(packed["completion"]["complete"])
                    displayed = " ".join(item["record"].get("preview", item["record"].get("summary", ""))
                                         for item in packed["evidence"]).casefold()
                    for phrase in phrases:
                        self.assertIn(phrase, displayed)
                    coverage = packed["requirements"][0]["selected_query_coverage"]
                    self.assertEqual(coverage["covered_groups"], coverage["total_groups"])

    def test_dominated_sections_do_not_spend_the_score_budget(self):
        query = "operator changes alert threshold manual review audit history"
        profile = json.loads(self.profile.read_text())
        profile["evalCases"] = [{
            "id": "distributed-rule-cost", "query": query, "limit": 4, "perDocLimit": 2,
            "mustPaths": ["docs/handbook.md"], "maxMatchedSections": 16, "maxCandidateRows": 200,
        }]
        self.profile.write_text(json.dumps(profile), encoding="utf-8")
        for intro, closing in (
            ("An operator changes an alert threshold.",
             "Manual review is required. Audit history records both values."),
            ("The operator checks a form.",
             "Changes to an alert threshold require manual review and preserve audit history."),
        ):
            with self.subTest(intro=intro):
                owner = self.root / "docs/handbook.md"
                owner.write_text(
                    f"# Operations\n\n## Editing\n\n{intro}\n\n" +
                    "".join(f"## Routine {index}\n\nThe operator checks the workstation.\n\n" for index in range(80)) +
                    f"## Recording\n\n{closing}\n", encoding="utf-8")
                self.invoke("build")
                result = self.invoke("brief", query, "--require-current", "--limit", "4", "--per-doc", "2")
                delivered = " ".join(hit.get("indexed_text", hit.get("summary", ""))
                                     for family in ("sections", "facts", "decisions", "contracts", "runbooks")
                                     for hit in result[family]).casefold()
                for phrase in ("alert threshold", "manual review", "audit history"):
                    self.assertIn(phrase, delivered)
                self.assertEqual(result["query_coverage"]["covered_groups"], 8)
                self.assertLessEqual(result["matched_sections"], 16)
                self.assertGreater(result["query_plan"].get("section_score_prunes", 0), 0)
                self.invoke("eval", "--strict", "--require-current")

    def test_compact_pack_enforces_the_actual_wire_byte_budget(self):
        paths = []
        for index in range(16):
            owner = self.root / f"docs/runbooks/platform-components/component-{index:02}/operation-plan.md"
            owner.parent.mkdir(parents=True, exist_ok=True)
            owner.write_text(
                f"# Component {index:02} operating procedure\n\n## Admission\n\n"
                "An operator records a reviewed component change before application.\n\n"
                "## Evidence\n\nThe audit journal retains the old and new values. Журнал подтверждений.\n",
                encoding="utf-8")
            paths.append(owner.relative_to(self.root).as_posix())
        request = {
            "schema_version": "shin.task-request.v1", "request_id": "bounded-compact-envelope",
            "task": {"objective": "Read every selected component operating rule", "must": [
                {"id": f"component-{index:02}", "query": path} for index, path in enumerate(paths)]},
            "budget": {"max_evidence_bytes": 56000, "max_records": 36,
                       "per_query_records": 7, "per_document_records": 2},
        }
        profile = json.loads(self.profile.read_text())
        profile["packEvalCases"] = [{
            "id": "bounded-compact-envelope", "request": request, "mustPaths": paths,
            "maxEvidenceBytes": 56000, "maxRecords": 36, "maxCompactBytes": 46000,
        }]
        self.profile.write_text(json.dumps(profile), encoding="utf-8")
        request_path = self.base / "compact-request.json"
        request_path.write_text(json.dumps(request), encoding="utf-8")
        self.invoke("build")
        full = self.invoke("pack", "--input", str(request_path), "--require-current", "--strict")
        process = subprocess.run([
            TOOL, "--profile", "natural", "--profile-dir", str(self.profiles),
            "--root", str(self.root), "--out-dir", str(self.out), "pack", "--input", str(request_path),
            "--projection", "compact", "--require-current",
        ], capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(process.returncode, 0, process.stderr)
        compact = json.loads(process.stdout)
        self.assertTrue(compact["completion"]["complete"])
        self.assertEqual(compact["evidence"], full["evidence"])
        self.assertEqual(compact["verification_paths"], full["verification_paths"])
        self.assertEqual(compact["receipt"]["receipt_id"], full["receipt"]["receipt_id"])
        self.assertEqual({item["path"] for item in compact["verification_paths"]}, set(paths))
        wire_bytes = len(process.stdout.removesuffix("\n").encode("utf-8"))
        self.assertLessEqual(wire_bytes, 46000)
        evaluation = self.invoke("pack-eval", "--require-current", "--strict")
        self.assertEqual(evaluation["cases"][0]["limits"]["compact_bytes"], wire_bytes)
        benchmark = self.invoke("pack-bench", "--input", str(request_path), "--projection", "compact",
                                "--iterations", "1", "--warmup", "0", "--require-current")
        self.assertEqual(benchmark["output_bytes"]["min"], wire_bytes)

    def test_additional_document_slots_never_duplicate_sections(self):
        for language in ("en", "ru"):
            for layout in ("sections", "deep-headings", "long"):
                _, queries = self.write_layout(language, layout)
                for per_doc in (2, 3, 4, 6):
                    with self.subTest(language=language, layout=layout, per_doc=per_doc):
                        result = self.invoke("brief", queries[0], "--require-current", "--limit", "8",
                                             "--per-doc", str(per_doc))
                        records = [tuple(hit.get(field) for field in
                                         ("path", "start_line", "end_line", "heading", "indexed_text"))
                                   for hit in result["sections"]]
                        self.assertEqual(len(records), len(set(records)))

    def test_one_record_does_not_fabricate_a_complete_answer(self):
        _, queries = self.write_layout("en", "sections")
        request = self.base / "one-record-task.json"
        request.write_text(json.dumps({
            "schema_version": "shin.task-request.v1", "request_id": "one-record-rule",
            "task": {"objective": "Read both parts of the rule", "must": [
                {"id": "rule", "query": queries[0], "require_full_coverage": True}]},
            "budget": {"max_evidence_bytes": 12000, "max_records": 1,
                       "per_query_records": 3, "per_document_records": 1},
        }), encoding="utf-8")
        result = self.invoke("pack", "--input", str(request), "--require-current")
        self.assertFalse(result["completion"]["complete"])
        selected = result["requirements"][0]["selected_query_coverage"]
        self.assertLess(selected["covered_groups"], selected["total_groups"])

    def test_unindexed_exact_path_still_admits_ordinary_sections(self):
        directory = self.root / "docs" / ("guide-" * 22)
        directory.mkdir()
        owner = directory / "manual.md"
        path = owner.relative_to(self.root).as_posix()
        self.assertGreater(len(path.encode()), 128)
        owner.write_text("# Handbook\n\nA guide.\n\n## Operator review threshold\n\nA summary.\n\n"
                         "## Implementation details\n\nAn operator applies the review threshold after validation.\n\n"
                         "## Notices\n\nThe notice and ledger are recorded.\n\n## Checks\n\nCheck queue and priority.\n",
                         encoding="utf-8")
        (self.root / "docs/second.md").write_text("# Companion\n\nReference entry.\n", encoding="utf-8")
        profile = json.loads(self.profile.read_text())
        profile["aliases"] = [{"term": "workbook", "expands": [path]}]
        profile["evalCases"] = [
            {"id": name, "query": query, "limit": 4, "perDocLimit": 4,
             "mustPaths": [path], "mustEvidence": [{"path": path, "contains": "after validation"}]}
            for name, query in [("alias", "operator review threshold workbook notice ledger queue priority"),
                                ("literal", f"operator review threshold {path} notice ledger queue docs/second.md")]]
        self.profile.write_text(json.dumps(profile), encoding="utf-8")
        self.invoke("build")
        self.invoke("eval", "--strict", "--require-current")

    def test_eval_checks_the_displayed_excerpt_for_both_predicates(self):
        phrase = "manual escalation"
        (self.root / "docs/policy.md").write_text("# Operations\n\n" + "Ordinary weekday processing. " * 90 +
                                                 f"Amber relay routing requires {phrase}.\n", encoding="utf-8")
        case = {"query": "amber relay routing", "limit": 1, "perDocLimit": 1, "mustTopN": 1}
        profile = json.loads(self.profile.read_text())
        profile["evalCases"] = [dict(case, id="required", mustEvidence=[{"contains": phrase}]),
                                dict(case, id="forbidden", mustNotEvidence=[{"contains": phrase}])]
        self.profile.write_text(json.dumps(profile), encoding="utf-8")
        self.invoke("build")
        brief = self.invoke("brief", case["query"], "--projection", "compact", "--limit", "1", "--per-doc", "1")
        self.assertIn(phrase, brief["evidence"][0]["excerpt"])
        result = self.invoke("eval", "--strict", "--require-current", expected_code=1)
        self.assertEqual([c["passed"] for c in result["cases"]], [True, False])
        self.assertEqual(result["cases"][1]["forbidden_evidence"], [{"contains": phrase}])

    def test_pack_edges_use_the_same_shared_excerpt(self):
        (self.root / "docs/guide.md").write_text("# Procedures\n\n" + "Ordinary service notes. " * 35 +
            "Amber routing requires manual escalation. " + "Ordinary service notes. " * 90 +
            "Violet routing requires external approval.\n", encoding="utf-8")
        requirements = [("early", "amber routing"), ("late", "violet routing")]
        request = {"schema_version": "shin.task-request.v1", "request_id": "shared-display",
                   "task": {"objective": "Find both procedures", "must": [
                       {"id": k, "query": query, "require_full_coverage": True} for k, query in requirements]},
                   "evidence_types": ["section"], "budget": {"max_records": 1, "per_document_records": 1,
                       "per_query_records": 1, "max_evidence_bytes": 6000}}
        task = self.base / "shared-task.json"
        task.write_text(json.dumps(request), encoding="utf-8")
        self.invoke("build")
        for projection in ("compact", "full"):
            result = self.invoke("pack", "--input", str(task), "--projection", projection, "--jobs", "2")
            self.assertEqual(len(result["evidence"]), 1)
            text = result["evidence"][0]["record"]["preview"].casefold()
            self.assertIn("amber", text)
            self.assertNotIn("violet", text)
            self.assertFalse(result["completion"]["complete"])
            self.assertEqual([r["selected_query_coverage"]["covered_groups"] for r in result["requirements"]], [2, 1])

        # A later optional request cannot displace a visible required rule.
        (self.root / "docs/guide.md").write_text("# Procedures\n\nAmber routing requires manual escalation. " +
            "Ordinary service notes. " * 100 + "Violet routing requires external approval.\n", encoding="utf-8")
        request["task"]["should"] = [request["task"]["must"].pop()]
        task.write_text(json.dumps(request), encoding="utf-8")
        self.invoke("build")
        result = self.invoke("pack", "--input", str(task), "--strict")
        self.assertTrue(result["requirements"][0]["satisfied"])
        self.assertFalse(result["requirements"][1]["satisfied"])
        self.assertIn("Amber routing", result["evidence"][0]["record"]["preview"])

    def test_wide_cli_preserves_all_groups_when_selecting_one_section(self):
        high = [f"measure{i:03}" for i in range(68)]
        missing = [f"unused{i:03}" for i in range(63)]
        (self.root / "docs/policy.md").write_text("# Operations\n\nProcedures.\n\n## Primary\n\n" +
            " ".join(high) + "\n\n## Additional\n\n" + " ".join(["apricot", *high[:65]]) + "\n", encoding="utf-8")
        self.invoke("build")
        for order in [["apricot", *missing, *high], [high[-1], *missing, *high[:-1], "apricot"]]:
            result = self.invoke("brief", " ".join(order), "--limit", "1", "--per-doc", "1")
            self.assertEqual(result["query_coverage"]["returned_evidence_covered_groups"], 68)
            self.assertEqual(result["sections"][0]["heading"], "Primary")

    def test_long_paths_keep_the_matching_suffix_in_the_excerpt(self):
        for repeats in (14, 80):
            path = "/".join(["servicecomponent"] * repeats + ["amber", "relay", "evidence"])
            (self.root / "docs/paths.md").write_text("# Instructions\n\n" + "Ordinary operating notes. " * 30 +
                                                    "Handle " + path + " immediately.\n", encoding="utf-8")
            self.invoke("build")
            result = self.invoke("brief", "amber relay evidence", "--projection", "compact")
            self.assertTrue(any("amber/relay/evidence" in hit["excerpt"] for hit in result["evidence"]))
            self.assertTrue(all(len(hit["excerpt"].encode()) <= 900 for hit in result["evidence"]))

    def test_focused_excerpt_preserves_original_unicode_coordinates(self):
        content = "# Unicode notes\n\n" + ("Ordinary deployment notes 🛠 use established conventions.\n\n" * 450)
        content += "Straße uses Cafe\u0301 as the configured display label.\n"
        (self.root / "docs" / "unicode.md").write_text(content, encoding="utf-8")
        self.invoke("build")
        result = self.invoke("brief", "STRASSE CAFÉ", "--require-current", "--limit", "3", "--per-doc", "2",
                             "--projection", "compact")
        excerpts = [x["excerpt"] for x in result["evidence"] if x["path"] == "docs/unicode.md"]
        self.assertTrue(excerpts)
        self.assertTrue(any("Straße" in text and "Cafe\u0301" in text for text in excerpts))
        self.assertTrue(all(len(text.encode()) <= 900 for text in excerpts))


if __name__ == "__main__":
    unittest.main()
