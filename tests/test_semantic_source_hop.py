#!/usr/bin/env python3
"""Hermetic link admission, one-hop and candidate-budget checks."""

import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from reproject_semantic_identifiers import publication_fingerprint
from run_local_semantic_pilot import candidate
from semantic_source_hop import follow_source_reference, read_bound_links

POLICY = {"candidate_limit": 20, "report_limit": 5, "per_document": 2}


class SourceHopTests(unittest.TestCase):
    def setUp(self):
        self.units = [{"project": "p", "path": f"src/f{i}.ts", "start_line": 10, "end_line": 20,
                       "source_sha256": f"sha{i}", "record_ids": [f"id{i}"]} for i in range(24)]
        self.project = {"id": "p", "sources": {u["path"]: {"sha256": u["source_sha256"]} for u in self.units}}
        self.paths = {u["path"]: [i] for i, u in enumerate(self.units)}
        self.scores = {i: 0.5 for i in range(len(self.units))}
        self.base = {"mode": "semantic-document-first-with-code-anchor", "code_anchor": {"status": "selected", "path": "src/f0.ts"},
                     "candidates": [candidate(u, 0.5) for u in self.units[:8]], "symbol_existence_asserted": False,
                     "current_source_verified": False, "pack_completion_asserted": False, "semantic_grants_lexical_coverage": False}
        self.link = {"path": "src/f0.ts", "line": 15, "kind": "source-reference", "status": "resolved",
                     "target": "payload", "resolved_path": "src/f23.ts"}

    def follow(self, rows=None):
        return follow_source_reference(self.base, self.units, self.paths, self.scores,
                                       {"src/f0.ts": [self.link] if rows is None else rows}, self.project, POLICY)

    def test_exact_physical_link_adds_bound_target_without_inheriting_lexical_claims(self):
        original = copy.deepcopy(self.base)
        result = self.follow()
        self.assertEqual(self.base, original)
        self.assertEqual(result["candidates"][1]["path"], "src/f23.ts")
        self.assertEqual(result["candidates"][1]["source_sha256"], "sha23")
        self.assertNotIn("lexical", result["candidates"][1]["signals"])
        self.assertNotIn("matched_group_indices", result["candidates"][1])
        self.assertEqual(result["source_hop"]["hops"], 1)
        self.assertFalse(result["current_source_verified"])

    def test_links_outside_selected_range_and_non_source_links_do_not_expand(self):
        for change in ({"line": 9}, {"line": 21}, {"kind": "local-markdown"}):
            result = self.follow([{**self.link, **change}])
            self.assertEqual(result["candidates"], self.base["candidates"])
            self.assertEqual(result["source_hop"]["status"], "no-source-link-in-selected-range")

    def test_unresolved_escaping_missing_and_directory_targets_suppress_hop(self):
        for change in ({"status": "unresolved"}, {"resolved_path": "../src/f23.ts"}, {"resolved_path": "/src/f23.ts"},
                       {"resolved_path": "src\\f23.ts"}, {"resolved_path": "src/missing.ts"},
                       {"resolved_path": "src"}, {"resolved_path": "src/./f23.ts"}):
            result = self.follow([{**self.link, **change}])
            self.assertEqual(result["source_hop"]["status"], "unresolved-or-unenrolled-target")
            self.assertEqual(result["candidates"], self.base["candidates"])

    def test_ambiguity_self_reference_and_excess_links_are_explicit(self):
        self.assertEqual(self.follow([self.link, {**self.link, "resolved_path": "src/f22.ts"}])["source_hop"]["status"], "ambiguous-source-targets")
        self.assertEqual(self.follow([{**self.link, "resolved_path": "src/f0.ts"}])["source_hop"]["status"], "self-reference")
        self.assertEqual(self.follow([self.link] * 21)["source_hop"]["status"], "source-link-budget-exceeded")
        self.assertEqual(self.follow([self.link] * 2)["source_hop"]["status"], "selected")

    def test_existing_targets_and_reciprocal_links_are_never_recursively_followed(self):
        self.assertEqual(self.follow([{**self.link, "resolved_path": "src/f4.ts"}])["source_hop"]["status"], "target-already-selected")
        links = {"src/f0.ts": [self.link], "src/f23.ts": [{**self.link, "path": "src/f23.ts", "resolved_path": "src/f0.ts"}]}
        result = follow_source_reference(self.base, self.units, self.paths, self.scores, links, self.project, POLICY)
        self.assertEqual(len(result["candidates"]), 9)
        self.assertEqual(result["source_hop"]["hops"], 1)

    def test_full_distinct_budget_cannot_drop_an_existing_path(self):
        self.base["candidates"] = [candidate(u, 0.5) for u in self.units[:20]]
        result = self.follow()
        self.assertEqual(result["source_hop"]["status"], "distinct-path-budget-full")
        self.assertEqual(result["candidates"], self.base["candidates"])

    def test_only_late_duplicate_range_is_removed_at_full_budget(self):
        self.base["candidates"] = [candidate(u, 0.5) for u in self.units[:19]]
        self.base["candidates"].append({**candidate(self.units[0], 0.4), "start_line": 25, "end_line": 30})
        result = self.follow()
        self.assertEqual(len(result["candidates"]), 20)
        self.assertTrue(result["source_hop"]["duplicate_range_removed"])
        self.assertTrue({c["path"] for c in self.base["candidates"]} <= {c["path"] for c in result["candidates"]})
        self.assertEqual(result["candidates"][0], self.base["candidates"][0])

    def test_best_target_range_uses_scores_with_deterministic_ties(self):
        self.units[22] = {**self.units[23], "start_line": 30, "end_line": 40, "record_ids": ["second"]}
        self.paths["src/f23.ts"] = [23, 22]
        self.assertEqual(self.follow()["candidates"][1]["record_ids"], ["second"])
        self.scores[23] = 0.7
        self.assertEqual(self.follow()["candidates"][1]["record_ids"], ["id23"])

    def test_in_place_resolution_keeps_origin_provenance_and_every_other_primary_row(self):
        self.base["candidates"] = [candidate(u, 0.5) for u in self.units[:20]]
        original = copy.deepcopy(self.base)
        result = follow_source_reference(self.base, self.units, self.paths, self.scores,
            {"src/f0.ts": [self.link]}, self.project, {**POLICY, "placement_mode": "resolve-origin"})
        self.assertEqual(len(result["candidates"]), 20)
        self.assertEqual(result["candidates"][0]["path"], "src/f23.ts")
        self.assertEqual(result["candidates"][1:], self.base["candidates"][1:])
        self.assertEqual(result["candidates"][0]["signals"]["source_reference"]["origin"], self.base["candidates"][0])
        self.assertEqual(result["source_hop"]["status"], "resolved-in-place")
        self.assertNotIn("src/f0.ts", [r["path"] for r in result["candidates"]])
        self.assertNotIn("lexical", result["candidates"][0]["signals"])
        self.assertEqual(self.base, original)

    def test_resolution_does_not_bypass_ambiguity_exact_scope_or_recursion_limits(self):
        policy = {**POLICY, "placement_mode": "resolve-origin"}
        links = {"src/f0.ts": [self.link, {**self.link, "resolved_path": "src/f22.ts"}]}
        result = follow_source_reference(self.base, self.units, self.paths, self.scores, links, self.project, policy)
        self.assertEqual(result["candidates"], self.base["candidates"])
        self.base["code_anchor"]["status"] = "not-applicable"
        result = follow_source_reference(self.base, self.units, self.paths, self.scores,
            {"src/f0.ts": [self.link]}, self.project, policy)
        self.assertEqual(result["candidates"], self.base["candidates"])
        with self.assertRaises(ValueError):
            follow_source_reference(self.base, self.units, self.paths, self.scores, {}, self.project,
                                    {**policy, "placement_mode": "unbounded"})

    def test_foreign_source_hash_child_and_nonfinite_score_are_rejected(self):
        for change in ({"project": "other"}, {"source_sha256": "changed"}, {"unit_kind": "semantic-subrange"}):
            original = self.units[23]
            self.units[23] = {**original, **change}
            with self.assertRaises(ValueError):
                self.follow()
            self.units[23] = original
        self.scores[23] = float("nan")
        with self.assertRaises(ValueError):
            self.follow()

    def test_exact_owner_unavailable_and_negative_without_anchor_are_unchanged(self):
        for mode in ("confirmed-identifier-lexical", "confirmed-owner-lexical", "lexical-only", "semantic-document-first"):
            self.base.update({"mode": mode, "code_anchor": {"status": "not-applicable"}})
            result = self.follow()
            self.assertEqual(result["candidates"], self.base["candidates"])
            self.assertEqual(result["source_hop"]["status"], "not-applicable")

    def test_published_links_verify_bytes_and_lf_delimited_unicode_data(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            data = (json.dumps({**self.link, "text": "data\u2028still-one-record"}, ensure_ascii=False) + "\n").encode()
            (root / "links.jsonl").write_bytes(data)
            manifest = {"generated_file_integrity": {"algorithm": "fnv1a64", "files": [{
                "path": "docs/kb/generated/links.jsonl", "bytes": len(data), "content_hash": publication_fingerprint(data)}]}}
            (root / "manifest.json").write_text(json.dumps(manifest))
            sha = hashlib.sha256((root / "manifest.json").read_bytes()).hexdigest()
            links, receipt = read_bound_links(root, sha)
            self.assertEqual(len(links["src/f0.ts"]), 1)
            self.assertEqual(receipt["bytes"], len(data))
            with self.assertRaises(ValueError):
                read_bound_links(root, "wrong")
            with self.assertRaises(ValueError):
                read_bound_links(root, sha, len(data) - 1)
            (root / "links.jsonl").write_bytes(data.replace(b"payload", b"PAYLOAD"))
            with self.assertRaisesRegex(ValueError, "links changed"):
                read_bound_links(root, sha)


if __name__ == "__main__":
    unittest.main()
