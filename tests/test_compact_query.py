#!/usr/bin/env python3
"""Lossless transport and conservative range labels, not another selector."""

from copy import deepcopy
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
if "shin_semantic" not in sys.modules:
    spec = importlib.util.spec_from_file_location("compact_entry", ROOT / "tools/shin_semantic.py")
    entry = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(entry)
from shin_semantic.artifact import Rejected, encode
from shin_semantic.compact import MAX_BYTES, from_adapter, from_semantic, group_native, group_navigation
from shin_semantic.compact import condense_query, expand_query, number_read, expand_read


class ReadProjectionTests(unittest.TestCase):
    def fixture(self, text="one\r\ntwo\r\n", start=110, end=111):
        return {"schema_version": "shin.agent-read.v1", "ok": True,
                "native": {"snapshot_id": "fnv1a64:0123456789abcdef", "complete": True,
                           "evidence": {"bodies": [{"path": "src/code.cpp", "state": "complete",
                               "start_line": start, "end_line": end, "text": text,
                               "source_range_bytes": len(text.encode())}], "omissions": []}}}

    def test_numbers_are_lossless_without_duplicate_raw_text(self):
        raw = self.fixture()
        before = deepcopy(raw)
        numbered = number_read(raw)
        body = numbered["native"]["evidence"]["bodies"][0]
        self.assertEqual(body["numbered_text"], "110\tone\r\n111\ttwo\r\n")
        self.assertNotIn("text", body)
        self.assertEqual(expand_read(numbered), raw)
        self.assertEqual(raw, before)

    def test_physical_lf_lines_empty_files_and_literal_prefixes(self):
        for text, count in (("", 1), ("\n", 1), ("\n\n", 2), ("a", 1), ("a\nb", 2),
                            ("a\r\v\u2028b\n", 1), ("110\tx\n111\ty", 2), ("\u042f\r\n", 1)):
            with self.subTest(text=text):
                raw = self.fixture(text, 110, 109 + count)
                self.assertEqual(expand_read(number_read(raw)), raw)

    def test_redaction_and_omissions_are_not_new_evidence(self):
        raw = self.fixture()
        raw["native"].update(complete=False)
        raw["native"]["evidence"]["bodies"][0].update(state="redacted", text=None)
        raw["native"]["evidence"]["omissions"] = [{"path": "src/other.cpp", "reason": "body-budget"}]
        self.assertEqual(expand_read(number_read(raw)), raw)
        self.assertNotIn("numbered_text", number_read(raw)["native"]["evidence"]["bodies"][0])
        raw["native"]["evidence"]["bodies"][0]["text"] = "should not exist"
        with self.assertRaises(Rejected):
            number_read(raw)

    def test_bad_ranges_labels_and_mixed_text_fail_closed(self):
        for end in (True, 110, 112):
            with self.assertRaises(Rejected):
                number_read(self.fixture(end=end))
        for replacement in ("109\tone\r\n111\ttwo\r\n", "110\tone\r\n110\ttwo\r\n", "one\r\ntwo\r\n"):
            numbered = number_read(self.fixture())
            numbered["native"]["evidence"]["bodies"][0]["numbered_text"] = replacement
            with self.assertRaises(Rejected):
                expand_read(numbered)
        numbered = number_read(self.fixture())
        numbered["native"]["evidence"]["bodies"][0]["text"] = "different"
        with self.assertRaises(Rejected):
            expand_read(numbered)

    def test_numbering_cost_is_bounded_without_silent_truncation(self):
        raw = self.fixture("x" * (MAX_BYTES - 350), 1, 1)
        raw["padding"] = ""
        raw["padding"] = "p" * (MAX_BYTES - len(encode(raw)) - 1)
        self.assertEqual(len(encode(raw)) + 1, MAX_BYTES)
        with self.assertRaisesRegex(Rejected, "agent-read-output-budget"):
            number_read(raw)

    def test_legacy_audit_is_unchanged_and_unknown_format_is_refused(self):
        raw = self.fixture()
        self.assertEqual(expand_read(raw), raw)
        numbered = number_read(raw)
        numbered["body_text_format"] = "other"
        with self.assertRaises(Rejected):
            expand_read(numbered)


