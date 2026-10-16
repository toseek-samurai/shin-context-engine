"""Hermetic differential coverage for indexed and source-scan context retrieval."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = str(Path(sys.argv.pop(1)).resolve())
CAPTURED_IDENTIFIER = "Captured" + ("Segment" * 17) + "X"


class QueryParityTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-query-parity-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "root"
        self.out = self.base / "index"
        self.profiles = self.base / "profiles"
        self.profiles.mkdir()
        (self.root / "docs" / "paths").mkdir(parents=True)
        (self.root / "docs" / "config").mkdir()
        (self.root / "src").mkdir()
        (self.root / "README.md").write_text("# Query parity fixture\n", encoding="utf-8")
        (self.profiles / "query-parity.json").write_text(json.dumps({
            "schemaVersion": "shin.profile.v1",
            "id": "query-parity",
            "sourceLabel": "Neutral query parity fixture",
            "rootFiles": ["README.md"],
            "sourceExtensions": [".md", ".json"],
            "sourceFactRoots": ["src"],
            "sourceFactExtensions": [".ts"],
            "sourceFactExtractors": ["language"],
            "sourceFactMaxFiles": 16,
            "sourceFactMaxFileBytes": 131072,
            "agentStartPaths": ["README.md"],
            "markdownMaxHeadingLevel": 4,
            "jsonKeySections": True,
            "jsonKeyIncludePrefixes": ["docs/config/"],
            "jsonKeyMaxDepth": 4,
            "jsonKeyMaxSectionsPerFile": 32,
            "defaultTopic": "fixture",
            "evalCases": [],
            "topicRules": [],
        }), encoding="utf-8")
        self.write_corpus()
        self.invoke("build")

    def write_corpus(self):
        (self.root / "docs" / "operations.md").write_text(
            "# Release Operations 0017\n\n"
            "## HTTP 401 Recovery\n\n"
            "HTTP 401 recovery follows the RetryWindow policy. "
            "Repeated repeated references stay stable.\n\n"
            "## OpenAPI Query Catalog\n\n"
            "OpenAPIQueryCatalog maps /api/v1/controls punctuation-boundary routes.\n\n"
            "## Customer SideBar\n\n"
            "CustomerSideBar navigation uses the operation matrix.\n",
            encoding="utf-8",
        )
        (self.root / "docs" / "paths" / "Route_0017.md").write_text(
            "# Route 0017\n\n"
            "The route_0017 request-id points to /api/v1/controls.\n",
            encoding="utf-8",
        )
        (self.root / "docs" / "config" / "runtime-policy.json").write_text(json.dumps({
            "serviceCatalog": {
                "HTTPGatewayV2": {
                    "route": "/api/v2/widgets/0017",
                    "statusCodes": [401, 403],
                    "retry_window": "retry window",
                    "bindingPhrase": "resource binding contract",
                }
            },
            "operationMatrix": {
                "CustomerSideBar": "current navigation",
                "requestId": "0017",
            },
        }, indent=2) + "\n", encoding="utf-8")
        (self.root / "src" / "RequestPolicy.ts").write_text(
            "export class HTTPGatewayV2 {\n"
            "  resolvePolicy_0017(httpStatus: number, retry_window: number) {\n"
            "    return httpStatus === 401 ? retry_window : 0;\n"
            "  }\n"
            "}\n\n"
            "export function OpenAPIQueryCatalog(request_id: string) {\n"
            "  return `/api/v1/controls/${request_id}`;\n"
            "}\n\n"
            "export function CustomerSideBarView(operation_matrix: string) {\n"
            "  return operation_matrix;\n"
            "}\n\n"
            f"export function {CAPTURED_IDENTIFIER}(request_id: string) {{\n"
            "  return request_id;\n"
            "}\n",
            encoding="utf-8",
        )

    def invoke(self, *args, expected=0):
        result = subprocess.run([
            TOOL, "--profile", "query-parity", "--profile-dir", str(self.profiles),
            "--root", str(self.root), "--out-dir", str(self.out), *args,
        ], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, expected, result.stderr or result.stdout)
        return json.loads(result.stdout or result.stderr)

    def context(self, query, source_scan=False):
        prefix = ["--source-scan"] if source_scan else []
        return self.invoke(*prefix, "context", query, "--require-current", "--limit", "100", "--per-doc", "8")

    @staticmethod
    def evidence_projection(result, key):
        def stable_id(row):
            if key == "facts":
                return (
                    row["id"], row["path"], row["record_type"],
                    row["evidence_kind"], row["verification_level"],
                    row["start_line"], row["end_line"],
                )
            return (
                row["path"], row["row_type"], row["heading"],
                row["start_line"], row["end_line"], row["chunk_index"],
            )

        return [
            (stable_id(row), row["score"], tuple(row["matched_group_indices"]))
            for row in result[key]
        ]

    def assert_query_parity(self, query):
        indexed = self.context(query)
        scanned = self.context(query, source_scan=True)
        self.assertEqual(
            self.evidence_projection(indexed, "sections"),
            self.evidence_projection(scanned, "sections"),
            f"section parity failed for {query!r}",
        )
        self.assertEqual(
            self.evidence_projection(indexed, "facts"),
            self.evidence_projection(scanned, "facts"),
            f"fact parity failed for {query!r}",
        )
        self.assertEqual(indexed["matched_sections"], scanned["matched_sections"])
        self.assertEqual(indexed["matched_facts"], scanned["matched_facts"])
        return indexed

    def test_indexed_context_matches_source_scan_for_lexical_corpus(self):
        queries = (
            "HTTP 401 recovery", "retry window", "RetryWindow", "retry_window",
            "retry-window", "OpenAPI query catalog", "OpenAPIQueryCatalog",
            "Customer SideBar", "customer_sidebar", "customer-sidebar", "release 0017",
            "0017", "401", "403", "route 0017", "Route_0017",
            "docs/paths/Route_0017.md", "service catalog", "HTTP Gateway V2",
            "HTTPGatewayV2", "gateway_v2", "resolve policy", "resolvePolicy0017",
            "resolve_policy_0017", "policy 0017", "resource binding",
            "resource-binding", "stable repeated", "repeated repeated",
            "punctuation boundary", "api v1 controls", "api/v1/controls", "v1 controls",
            "request id 0017", "request-id", "docs/config/runtime-policy.json",
            "operation matrix", "matrix 401", "lease release", "ateway HTTP",
            "001 0017", "40 401", "ide sidebar", "unrelated opaque",
            "gateway opaque", "window opaque", "catalog opaque", "route opaque",
            CAPTURED_IDENTIFIER,
        )
        self.assertEqual(len(queries), 49)
        for query in queries:
            with self.subTest(query=query):
                self.assert_query_parity(query)

    def test_positive_controls_return_expected_evidence(self):
        controls = {
            "HTTP 401 recovery": "docs/operations.md",
            "docs/paths/Route_0017.md": "docs/paths/Route_0017.md",
            "service catalog": "docs/config/runtime-policy.json",
            "resolve policy 0017": "src/RequestPolicy.ts",
            "Customer SideBar": "src/RequestPolicy.ts",
            CAPTURED_IDENTIFIER: "src/RequestPolicy.ts",
        }
        for query, expected_path in controls.items():
            with self.subTest(query=query):
                result = self.assert_query_parity(query)
                returned_paths = {row["path"] for row in result["sections"]}
                returned_paths.update(row["path"] for row in result["facts"])
                self.assertIn(expected_path, returned_paths)

    def test_128_byte_captured_identifier_is_exact_source_evidence(self):
        self.assertEqual(len(CAPTURED_IDENTIFIER.encode("ascii")), 128)
        result = self.assert_query_parity(CAPTURED_IDENTIFIER)
        rows = [row for row in result["facts"] if row["path"] == "src/RequestPolicy.ts"]
        self.assertTrue(rows)
        self.assertTrue(all(row["matched_group_indices"] == [0] for row in rows))

    def test_long_structural_tag_does_not_publish_an_invalid_identifier(self):
        profile_path = self.profiles / "query-parity.json"
        profile = json.loads(profile_path.read_text())
        profile.update(sourceFactRoots=["apps"], sourceFactExtensions=[".rsx"],
                       sourceFactExtractors=["retool"])
        profile_path.write_text(json.dumps(profile))
        app = self.root / "apps" / "Fixture"
        app.mkdir(parents=True)
        tag = "Q" * 129
        (app / "main.rsx").write_text(
            f'<{tag} id="LoadWidget" resourceName="FixtureApi" />\n')
        self.invoke("build")
        result = self.assert_query_parity("FixtureApi resource binding")
        self.assertTrue(any(row["record_type"] == "retool-resource-binding"
                            for row in result["facts"]))
        facts = [json.loads(line) for line in
                 (self.out / "docs/kb/generated/facts.jsonl").read_text().splitlines()]
        self.assertTrue(all(len(term) <= 128 for fact in facts
                            for term in fact["identifier_terms"]))

    def test_internal_substring_controls_do_not_admit_rows(self):
        for query in ("lease release", "ateway HTTP", "001 0017", "40 401", "ide sidebar"):
            with self.subTest(query=query):
                result = self.assert_query_parity(query)
                self.assertEqual(result["sections"], [])
                self.assertEqual(result["facts"], [])

    def published_sections(self, path):
        return [row for row in map(json.loads,
                (self.out / "docs/kb/generated/sections.jsonl").read_text(encoding="utf-8").splitlines())
                if row["path"] == path]

    def test_markdown_body_beyond_preview_is_searchable_without_new_rows(self):
        body = "ordinary filler " * 100 + "LatePublishedNeedle owns the retained evidence."
        (self.root / "docs" / "late.md").write_text("# Bounded section\n\n" + body + "\n")
        self.invoke("build")
        rows = self.published_sections("docs/late.md")
        self.assertEqual(len(rows), 1)
        self.assertNotIn("LatePublishedNeedle", rows[0]["preview"])
        result = self.assert_query_parity("LatePublishedNeedle")
        self.assertEqual([row["path"] for row in result["sections"]], ["docs/late.md"])
        self.assertEqual(rows[0]["indexed_text"], body)
        self.assertEqual(rows[0]["indexed_text_omitted_bytes"], 0)
        self.assertEqual(result["sections"][0]["indexed_text"], body)
        self.assertEqual(result["sections"][0]["matched_group_indices"], [0])
        self.assertEqual(self.context("LatePublishedNeedle")["sections"],
                         self.context("LatePublishedNeedle", source_scan=True)["sections"])

    def test_markdown_indexed_text_caps_are_explicit_and_utf8_safe(self):
        body = "\u0442\u0435\u043a\u0441\u0442 abc " * 2000 + "BeyondPublicationCap"
        (self.root / "docs" / "capped.md").write_text(
            "".join(f"## Section {i}\n{body}\n" for i in range(12)), encoding="utf-8")
        self.invoke("build")
        rows = self.published_sections("docs/capped.md")
        self.assertTrue(rows)
        self.assertLessEqual(sum(len(row["indexed_text"].encode()) for row in rows), 131072)
        self.assertTrue(any(not row["indexed_text"] for row in rows))
        for row in rows:
            kept = len(row["indexed_text"].encode())
            self.assertLessEqual(kept, 16384)
            self.assertEqual(kept + row["indexed_text_omitted_bytes"], len(body.encode()))
            self.assertNotIn("BeyondPublicationCap", row["indexed_text"])
        result = self.assert_query_parity("BeyondPublicationCap")
        self.assertEqual(result["sections"], [])

    def test_markdown_indexed_text_redacts_before_clipping(self):
        secret = "sk-" + "Q7x9" * 16
        body = "ordinary " * 1819 + secret + " OmittedSecretTail"
        (self.root / "docs" / "sensitive.md").write_text("# Sensitive fixture\n" + body + "\n")
        self.invoke("build")
        rows = self.published_sections("docs/sensitive.md")
        self.assertEqual(rows, [], "quarantine excludes the complete document before clipping")
        self.assertEqual(self.assert_query_parity("OmittedSecretTail")["sections"], [])
        for path in (self.out / "docs/kb/generated").iterdir():
            if path.is_file():
                self.assertNotIn(secret.encode(), path.read_bytes(), path.name)

    def test_pack_keeps_search_text_out_of_budget_but_in_exact_identity(self):
        body = "ordinary filler " * 500 + "LatePublishedNeedle tailalpha"
        path = self.root / "docs" / "pack-body.md"
        path.write_text("# Pack body\n" + body + "\n")
        request = self.base / "request.json"
        request.write_text(json.dumps({
            "schema_version": "shin.task-request.v1", "request_id": "body-publication",
            "task": {"objective": "Find the late source evidence", "must": [
                {"id": "late", "query": "LatePublishedNeedle", "require_full_coverage": True},
                {"id": "same", "query": "LatePublishedNeedle", "require_full_coverage": True}]},
            "evidence_types": ["section"],
            "budget": {"max_evidence_bytes": 4000, "max_records": 1,
                       "per_query_records": 1, "per_document_records": 1},
        }))
        self.invoke("build")
        pack = self.invoke("pack", "--input", str(request), "--strict")
        self.assertTrue(pack["completion"]["complete"])
        self.assertEqual(len(pack["evidence"]), 1)
        evidence = pack["evidence"][0]
        self.assertEqual(len(evidence["matches"]), 2)
        self.assertGreater(evidence["record"]["indexed_text_bytes"], 4000)
        self.assertNotIn("indexed_text", evidence["record"])
        self.assertNotIn("LatePublishedNeedle", evidence["record"]["preview"])
        self.assertLessEqual(len(json.dumps(pack["evidence"], separators=(",", ":")).encode()), 4000)
        path.write_text("# Pack body\n" + body.replace("tailalpha", "tailomega") + "\n")
        self.invoke("build")
        changed = self.invoke("pack", "--input", str(request), "--strict")
        self.assertEqual(evidence["record"], changed["evidence"][0]["record"])
        self.assertNotEqual(evidence["evidence_id"], changed["evidence"][0]["evidence_id"])

    def test_late_credential_assignment_is_redacted_before_line_joining(self):
        secret = "K7z9-" * 8
        (self.root / "docs" / "assigned.md").write_text(
            "# Assignment fixture\n" + "ordinary filler " * 100 +
            "\npassword = " + secret + "\nSafeFollowingNeedle\n")
        self.invoke("build")
        rows = self.published_sections("docs/assigned.md")
        self.assertEqual(rows, [], "quarantine includes the late credential assignment")
        result = self.assert_query_parity("SafeFollowingNeedle")
        self.assertEqual(result["sections"], [], "safe-looking fragments of a quarantined document are withheld")
        for path in (self.out / "docs/kb/generated").iterdir():
            if path.is_file():
                self.assertNotIn(secret.encode(), path.read_bytes(), path.name)

    def test_preview_redacts_keys_before_summary_section_and_chunk_cutoffs(self):
        secret = "sk-" + "Q7x9" * 16
        for limit in (280, 420, 900):
            with self.subTest(limit=limit):
                body = "p" * (limit - 17) + " " + secret
                (self.root / "docs" / "preview-key.md").write_text(
                    "# Preview boundary\n" + body + "\n")
                profile_path = self.profiles / "query-parity.json"
                profile = json.loads(profile_path.read_text())
                profile.update(markdownChunkLines=10, markdownChunkMinLines=1)
                profile_path.write_text(json.dumps(profile))
                self.invoke("build")
                rows = self.published_sections("docs/preview-key.md")
                self.assertEqual(rows, [], "no sections or chunks from a quarantined document")
                for path in (self.out / "docs/kb/generated").iterdir():
                    if path.is_file():
                        self.assertFalse(secret[:8].encode() in path.read_bytes(), path.name)


    def test_preview_preserves_credential_line_boundary(self):
        secret = "K7z9-" * 8
        (self.root / "docs" / "preview-assignment.md").write_text(
            "# Assignment preview\n" + "p" * 160 + "\npassword = " + secret +
            "\nSafeFollowingNeedle\n")
        self.invoke("build")
        rows = self.published_sections("docs/preview-assignment.md")
        self.assertEqual(rows, [])
        self.assertEqual(self.assert_query_parity("SafeFollowingNeedle")["sections"], [])
        for path in (self.out / "docs/kb/generated").iterdir():
            if path.is_file():
                self.assertFalse(secret.encode() in path.read_bytes(), path.name)

    def test_json_and_jsonl_summaries_redact_before_clipping(self):
        secret = "sk-" + "Q7x9" * 16
        profile_path = self.profiles / "query-parity.json"
        profile = json.loads(profile_path.read_text())
        profile["sourceExtensions"].append(".jsonl")
        profile["jsonlMaxRowsPerFile"] = 10
        profile_path.write_text(json.dumps(profile))
        (self.root / "docs" / "summary-key.json").write_text(json.dumps({
            "summary": "p" * 263 + " " + secret}))
        (self.root / "docs" / "summary-key.jsonl").write_text("\n".join(map(json.dumps, [
            {"id": "direct", "type": "fact", "summary": "p" * 403 + " " + secret},
            {"id": "fallback", "type": "fact", "snippet": "p" * 395 + " " + secret},
        ])) + "\n")
        self.invoke("build")
        facts = [json.loads(line) for line in
                 (self.out / "docs/kb/generated/facts.jsonl").read_text().splitlines()]
        selected = [row for row in facts if row["path"] == "docs/summary-key.jsonl"]
        self.assertEqual(selected, [], "structured facts from quarantined documents are omitted")
        documents = [json.loads(line) for line in
                     (self.out / "docs/kb/generated/documents.jsonl").read_text().splitlines()]
        summary = next(row["summary"] for row in documents if row["path"] == "docs/summary-key.json")
        self.assertEqual(summary, "Source content withheld after secret detection.")
        for path in (self.out / "docs/kb/generated").iterdir():
            if path.is_file():
                self.assertFalse(secret[:8].encode() in path.read_bytes(), path.name)

    def test_source_declaration_summary_redacts_before_clipping(self):
        secret = "sk-" + "Q7x9" * 16
        prefix = 'export function PreviewKey(value = "'
        declaration = prefix + "p" * (404 - len(prefix)) + " " + secret + '") { return value; }'
        (self.root / "src" / "PreviewKey.ts").write_text(declaration + "\n")
        self.invoke("build")
        facts = [json.loads(line) for line in
                 (self.out / "docs/kb/generated/facts.jsonl").read_text().splitlines()]
        selected = [row for row in facts if row["path"] == "src/PreviewKey.ts"]
        self.assertEqual(selected, [], "quarantined source has no searchable declarations")
        for path in (self.out / "docs/kb/generated").iterdir():
            if path.is_file():
                self.assertFalse(secret[:8].encode() in path.read_bytes(), path.name)


if __name__ == "__main__":
    unittest.main()
