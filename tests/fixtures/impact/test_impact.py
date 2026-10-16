"""Hermetic CLI checks for the read-only impact envelope and source ranges."""

import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest


TOOL = str(Path(sys.argv.pop(1)).resolve())
FIXTURE = Path(__file__).resolve().parent
MEMBER = "src/PriceService.ts"
PAYLOAD = "apps/admin/Impact/lib/LoadPrice.js"
SMOKE = "apps/admin/Impact/tests/smoke.test.js"
STRUCTURAL_RELATIONS = {"source-include", "source-payload", "source-reference", "source-import"}
PROVENANCE_FIELDS = ("evidence_kind", "verification_level", "status", "authority_level")


def fingerprint(content):
    value = 1469598103934665603
    for byte in content:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"fnv1a64:{value:016x}"


def tree_state(root):
    if not root.exists() and not root.is_symlink():
        return {}
    result = {}
    for path in [root, *sorted(root.rglob("*"))]:
        metadata = path.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            content = ("symlink", os.readlink(path))
        elif stat.S_ISREG(metadata.st_mode):
            content = ("file", hashlib.sha256(path.read_bytes()).hexdigest())
        else:
            content = ("directory",)
        result[str(path.relative_to(root))] = (
            metadata.st_mode, metadata.st_size, metadata.st_mtime_ns,
            metadata.st_ctime_ns, metadata.st_ino, content,
        )
    return result


class ImpactTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-impact-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "root"
        self.out = self.base / "index"
        self.profiles = self.base / "profiles"
        self.sentinel = self.base / "executed-sentinel"
        shutil.copytree(FIXTURE / "root", self.root)
        shutil.copytree(FIXTURE / "profiles", self.profiles)
        profile_paths = sorted(self.profiles.glob("*.json"))
        self.assertEqual(len(profile_paths), 1, "impact fixture must select one explicit profile")
        self.profile_path = profile_paths[0]
        self.profile_id = json.loads(self.profile_path.read_text())["id"]
        self.environment = dict(os.environ, LC_ALL="C", SHIN_IMPACT_EXECUTION_SENTINEL=str(self.sentinel))
        self.build()

    def argv(self, *args):
        return [TOOL, "--profile", self.profile_id, "--profile-dir", str(self.profiles),
                "--root", str(self.root), "--out-dir", str(self.out), *args]

    def build(self):
        process = subprocess.run(self.argv("build"), capture_output=True, text=True,
                                 env=self.environment, timeout=90)
        self.assertEqual(process.returncode, 0, process.stderr or process.stdout)
        manifest = self.out / "docs/kb/generated/manifest.json"
        self.snapshot_id = fingerprint(manifest.read_bytes())
        manifest_data = json.loads(manifest.read_text())
        self.baseline_document_paths = {row["path"] for row in manifest_data["sources"]}

        def rows(store):
            return [json.loads(line) for line in (manifest.parent / store).read_text().splitlines()]

        self.baseline_facts = rows("facts.jsonl")
        self.baseline_sections = rows("sections.jsonl")
        self.baseline_links = rows("links.jsonl")
        self.baseline_scope_rows = list(self.baseline_facts)
        for store in ("decisions.jsonl", "contracts.jsonl", "runbooks.jsonl"):
            self.baseline_scope_rows.extend(rows(store))

    def watched_state(self):
        return {str(path): tree_state(path) for path in (self.root, self.out, self.profiles)}

    def assert_no_writes(self, before):
        after = self.watched_state()
        differences = []
        for root in before:
            for path in sorted(before[root].keys() | after[root].keys()):
                if before[root].get(path) != after[root].get(path):
                    differences.append(str(Path(root) / path))
        self.assertFalse(differences, f"impact changed watched files or metadata: {differences[:8]}")
        self.assertFalse(self.sentinel.exists(), "impact executed indexed test source")

    def impact(self, *args, expected=0, error=None):
        before = self.watched_state()
        process = subprocess.run(self.argv("impact", *args), capture_output=True, text=True,
                                 env=self.environment, timeout=90)
        self.last_stdout = process.stdout
        self.assert_no_writes(before)
        self.assertEqual(process.returncode, expected, process.stderr or process.stdout)
        if error is not None:
            self.assertEqual(process.stdout, "", "guard failure published an impact envelope")
            result = json.loads(process.stderr)
            self.assertEqual(result["schema_version"], "shin.error.v1")
            self.assertFalse(result["ok"])
            self.assertEqual(result["error"]["code"], error)
            return result
        self.assertEqual(process.stderr, "")
        self.assertLessEqual(len(process.stdout.encode()), 512 * 1024 + 1)
        result = json.loads(process.stdout)
        self.assert_policy(result)
        return result

    def assert_policy(self, result):
        self.assertEqual(result["schema_version"], "shin.impact-pack.v1")
        self.assertEqual(result["command"], "impact")
        self.assertEqual(result["profile"], self.profile_id)
        self.assertEqual(result["snapshot_id"], self.snapshot_id)
        self.assertEqual(result["root"], str(self.root.resolve()))
        self.assertEqual(result["out_root"], str(self.out.resolve()))
        policy = result["policy"]
        self.assertTrue(policy["read_only"])
        self.assertEqual(policy["graph_basis"], "published-snapshot")
        for key in ("publication", "actions_executed", "runtime_reachability_proven", "complete_semantic_impact"):
            self.assertFalse(policy[key], key)
        self.assertFalse(result["changes"]["comparison"]["git_required"])
        coverage = result["coverage"]
        self.assertEqual(coverage["returned_paths"], len(result["reread"]))
        self.assertEqual(coverage["omitted_paths"], coverage["candidate_paths"] - len(result["reread"]))
        self.assertIsInstance(coverage["evidence_complete"], bool)
        self.assertEqual(coverage["inspection_complete"],
                         coverage["selection_complete"] and coverage["evidence_complete"])
        self.assertEqual(result["ok"],
                         (not result["require_current"] or result["index_current"]) and
                         (not result["strict"] or coverage["inspection_complete"]))
        encoded = json.dumps(result["reread"], separators=(",", ":"), ensure_ascii=False).encode()
        self.assertEqual(coverage["evidence_bytes"], len(encoded))
        self.assertLessEqual(coverage["evidence_bytes"], coverage["max_evidence_bytes"])
        paths = [(record["depth"], record["path"]) for record in result["reread"]]
        self.assertEqual(paths, sorted(set(paths)))
        for record in result["reread"]:
            self.assertEqual(record["scope_coverage"], "recommendations-not-complete-text-diff")
            self.assertIsInstance(record["current_reread_omitted"], bool)
            self.assertIsInstance(record["syntax_observation_complete"], bool)
            self.assertIs(type(record["relations_omitted"]), int)
            self.assertGreaterEqual(record["relations_omitted"], 0)
            self.assertLessEqual(len(record["relations"]), 8)
            self.assertLessEqual(len(record["scopes"]), 6)
            if (not record["baseline_scope_probe_complete"] or record["current_reread_omitted"] or
                    not record["syntax_observation_complete"] or record["relations_omitted"] or
                    record["scopes_omitted"]):
                self.assertFalse(coverage["evidence_complete"], f"unreported evidence omission: {record['path']}")
            for scope in record["scopes"]:
                self.assert_scope_provenance(record, scope)
                self.assertGreaterEqual(scope["start_line"], 1)
                self.assertGreaterEqual(scope["end_line"], scope["start_line"])
            for relation in record["relations"]:
                self.assertEqual(relation["basis"], "published-snapshot")
                expected_level = "source-observed" if relation["kind"] in STRUCTURAL_RELATIONS else "declared"
                self.assertEqual(relation["verification_level"], expected_level)
                self.assertTrue(any(
                    row["path"] == relation["from"] and row["resolved_path"] == relation["to"] and
                    row["kind"] == relation["kind"] and row["line"] == relation["line"]
                    for row in self.baseline_links), "relation must come from the published snapshot")
                self.assertFalse(relation["runtime_reachability_proven"])
        for review in result["tests_to_review"]:
            self.assertEqual(review["execution"], "not-run")
            self.assertTrue(review["requires_review"])
            self.assertEqual(review["reason"], "changed-or-statically-related-test-source")
        for action in result["next_actions"]:
            self.assertFalse(action["automatic_execution"])

    def assert_scope_provenance(self, record, scope):
        document = record["path"] in self.baseline_document_paths
        self.assertEqual(scope["verification_level"], "declared" if document else "source-observed")
        if scope["basis"] == "observed-worktree":
            self.assertFalse(document, "document evidence must not be promoted to source observation")
            expected_kind = "test-source" if record["test_source"] else "implementation-source"
            self.assertEqual(scope["evidence_kind"], expected_kind)
            self.assertEqual(scope["status"], "active")
            self.assertEqual(scope["authority_level"], "source-fact")
            return
        rows = self.baseline_sections if "heading" in scope else self.baseline_scope_rows
        candidates = []
        for row in rows:
            if row["path"] != record["path"]:
                continue
            start = row.get("start_line", row.get("line", 1))
            end = row.get("end_line", start)
            if (start, end) != (scope["start_line"], scope["end_line"]):
                continue
            if "heading" in scope:
                if row["heading"] != scope["heading"]:
                    continue
            elif (row.get("symbol_name") or row.get("title")) != scope["symbol"]:
                continue
            candidates.append(tuple(row[field] for field in PROVENANCE_FIELDS))
        self.assertTrue(candidates, f"scope has no exact published row: {record['path']} {scope}")
        self.assertIn(tuple(scope[field] for field in PROVENANCE_FIELDS), candidates,
                      "published provenance, status and authority must be preserved together")

    def record(self, result, path):
        matches = [record for record in result["reread"] if record["path"] == path]
        self.assertEqual(len(matches), 1, f"expected one record for {path}: {result['reread']}")
        return matches[0]

    def scope(self, record, symbol):
        matches = [scope for scope in record["scopes"] if scope.get("symbol") == symbol]
        self.assertEqual(len(matches), 1, f"expected one scope for {symbol}: {record['scopes']}")
        return matches[0]

    def change_member(self, *, moved=False, preserve_mtime=False):
        path = self.root / MEMBER
        metadata = path.stat()
        original = path.read_bytes()
        self.assertEqual(original.count(b"return value + 1;"), 1)
        content = original.replace(b"return value + 1;", b"return value + 2;")
        if moved:
            content = b"// inserted line\n\n" + content
        path.write_bytes(content)
        if preserve_mtime:
            os.utime(path, ns=(metadata.st_atime_ns, metadata.st_mtime_ns))
            self.assertEqual(path.stat().st_mtime_ns, metadata.st_mtime_ns)
            self.assertEqual(path.stat().st_size, metadata.st_size)
        return content

    def change_payload(self):
        path = self.root / PAYLOAD
        path.write_bytes(path.read_bytes().replace(b"input + 1", b"input + 2"))

    def change_profile(self, **changes):
        value = json.loads(self.profile_path.read_text())
        value.update(changes)
        self.profile_path.write_text(json.dumps(value, indent=2) + "\n")

    @staticmethod
    def padded_source(source, byte_count):
        prefix = source + b"\n/*"
        suffix = b"*/\n"
        padding = byte_count - len(prefix) - len(suffix)
        if padding < 0:
            raise ValueError("requested source size is smaller than its method")
        return prefix + b"x" * padding + suffix

    def assert_evidence_partial_with_all_paths(self, result):
        coverage = result["coverage"]
        self.assertTrue(coverage["selection_complete"], "the regression must not be a path-selection failure")
        self.assertEqual(coverage["omitted_paths"], 0)
        self.assertFalse(coverage["byte_budget_constrained"])
        self.assertFalse(coverage["evidence_complete"])
        self.assertFalse(coverage["inspection_complete"])
        self.assertIn("review-impact-omissions", [action["code"] for action in result["next_actions"]])

    def test_current_default_and_require_current(self):
        for options in ((), ("--require-current",), ("--strict",), ("--strict", "--require-current")):
            with self.subTest(options=options):
                result = self.impact(*options)
                self.assertTrue(result["ok"])
                self.assertTrue(result["index_current"])
                self.assertTrue(result["changes"]["current"])
                self.assertEqual(result["changes"]["summary"]["total_changes"], 0)
                self.assertEqual(result["reread"], [])
                self.assertEqual(result["tests_to_review"], [])
                self.assertTrue(result["coverage"]["selection_complete"])
                self.assertTrue(result["coverage"]["evidence_complete"])
                self.assertTrue(result["coverage"]["inspection_complete"])
                self.assertEqual(result["next_actions"], [])

    def test_stale_default_require_current_and_strict_are_distinct(self):
        current_bytes = self.change_member(preserve_mtime=True)
        soft = self.impact("--depth", "0")
        required = self.impact("--depth", "0", "--require-current", expected=1)
        strict = self.impact("--depth", "0", "--strict")
        self.assertTrue(soft["ok"])
        self.assertFalse(required["ok"])
        self.assertTrue(strict["ok"], "--strict concerns inspection completeness, not index freshness")
        for result in (soft, required, strict):
            self.assertFalse(result["index_current"])
            self.assertTrue(result["coverage"]["selection_complete"])
            self.assertTrue(result["coverage"]["evidence_complete"])
            self.assertTrue(result["coverage"]["inspection_complete"])
            self.assertEqual(result["changes"]["summary"]["source_facts_changed"], 1)
            self.assertEqual(self.record(result, MEMBER)["source_hash"], fingerprint(current_bytes))
            self.assertEqual(result["next_actions"][0]["code"], "review-changes-then-publish")
        self.assertEqual(soft["reread"], required["reread"])
        self.assertEqual(soft["reread"], strict["reread"])

    def test_changed_member_byte_span_excludes_unchanged_member(self):
        prior = [row for row in self.baseline_facts if row["path"] == MEMBER and
                 row["record_type"] == "ts-method" and row.get("symbol_name") == "changedMember"]
        self.assertEqual(len(prior), 1)
        current_bytes = self.change_member()
        record = self.record(self.impact("--depth", "0"), MEMBER)
        scope = self.scope(record, "changedMember")
        self.assertEqual((scope["start_line"], scope["end_line"]), (2, 4))
        self.assertEqual(scope["span_hash"], fingerprint(b"".join(current_bytes.splitlines(keepends=True)[1:4])))
        self.assertNotEqual(scope["span_hash"], prior[0]["span_hash"])
        self.assertEqual(scope["basis"], "observed-worktree")
        self.assertEqual(scope["comparison"], "span-not-seen-in-bounded-baseline")
        self.assertFalse(any(item.get("symbol") == "stableMember" for item in record["scopes"]))
        self.assertTrue(record["baseline_scope_probe_complete"])

    def test_moved_lines_report_current_changed_member_range(self):
        self.change_member(moved=True)
        record = self.record(self.impact("--depth", "0"), MEMBER)
        scope = self.scope(record, "changedMember")
        self.assertEqual((scope["start_line"], scope["end_line"]), (4, 6))
        self.assertEqual(scope["basis"], "observed-worktree")
        self.assertFalse(any(item.get("symbol") == "stableMember" for item in record["scopes"]))

    def test_line_moves_without_member_byte_changes_do_not_promote_members(self):
        path = self.root / MEMBER
        path.write_bytes(b"// inserted line\n\n" + path.read_bytes())
        record = self.record(self.impact("--depth", "0"), MEMBER)
        self.assertEqual(record["source_state"], "changed")
        self.assertFalse(any(item.get("symbol") in {"changedMember", "stableMember"} for item in record["scopes"]))

    def test_removed_source_is_historical_not_current_evidence(self):
        (self.root / MEMBER).unlink()
        result = self.impact("--depth", "0")
        record = self.record(result, MEMBER)
        self.assertEqual(record["source_state"], "removed")
        self.assertIsNone(record["source_hash"])
        self.assertIsNone(record["current_scope_candidates"])
        self.assertTrue(record["unlocalized_source_change"])
        self.assertTrue(record["scopes"])
        for scope in record["scopes"]:
            self.assertEqual(scope["basis"], "historical-snapshot")
            self.assertEqual(scope["comparison"], "removed")
        self.assertEqual(result["changes"]["summary"]["source_facts_removed"], 1)

    def test_static_documents_and_import_tests_are_review_only(self):
        self.change_member()
        result = self.impact("--depth", "1")
        document = self.record(result, "docs/architecture.md")
        self.assertEqual(document["depth"], 1)
        self.assertEqual(document["source_state"], "unchanged")
        self.assertEqual(document["reason"], "declared-dependency-neighbor")
        self.assertTrue(any(edge["from"] == "docs/architecture.md" and edge["to"] == MEMBER
                            for edge in document["relations"]))
        self.assertTrue(all(edge["verification_level"] == "declared" for edge in document["relations"]))
        self.assertTrue(document["scopes"])
        self.assertTrue(all(scope["basis"] == "current-published-source" for scope in document["scopes"]))
        test = self.record(result, "tests/price.test.ts")
        self.assertTrue(test["test_source"])
        self.assertTrue(any(edge["kind"] == "source-import" and edge["to"] == MEMBER
                            for edge in test["relations"]))
        self.assertEqual({item["path"] for item in result["tests_to_review"]}, {"tests/price.test.ts"})
        paths = {record["path"] for record in result["reread"]}
        self.assertNotIn("docs/unrelated.md", paths)
        self.assertNotIn("tests/unrelated.test.ts", paths)
        for scope in test["scopes"]:
            self.assertEqual(scope["evidence_kind"], "test-source")
            self.assertEqual(scope["comparison"], "source-bytes-current")

    def test_changed_document_sections_are_labeled_historical(self):
        path = self.root / "docs/architecture.md"
        path.write_text("# New current heading\n\n" + path.read_text())
        result = self.impact("--depth", "0")
        record = self.record(result, "docs/architecture.md")
        self.assertEqual(record["source_state"], "changed")
        self.assertTrue(record["scopes"])
        self.assertTrue(all(scope["basis"] == "historical-snapshot" for scope in record["scopes"]))

    def test_historical_document_fact_preserves_declared_provenance(self):
        relative = "docs/history/declared-facts.jsonl"
        path = self.root / relative
        path.parent.mkdir()
        shutil.copyfile(FIXTURE / "provenance/declared-facts.jsonl", path)
        extensions = json.loads(self.profile_path.read_text())["sourceExtensions"]
        self.change_profile(sourceExtensions=[*extensions, ".jsonl"])
        self.build()
        published = [row for row in self.baseline_facts if row["path"] == relative]
        self.assertEqual(len(published), 1)
        prior = published[0]
        self.assertEqual(prior["verification_level"], "declared")
        self.assertEqual(prior["status"], "superseded")
        self.assertEqual(prior["authority_level"], "history")
        current = json.loads(path.read_text())
        current.update(title="Current Price Policy", status="active", authority_level="source-of-truth")
        path.write_text(json.dumps(current) + "\n")
        record = self.record(self.impact("--depth", "0", "--strict"), relative)
        scope = self.scope(record, prior["title"])
        self.assertEqual(scope["basis"], "historical-snapshot")
        self.assertEqual(scope["comparison"], "changed")
        self.assertEqual(scope["verification_level"], "declared")
        for field in PROVENANCE_FIELDS:
            self.assertEqual(scope[field], prior[field])

    def test_historical_document_sections_preserve_declared_heading_and_authority(self):
        relative = "docs/history/notes.md"
        path = self.root / relative
        path.parent.mkdir()
        shutil.copyfile(FIXTURE / "provenance/history-notes.md", path)
        self.build()
        self.assertFalse(any(row["path"] == relative for row in self.baseline_scope_rows),
                         "fixture must exercise section fallback, not document facts")
        published = [row for row in self.baseline_sections if row["path"] == relative]
        self.assertGreater(len(published), 0)
        self.assertLessEqual(len(published), 3)
        for prior in published:
            self.assertEqual(prior["verification_level"], "declared")
            self.assertEqual(prior["status"], "superseded")
            self.assertEqual(prior["authority_level"], "history")
        path.write_text("# Replacement Notes\n\nStatus: active\n\nThe current text has changed.\n")
        record = self.record(self.impact("--depth", "0", "--strict"), relative)
        self.assertEqual(len(record["scopes"]), len(published))
        for scope, prior in zip(record["scopes"], published):
            self.assertEqual(scope["heading"], prior["heading"])
            self.assertEqual((scope["start_line"], scope["end_line"]), (prior["start_line"], prior["end_line"]))
            self.assertEqual(scope["basis"], "historical-snapshot")
            self.assertEqual(scope["verification_level"], "declared")
            for field in PROVENANCE_FIELDS:
                self.assertEqual(scope[field], prior[field])

    def test_retool_source_neighbor_is_static_test_source_not_executed(self):
        self.change_payload()
        result = self.impact("--depth", "1")
        record = self.record(result, SMOKE)
        self.assertTrue(record["test_source"])
        self.assertEqual(record["source_state"], "unchanged")
        self.assertTrue(any(edge["kind"] == "source-reference" and edge["to"] == PAYLOAD
                            for edge in record["relations"]))
        self.assertEqual({test["path"] for test in result["tests_to_review"]}, {SMOKE})
        self.assertTrue(all(scope["evidence_kind"] == "test-source" for scope in record["scopes"]))

    def test_depth_path_limit_and_strict_partial(self):
        self.change_member()
        zero = self.impact("--depth", "0")
        self.assertEqual([record["path"] for record in zero["reread"]], [MEMBER])
        two = self.impact("--depth", "2", "--strict")
        self.assertEqual(self.record(two, "README.md")["depth"], 2)
        self.assertTrue(two["coverage"]["selection_complete"])
        for strict, status in ((False, 0), (True, 1)):
            args = ["--depth", "2", "--limit", "1"] + (["--strict"] if strict else [])
            result = self.impact(*args, expected=status)
            self.assertEqual(len(result["reread"]), 1)
            self.assertEqual(result["reread"][0]["path"], MEMBER)
            self.assertGreater(result["coverage"]["omitted_paths"], 0)
            self.assertFalse(result["coverage"]["selection_complete"])
            self.assertEqual(result["ok"], not strict)
            self.assertIn("review-impact-omissions", [action["code"] for action in result["next_actions"]])

    def test_evidence_byte_limit_soft_and_strict(self):
        self.change_member()
        for strict, status in ((False, 0), (True, 1)):
            args = ["--max-evidence-bytes", "2"] + (["--strict"] if strict else [])
            result = self.impact(*args, expected=status)
            self.assertEqual(result["reread"], [])
            self.assertEqual(result["tests_to_review"], [])
            self.assertTrue(result["coverage"]["byte_budget_constrained"])
            self.assertEqual(result["coverage"]["evidence_bytes"], 2)
            self.assertFalse(result["coverage"]["selection_complete"])

    def test_one_byte_budget_is_usage_error(self):
        self.change_member()
        result = self.impact("--max-evidence-bytes", "1", expected=2, error="invalid-option-value")
        self.assertEqual(result["error"]["category"], "usage")

    def test_option_bounds_duplicates_and_source_scan_rejected(self):
        invalid = [
            (["--limit", "0"], "invalid-option-value"),
            (["--limit", "129"], "invalid-option-value"),
            (["--depth", "-1"], "invalid-option-value"),
            (["--depth", "3"], "invalid-option-value"),
            (["--max-evidence-bytes", "0"], "invalid-option-value"),
            (["--max-evidence-bytes", "131073"], "invalid-option-value"),
            (["--limit", "one"], "invalid-option-value"),
            (["--limit"], "missing-option-value"),
            (["--strict", "--strict"], "duplicate-option"),
            (["--limit", "2", "--limit", "3"], "duplicate-option"),
            (["--publish"], "unsupported-option"),
            (["--source-scan"], "unsupported-option"),
        ]
        for options, code in invalid:
            with self.subTest(options=options):
                result = self.impact(*options, expected=2, error=code)
                self.assertEqual(result["error"]["category"], "usage")
        result = self.impact("--limit", "128", "--depth", "2", "--max-evidence-bytes", "131072")
        self.assertTrue(result["ok"])

    def test_change_detail_omissions_are_explicit_and_strict(self):
        for index in range(24):
            (self.root / f"src/Added{index:02}.ts").write_text(f"export const added{index} = {index};\n")
        result = self.impact("--depth", "0", "--limit", "1", "--strict", expected=1)
        self.assertEqual(result["changes"]["detail_limit"], 10)
        self.assertEqual(result["changes"]["details_returned"], 10)
        self.assertGreater(result["changes"]["details_omitted"], 0)
        self.assertFalse(result["coverage"]["selection_complete"])
        self.assertEqual(result["coverage"]["returned_paths"], 1)

    def test_profile_drift_disables_evidence(self):
        self.change_member()
        self.change_profile(sourceLabel="Changed interpretation policy")
        for options, status in (((), 0), (("--strict",), 1), (("--require-current",), 1)):
            with self.subTest(options=options):
                result = self.impact(*options, expected=status)
                self.assertFalse(result["index_current"])
                self.assertFalse(result["coverage"]["profile_configuration_current"])
                self.assertFalse(result["coverage"]["selection_complete"])
                self.assertEqual(result["reread"], [])
                self.assertEqual(result["tests_to_review"], [])
                self.assertGreater(result["coverage"]["omitted_paths"], 0)
                self.assertTrue(any(detail.get("key") == "profile_config_hash" for detail in result["changes"]["details"]))

    def test_capped_inventory_is_not_complete_even_without_byte_drift(self):
        self.change_profile(sourceFactMaxFiles=1)
        self.build()
        result = self.impact("--strict", expected=1)
        self.assertTrue(result["changes"]["current"])
        self.assertFalse(result["index_current"])
        self.assertGreater(result["worktree"]["source_fact_capped_files"], 0)
        self.assertFalse(result["coverage"]["selection_complete"])

    def test_skipped_large_source_remains_unobserved_and_partial(self):
        (self.root / "src/Oversized.ts").write_bytes(b"//" + b"x" * (6 * 1024 * 1024))
        result = self.impact("--depth", "0", "--strict", expected=1)
        record = self.record(result, "src/Oversized.ts")
        self.assertEqual(record["source_state"], "unobserved")
        self.assertIsNone(record["source_hash"])
        self.assertIsNone(record["current_scope_candidates"])
        self.assertEqual(record["scopes"], [])
        self.assertFalse(result["coverage"]["selection_complete"])

    def test_per_path_scope_omissions_are_counted(self):
        methods = [f"  member{index}(): number {{\n    return {index};\n  }}\n" for index in range(12)]
        (self.root / MEMBER).write_text("export class PriceService {\n" + "".join(methods) + "}\n")
        for options, status in (((), 0), (("--strict",), 1)):
            result = self.impact("--depth", "0", *options, expected=status)
            record = self.record(result, MEMBER)
            self.assertEqual(len(record["scopes"]), 6)
            self.assertGreaterEqual(record["current_scope_candidates"], 12)
            self.assertEqual(record["scopes_omitted"], record["current_scope_candidates"] - 6)
            self.assertTrue(record["baseline_scope_probe_complete"])
            self.assertFalse(record["current_reread_omitted"])
            self.assertTrue(record["syntax_observation_complete"])
            self.assertEqual(record["relations_omitted"], 0)
            self.assert_evidence_partial_with_all_paths(result)

    def test_baseline_probe_over_256_facts_is_explicit_and_strict(self):
        methods = [f"  baseline{index:03}(): number {{\n    return {index};\n  }}\n" for index in range(270)]
        path = self.root / MEMBER
        path.write_text("export class PriceService {\n" + "".join(methods) + "}\n")
        self.build()
        baseline = [fact for fact in self.baseline_facts if fact["path"] == MEMBER]
        self.assertGreater(len(baseline), 256, "fixture did not exceed the baseline probe bound")
        path.write_text("export class PriceService {\n  currentMember(): number {\n    return 900;\n  }\n}\n")
        for options, status in (((), 0), (("--strict",), 1)):
            result = self.impact("--depth", "0", *options, expected=status)
            record = self.record(result, MEMBER)
            self.assertFalse(record["baseline_scope_probe_complete"])
            self.assertFalse(record["current_reread_omitted"])
            self.assertTrue(record["syntax_observation_complete"])
            self.assertEqual(record["scopes_omitted"], 0)
            self.assertEqual(record["relations_omitted"], 0)
            self.assertLessEqual(record["current_scope_candidates"], 6)
            self.assertEqual(self.scope(record, "currentMember")["basis"], "observed-worktree")
            self.assert_evidence_partial_with_all_paths(result)

    def test_removed_source_counts_omitted_historical_scopes(self):
        methods = [f"  removed{index:02}(): number {{\n    return {index};\n  }}\n" for index in range(12)]
        path = self.root / MEMBER
        path.write_text("export class PriceService {\n" + "".join(methods) + "}\n")
        self.build()
        baseline = [fact for fact in self.baseline_facts if fact["path"] == MEMBER]
        self.assertGreater(len(baseline), 6)
        self.assertLessEqual(len(baseline), 256, "isolate historical truncation from the probe bound")
        path.unlink()
        for options, status in (((), 0), (("--strict",), 1)):
            result = self.impact("--depth", "0", *options, expected=status)
            record = self.record(result, MEMBER)
            self.assertEqual(record["source_state"], "removed")
            self.assertTrue(record["baseline_scope_probe_complete"])
            self.assertFalse(record["current_reread_omitted"])
            self.assertEqual(len(record["scopes"]), 6)
            self.assertEqual(record["scopes_omitted"], len(baseline) - 6)
            self.assertTrue(all(scope["basis"] == "historical-snapshot" for scope in record["scopes"]))
            self.assert_evidence_partial_with_all_paths(result)

    def test_removed_source_preserves_incomplete_baseline_syntax(self):
        path = self.root / MEMBER
        path.write_text("export class PriceService {\n  validMember(): number {\n"
                        "    return 1;\n  }\n  brokenMember(\n}\n")
        self.build()
        baseline = [fact for fact in self.baseline_facts if fact["path"] == MEMBER]
        self.assertTrue(any(fact["record_type"].endswith("-syntax-incomplete") for fact in baseline))
        self.assertLessEqual(len(baseline), 6, "isolate incomplete syntax from historical scope truncation")
        path.unlink()
        for options, status in (((), 0), (("--strict",), 1)):
            with self.subTest(options=options):
                result = self.impact("--depth", "0", *options, expected=status)
                record = self.record(result, MEMBER)
                self.assertEqual(record["source_state"], "removed")
                self.assertTrue(record["baseline_scope_probe_complete"])
                self.assertFalse(record["current_reread_omitted"])
                self.assertFalse(record["syntax_observation_complete"])
                self.assertTrue(any(scope["record_type"].endswith("-syntax-incomplete") for scope in record["scopes"]))
                self.assert_evidence_partial_with_all_paths(result)

    def test_current_eligible_file_over_two_mib_reports_reread_omission(self):
        self.change_profile(sourceFactMaxFileBytes=4 * 1024 * 1024)
        self.build()
        path = self.root / MEMBER
        content = self.padded_source(path.read_bytes().replace(b"value + 1", b"value + 2"),
                                     2 * 1024 * 1024 + 1)
        path.write_bytes(content)
        self.assertLess(path.stat().st_size, 4 * 1024 * 1024)
        current_hash = fingerprint(content)
        for options, status in (((), 0), (("--strict",), 1)):
            result = self.impact("--depth", "0", *options, expected=status)
            record = self.record(result, MEMBER)
            self.assertEqual(record["source_state"], "changed")
            self.assertEqual(record["source_hash"], current_hash)
            self.assertTrue(record["current_reread_omitted"])
            self.assertTrue(record["baseline_scope_probe_complete"])
            self.assertIsNone(record["current_scope_candidates"])
            self.assertTrue(record["scopes"])
            self.assertTrue(all(scope["basis"] == "historical-snapshot" for scope in record["scopes"]))
            detail = next(item for item in result["changes"]["details"] if item.get("path") == MEMBER)
            self.assertEqual(detail["worktree"]["status"], "scanned", "source must remain profile-eligible")
            self.assert_evidence_partial_with_all_paths(result)

    def test_more_than_eight_causal_edges_report_exact_omissions(self):
        self.change_profile(sourceFactRoots=["src"])
        document = "docs/causal-links.md"
        (self.root / document).write_text("# Declared Links\n\n" + "".join(
            f"[Declared edge {index}](../src/PriceService.ts)\n" for index in range(9)))
        self.build()
        links = [json.loads(line) for line in
                 (self.out / "docs/kb/generated/links.jsonl").read_text().splitlines()]
        baseline_edges = [edge for edge in links if edge["path"] == document and edge["resolved_path"] == MEMBER]
        self.assertEqual(len(baseline_edges), 9)
        self.change_member()
        for options, status in (((), 0), (("--strict",), 1)):
            result = self.impact("--depth", "1", *options, expected=status)
            record = self.record(result, document)
            self.assertEqual(len(record["relations"]), 8)
            self.assertEqual(record["relations_omitted"], 1)
            self.assertEqual(len(record["relations"]) + record["relations_omitted"], len(baseline_edges))
            self.assertEqual(len({edge["line"] for edge in record["relations"]}), 8)
            self.assertTrue(all(edge["from"] == document and edge["to"] == MEMBER for edge in record["relations"]))
            for item in result["reread"]:
                self.assertTrue(item["baseline_scope_probe_complete"])
                self.assertFalse(item["current_reread_omitted"])
                self.assertTrue(item["syntax_observation_complete"])
                self.assertIn(item["scopes_omitted"], (0, None))
            self.assert_evidence_partial_with_all_paths(result)

    def test_incomplete_syntax_fails_strict_with_complete_path_selection(self):
        (self.root / MEMBER).write_text(
            "export class PriceService {\n  changedMember(value: number): number {\n"
            "    return value + 2;\n  }\n  brokenMember(\n}\n")
        for options, status in (((), 0), (("--strict",), 1)):
            result = self.impact("--depth", "0", *options, expected=status)
            record = self.record(result, MEMBER)
            self.assertFalse(record["syntax_observation_complete"])
            self.assertTrue(record["baseline_scope_probe_complete"])
            self.assertFalse(record["current_reread_omitted"])
            self.assertEqual(record["scopes_omitted"], 0)
            self.assertEqual(record["relations_omitted"], 0)
            self.assertTrue(any(scope["record_type"].endswith("-syntax-incomplete") for scope in record["scopes"]))
            self.assert_evidence_partial_with_all_paths(result)

    def test_total_sixteen_mib_reread_cap_reports_ninth_file(self):
        self.change_profile(sourceFactRoots=["src"], sourceFactExtensions=[".cpp"],
                            sourceFactMaxFileBytes=4 * 1024 * 1024)
        directory = self.root / "src/read-budget"
        directory.mkdir()
        paths = [f"src/read-budget/Part{index:02}.cpp" for index in range(9)]
        for index, relative in enumerate(paths):
            (self.root / relative).write_text(f"int budget{index:02}() {{\n  return 1;\n}}\n")
        self.build()
        byte_count = 2 * 1024 * 1024
        for relative in paths:
            path = self.root / relative
            path.write_bytes(self.padded_source(path.read_bytes().replace(b"return 1", b"return 2"), byte_count))
            self.assertEqual(path.stat().st_size, byte_count)
        for options, status in (((), 0), (("--strict",), 1)):
            result = self.impact("--depth", "0", "--max-evidence-bytes", "131072", *options, expected=status)
            self.assertEqual([record["path"] for record in result["reread"]], paths)
            omitted = [record["path"] for record in result["reread"] if record["current_reread_omitted"]]
            self.assertEqual(omitted, [paths[-1]])
            for record in result["reread"][:-1]:
                self.assertTrue(record["baseline_scope_probe_complete"])
                # The 2MiB reread fits Impact, but exceeds the C++ parser's 1MiB cap.
                self.assertFalse(record["syntax_observation_complete"])
                self.assertTrue(any(scope["record_type"] == "cpp-syntax-incomplete"
                                    for scope in record["scopes"]))
                self.assertEqual(record["scopes_omitted"], 0)
                self.assertEqual(record["relations_omitted"], 0)
                self.assertGreater(record["current_scope_candidates"], 0)
                self.assertTrue(all(scope["basis"] == "observed-worktree" for scope in record["scopes"]))
            last = self.record(result, paths[-1])
            self.assertIsNone(last["current_scope_candidates"])
            self.assertEqual(last["source_state"], "changed")
            self.assertTrue(last["scopes"])
            self.assertTrue(all(scope["basis"] == "historical-snapshot" for scope in last["scopes"]))
            self.assert_evidence_partial_with_all_paths(result)

    def test_current_and_stale_outputs_are_deterministic_without_writes(self):
        self.assertFalse((self.root / ".git").exists())
        for changed in (False, True):
            if changed:
                self.change_member()
            first = self.impact()
            first_stdout = self.last_stdout
            second = self.impact()
            self.assertEqual(first_stdout, self.last_stdout)
            self.assertEqual(first, second)
            self.assertEqual(first["index_current"], not changed)

    def test_source_file_symlink_is_not_followed(self):
        outside = self.base / "outside.ts"
        outside.write_text("export function OUTSIDE_SOURCE_MUST_NOT_APPEAR() { return 99; }\n")
        before = tree_state(outside)
        (self.root / MEMBER).unlink()
        (self.root / MEMBER).symlink_to(outside)
        record = self.record(self.impact("--depth", "0"), MEMBER)
        self.assertEqual(record["source_state"], "removed")
        self.assertIsNone(record["source_hash"])
        self.assertTrue(all(scope["basis"] == "historical-snapshot" for scope in record["scopes"]))
        self.assertNotIn("OUTSIDE_SOURCE_MUST_NOT_APPEAR", json.dumps(record))
        self.assertEqual(tree_state(outside), before)

    def test_source_directory_symlink_is_not_followed(self):
        outside = self.base / "outside-src"
        shutil.move(str(self.root / "src"), outside)
        (outside / "PriceService.ts").write_text("export function OUTSIDE_DIRECTORY_MUST_NOT_APPEAR() {}\n")
        (self.root / "src").symlink_to(outside, target_is_directory=True)
        before = tree_state(outside)
        result = self.impact("--depth", "0")
        self.assertEqual(self.record(result, MEMBER)["source_state"], "removed")
        self.assertNotIn("OUTSIDE_DIRECTORY_MUST_NOT_APPEAR", json.dumps(result))
        self.assertEqual(tree_state(outside), before)

    def test_generated_symlink_is_rejected_without_repair(self):
        links = self.out / "docs/kb/generated/links.jsonl"
        outside = self.base / "outside-links.jsonl"
        links.rename(outside)
        links.symlink_to(outside)
        self.impact(expected=1, error="snapshot-file-invalid")
        self.assertTrue(links.is_symlink())

    def test_corrupt_snapshot_is_rejected_without_repair(self):
        links = self.out / "docs/kb/generated/links.jsonl"
        links.write_bytes(links.read_bytes() + b"\n")
        self.impact(expected=1, error="snapshot-integrity-failed")

    def test_missing_snapshot_does_not_create_output(self):
        self.out = self.base / "missing-index"
        self.impact(expected=1, error="index-unavailable")
        self.assertFalse(self.out.exists())

    def test_missing_lock_is_rejected_without_recreating_it(self):
        lock_path = self.out / "docs/kb/.shin-context.lock"
        self.assertTrue(lock_path.is_file(), "build must establish the publication lock")
        lock_path.unlink()
        for options in ((), ("--require-current",), ("--strict",), ("--strict", "--require-current")):
            with self.subTest(options=options):
                result = self.impact(*options, expected=1, error="index-unavailable")
                self.assertEqual(result["error"]["category"], "index")
                self.assertEqual(result["error"]["message"],
                                 "An existing published index and reader lock are required.")
                self.assertFalse(lock_path.exists(), "impact must not repair the missing publication lock")

    @staticmethod
    def stop_process(process):
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=5)

    def test_change_while_waiting_for_snapshot_lock_uses_current_bytes(self):
        lock_path = self.out / "docs/kb/.shin-context.lock"
        with lock_path.open("r+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            process = subprocess.Popen(self.argv("impact", "--depth", "0"), stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, env=self.environment)
            self.addCleanup(self.stop_process, process)
            try:
                time.sleep(0.1)
                self.assertIsNone(process.poll(), "impact bypassed the publication lock")
                content = self.change_member()
                before = self.watched_state()
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)
            try:
                stdout, stderr = process.communicate(timeout=90)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()
        self.assertEqual(process.returncode, 0, stderr)
        result = json.loads(stdout)
        self.assert_policy(result)
        self.assertFalse(result["index_current"])
        self.assertEqual(self.record(result, MEMBER)["source_hash"], fingerprint(content))
        self.assert_no_writes(before)

    def test_concurrent_source_metadata_change_is_rejected(self):
        path = self.root / MEMBER
        path.write_bytes(path.read_bytes() + b"\n/*" + b"x" * (4 * 1024 * 1024) + b"*/\n")
        self.build()
        outside_before = {str(root): tree_state(root) for root in (self.out, self.profiles)}
        stop = threading.Event()
        started = threading.Event()
        mutations = [0]

        def mutate_metadata():
            while not stop.is_set():
                os.utime(path, None)
                mutations[0] += 1
                started.set()
                time.sleep(0.0002)

        lock_path = self.out / "docs/kb/.shin-context.lock"
        with lock_path.open("r+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            process = subprocess.Popen(self.argv("impact", "--depth", "0"), stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, env=self.environment)
            self.addCleanup(self.stop_process, process)
            writer = threading.Thread(target=mutate_metadata, daemon=True)
            writer.start()
            self.addCleanup(writer.join, 5)
            self.addCleanup(stop.set)
            try:
                self.assertTrue(started.wait(5), "race writer did not start")
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)
            try:
                stdout, stderr = process.communicate(timeout=90)
            finally:
                stop.set()
                writer.join(timeout=5)
                if process.poll() is None:
                    process.kill()
                    process.communicate()
        self.assertGreater(mutations[0], 1, "no concurrent metadata changes were exercised")
        self.assertEqual(process.returncode, 1, stdout or stderr)
        self.assertEqual(stdout, "", "racy source observation produced an impact envelope")
        result = json.loads(stderr)
        self.assertEqual(result["schema_version"], "shin.error.v1")
        self.assertEqual(result["error"]["code"], "file-changed-during-verification")
        self.assertTrue(result["error"]["retryable"])
        for root in (self.out, self.profiles):
            self.assertEqual(tree_state(root), outside_before[str(root)])
        self.assertFalse(self.sentinel.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
