#!/usr/bin/env python3
"""Keep search observations from converting stale or leaked paths into success."""

from pathlib import Path
import tempfile
import unittest

from run_carry_forward_comparison import digest
from run_project_search_comparison import metadata_path, observe, source_record, summarize, validate_spec


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "source.cpp").write_text("first\ncurrent body\nlast\n")
        self.case = {"id": "test", "kind": "exact", "expected_paths": ["source.cpp"]}
        self.before = {"source.cpp": digest(self.root / "source.cpp")}

    def response(self, content="current body"):
        return {"exit_code": 0, "wall_ms": 1, "response_bytes": 20, "result": {
            "type": "search_result", "revision": "a-label-not-a-hash", "results": [{"content": content,
            "result": {"sourcePosition": {"relativePath": "source.cpp"}}}]}}

    def test_fragment_matches_current_source_not_revision_label(self):
        row = observe(self.root, self.case, "jbcontext", self.response(), self.before)
        self.assertEqual(row["first_expected_record_rank"], 1)
        self.assertEqual(row["records"][0]["sha256"], self.before["source.cpp"])

    def test_stale_fragment_does_not_count_as_hit(self):
        row = observe(self.root, self.case, "jbcontext", self.response("stale body"), self.before)
        self.assertIsNone(row["first_expected_record_rank"])
        self.assertFalse(row["records"][0]["valid"])

    def test_source_change_during_observation_voids_hit(self):
        (self.root / "source.cpp").write_text("current body\nchanged\n")
        row = observe(self.root, self.case, "jbcontext", self.response(), self.before)
        self.assertFalse(row["expected_sources_unchanged"])
        self.assertIsNone(row["first_expected_record_rank"])

    def test_metadata_is_not_implementation_evidence(self):
        for path in ("profiles/test.json", "docs/machine/shin-profiles/test.json", "tests/evidence/review.md", "docs/kb/generated/facts.jsonl"):
            self.assertTrue(metadata_path(path))
        self.assertFalse(metadata_path("src/test/ProfileServiceTests.java"))
        profile = self.root / "profiles/test.json"
        profile.parent.mkdir()
        profile.write_text("current body")
        response = self.response()
        response["result"]["results"][0]["result"]["sourcePosition"]["relativePath"] = "profiles/test.json"
        case = {**self.case, "expected_paths": ["profiles/test.json"]}
        self.assertIsNone(observe(self.root, case, "jbcontext", response, {})["first_expected_record_rank"])

    def test_coordinates_and_path_containment(self):
        self.assertTrue(source_record(self.root, "source.cpp", 1, 3)["valid"])
        self.assertFalse(source_record(self.root, "source.cpp", 0, 3)["valid"])
        self.assertFalse(source_record(self.root, "source.cpp", 1, 4)["valid"])
        for path in ("../escape", "/etc/hosts"):
            with self.assertRaises(ValueError):
                source_record(self.root, path, 1, 1)
        (self.root / "outside").symlink_to("/etc/hosts")
        with self.assertRaises(ValueError):
            source_record(self.root, "outside", 1, 1)

    def test_failed_and_negative_searches_are_not_semantic_success(self):
        response = self.response()
        response["exit_code"] = 1
        row = observe(self.root, self.case, "jbcontext", response, self.before)
        self.assertFalse(row["successful"])
        self.assertIsNone(row["first_expected_record_rank"])
        negative = {**self.case, "kind": "negative", "expected_paths": []}
        row = observe(self.root, negative, "jbcontext", self.response(), self.before)
        self.assertEqual(row["negative_returned_records"], 1)
        self.assertIsNone(summarize([row])["jbcontext"]["negative"]["expected_path_in_top5"])

    def test_missing_result_array_is_not_an_empty_success(self):
        for payload in ({}, {"type": "search_result"}, {"type": "error", "results": []}):
            response = {**self.response(), "result": payload}
            row = observe(self.root, self.case, "jbcontext", response, self.before)
            self.assertFalse(row["successful"])

    def test_output_and_fixture_paths_cannot_escape_their_owned_scope(self):
        spec = {"schema_version": "shin.project-search-comparison.v1", "limit": 5, "per_document": 2,
                "projects": [{"id": "project", "root": str(self.root), "cases": [self.case]}]}
        output = self.root.parent / "separate-output"
        validate_spec(spec, output)
        with self.assertRaises(ValueError):
            validate_spec(spec, self.root / "index")
        self.case["id"] = "../escape"
        with self.assertRaises(ValueError):
            validate_spec(spec, output)
        self.case["id"] = "test"
        self.case["expected_paths"] = ["../outside"]
        with self.assertRaises(ValueError):
            validate_spec(spec, output)


if __name__ == "__main__":
    unittest.main()
