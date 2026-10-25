#!/usr/bin/env python3
"""Retool topology and existing JS language extraction compose on a synthetic project."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

if len(sys.argv) < 3:
    raise RuntimeError("usage: test_retool_language_composition.py <shin-context> <profile-dir>")
CORE = str(Path(sys.argv.pop(1)).resolve())
PROFILES = Path(sys.argv.pop(1)).resolve()


class RetoolLanguageCompositionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-extractor-composition-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root = self.base / "root"
        self.lib = self.root / "apps/demo/App/lib"
        self.lib.mkdir(parents=True)
        (self.root / "README.md").write_text("# Synthetic extractor composition fixture\n")
        self.source = self.lib / "inspectRecords.js"
        self.source.write_text(
            "const MAX_ENTRY_BYTES = 1500000;\n"
            "const sample = 'STRING_ONLY_SYNTHETIC_TOKEN';\n"
            "// COMMENT_ONLY_SYNTHETIC_TOKEN is not a code identifier.\n"
            "function isWithinLimit(record) {\n"
            "  return record.size <= MAX_ENTRY_BYTES;\n"
            "}\n"
            "await LoadQuery.trigger();\n"
            "return RecordTable.data.filter(isWithinLimit);\n")
        (self.lib.parent / "main.rsx").write_text(
            '<App><JavascriptQuery id="InspectRecords" query={include("./lib/inspectRecords.js", "string")} />'
            '<State id="RecordTable" value={[]} /><JavascriptQuery id="LoadQuery" /></App>\n')
        (self.lib / "Records.sql").write_text('select 1 where id = {{ RecordTable.data }};\n')
        (self.base / "profiles").mkdir()
        self.generic_profile = json.loads((PROFILES / "generic-retool.json").read_text())

    def build(self, extractors, name):
        profile = {**self.generic_profile, "id": name, "sourceLabel": "Synthetic extractor fixture",
                   "rootFiles": ["README.md"], "optionalRootFiles": [], "sourceFactRoots": ["apps"],
                   "sourceFactExtensions": [".rsx", ".js", ".sql"],
                   "sourceFactExtractors": extractors, "defaultTopic": "fixture"}
        (self.base / "profiles" / (name + ".json")).write_text(json.dumps(profile))
        out = self.base / name
        prefix = [CORE, "--profile", name, "--profile-dir", str(self.base / "profiles"),
                  "--root", str(self.root), "--out-dir", str(out)]
        result = subprocess.run([*prefix, "build"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        generated = out / "docs/kb/generated"
        facts = [json.loads(line) for line in (generated / "facts.jsonl").read_text().splitlines()]
        links = [json.loads(line) for line in (generated / "links.jsonl").read_text().splitlines()]
        return prefix, facts, links

    def brief(self, prefix, query):
        result = subprocess.run([*prefix, "brief", query, "--projection", "compact", "--require-current"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def test_retool_only_reproduces_local_identifier_gap_composition_fixes_it(self):
        original, old, _ = self.build(["retool"], "only")
        combined, new, _ = self.build(["retool", "language"], "combined")
        self.assertFalse(any("MAX_ENTRY_BYTES" in f.get("identifier_terms", []) for f in old))
        self.assertEqual(self.brief(original, "MAX_ENTRY_BYTES")["evidence"], [])
        self.assertTrue(any("MAX_ENTRY_BYTES" in f.get("identifier_terms", []) for f in new))
        for query in ("MAX_ENTRY_BYTES", "isWithinLimit"):
            result = self.brief(combined, query)
            self.assertEqual(result["retrieval_mode"], "identifier-scoped")
            self.assertEqual(result["evidence"][0]["path"], "apps/demo/App/lib/inspectRecords.js")
            self.assertTrue(result["index_freshness"]["current"])

    def test_retool_records_and_relations_are_preserved(self):
        _, original, links = self.build(["retool"], "only")
        _, combined, more_links = self.build(["retool", "language"], "combined")
        retool = lambda rows: [row for row in rows if row["record_type"].startswith("retool-")]
        self.assertEqual(retool(original), retool(combined))
        self.assertTrue(all(link in more_links for link in links))
        self.assertTrue(any(row["record_type"] == "retool-call" for row in combined))
        self.assertTrue(any(row["record_type"] == "retool-payload-owner" for row in combined))
        self.assertFalse(any(row["source"] == "source-fact:ts" and row["path"].endswith((".rsx", ".sql")) for row in combined))

    def test_order_does_not_change_facts_or_links(self):
        _, a, links_a = self.build(["retool", "language"], "left")
        _, b, links_b = self.build(["language", "retool"], "right")
        self.assertEqual(a, b)
        self.assertEqual(links_a, links_b)

    def test_snippets_keep_physical_ranges_and_do_not_confirm_strings_or_comments(self):
        prefix, facts, _ = self.build(["retool", "language"], "combined")
        path = "apps/demo/App/lib/inspectRecords.js"
        source_facts = [row for row in facts if row["path"] == path]
        self.assertTrue(source_facts)
        for row in source_facts:
            self.assertTrue(1 <= row["start_line"] <= row["end_line"] <= 8)
            self.assertTrue(row["span_hash"].startswith("fnv1a64:"))
            self.assertEqual(row["verification_level"], "source-observed")
        for decoy in ("STRING_ONLY_SYNTHETIC_TOKEN", "COMMENT_ONLY_SYNTHETIC_TOKEN", "AbsentSyntheticDeclaration"):
            self.assertFalse(any(decoy in row.get("identifier_terms", []) for row in facts))
            self.assertNotEqual(self.brief(prefix, decoy)["retrieval_mode"], "identifier-scoped")

    def test_incomplete_snippet_retains_identifiers_and_explicit_incompleteness(self):
        self.source.write_text("const MAX_ENTRY_BYTES = 1500000;\nreturn RecordTable.data.filter(\n")
        prefix, facts, _ = self.build(["retool", "language"], "combined")
        rows = [row for row in facts if "MAX_ENTRY_BYTES" in row.get("identifier_terms", [])]
        self.assertTrue(rows)
        self.assertTrue(all(row["range_origin"] != "syntax" for row in rows))
        self.assertTrue(any(row["record_type"] == "ts-syntax-incomplete" for row in facts))
        self.assertEqual(self.brief(prefix, "MAX_ENTRY_BYTES")["retrieval_mode"], "identifier-scoped")
        self.assertTrue(any(row["record_type"] == "retool-payload" for row in facts))

    def test_language_composition_does_not_publish_secret_literal(self):
        canary = "sk-" + "Q7x9" * 16
        self.source.write_text('const syntheticCredential = "' + canary + '";\nreturn RecordTable.data;\n')
        _, facts, links = self.build(["retool", "language"], "combined")
        self.assertNotIn(canary, json.dumps(facts) + json.dumps(links))

    def test_public_bootstrap_profile_enables_both_existing_extractors(self):
        profile = json.loads((PROFILES / "generic-retool.json").read_text())
        self.assertEqual(profile["id"], "generic-retool")
        self.assertEqual(profile["sourceFactExtractors"], ["retool", "language"])


if __name__ == "__main__":
    unittest.main()