def core_fixture():
    path = "docs/decision.md"
    common = {"path": path, "status": "accepted", "authority_level": "accepted",
              "evidence_kind": "decision-record", "verification_level": "declared", "matched_group_indices": [0]}
    section = {**common, "rank": 1, "type": "section", "line": 12, "start_line": 12, "end_line": 44,
               "heading": "Decision", "excerpt": "The actual rule is later in this section."}
    typed = {**common, "rank": 2, "type": "contract", "record_type": "contract", "id": "contract",
             "line": 1, "start_line": 1, "end_line": 1, "title": "Decision", "excerpt": "Status: accepted", "source": path}
    decision = {**typed, "rank": 3, "type": "decision", "record_type": "decision", "id": "decision"}
    source = {**common, "path": "src/Test.java", "rank": 4, "type": "fact", "record_type": "java-method", "id": "source",
              "line": 21, "start_line": 20, "end_line": 35, "title": "someTest", "excerpt": "test declaration",
              "source": "source-fact:java", "range_origin": "syntax", "evidence_kind": "test-source",
              "verification_level": "source-observed", "authority_level": "source-fact"}
    return {"schema_version": "shin.context-brief-compact.v1", "tool": "shin-context", "command": "brief", "ok": True,
            "projection": "compact", "index_mode": "generated-index", "profile": "fixture", "snapshot_id": "fnv1a64:0123456789abcdef",
            "index_freshness": {"current": True, "fully_verified": True, "status": "current", "snapshot_id": "fnv1a64:0123456789abcdef"},
            "query": "why is this hidden", "retrieval_mode": "relaxed", "ranking_policy": "global-evidence-order-unique-path-v1",
            "query_coverage": {"covered_groups": 1}, "evidence_budget": {"max_records": 20, "returned_records": 4},
            "ranked_paths": [{"path": path, "unique_path_rank": 1, "first_evidence_rank": 1},
                             {"path": "src/Test.java", "unique_path_rank": 2, "first_evidence_rank": 4}],
            "evidence": [section, typed, decision, source], "verification_paths": [
                {"path": path, "evidence_ids": ["section:decision:12:44:section", "contract", "decision"],
                 "ranges": [{"start_line": 1, "end_line": 1}, {"start_line": 12, "end_line": 44}]},
                {"path": "src/Test.java", "evidence_ids": ["source"], "ranges": [{"start_line": 20, "end_line": 35}]}]}


def semantic_fixture():
    core = core_fixture()
    origin = {"project": "fixture", "path": "src/Test.java", "start_line": 20, "end_line": 35, "source_sha256": "a" * 64,
              "record_ids": ["source"], "channel": "semantic-navigation", "status": "navigation-candidate-not-an-answer",
              "current_source_verified": False, "signals": {"lexical": {"rank": 1, "evidence": [deepcopy(core["evidence"][3])]}}}
    target = {"project": "fixture", "path": "src/Target.java", "start_line": 4, "end_line": 4, "source_sha256": "b" * 64,
              "record_ids": ["target"], "channel": "resolved-source-reference-navigation", "similarity": 0.7,
              "status": "navigation-candidate-not-an-answer", "current_source_verified": False,
              "signals": {"source_reference": {"path": origin["path"], "line": 22, "origin": origin}}}
    navigation = {"mode": "code-anchor-resolved-source-reference", "candidates": [target],
                  "semantic_grants_lexical_coverage": False, "symbol_existence_asserted": False,
                  "current_source_verified": False, "pack_completion_asserted": False,
                  "source_hop": {"status": "resolved-in-place", "hops": 1}}
    return {"schema_version": "shin.semantic-query.v1", "ok": True, "core": core, "navigation": navigation,
            "semantic_status": "available", "source_admission": "current-at-core-admission-not-a-future-mutation-guarantee",
            "project": "fixture", "artifact_sha256": "c" * 64}


def restore_native(projected):
    native = deepcopy(projected["native"])
    native["schema_version"] = native.pop("source_schema")
    native["projection"] = "compact"
    native["evidence"], native["verification_paths"] = [], []
    references = {}
    for file in native.pop("files"):
        ids, ranges = [], set()
        for row in file["records"]:
            ids.append(row.pop("evidence_id"))
            location = row.pop("range")
            row.update(path=file["path"], start_line=location["start_line"], end_line=location["end_line"],
                       excerpt=file["excerpts"][row.pop("excerpt_ref")])
            ranges.add((row["start_line"], row["end_line"]))
            reference = row.pop("ref")
            references[reference] = deepcopy(row)
            native["evidence"].append(row)
        native["verification_paths"].append({"path": file["path"], "evidence_ids": ids,
            "ranges": [{"start_line": start, "end_line": end} for start, end in sorted(ranges)]})
    native["evidence"].sort(key=lambda row: row["rank"])
    return native, references


