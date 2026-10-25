#!/usr/bin/env python3
"""Offline invariant tests; no model download, ML dependency or external service."""

import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from prepare_semantic_pilot import owned_path, project_units, sources_current, published_identifiers, capture_profile
from reproject_semantic_identifiers import enrich_units, publication_fingerprint, read_bound_facts
from reproject_semantic_bodies import apply_ranges
from compare_semantic_projections import compare
from evaluate_semantic_selection import select_by_path
from evaluate_semantic_chunks import verify_freeze, validate_heldout
from semantic_chunk_projection import child_units
from run_carry_forward_comparison import save
from run_local_semantic_pilot import (
    MODEL_FILES, candidate, check_budget, fingerprint, load_input, load_model, seal_model, verify_model,
    same_vector_input, reuse_compatible_policy, summarize_quality, augmented_reuse_mapping, vector_input_key,
    profile_rebound_mapping,
)


class SemanticPilotTests(unittest.TestCase):
    def chunk_fixture(self):
        units, _ = self.units([self.row, {**self.row, "id": "fact-2"}])
        child = {"path": "owned.cpp", "start_line": 2, "end_line": 2,
                 "parent_start_line": 1, "parent_end_line": 2, "parent_span_hash": "fnv1a64:1111111111111111",
                 "span_hash": "fnv1a64:2222222222222222", "range_origin": "semantic-line-window",
                 "text": "return value;", "redacted": False, "truncated": False}
        record = {**self.row, "record_id": "fact-1", "span_hash": child["parent_span_hash"],
                  "symbol": "functionName", "redacted": False, "truncated": True, "chunking": {
                      "status": "partitioned", "omitted_lines": 0, "clipped_chunks": 0, "chunks": [child]}}
        return units, record

    def test_child_units_merge_equal_alias_ranges_without_forging_record_ids(self):
        units, record = self.chunk_fixture()
        children, receipt = child_units(units, [record, {**record, "record_id": "fact-2"}])
        self.assertEqual(receipt["child_units"], 1)
        self.assertEqual(children[0]["record_ids"], ["fact-1", "fact-2"])
        self.assertEqual(children[0]["parent_ranges"][0]["start_line"], 1)
        self.assertEqual(children[0]["start_line"], 2)
        evidence = candidate(children[0], 0.8)
        self.assertEqual(evidence["unit_kind"], "semantic-subrange")
        self.assertEqual(evidence["parent_ranges"], children[0]["parent_ranges"])
        self.assertFalse(evidence["current_source_verified"])

    def test_child_units_reject_parent_escape_clipping_bypass_and_wrong_bindings(self):
        units, record = self.chunk_fixture()
        child = record["chunking"]["chunks"][0]
        for change in ({"start_line": 0}, {"end_line": 3}, {"parent_span_hash": "wrong"},
                       {"span_hash": "wrong"}, {"text": "x" * 1025}, {"path": "other.cpp"}):
            invalid = {**record, "chunking": {**record["chunking"], "chunks": [{**child, **change}]}}
            with self.assertRaises(ValueError):
                child_units(units, [invalid])
        for change in ({"redacted": True}, {"record_id": "unpublished-parent"}):
            with self.assertRaises(ValueError):
                child_units(units, [{**record, **change}])

    def test_unclipped_parent_does_not_grow_the_model_corpus(self):
        units, record = self.chunk_fixture()
        children, receipt = child_units(units, [{**record, "truncated": False}])
        self.assertEqual(children, [])
        self.assertEqual(receipt["skipped_unclipped_parents"], 1)
        with self.assertRaisesRegex(ValueError, "one project"):
            child_units([*units, {**units[0], "project": "other"}], [record])

    def test_path_cap_preserves_order_and_fills_from_distinct_paths(self):
        paths = ["a", "a", "a", "b", "c", "c", "d"]
        self.assertEqual(select_by_path(range(7), paths, 5, 2), [0, 1, 3, 4, 5])
        self.assertEqual(select_by_path([4, 2, 0, 1, 3, 5, 6], paths, 6, 2), [4, 2, 0, 3, 5, 6])

    def test_path_cap_keeps_small_sets_and_rejects_invalid_limits(self):
        self.assertEqual(select_by_path([0, 1, 2], ["a", "a", "a"], 20, 2), [0, 1])
        self.assertEqual(select_by_path([], [], 20, 2), [])
        for limit, cap in ((0, 2), (20, 0), (True, 2), (20, 1.5)):
            with self.assertRaises(ValueError):
                select_by_path([], [], limit, cap)

    def test_quality_keeps_language_cohorts_and_top_five_separate(self):
        policy = {"quality": {"cohort_size_per_language": 5,
                              "english_min_expected_paths_at_20": 3, "russian_min_expected_paths_at_20": 3}}
        rows = [{"kind": kind, "expected_rank_at_20": 9 if i < 3 else None,
                 "expected_in_first_5": False} for kind in ("english", "russian") for i in range(5)]
        summary, passed = summarize_quality(rows, policy)
        self.assertTrue(passed)
        self.assertEqual(summary["russian"]["expected_in_first_5"], 0)
        self.assertFalse(summarize_quality(rows[:-1], policy)[1])
        rows[5]["expected_rank_at_20"] = None
        self.assertFalse(summarize_quality(rows, policy)[1])

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.root = self.base / "source"
        self.root.mkdir()
        (self.root / "owned.cpp").write_text("unpublished-canary\nint value;\n")
        self.generated = self.base / "generated"
        self.generated.mkdir()
        self.row = {"id": "fact-1", "path": "owned.cpp", "start_line": 1, "end_line": 2,
                    "title": "Owned function", "summary": "Published redacted summary"}
        self.project = {"id": "project", "root": str(self.root)}

    def units(self, facts, sections=()):
        for name, rows in (("facts", facts), ("sections", sections)):
            (self.generated / (name + ".jsonl")).write_text("".join(json.dumps(row) + "\n" for row in rows))
        return project_units(self.project, self.generated)

    def test_merges_only_equal_ranges_and_keeps_record_ids(self):
        rows = [self.row, {**self.row, "id": "fact-2"}, {**self.row, "id": "fact-3", "start_line": 2}]
        units, inventory = self.units(rows)
        self.assertEqual(len(units), 2)
        self.assertEqual(units[0]["record_ids"], ["fact-1", "fact-2"])
        self.assertEqual(units[0]["text"].count("Published redacted summary"), 1)
        self.assertEqual(inventory["input_rows"], 3)

    def test_model_input_uses_published_text_not_raw_source(self):
        units, inventory = self.units([self.row])
        self.assertNotIn("unpublished-canary", units[0]["text"])
        self.assertIn("Published redacted summary", units[0]["text"])
        self.assertEqual(units[0]["source_sha256"], fingerprint(self.root / "owned.cpp"))
        self.assertEqual(sources_current(self.root, inventory["sources"]), [])
        (self.root / "owned.cpp").write_text("changed\n")
        self.assertEqual(sources_current(self.root, inventory["sources"]), ["owned.cpp"])

    def test_distinct_paths_never_share_provenance(self):
        (self.root / "other.cpp").write_text((self.root / "owned.cpp").read_text())
        units, _ = self.units([self.row, {**self.row, "path": "other.cpp"}])
        self.assertEqual(len(units), 2)
        self.assertNotEqual(units[0]["path"], units[1]["path"])

    def test_sections_keep_published_body_and_metadata_is_excluded(self):
        section = {"document_id": "doc", "path": "owned.cpp", "start_line": 1, "end_line": 2,
                   "heading": "Rule", "indexed_text": "full published body", "preview": "short preview"}
        units, inventory = self.units([{**self.row, "path": "profiles/answers.json"}], [section])
        self.assertEqual(inventory["excluded_metadata_rows"], 1)
        self.assertIn("full published body", units[0]["text"])
        self.assertNotIn("short preview", units[0]["text"])

    def test_bad_coordinates_and_outside_sources_are_rejected(self):
        for row in ({**self.row, "start_line": 0}, {**self.row, "end_line": 3},
                    {**self.row, "start_line": True}):
            with self.assertRaises(ValueError):
                self.units([row])
        outside = self.base / "outside.cpp"
        outside.write_text("outside")
        (self.root / "link.cpp").symlink_to(outside)
        for path in ("../outside.cpp", str(outside), "link.cpp"):
            with self.assertRaises(ValueError):
                owned_path(self.root, path)

    def model_fixture(self):
        directory = self.base / "model"
        directory.mkdir()
        for name in MODEL_FILES:
            (directory / name).write_text("{}")
        (directory / "tokenizer_config.json").write_text('{"model_max_length":512}')
        policy = {"model": "fixture", "revision": "pinned", "model_sha256": fingerprint(directory / "model.onnx"),
                  "max_model_bytes": 1000, "max_tokens": 512}
        seal_model(directory, policy)
        return directory, policy

    def test_model_assets_are_pinned_and_tampering_is_rejected(self):
        directory, policy = self.model_fixture()
        self.assertEqual(verify_model(directory, policy)["revision"], "pinned")
        (directory / "tokenizer.json").write_text("changed")
        with self.assertRaises(ValueError):
            verify_model(directory, policy)

    def test_model_manifest_cannot_expand_to_arbitrary_files(self):
        directory, policy = self.model_fixture()
        manifest = json.loads((directory / "manifest.json").read_text())
        manifest["files"]["../outside"] = {"sha256": ""}
        save(directory / "manifest.json", manifest)
        with self.assertRaises(ValueError):
            verify_model(directory, policy)

    def test_missing_model_fails_before_import_or_download(self):
        with patch.dict(sys.modules, {"fastembed": None}):
            with self.assertRaisesRegex(FileNotFoundError, "explicit provisioning"):
                load_model(self.base / "missing", {})

    def test_candidate_does_not_grant_coverage_or_currentness(self):
        units, _ = self.units([self.row])
        result = candidate(units[0], 0.91)
        self.assertFalse(result["current_source_verified"])
        self.assertEqual(result["status"], "navigation-candidate-not-an-answer")
        for forbidden in ("matched_group_indices", "complete", "ready", "authority_level"):
            self.assertNotIn(forbidden, result)

    def test_frozen_input_integrity_is_checked(self):
        units, _ = self.units([self.row])
        directory = self.base / "input"
        directory.mkdir()
        save(directory / "spec.json", {})
        save(directory / "policy.json", {"max_units": 10, "max_input_bytes": 10000})
        (directory / "units.jsonl").write_text(json.dumps(units[0]) + "\n")
        manifest = {"units": 1, **{key: fingerprint(directory / name) for name, key in (
            ("spec.json", "spec_sha256"), ("policy.json", "policy_sha256"), ("units.jsonl", "units_sha256"))}}
        save(directory / "input-manifest.json", manifest)
        self.assertEqual(len(load_input(directory)[3]), 1)
        (directory / "units.jsonl").write_text("{}\n")
        with self.assertRaisesRegex(ValueError, "Frozen input changed"):
            load_input(directory)

    def test_resource_limits_stop_instead_of_expanding(self):
        policy = {"max_run_seconds": 1, "max_peak_rss_bytes": 10}
        with self.assertRaisesRegex(RuntimeError, "wall-time"):
            check_budget(policy, time.monotonic() - 10)
        with patch("run_local_semantic_pilot.peak_rss_bytes", return_value=11):
            with self.assertRaisesRegex(RuntimeError, "peak-RSS"):
                check_budget(policy, time.monotonic())

    def test_profile_capture_keeps_original_bytes_and_rejects_drift_or_overwrite(self):
        profile = self.base / "profile.json"
        profile.write_bytes(b'{"id": "fixture"}\n')
        expected = fingerprint(profile)
        captured = capture_profile(profile, self.base / "captured", expected)
        self.assertEqual(fingerprint(captured), expected)
        with self.assertRaises(FileExistsError):
            capture_profile(profile, captured.parent, expected)
        profile.write_bytes(b'{"id": "changed"}\n')
        with self.assertRaisesRegex(ValueError, "changed"):
            capture_profile(profile, self.base / "failed", expected)
        self.assertEqual(fingerprint(captured), expected)

    def test_published_references_are_preserved_without_aliases(self):
        row = {**self.row, "identifier_terms": ["refreshDocument", "documentReads", "refreshDocument"]}
        units, _ = self.units([row])
        self.assertNotIn("Identifier references:", units[0]["text"])
        enriched, _ = enrich_units(units, [row])
        self.assertTrue(enriched[0]["text"].endswith("Identifier references: refreshDocument documentReads"))
        for invalid in ("single-string", [1], [""], None):
            with self.assertRaises(ValueError):
                published_identifiers({"identifier_terms": invalid})

    def test_reprojection_keeps_corpus_and_source_bindings(self):
        units, _ = self.units([self.row])
        original = json.loads(json.dumps(units))
        facts = [{**self.row, "identifier_terms": ["refreshDocument"]}]
        changed, count = enrich_units(units, facts)
        self.assertEqual(units, original)
        self.assertEqual(count, 1)
        self.assertFalse(same_vector_input(original[0], changed[0]))
        self.assertTrue(same_vector_input(original[0], dict(original[0])))
        with self.assertRaises(ValueError):
            same_vector_input(original[0], {**changed[0], "source_sha256": "different"})
        with self.assertRaises(ValueError):
            enrich_units(units, [{**facts[0], "id": "unowned-record"}])

    def test_reprojection_validates_original_publication_integrity(self):
        content = (json.dumps(self.row) + "\n").encode()
        (self.generated / "facts.jsonl").write_bytes(content)
        manifest = {"generated_file_integrity": {"algorithm": "fnv1a64", "files": [{
            "path": "docs/kb/generated/facts.jsonl", "bytes": len(content),
            "content_hash": publication_fingerprint(content)}]}}
        save(self.generated / "manifest.json", manifest)
        bound = fingerprint(self.generated / "manifest.json")
        self.assertEqual(read_bound_facts(self.generated, bound)[0], [self.row])
        (self.generated / "facts.jsonl").write_bytes(content.replace(b"fact-1", b"fact-2"))
        with self.assertRaisesRegex(ValueError, "facts changed"):
            read_bound_facts(self.generated, bound)

    def test_reuse_allows_only_projection_policy_difference(self):
        policy = {"model": "fixed", "max_tokens": 512, "projection": "summary"}
        reuse_compatible_policy(policy, {**policy, "projection": "references"})
        for changed in ({**policy, "model": "other"}, {**policy, "max_tokens": 256}, {**policy, "extra": True}):
            with self.assertRaises(ValueError):
                reuse_compatible_policy(policy, changed)

    def mapping_fixture(self):
        units, inventory = self.units([self.row, {**self.row, "start_line": 2, "id": "fact-2"}])
        source = self.base / "reuse-input"
        source.mkdir()
        (source / "units.jsonl").write_text("".join(json.dumps(u) + "\n" for u in units))
        manifest = {"units": len(units), "units_sha256": fingerprint(source / "units.jsonl"),
                    "projects": [{**self.project, "profile_sha256": "profile", "sources": inventory["sources"]}]}
        return source, manifest, units

    def test_augmented_reuse_reorders_adds_and_removes_without_rebinding(self):
        source, manifest, units = self.mapping_fixture()
        new = {**units[0], "text": "new text"}
        self.assertEqual(augmented_reuse_mapping(source, manifest, manifest, [units[1], new, units[0]]), [1, None, 0])
        self.assertEqual(augmented_reuse_mapping(source, manifest, manifest, [units[1]]), [1])
        self.assertEqual(vector_input_key(units[0]), vector_input_key(dict(reversed(list(units[0].items())))))

    def test_augmented_reuse_never_copies_changed_metadata(self):
        source, manifest, units = self.mapping_fixture()
        for change in ({"record_ids": ["other"]}, {"start_line": 2}, {"redacted": True},
                       {"span_hash": "changed"}, {"parent_ranges": [{"start_line": 1}]}):
            self.assertEqual(augmented_reuse_mapping(source, manifest, manifest, [{**units[0], **change}]), [None])
        current = json.loads(json.dumps(manifest))
        current["projects"][0]["sources"]["owned.cpp"]["sha256"] = "new-source"
        self.assertEqual(augmented_reuse_mapping(source, manifest, current,
            [{**units[0], "source_sha256": "new-source"}]), [None])
        with self.assertRaisesRegex(ValueError, "source binding"):
            augmented_reuse_mapping(source, manifest, manifest, [{**units[0], "source_sha256": "unbound"}])

    def test_augmented_reuse_rejects_scope_drift_duplicates_and_tampering(self):
        source, manifest, units = self.mapping_fixture()
        for field in ("root", "profile_sha256"):
            changed = {**manifest, "projects": [{**manifest["projects"][0], field: "other"}]}
            with self.assertRaisesRegex(ValueError, "ownership"):
                augmented_reuse_mapping(source, manifest, changed, units)
        with self.assertRaisesRegex(ValueError, "destination"):
            augmented_reuse_mapping(source, manifest, manifest, [units[0], units[0]])
        for content in (json.dumps(units[0]) + "\n", (json.dumps(units[0]) + "\n") * 2):
            (source / "units.jsonl").write_text(content)
            with self.assertRaises(ValueError):
                augmented_reuse_mapping(source, manifest, manifest, units)

    def transition_fixture(self):
        source, old, units = self.mapping_fixture()
        new = json.loads(json.dumps(old))
        new["projects"][0]["profile_sha256"] = "new-profile"
        project = old["projects"][0]
        admission = {"schema_version": "shin.semantic-profile-transition.v1",
            "donor_manifest_digest": vector_input_key(old).hex(),
            "destination_manifest_digest": vector_input_key(new).hex(),
            "changes": [{"project": project["id"], "root": project["root"], "from": "profile", "to": "new-profile"}]}
        return source, old, new, units, admission

    def test_reviewed_profile_transition_reuses_only_exact_rows_and_keeps_old_guard(self):
        source, old, new, units, admission = self.transition_fixture()
        with self.assertRaisesRegex(ValueError, "ownership"):
            augmented_reuse_mapping(source, old, new, units)
        self.assertEqual(profile_rebound_mapping(source, old, new, [units[1], units[0]], admission), [1, 0])
        for change in ({"text": "different"}, {"record_ids": ["changed"]}, {"redacted": True}, {"start_line": 12}):
            self.assertEqual(profile_rebound_mapping(source, old, new, [{**units[0], **change}], admission), [None])

    def test_profile_transition_rejects_unreviewed_pair_roots_and_inventory_changes(self):
        source, old, new, units, admission = self.transition_fixture()
        for change in ({"changes": []}, {"donor_manifest_digest": "wrong"}, {"destination_manifest_digest": "wrong"}):
            with self.assertRaisesRegex(ValueError, "reviewed"):
                profile_rebound_mapping(source, old, new, units, {**admission, **change})
        changed = json.loads(json.dumps(new))
        changed["projects"][0]["root"] = "elsewhere"
        with self.assertRaisesRegex(ValueError, "roots"):
            profile_rebound_mapping(source, old, changed, units, admission)
        with self.assertRaisesRegex(ValueError, "reviewed"):
            profile_rebound_mapping(source, old, {**new, "units": 3}, units, admission)
        with self.assertRaisesRegex(ValueError, "source binding"):
            profile_rebound_mapping(source, old, new, [{**units[0], "source_sha256": "unbound"}], admission)
        with self.assertRaisesRegex(ValueError, "Duplicate destination"):
            profile_rebound_mapping(source, old, new, [units[0], units[0]], admission)

    def test_evaluation_freeze_rejects_late_or_tampered_questions(self):
        directory = self.base / "freeze"
        directory.mkdir()
        save(directory / "heldout.json", {})
        save(directory / "selection.json", {})
        receipt = {"schema_version": "shin.semantic-heldout-freeze.v1", "input_manifest_sha256": "input",
                   "heldout_spec_sha256": fingerprint(directory / "heldout.json"),
                   "selection_sha256": fingerprint(directory / "selection.json")}
        save(directory / "freeze.json", receipt)
        binding = {"input_manifest_sha256": "input", "evaluation_freeze_sha256": fingerprint(directory / "freeze.json")}
        self.assertEqual(verify_freeze(directory, binding), receipt)
        with self.assertRaises(ValueError):
            verify_freeze(directory, {**binding, "evaluation_freeze_sha256": None})
        save(directory / "heldout.json", {"changed": True})
        with self.assertRaisesRegex(ValueError, "changed"):
            verify_freeze(directory, binding)

    def test_heldout_requires_new_enrolled_topics_and_matching_frozen_source(self):
        units, inventory = self.units([self.row])
        development, heldout, projects, enrolled = [], [], [], []
        for i in range(5):
            project = {"id": f"p{i}", "root": str(self.root), "profile": "fixture-profile"}
            development.append({**project, "cases": [{"id": f"old-{i}", "query": f"old query {i}",
                                                      "expected_paths": ["old.cpp"]}]})
            heldout.append({**project, "cases": [{"id": f"new-{i}-{kind}", "kind": kind,
                "query": f"{kind} query {i}", "expected_paths": [] if kind == "negative" else ["owned.cpp"]}
                for kind in ("exact", "english", "russian", "negative")]})
            projects.append({**project, "sources": inventory["sources"]})
            enrolled.extend({**u, "project": project["id"]} for u in units)
        spec, old, manifest = {"projects": heldout}, {"projects": development}, {"projects": projects}
        self.assertEqual(len(validate_heldout(spec, old, manifest, enrolled)), 5)
        for change in ({"query": "old query 0"}, {"expected_paths": ["old.cpp"]},
                       {"expected_paths": ["absent.cpp"]}, {"kind": "english"}):
            invalid = json.loads(json.dumps(spec))
            invalid["projects"][0]["cases"][0].update(change)
            with self.assertRaises(ValueError):
                validate_heldout(invalid, old, manifest, enrolled)
        invalid = json.loads(json.dumps(spec))
        invalid["projects"][0]["root"] = "other-root"
        with self.assertRaises(ValueError):
            validate_heldout(invalid, old, manifest, enrolled)
        (self.root / "owned.cpp").write_text("changed source\n")
        with self.assertRaisesRegex(ValueError, "changed since"):
            validate_heldout(spec, old, manifest, enrolled)

    def test_body_projection_requires_original_records_and_retains_all_units(self):
        units, _ = self.units([self.row, {**self.row, "id": "fact-2", "start_line": 2}])
        record = {"path": "owned.cpp", "start_line": 1, "end_line": 2,
                  "record_id": "fact-1", "text": "safe source", "truncated": True, "redacted": False}
        enriched, receipt = apply_ranges(units, [record])
        self.assertEqual(len(enriched), len(units))
        self.assertEqual(enriched[1], units[1])
        self.assertFalse(same_vector_input(units[0], enriched[0]))
        self.assertEqual(receipt, {"changed_units": 1, "byte_truncated_units": 1, "redacted_units": 0})
        for invalid in ([{**record, "record_id": "other"}], [record, record]):
            with self.assertRaises(ValueError):
                apply_ranges(units, invalid)

    def test_completed_projection_comparison_rejects_changed_controls(self):
        units, _ = self.units([self.row])
        runs = []
        for name in ("control", "candidate"):
            run = self.base / name
            source = run / "input"
            source.mkdir(parents=True)
            unit = {**units[0], "text": name}
            (source / "units.jsonl").write_text(json.dumps(unit) + "\n")
            save(source / "spec.json", {"projects": [{"cases": [{"id": "query", "kind": "english"}]}]})
            policy = {"model": "fixture", "projection": name, "max_units": 10, "max_input_bytes": 10000}
            save(source / "policy.json", policy)
            manifest = {"units": 1, "projects": [{"id": "fixture"}], **{
                key: fingerprint(source / file) for file, key in (("spec.json", "spec_sha256"),
                ("policy.json", "policy_sha256"), ("units.jsonl", "units_sha256"))}}
            save(source / "input-manifest.json", manifest)
            save(run / "run-binding.json", {"input": str(source), "policy": policy,
                 "input_manifest_sha256": fingerprint(source / "input-manifest.json")})
            (run / "vectors.npy").write_bytes(b"synthetic comparison fixture, not model vectors")
            report = {"completed": True, "units": 1, "vectors_sha256": fingerprint(run / "vectors.npy"),
                "runtime_versions": {}, "runtime_options": {}, "model_manifest_sha256": "fixture",
                "summary": {}, "candidate_quality_threshold_met": False,
                "results": [{"id": "query", "kind": "english", "expected_rank_at_20": None, "candidates": []}]}
            save(run / "report.json", report)
            runs.append(run)
        self.assertEqual(compare(*runs)["changed_units"], 1)
        for field, value in (("completed", False), ("runtime_versions", {"changed": True}),
                             ("model_manifest_sha256", "other")):
            save(runs[1] / "report.json", {**report, field: value})
            with self.assertRaises(ValueError):
                compare(*runs)
        save(runs[1] / "report.json", report)
        source = runs[1] / "input"
        (source / "units.jsonl").write_text(json.dumps({**unit, "source_sha256": "changed-source"}) + "\n")
        with self.assertRaises(ValueError):
            compare(*runs)
        manifest["units_sha256"] = fingerprint(source / "units.jsonl")
        save(source / "input-manifest.json", manifest)
        binding = json.loads((runs[1] / "run-binding.json").read_text())
        binding["input_manifest_sha256"] = fingerprint(source / "input-manifest.json")
        save(runs[1] / "run-binding.json", binding)
        with self.assertRaisesRegex(ValueError, "source units"):
            compare(*runs)


if __name__ == "__main__":
    unittest.main()
