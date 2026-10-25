#!/usr/bin/env python3
"""Pure fusion and hermetic native admission tests; no model or network required."""

import copy
import json
import random
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from run_semantic_hybrid import evaluate, freeze, lexical_batch, load_policy, model_available, verify_publications
from run_local_semantic_pilot import fingerprint
from semantic_hybrid import combine, map_lexical, parent_lookup, regression_gate

CORE, EXPORTER = [str(Path(sys.argv.pop(1)).resolve()) for _ in range(2)]
POLICY = load_policy(Path(__file__).resolve().parents[1] / "fixtures/semantic-hybrid-policy.json")
DOCUMENT_FIRST = load_policy(Path(__file__).resolve().parents[1] / "fixtures/semantic-document-first-policy.json")
CODE_ANCHOR = load_policy(Path(__file__).resolve().parents[1] / "fixtures/semantic-code-anchor-policy.json")


class CombinationTests(unittest.TestCase):
    def setUp(self):
        self.units = [{"project": "p", "path": f"src/file{i}.ts", "start_line": 1, "end_line": 2,
                       "source_sha256": "bound", "record_ids": [f"fact{i}"]} for i in range(40)]

    def lexical(self, indexes):
        return {i: {"rank": rank + 1, "evidence": [{"id": f"fact{i}", "matched_group_indices": [rank]}]}
                for rank, i in enumerate(indexes)}

    def semantic(self, indexes):
        return {i: {"rank": rank + 1, "similarity": 0.9 - rank * 0.01} for rank, i in enumerate(indexes)}

    def code_lexical(self, indexes):
        result = self.lexical(indexes)
        for row in result.values():
            row["evidence"][0].update({"type": "fact", "evidence_kind": "implementation-source",
                "authority_level": "source-fact", "verification_level": "source-observed", "rank": row["rank"]})
        return result

    def ids(self, result):
        return [row["record_ids"][0] for row in result["candidates"]]

    def test_confirmed_identifiers_keep_lexical_order_without_semantic_neighbors(self):
        result = combine(self.units, self.lexical([3, 1]), self.semantic([2, 1, 3]), True, POLICY)
        self.assertEqual(self.ids(result), ["fact3", "fact1"])
        self.assertTrue(all("semantic" not in c["signals"] for c in result["candidates"]))

    def test_absent_model_preserves_lexical_candidates_and_order(self):
        result = combine(self.units, self.lexical([3, 1]), {}, False, POLICY)
        self.assertEqual(self.ids(result), ["fact3", "fact1"])
        self.assertEqual(result["mode"], "lexical-only")
        with tempfile.TemporaryDirectory() as temporary:
            self.assertFalse(model_available(Path(temporary) / "missing"))
            self.assertTrue(model_available(Path(temporary)))

    def test_rank_union_rewards_agreement_and_has_deterministic_ties(self):
        lexical, semantic = self.lexical([3, 1]), self.semantic([2, 1])
        first = combine(self.units, lexical, semantic, False, POLICY)
        self.assertEqual(self.ids(first), ["fact1", "fact3", "fact2"])
        self.assertEqual(first, combine(self.units, dict(reversed(list(lexical.items()))),
                                       dict(reversed(list(semantic.items()))), False, POLICY))

    def test_empty_lexical_keeps_semantic_order_but_does_not_assert_existence(self):
        result = combine(self.units, {}, self.semantic([5, 2, 1]), False, POLICY)
        self.assertEqual(self.ids(result), ["fact5", "fact2", "fact1"])
        self.assertFalse(result["symbol_existence_asserted"])
        self.assertFalse(result["pack_completion_asserted"])
        self.assertFalse(result["semantic_grants_lexical_coverage"])
        self.assertTrue(all(not c["current_source_verified"] for c in result["candidates"]))

    def test_union_never_borrows_or_merges_lexical_group_coverage(self):
        lexical = self.lexical([0])
        original = copy.deepcopy(lexical)
        result = combine(self.units, lexical, self.semantic([0, 1]), False, POLICY)
        self.assertEqual(lexical, original)
        self.assertEqual(result["candidates"][0]["signals"]["lexical"], lexical[0])
        for c in result["candidates"]:
            self.assertNotIn("matched_group_indices", c)
            self.assertNotIn("authority_level", c)
        self.assertNotIn("lexical", result["candidates"][1]["signals"])

    def test_corpus_growth_and_overlap_cannot_expand_output_or_document_caps(self):
        self.units[1]["path"] = self.units[2]["path"] = self.units[0]["path"]
        result = combine(self.units, self.lexical(range(20)), self.semantic(range(10, 30)), False, POLICY)
        self.assertEqual(len(result["candidates"]), 20)
        self.assertEqual(len({c["record_ids"][0] for c in result["candidates"]}), 20)
        self.assertLessEqual(sum(c["path"] == self.units[0]["path"] for c in result["candidates"]), 2)
        smaller = combine(self.units, self.lexical([0]), self.semantic([0]), False, POLICY)
        self.assertEqual(len(smaller["candidates"]), 1)

    def test_invalid_cross_project_child_ranks_and_scores_are_rejected(self):
        for field, value in (("project", "other"), ("unit_kind", "semantic-subrange")):
            units = copy.deepcopy(self.units)
            units[1][field] = value
            with self.assertRaises(ValueError):
                combine(units, self.lexical([0]), self.semantic([1]), False, POLICY)
        for semantic in ({0: {"rank": 2, "similarity": 1}}, {0: {"rank": 1, "similarity": float("nan")}}):
            with self.assertRaises(ValueError):
                combine(self.units, {}, semantic, False, POLICY)
        with self.assertRaises(ValueError):
            combine(self.units, self.lexical(range(21)), {}, False, POLICY)

    def test_mapping_preserves_rows_rejects_unbound_ids_and_counts_exclusions(self):
        row = {"type": "fact", "id": "fact0", "path": "src/file0.ts", "start_line": 1, "end_line": 2,
               "matched_group_indices": [0]}
        payload = {"ok": True, "evidence_budget": {"max_records": 20}, "evidence": [row,
            {**row, "path": "profiles/answers.json"}, {**row, "type": "contract"}]}
        lookup = parent_lookup(self.units)
        mapped, excluded = map_lexical("p", payload, self.units, lookup)
        self.assertEqual(mapped, {0: {"rank": 1, "evidence": [row]}})
        self.assertEqual(excluded, {"evaluation-metadata": 1, "non-parent-record-type": 1})
        for change in ({"id": "wrong"}, {"end_line": 3}, {"path": "outside.ts"}):
            with self.assertRaises(ValueError):
                map_lexical("p", {**payload, "evidence": [{**row, **change}]}, self.units, lookup)
        with self.assertRaises(ValueError):
            parent_lookup([self.units[0], self.units[0]])

    def test_regression_gate_preserves_cases_not_only_aggregate_counts(self):
        rows = [{"id": "a", "kind": "english", "semantic_rank": 4, "hybrid_rank": 6, "lexical_rank": None},
                {"id": "b", "kind": "english", "semantic_rank": None, "hybrid_rank": 1, "lexical_rank": 1}]
        self.assertFalse(regression_gate(rows)["passed"])
        rows[0]["hybrid_rank"] = 3
        self.assertTrue(regression_gate(rows)["passed"])

    def test_document_first_prioritizes_distinct_paths_then_lexical_additions_then_ranges(self):
        self.units[1]["path"] = self.units[0]["path"]
        result = combine(self.units, self.lexical([8, 2]), self.semantic([0, 1, 2]), False, DOCUMENT_FIRST)
        self.assertEqual(self.ids(result), ["fact0", "fact2", "fact8", "fact1"])
        self.assertEqual(result["candidates"][1]["signals"]["lexical"]["rank"], 2)
        self.assertTrue(all("fusion_score" not in c for c in result["candidates"]))

    def test_document_first_preserves_confirmed_owner_and_absent_model_order(self):
        lexical, semantic = self.lexical([3, 1]), self.semantic([2, 1, 3])
        owned = combine(self.units, lexical, semantic, False, DOCUMENT_FIRST, owner_confirmed=True)
        self.assertEqual(self.ids(owned), ["fact3", "fact1"])
        self.assertEqual(owned["mode"], "confirmed-owner-lexical")
        self.assertTrue(all("semantic" not in c["signals"] for c in owned["candidates"]))
        missing = combine(self.units, lexical, {}, False, DOCUMENT_FIRST)
        self.assertEqual(self.ids(missing), ["fact3", "fact1"])

    def test_document_first_does_not_expand_full_semantic_path_budget(self):
        result = combine(self.units, self.lexical(range(20, 40)), self.semantic(range(20)), False, DOCUMENT_FIRST)
        self.assertEqual(self.ids(result), [f"fact{i}" for i in range(20)])

    def test_document_first_preserves_every_semantic_path_first_rank_for_arbitrary_overlap(self):
        rng = random.Random(272)
        for _ in range(200):
            for i, unit in enumerate(self.units):
                unit["path"] = f"src/file{i // 2}.ts"
            semantic_ids, lexical_ids = rng.sample(range(40), 20), rng.sample(range(40), 20)
            semantic, lexical = self.semantic(semantic_ids), self.lexical(lexical_ids)
            result = combine(self.units, lexical, semantic, False, DOCUMENT_FIRST)
            paths = [c["path"] for c in result["candidates"]]
            self.assertLessEqual(len(paths), 20)
            self.assertTrue(all(paths.count(p) <= 2 for p in paths))
            for rank, index in enumerate(semantic_ids):
                self.assertLessEqual(paths.index(self.units[index]["path"]), rank)
            self.assertEqual(result, combine(self.units, dict(reversed(list(lexical.items()))),
                dict(reversed(list(semantic.items()))), False, DOCUMENT_FIRST))

    def test_code_anchor_reserves_only_one_original_first5_source_row(self):
        lexical = self.code_lexical([8, 9])
        lexical[8]["evidence"][0]["rank"] = 5
        result = combine(self.units, lexical, self.semantic([0, 1, 2]), False, CODE_ANCHOR)
        self.assertEqual(self.ids(result), ["fact8", "fact0", "fact1", "fact2", "fact9"])
        self.assertEqual(result["code_anchor"], {"status": "selected", "path": "src/file8.ts", "core_rank": 5})
        self.assertEqual(result["candidates"][0]["signals"]["lexical"], lexical[8])
        self.assertNotIn("matched_group_indices", result["candidates"][0])

    def test_code_anchor_rejects_advisory_rows_and_post_filter_rank_promotion(self):
        for field, value in (("type", "section"), ("evidence_kind", "machine-contract"),
                             ("authority_level", "advisory"), ("verification_level", "declared"),
                             ("rank", 6), ("rank", True), ("rank", 0)):
            lexical = self.code_lexical([8])
            lexical[8]["evidence"][0][field] = value
            result = combine(self.units, lexical, self.semantic([0, 1]), False, CODE_ANCHOR)
            self.assertEqual(self.ids(result), ["fact0", "fact1", "fact8"])
            self.assertEqual(result["code_anchor"]["status"], "no-eligible-source-fact-in-core-first5")

    def test_code_anchor_keeps_exact_owner_and_absent_model_routes_unchanged(self):
        lexical, semantic = self.code_lexical([3, 1]), self.semantic([0, 2])
        for confirmed, owner, candidates in ((True, False, semantic), (False, True, semantic), (False, False, {})):
            result = combine(self.units, lexical, candidates, confirmed, CODE_ANCHOR, owner)
            self.assertEqual(self.ids(result), ["fact3", "fact1"])
            self.assertEqual(result["code_anchor"]["status"], "not-applicable")
            self.assertTrue(all("semantic" not in row["signals"] for row in result["candidates"]))

    def test_code_anchor_cannot_displace_a_semantic_path_from_saturated_output(self):
        result = combine(self.units, self.code_lexical([35]), self.semantic(range(20)), False, CODE_ANCHOR)
        self.assertEqual(self.ids(result), [f"fact{i}" for i in range(20)])
        self.assertEqual(result["code_anchor"]["status"], "semantic-path-budget-full")

    def test_code_anchor_bounds_semantic_document_displacement_and_duplicate_ranges(self):
        rng = random.Random(929)
        for _ in range(200):
            for i, unit in enumerate(self.units):
                unit["path"] = f"src/file{i // 2}.ts"
            lexical, semantic = self.code_lexical(rng.sample(range(40), 20)), self.semantic(rng.sample(range(40), 20))
            result = combine(self.units, lexical, semantic, False, CODE_ANCHOR)
            paths = [c["path"] for c in result["candidates"]]
            semantic_paths = list(dict.fromkeys(self.units[i]["path"] for i in semantic))
            self.assertTrue(all(paths.index(path) <= rank + 1 for rank, path in enumerate(semantic_paths)))
            self.assertLessEqual(len(paths), 20)
            self.assertTrue(all(paths.count(path) <= 2 for path in paths))
            self.assertEqual(result, combine(self.units, dict(reversed(list(lexical.items()))),
                                            dict(reversed(list(semantic.items()))), False, CODE_ANCHOR))

    def test_extended_gate_catches_ordinary_lexical_and_document_first_losses(self):
        rows = [{"id": "lexical-loss", "kind": "russian", "semantic_rank": None,
                 "lexical_rank": 2, "document_first_rank": 18, "hybrid_rank": 18},
                {"id": "navigation-loss", "kind": "english", "semantic_rank": 6,
                 "lexical_rank": None, "document_first_rank": 4, "hybrid_rank": 6}]
        self.assertTrue(regression_gate(rows)["passed"])
        self.assertEqual(regression_gate(rows, True)["regressions"], [
            {"id": "lexical-loss", "reason": "lexical-language-first5-hit-lost"},
            {"id": "navigation-loss", "reason": "document_first-language-first5-hit-lost"}])


class NativeAdmissionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-hybrid-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root, self.out = self.base / "root", self.base / "p/index"
        self.root.mkdir()
        (self.root / "src").mkdir()
        (self.base / "profiles").mkdir()
        (self.root / "README.md").write_text("# Example\n\nBound cache source.\n")
        self.profile = self.base / "profiles/fixture.json"
        self.profile.write_text(json.dumps({"id": "fixture", "sourceLabel": "Fixture", "rootFiles": ["README.md"],
            "sourceExtensions": [".md"], "sourceFactRoots": ["src"], "sourceFactExtensions": [".ts"],
            "sourceFactExtractors": ["language"], "defaultTopic": "fixture"}))
        self.source = self.root / "src/cache.ts"
        self.source.write_text("export function cacheScope(brand: string) {\n return ['cache', brand];\n}\n")
        build = subprocess.run([CORE, "--root", str(self.root), "--out-dir", str(self.out), "--profile", "fixture",
                                "--profile-dir", str(self.profile.parent), "build"], capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)

    def query(self, query="cacheScope", current=False, success=True):
        result = subprocess.run([EXPORTER, str(self.root), str(self.out), str(self.profile.parent), "fixture",
                                 *(["--require-current"] if current else [])],
                                input=json.dumps([query]), capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return json.loads(result.stdout) if success else result

    def test_verified_snapshot_exposes_confirmed_identifier_and_original_rows(self):
        result = self.query()
        self.assertTrue(result["completed"])
        row = result["results"][0]
        self.assertEqual(row["retrieval_mode"], "identifier-scoped")
        self.assertEqual(row["index_freshness"]["status"], "not-assessed")
        self.assertEqual(row["evidence"][0]["path"], "src/cache.ts")
        self.assertEqual(result["snapshot_id"], row["snapshot_id"])

    def test_profile_recovery_requires_exact_frozen_bytes_and_keeps_live_changes(self):
        project = {"id": "p", "root": str(self.root), "profile": str(self.profile)}
        manifest = {"projects": [{**project, "profile_sha256": fingerprint(self.profile),
            "publication_sha256": fingerprint(self.out / "docs/kb/generated/manifest.json")}]}
        spec = {"projects": [project]}
        expected = verify_publications(spec, manifest, self.base)
        frozen = self.base / "frozen.json"
        frozen.write_bytes(self.profile.read_bytes())
        self.profile.write_text(self.profile.read_text() + "\n")
        with self.assertRaises(ValueError):
            verify_publications(spec, manifest, self.base)
        self.assertEqual(verify_publications(spec, manifest, self.base, {"p": str(frozen)}), expected)
        self.assertNotEqual(fingerprint(self.profile), fingerprint(frozen))
        frozen.write_text("{}")
        with self.assertRaises(ValueError):
            verify_publications(spec, manifest, self.base, {"p": str(frozen)})

    def test_fresh_freeze_prefers_captured_profiles_and_rejects_late_vector_binding(self):
        project = {"id": "p", "root": str(self.root), "profile": str(self.profile), "cases": []}
        spec = {"projects": [project]}
        source = self.base / "input"
        source.mkdir()
        (source / "spec.json").write_text(json.dumps(spec))
        manifest = {"projects": [{**project, "profile_sha256": fingerprint(self.profile),
            "publication_sha256": fingerprint(self.out / "docs/kb/generated/manifest.json")}]}
        (source / "input-manifest.json").write_text(json.dumps(manifest))
        captured = self.base / "p/profiles" / self.profile.name
        captured.parent.mkdir()
        captured.write_bytes(self.profile.read_bytes())
        self.profile.write_text(self.profile.read_text() + "\n")
        frozen = self.base / "frozen"
        with patch("run_semantic_hybrid.load_input", return_value=(manifest, spec, {}, [])), patch("builtins.print"):
            freeze(None, self.base, source / "spec.json", Path(__file__).resolve().parents[1] / "fixtures/semantic-document-first-policy.json",
                   Path(EXPORTER), None, frozen, input_dir=source)
        receipt = json.loads((frozen / "freeze.json").read_text())
        self.assertEqual(fingerprint(Path(receipt["profiles"]["p"])), fingerprint(captured))
        self.assertIsNone(receipt["parent_control"])
        with self.assertRaisesRegex(ValueError, "pre-bound vector run"):
            evaluate(frozen, self.base / "absent-model", self.base / "result")
        binding = {"input_manifest_sha256": fingerprint(source / "input-manifest.json"),
                   "evaluation_freeze_sha256": None}
        with patch("run_semantic_hybrid.load_completed_run", return_value=({}, binding, manifest, source)):
            with self.assertRaisesRegex(ValueError, "not bound before"):
                evaluate(frozen, self.base / "absent-model", self.base / "result", vector_run=self.base / "run")
        self.assertFalse((self.base / "result").exists())
        (frozen / "policy.json").write_text("{}")
        with self.assertRaisesRegex(ValueError, "Frozen policy"):
            evaluate(frozen, self.base / "absent-model", self.base / "result", vector_run=self.base / "run")

    def test_selection_control_cannot_borrow_another_vector_run(self):
        source = self.base / "input"
        source.mkdir()
        project = {"id": "p", "root": str(self.root), "profile": str(self.profile), "cases": []}
        (source / "spec.json").write_text(json.dumps({"projects": [project]}))
        run = self.base / "vectors"
        run.mkdir()
        (run / "report.json").write_text("{}")
        control = self.base / "control"
        control.mkdir()
        (control / "report.json").write_text(json.dumps({"completed": True, "results": {"development": []}}))
        (control / "binding.json").write_text(json.dumps({"run_report_sha256": "different-run"}))
        with patch("run_semantic_hybrid.load_completed_run", return_value=({}, {}, {}, source)):
            with self.assertRaisesRegex(ValueError, "different vector run"):
                freeze(run, self.base, source / "spec.json", Path(__file__).resolve().parents[1] / "fixtures/semantic-code-anchor-policy.json",
                       Path(EXPORTER), control / "report.json", self.base / "rejected")
        self.assertFalse((self.base / "rejected").exists())

    def test_current_admission_rejects_drift_without_replacing_frozen_source(self):
        frozen = self.query()
        self.query(current=True)
        self.source.write_text("export function replacement() { return 2; }\n")
        self.assertEqual(self.query(), frozen)
        self.query(current=True, success=False)
        project = {"root": str(self.root), "profile": str(self.profile)}
        with self.assertRaisesRegex(ValueError, "no semantic fallback"):
            lexical_batch(Path(EXPORTER), project, self.out, ["cacheScope"], frozen["snapshot_id"], True)

    def test_corrupt_snapshot_is_rejected_and_never_scans_source(self):
        (self.out / "docs/kb/generated/facts.jsonl").write_text("{}\n")
        result = self.query(success=False)
        self.assertEqual(result.stdout, "")
        self.assertNotIn("cacheScope", result.stderr)

    def test_negative_identifier_is_not_confirmed(self):
        row = self.query("AbsentQzxHybridCapabilityNotImplemented")["results"][0]
        self.assertNotEqual(row["retrieval_mode"], "identifier-scoped")
        self.assertEqual(row["evidence"], [])

    def test_query_batch_is_bounded_and_errors_are_sanitized(self):
        result = subprocess.run([EXPORTER, str(self.root), str(self.out), str(self.profile.parent), "fixture"],
            input=json.dumps(["private-input-canary"] * 129), capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertNotIn("private-input-canary", result.stderr)


if __name__ == "__main__":
    unittest.main()