def restore_navigation(projected, references):
    navigation = deepcopy(projected["navigation"])
    if navigation is None:
        return None

    def unpack(row):
        row.pop("range_kind")
        signals = row.get("signals", {})
        if "lexical" in signals:
            lexical = signals["lexical"]
            lexical["evidence"] = [references[ref] for ref in lexical.pop("evidence_refs")]
        if "source_reference" in signals:
            unpack(signals["source_reference"]["origin"])

    for row in navigation["candidates"]:
        unpack(row)
    return navigation


def assert_lossless(original):
    projected = from_semantic(original)
    core, references = restore_native(projected)
    if core != original["core"] or restore_navigation(projected, references) != original["navigation"]:
        raise AssertionError("Grouped transport changed native evidence or semantic navigation")
    return projected


def assert_transport_lossless(core, navigation):
    # Historical controls prove formatting only; their unassessed freshness
    # must never be relabeled as successful live admission.
    native, references = group_native(core)
    result = {"native": native, "navigation": group_navigation(navigation, references)}
    restored, refs = restore_native(result)
    if restored != core or restore_navigation(result, refs) != navigation:
        raise AssertionError("Grouped transport changed the historical control")
    return result


class CompactTests(unittest.TestCase):
    def test_shared_metadata_roundtrip_and_no_input_mutation(self):
        original = from_semantic(semantic_fixture())
        before = deepcopy(original)
        compact = condense_query(original)
        self.assertEqual(original, before)
        self.assertEqual(expand_query(compact), original)
        self.assertEqual(restore_native(expand_query(compact))[0], semantic_fixture()["core"])
        self.assertEqual(compact["schema_version"], "shin.agent-query.v2")
        self.assertNotIn("evidence_id", compact["native"]["files"][1]["records"][0])

    def test_shared_defaults_preserve_distinct_types_and_empty_navigation(self):
        original = from_semantic(semantic_fixture())
        for navigation in (None, {**original["navigation"], "candidates": []}):
            original["navigation"] = navigation
            self.assertEqual(expand_query(condense_query(original)), original)
        original["native"]["files"] = []
        self.assertEqual(expand_query(condense_query(original)), original)

    def test_conflicting_defaults_refuse_instead_of_overwriting(self):
        compact = condense_query(from_semantic(semantic_fixture()))
        compact["native"]["record_defaults"]["rank"] = 1
        with self.assertRaisesRegex(Rejected, "invalid-agent-defaults"):
            expand_query(compact)

    def test_many_source_rows_compact_without_loss(self):
        original = from_semantic(semantic_fixture())
        row = original["native"]["files"][1]["records"][0]
        original["native"]["files"] = [{"path": "src/Test.java", "excerpts": ["source"],
            "records": [{**deepcopy(row), "id": "source-" + str(i), "evidence_id": "source-" + str(i), "rank": i + 1}
                        for i in range(20)]}]
        compact = condense_query(original)
        self.assertLess(len(encode(compact)), len(encode(original)) * 0.8)
        self.assertEqual(expand_query(compact), original)

    def test_native_and_nested_navigation_roundtrip_without_mutation(self):
        original = semantic_fixture()
        before = deepcopy(original)
        result = assert_lossless(original)
        self.assertEqual(original, before)
        self.assertFalse(result["interpretation"]["navigation_is_evidence"])
        self.assertIsNone(result["admission"]["ready"])
        target = result["navigation"]["candidates"][0]
        self.assertEqual(target["range_kind"], "unclassified-navigation-range")
        origin = target["signals"]["source_reference"]["origin"]
        self.assertEqual(origin["signals"]["lexical"]["evidence_refs"], ["n4"])

    def test_anchor_is_not_a_summary_span_or_execution_proof(self):
        result = assert_lossless(semantic_fixture())
        files = result["native"]["files"]
        self.assertEqual(len(files), 2)
        self.assertEqual(len(files[0]["records"]), 3)
        self.assertEqual(len(files[0]["excerpts"]), 2)
        self.assertEqual([row["range"]["kind"] for row in files[0]["records"]], ["section-span", "document-anchor", "document-anchor"])
        self.assertEqual(files[0]["records"][1]["range"]["read_scope"], "document")
        source = files[1]["records"][0]
        self.assertEqual(source["range"]["kind"], "source-span")
        self.assertEqual(source["range_origin"], "syntax")
        self.assertEqual(source["verification_level"], "source-observed")

    def test_unknown_fact_keeps_coordinates_but_requires_document_read(self):
        original = semantic_fixture()
        original["core"]["evidence"][1]["type"] = "fact"
        result = assert_lossless(original)
        self.assertEqual(result["native"]["files"][0]["records"][1]["range"]["kind"], "unclassified-range")

    def test_redacted_preview_is_preserved_without_reconstruction(self):
        original = semantic_fixture()
        original["core"]["evidence"][1]["excerpt"] = "[REDACTED:credential-assignment]"
        result = assert_lossless(original)
        self.assertIn("[REDACTED:credential-assignment]", result["native"]["files"][0]["excerpts"])

    def test_missing_optional_semantics_keeps_all_native_records(self):
        original = semantic_fixture()
        original.update(navigation=None, semantic_status="model-unavailable-lexical-only")
        self.assertIsNone(assert_lossless(original)["navigation"])

    def test_same_excerpt_does_not_merge_different_authority_or_lifecycle(self):
        original = semantic_fixture()
        original["core"]["evidence"][2].update(status="superseded", authority_level="history")
        result = assert_lossless(original)
        rows = result["native"]["files"][0]["records"]
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows[1]["excerpt_ref"], rows[2]["excerpt_ref"])
        self.assertNotEqual(rows[1]["authority_level"], rows[2]["authority_level"])
        self.assertNotEqual(rows[1]["evidence_id"], rows[2]["evidence_id"])

    def test_global_order_survives_interleaved_file_groups(self):
        original = semantic_fixture()
        original.update(navigation=None)
        core = original["core"]
        core["evidence"] = [core["evidence"][0], core["evidence"][3], core["evidence"][1], core["evidence"][2]]
        for rank, row in enumerate(core["evidence"], 1):
            row["rank"] = rank
        core["ranked_paths"][1]["first_evidence_rank"] = 2
        result = assert_lossless(original)
        self.assertEqual([row["rank"] for row in result["native"]["files"][0]["records"]], [1, 3, 4])

    def test_bad_range_ids_paths_ranks_or_stale_data_rejected(self):
        for change in (lambda c: c["evidence"][0].update(start_line=0),
                       lambda c: c["evidence"][0].update(rank=2),
                       lambda c: c["evidence"][0].update(path="../outside"),
                       lambda c: c["verification_paths"][0]["evidence_ids"].pop(),
                       lambda c: c["index_freshness"].update(current=False),
                       lambda c: c["evidence"][0].update(excerpt_ref=0)):
            original = semantic_fixture()
            change(original["core"])
            with self.assertRaises(Rejected):
                from_semantic(original)

    def test_navigation_cannot_invent_evidence_or_escalate_trust(self):
        original = semantic_fixture()
        original["navigation"]["candidates"][0]["signals"]["source_reference"]["origin"]["signals"]["lexical"]["evidence"][0]["excerpt"] = "forged"
        with self.assertRaises(Rejected):
            from_semantic(original)
        for field in ("semantic_grants_lexical_coverage", "symbol_existence_asserted", "current_source_verified", "pack_completion_asserted"):
            original = semantic_fixture()
            original["navigation"][field] = True
            with self.assertRaises(Rejected):
                from_semantic(original)

    def test_bounded_response_rejects_instead_of_silently_losing_records(self):
        original = semantic_fixture()
        original["core"]["evidence"][0]["excerpt"] = "x" * MAX_BYTES
        with self.assertRaisesRegex(Rejected, "agent-query-output-budget"):
            from_semantic(original)

    def test_adapter_requires_matching_strict_readiness(self):
        core = core_fixture()
        route = {"profile": "fixture", "root": "/root", "out_dir": "/index", "marker": "docs/adoption.json"}
        ready = {"schema_version": "shin.agent-readiness.v1", "ok": True, "ready": True, "strict": True,
                 "profile": "fixture", "root": "/root", "out_root": "/index", "marker": "docs/adoption.json"}
        original = {"schema_version": "shin.hermes-result.v1", "operation": "brief", "ok": True,
                    "result": core, "readiness": ready, "route": route}
        self.assertEqual(restore_native(from_adapter(original))[0], core)
        self.assertFalse(from_adapter(original)["admission"]["readiness_and_query_atomic"])
        for key, value in (("strict", False), ("ready", False), ("profile", "other"), ("root", "/other"), ("out_root", "/other")):
            candidate = deepcopy(original)
            candidate["readiness"][key] = value
            with self.assertRaises(Rejected):
                from_adapter(candidate)


if __name__ == "__main__":
    unittest.main()
