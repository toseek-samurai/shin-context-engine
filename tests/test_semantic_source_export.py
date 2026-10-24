#!/usr/bin/env python3
"""Hermetic checks for the native, test-only source projection exporter."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from prepare_semantic_pilot import prepare
from reproject_semantic_identifiers import publication_fingerprint

CORE, EXPORTER = [str(Path(sys.argv.pop(1)).resolve()) for _ in range(2)]


class SourceExportTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-semantic-source-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root, self.out = self.base / "root", self.base / "index"
        self.root.mkdir()
        (self.root / "src").mkdir()
        (self.base / "profiles").mkdir()
        (self.root / "README.md").write_text("# Projection fixture\n\nBound source projection checks.\n")
        profile = {"id": "fixture", "sourceLabel": "Fixture", "rootFiles": ["README.md"],
                   "sourceExtensions": [".md"], "sourceFactRoots": ["src"],
                   "sourceFactExtensions": [".ts"], "sourceFactExtractors": ["language"],
                   "sourceFactMaxFileBytes": 1048576, "defaultTopic": "fixture"}
        (self.base / "profiles/fixture.json").write_text(json.dumps(profile))
        self.source = self.root / "src/cache.ts"
        self.source.write_text("export function cacheScope(brand: string) {\n"
                               "  // Separate cached responses for each brand.\n"
                               "  return ['cache', brand];\n}\n")
        self.build()

    def build(self):
        subprocess.run([CORE, "--root", str(self.root), "--out-dir", str(self.out),
                        "--profile", "fixture", "--profile-dir", str(self.base / "profiles"),
                        "build"], check=True, capture_output=True)

    def export(self, success=True, chunks=False, structural=False):
        args = [EXPORTER, str(self.root), str(self.out)] + (["--structural-chunks"] if structural else (["--chunks"] if chunks else []))
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result

    def rows(self, chunks=False):
        records = [json.loads(line) for line in self.export(chunks=chunks).stdout.splitlines()]
        self.assertEqual(records[-1]["type"], "complete")
        return [row for row in records if row["type"] == "range"]

    def long_source(self, count=200):
        self.source.write_text("export function cacheScope() {\n" +
                               "".join(f"  const value{i} = {i};\n" for i in range(count)) +
                               "  assertThat(cacheReads).hasValue(3);\n}\n")
        self.build()

    def structural(self):
        result = self.export(structural=True)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(rows[0]["projection"], "core-bound-structural-chunks-v1")
        self.assertEqual(rows[-1]["type"], "complete")
        return rows

    def check_structural_coverage(self):
        rows = self.structural()
        file = next(row for row in rows if row["type"] == "file")
        self.assertTrue(file["coverage_complete"])
        self.assertEqual(file["omitted_bytes"], 0)
        chunks = [row for row in rows if row["type"] == "chunk"]
        raw = self.source.read_bytes()
        self.assertEqual(b"".join(row["text"].encode() for row in chunks), raw)
        end = 0
        for row in chunks:
            self.assertEqual(row["byte_start"], end)
            end = row["byte_end"]
            part = raw[row["byte_start"]:end]
            self.assertEqual(row["text"].encode(), part)
            self.assertEqual(row["span_hash"], publication_fingerprint(part))
            self.assertEqual(row["source_content_hash"], publication_fingerprint(raw))
            self.assertLessEqual(len(part), 1024)
            self.assertEqual(row["start_line"], raw[:row["byte_start"]].count(b"\n") + 1)
            self.assertEqual(row["end_line"], raw[:end - 1].count(b"\n") + 1)
        self.assertEqual(end, len(raw))
        return file, chunks

    def test_structural_round_trip_and_late_body(self):
        self.long_source(500)
        file, chunks = self.check_structural_coverage()
        self.assertTrue(file["syntax_complete"])
        self.assertTrue(any("assertThat(cacheReads)" in row["text"] for row in chunks))
        self.assertEqual(self.export(structural=True).stdout, self.export(structural=True).stdout)

    def test_structural_does_not_split_fitting_functions(self):
        functions = [f"export function item{i}() {{ return " + str(i) + "; }" for i in range(80)]
        self.source.write_text("\n".join(functions) + "\n")
        self.build()
        _, chunks = self.check_structural_coverage()
        for function in functions:
            self.assertTrue(any(function in row["text"] for row in chunks), function)

    def test_structural_long_utf8_leaf_is_complete_and_marked(self):
        self.source.write_text('export function cacheScope() { return "' + "\u0436\U0001f310" * 1800 + '"; }\n')
        self.build()
        _, chunks = self.check_structural_coverage()
        self.assertTrue(any(row["range_origin"] == "bounded-text-fallback" for row in chunks))

    def test_structural_secret_scan_precedes_partition(self):
        secret = "sk-" + "Q7x9" * 16
        self.source.write_text("// unrelated preamble\n" * 80 +
                               '// ' + secret + '\nexport function cacheScope() { return 42; }\n')
        self.build()
        result = self.export(structural=True)
        self.assertNotIn(secret, result.stdout + result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        chunks = [row for row in rows if row["type"] == "chunk"]
        self.assertTrue(chunks)
        self.assertTrue(all(row["redacted"] and row["text"] == "[REDACTED:source-file]" for row in chunks))

    def test_structural_refuses_stale_and_symlink_source(self):
        self.source.write_text(self.source.read_text().replace("brand", "other"))
        self.export(False, structural=True)
        self.build()
        other = self.base / "unbound.ts"
        self.source.rename(other)
        self.source.symlink_to(other)
        self.export(False, structural=True)

    def test_structural_includes_enrolled_files_without_declarations(self):
        (self.root / "src/notes.ts").write_text("// no declarations\n")
        (self.root / "src/empty.ts").write_text("")
        self.build()
        rows = self.structural()
        files = {row["path"]: row for row in rows if row["type"] == "file"}
        self.assertEqual(set(files), {"src/cache.ts", "src/notes.ts", "src/empty.ts"})
        self.assertEqual(rows[0]["selected_source_files"], len(files))
        self.assertTrue(files["src/empty.ts"]["coverage_complete"])
        self.assertEqual(files["src/empty.ts"]["chunks"], 0)

    def test_chunks_reach_late_body_with_parent_and_physical_hashes(self):
        self.long_source()
        rows = self.rows(chunks=True)
        parent = next(row for row in rows if row["start_line"] == 1 and row["end_line"] == 203)
        self.assertNotIn("assertThat(cacheReads)", parent["text"])
        chunks = parent["chunking"]["chunks"]
        self.assertTrue(any("assertThat(cacheReads)" in child["text"] for child in chunks))
        self.assertEqual(parent["chunking"]["status"], "partitioned")
        lines = self.source.read_bytes().splitlines(keepends=True)
        for child in chunks:
            self.assertEqual(child["parent_span_hash"], parent["span_hash"])
            self.assertEqual(child["span_hash"], publication_fingerprint(
                b"".join(lines[child["start_line"] - 1:child["end_line"]])))
            self.assertLessEqual(child["end_line"] - child["start_line"] + 1, 24)
            self.assertLessEqual(len(child["text"].encode()), 1024)

    def test_nested_scopes_do_not_duplicate_child_lines_in_outer_chunks(self):
        self.source.write_text("export function outer() {\n" + "  let before = 1;\n" * 30 +
                               "  function inner() {\n" + "    let nested = 2;\n" * 30 +
                               "  }\n" + "  let after = 3;\n" * 30 + "}\n")
        self.build()
        rows = self.rows(chunks=True)
        inner = next(row for row in rows if row["symbol"] == "inner")
        outer = next(row for row in rows if row["symbol"] == "outer")
        self.assertGreater(outer["chunking"]["delegated_lines"], 0)
        self.assertTrue(inner["chunking"]["chunks"])
        self.assertTrue(all(child["end_line"] < inner["start_line"] or child["start_line"] > inner["end_line"]
                            for child in outer["chunking"]["chunks"]))

    def test_chunk_cap_and_long_line_clipping_are_explicit(self):
        self.long_source(500)
        parent = next(row for row in self.rows(chunks=True) if row["start_line"] == 1)
        self.assertEqual(len(parent["chunking"]["chunks"]), 16)
        self.assertGreater(parent["chunking"]["omitted_lines"], 0)
        self.assertEqual(parent["chunking"]["status"], "bounded-incomplete")
        self.source.write_text('export function cacheScope() { return "' + "\u0436" * 1800 + '"; }\n')
        self.build()
        parent = next(row for row in self.rows(chunks=True) if row["start_line"] == 1)
        self.assertEqual(parent["chunking"]["clipped_chunks"], 1)
        self.assertLessEqual(len(parent["chunking"]["chunks"][0]["text"].encode()), 1024)

    def test_chunking_inherits_sensitive_parent_range(self):
        secret = "sk-" + "Q7x9" * 16
        self.source.write_text("export function outer() {\n" + "  let value = 0;\n" * 30 +
                               "  function inner() { return 42; }\n" +
                               '  const credential = "' + secret + '"; // gitleaks:allow\n}\n')
        self.build()
        result = self.export(chunks=True)
        self.assertNotIn(secret, result.stdout + result.stderr)
        self.assertEqual(self.rows(chunks=True), [], "quarantine also withholds clean nested declarations")

    def test_partial_overlap_is_not_promoted_to_owned_chunks(self):
        self.long_source(100)
        lines = self.source.read_bytes().splitlines(keepends=True)
        def overlap(row):
            start, end = (1, 50) if row["record_type"] == "ts-export" else (30, 80)
            row.update(start_line=start, end_line=end, line=start, range_origin="syntax",
                       span_hash=publication_fingerprint(b"".join(lines[start - 1:end])))
        self.rewrite_facts(overlap)
        rows = self.rows(chunks=True)
        self.assertTrue(all(row["chunking"]["status"] == "ambiguous-overlap" for row in rows))
        self.assertTrue(all(row["chunking"]["chunks"] == [] for row in rows))

    def rewrite_facts(self, change):
        generated = self.out / "docs/kb/generated"
        facts = [json.loads(line) for line in (generated / "facts.jsonl").read_text().splitlines()]
        for row in facts:
            change(row)
        content = "".join(json.dumps(row) + "\n" for row in facts).encode()
        (generated / "facts.jsonl").write_bytes(content)
        manifest = json.loads((generated / "manifest.json").read_text())
        for entry in manifest["generated_file_integrity"]["files"]:
            if entry["path"] == "docs/kb/generated/facts.jsonl":
                entry.update(bytes=len(content), content_hash=publication_fingerprint(content))
        (generated / "manifest.json").write_text(json.dumps(manifest))

    def test_body_and_inline_comments_keep_exact_record_bindings(self):
        rows = self.rows()
        row = next(row for row in rows if row["start_line"] == 1 and row["end_line"] == 4)
        self.assertIn("Separate cached responses", row["text"])
        self.assertIn("return ['cache', brand]", row["text"])
        self.assertFalse(row["truncated"])
        self.assertEqual(row["span_hash"], publication_fingerprint(self.source.read_bytes()))
        self.assertTrue(row["record_id"])

    def test_stale_source_and_symlink_fail_closed(self):
        self.source.write_text(self.source.read_text().replace("brand", "other"))
        self.export(False)
        self.build()
        outside = self.base / "outside.ts"
        self.source.rename(outside)
        self.source.symlink_to(outside)
        self.export(False)

    def test_out_of_range_and_wrong_span_fail_even_with_valid_publication_hash(self):
        self.rewrite_facts(lambda row: row.update(end_line=99999))
        self.export(False)
        self.build()
        self.rewrite_facts(lambda row: row.update(span_hash="fnv1a64:0000000000000000"))
        self.export(False)

    def test_secrets_are_redacted_before_clipping_and_not_printed(self):
        secret = "sk-" + "Q7x9" * 16
        self.source.write_text('export function cacheScope() { return "' + "x" * 2030 +
                               " " + secret + '"; }\n')
        self.build()
        result = self.export()
        self.assertNotIn(secret[:8], result.stdout + result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines() if '"type":"range"' in line]
        self.assertEqual(rows, [], "quarantine withholds every declaration before export/clipping")

    def test_utf8_clipping_is_explicit_and_keeps_valid_json(self):
        self.source.write_text('export function cacheScope() { return "' + "\u0436" * 1800 + '"; }\n')
        self.build()
        rows = self.rows()
        self.assertTrue(all(row["truncated"] for row in rows))
        self.assertTrue(all(len(row["text"].encode()) <= 2048 for row in rows))

    def test_tampered_artifact_is_rejected(self):
        with (self.out / "docs/kb/generated/facts.jsonl").open("a") as stream:
            stream.write("{}\n")
        self.export(False)

    def test_source_suppression_cannot_authorize_secret_export(self):
        secret = "sk-" + "Q7x9" * 16
        self.source.write_text('export function cacheScope() { return "' + secret +
                               '"; } // gitleaks:allow\n')
        self.build()
        result = self.export()
        self.assertNotIn(secret[:8], result.stdout + result.stderr)
        self.assertTrue(all(row["redacted"] for row in self.rows()))

    def test_sensitive_assignment_after_other_statements_is_redacted(self):
        # Assemble the synthetic value so this test file remains safe to index.
        secret = "R7b9F2w8K4d6" "S1m5P3c0A9n2"
        self.source.write_text("export function cacheScope() {\n"
                               "  const ordinary = 1;\n"
                               '  const api_key = "' + secret + '"; // gitleaks:allow\n'
                               "  return ordinary;\n}\n")
        self.build()
        result = self.export()
        self.assertNotIn(secret, result.stdout + result.stderr)

    def test_export_is_read_only_and_oversized_source_fails(self):
        generated = self.out / "docs/kb/generated"
        before = {path.name: path.read_bytes() for path in generated.iterdir() if path.is_file()}
        source = self.source.read_bytes()
        self.export()
        self.assertEqual(before, {path.name: path.read_bytes() for path in generated.iterdir() if path.is_file()})
        self.assertEqual(source, self.source.read_bytes())
        self.source.write_bytes(source + b" " * 1048576)
        self.export(False)

    def test_common_freeze_survives_later_source_drift_but_rejects_export_tampering(self):
        spec = {"schema_version": "shin.project-search-comparison.v1", "limit": 5, "per_document": 2,
                "projects": [{"id": "fixture", "root": str(self.root),
                    "profile": str(self.base / "profiles/fixture.json"), "cases": [
                        {"id": "exact", "kind": "exact", "query": "cacheScope", "expected_paths": ["src/cache.ts"]}]}]}
        spec_path = self.base / "spec.json"
        spec_path.write_text(json.dumps(spec))
        control = self.base / "control"
        prepare(spec_path, Path(__file__).parent / "fixtures/local-semantic-pilot-policy.json",
                control, Path(CORE), Path(EXPORTER))
        self.source.write_text("export function changedLater() {}\n")
        script = Path(__file__).parent / "reproject_semantic_bodies.py"
        args = [sys.executable, str(script), "--input", str(control), "--output", str(self.base / "body")]
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        body = (self.base / "body/units.jsonl").read_text()
        self.assertIn("Separate cached responses", body)
        self.assertNotIn("changedLater", body)
        with (control / "fixture/source-ranges.jsonl").open("a") as stream:
            stream.write("{}\n")
        args[-1] = str(self.base / "tampered-body")
        self.assertNotEqual(subprocess.run(args, capture_output=True).returncode, 0)

    def test_jsonl_reprojection_keeps_unicode_line_separators_inside_strings(self):
        value = "alpha\u2028beta\u2029gamma\u0085delta"
        self.source.write_text('export function cacheScope() { return "' + value + '"; }\n')
        spec_path = self.base / "spec.json"
        spec_path.write_text(json.dumps({"schema_version": "shin.project-search-comparison.v1",
            "limit": 5, "per_document": 2, "projects": [{"id": "fixture", "root": str(self.root),
            "profile": str(self.base / "profiles/fixture.json"), "cases": [
                {"id": "exact", "kind": "exact", "query": "cacheScope", "expected_paths": ["src/cache.ts"]}]}]}))
        control, body = self.base / "unicode-control", self.base / "unicode-body"
        prepare(spec_path, Path(__file__).parent / "fixtures/local-semantic-pilot-policy.json",
                control, Path(CORE), Path(EXPORTER))
        raw = (control / "fixture/source-ranges.jsonl").read_text(encoding="utf-8")
        self.assertIn(value, raw)
        result = subprocess.run([sys.executable, str(Path(__file__).parent / "reproject_semantic_bodies.py"),
            "--input", str(control), "--output", str(body)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        with (body / "units.jsonl").open("rb") as stream:
            units = [json.loads(line) for line in stream]
        self.assertTrue(any("Source range:" in unit["text"] and value in unit["text"] for unit in units))

    def test_chunk_freeze_and_reprojection_preserve_omissions_and_enforce_corpus_budget(self):
        self.long_source()
        spec = {"schema_version": "shin.project-search-comparison.v1", "limit": 5, "per_document": 2,
                "projects": [{"id": "fixture", "root": str(self.root),
                    "profile": str(self.base / "profiles/fixture.json"), "cases": [
                        {"id": "exact", "kind": "exact", "query": "cacheScope", "expected_paths": ["src/cache.ts"]}]}]}
        spec_path = self.base / "spec.json"
        spec_path.write_text(json.dumps(spec))
        control = self.base / "chunk-control"
        prepare(spec_path, Path(__file__).parent / "fixtures/local-semantic-pilot-policy.json",
                control, Path(CORE), Path(EXPORTER), source_chunks=True)
        self.source.write_text("export function changedLater() {}\n")
        script = Path(__file__).parent / "reproject_semantic_bodies.py"
        args = [sys.executable, str(script), "--input", str(control), "--output", str(self.base / "chunks"), "--chunks"]
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        units = [json.loads(line) for line in (self.base / "chunks/units.jsonl").read_text().splitlines()]
        children = [unit for unit in units if unit.get("unit_kind") == "semantic-subrange"]
        self.assertTrue(any("assertThat(cacheReads)" in child["text"] for child in children))
        self.assertTrue(all(child["parent_ranges"] for child in children))
        from run_local_semantic_pilot import fingerprint
        policy_path = control / "policy.json"
        policy = json.loads(policy_path.read_text())
        manifest = json.loads((control / "input-manifest.json").read_text())
        policy["max_units"] = manifest["units"]
        policy_path.write_text(json.dumps(policy))
        manifest["policy_sha256"] = fingerprint(policy_path)
        (control / "input-manifest.json").write_text(json.dumps(manifest))
        args[-2] = str(self.base / "over-budget")
        self.assertNotEqual(subprocess.run(args, capture_output=True).returncode, 0)
        self.assertTrue((self.base / "over-budget/rejected.json").exists())
        self.assertFalse((self.base / "over-budget/input-manifest.json").exists())


if __name__ == "__main__":
    unittest.main()
