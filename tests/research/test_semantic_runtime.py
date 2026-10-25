#!/usr/bin/env python3
"""Route/lifecycle tests; vectors use optional NumPy, inference an explicit model."""

import argparse
from copy import deepcopy
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from prepare_semantic_pilot import project_units

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


entry = load("semantic_entry", ROOT / "tools/shin_semantic.py")
fixture_support = load("observer_fixture", ROOT / "observer/tests/integration_test.py")
from shin_semantic.artifact import Artifact, Rejected, decode, encode, publication_id, read, root_identity, sha
from shin_semantic.compact import expand_read
from shin_semantic.model import MODEL
from shin_semantic import runtime

CORE = MODEL_DIR = None


def write(path, data):
    path.write_bytes(data)
    path.chmod(0o600)


def snapshot(path):
    return (fixture_support.tree_metadata(path),
            [(str(p.relative_to(path)), sha(p.read_bytes())) for p in sorted(path.rglob("*")) if p.is_file()])


class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="shin-semantic-query-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        self.fixture = fixture_support.Fixture(self.base, "fixture")
        self.fixture.write_source("# Observer Status\n\nCached workspace observation is not live source verification.\n")
        self.build()
        self.artifact_dir = self.base / "artifact"
        self.model_dir = self.base / "missing-model"
        self.config_path = self.base / "route.json"
        self.config = {"schema_version": "shin.semantic-route.v1", "core_binary": str(CORE),
                       "core_sha256": sha(CORE.read_bytes()), "root": str(self.fixture.root),
                       "root_identity": root_identity(self.fixture.root), "project": "fixture", "profile": "observer-fixture",
                       "profile_path": str(self.fixture.profile_path), "profile_sha256": sha(self.fixture.profile_path.read_bytes()),
                       "out_dir": str(self.fixture.out), "artifact_dir": str(self.artifact_dir),
                       "artifact_sha256": "0" * 64, "model_dir": str(self.model_dir)}
        self.save_config()

    def save_config(self):
        write(self.config_path, encode(self.config))

    def build(self):
        value = subprocess.run([str(CORE), "--root", str(self.fixture.root), "--profile", "observer-fixture",
                                "--profile-dir", str(self.fixture.profiles), "--out-dir", str(self.fixture.out), "build"],
                               capture_output=True, timeout=10)
        self.assertEqual(value.returncode, 0, value.stderr)

    def cli(self, query="cached workspace observation"):
        value = subprocess.run([sys.executable, str(ROOT / "tools/shin_semantic.py"), "--config", str(self.config_path), query],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        self.assertEqual(value.stderr, b"")
        self.assertLessEqual(len(value.stdout), runtime.MAX_OUTPUT + 1)
        return value.returncode, decode(value.stdout)


class PackTests(FixtureTests):
    def setUp(self):
        super().setUp()
        profile = decode(self.fixture.profile_path.read_bytes())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".cpp"])
        write(self.fixture.profile_path, encode(profile))
        (self.fixture.root / "src").mkdir()
        self.source = self.fixture.root / "src/pipeline.cpp"
        self.source.write_text("".join(f"int {name}() {{\n    return {i};\n}}\n" + "\n" * 27
                                      for i, name in enumerate(("acquire_pending", "confirm_delivery", "record_failure"), 1)))
        self.build()
        self.config["project"] = "logical-project-not-profile"
        self.config["profile_sha256"] = sha(self.fixture.profile_path.read_bytes())
        self.save_config()
        self.task = {"schema_version": "shin.task-request.v1", "request_id": "source-collection",
            "task": {"objective": "Collect the three source operations", "scope": [], "should": [],
                     "must": [{"id": name, "query": "src/pipeline.cpp " + name, "require_full_coverage": True}
                              for name in ("acquire_pending", "confirm_delivery", "record_failure")]},
            "evidence_types": ["fact"],
            "budget": {"max_evidence_bytes": 8192, "max_records": 3, "per_query_records": 3, "per_document_records": 3}}
        self.task_path = self.base / "task request.json"
        write(self.task_path, encode(self.task))

    def pack_cli(self, *options):
        process = subprocess.run([sys.executable, str(ROOT / "tools/shin.py"), "--config", str(self.config_path),
                                  "--task", str(self.task_path), *options], capture_output=True, timeout=30)
        return process, decode(process.stdout) if process.stdout else None

    def test_collects_three_ranges_without_model_or_profile_guessing(self):
        before = snapshot(self.base)
        process, result = self.pack_cli()
        self.assertEqual((process.returncode, process.stderr), (0, b""), result)
        self.assertEqual(result["schema_version"], "shin.agent-pack.v1")
        self.assertEqual(result["project"], "logical-project-not-profile")
        self.assertEqual(result["native"]["profile"], "observer-fixture")
        self.assertIsNone(result["admission"]["ready"])
        self.assertEqual(result["semantic_status"], "not-used-native-pack")
        self.assertTrue(result["native"]["completion"]["complete"])
        ranges = [row["location"] for row in result["native"]["evidence"]]
        self.assertEqual(len(ranges), 3)
        self.assertTrue(all(any(row["start_line"] <= line <= row["end_line"] for row in ranges) for line in (1, 31, 61)))
        self.assertEqual(snapshot(self.base), before)

    def test_native_pack_is_preserved_without_reading_optional_semantics(self):
        self.artifact_dir.mkdir()
        write(self.artifact_dir / "manifest.json", b"invalid optional artifact")
        session = runtime.Session(self.config_path)
        expected = subprocess.run(session.core_arguments() + ["pack", "--input", "-", "--jobs", "1",
            "--projection", "compact", "--require-current", "--strict"], input=encode(self.task), capture_output=True, timeout=30)
        self.assertEqual(expected.returncode, 0, expected.stderr)
        with patch.object(runtime, "Artifact") as artifact, patch.object(runtime.model, "load") as model:
            result = session.pack(self.task)
            artifact.assert_not_called()
            model.assert_not_called()
        self.assertEqual(result["native"], decode(expected.stdout))

    def test_client_budget_is_not_raised_for_collection(self):
        self.task["budget"]["per_document_records"] = 5
        write(self.task_path, encode(self.task))
        process, result = self.pack_cli()
        self.assertEqual(process.returncode, 1)
        self.assertNotIn("native", result)

    def test_changed_route_refuses_pack(self):
        session = runtime.Session(self.config_path)
        write(self.config_path, self.config_path.read_bytes() + b" ")
        with patch.object(runtime.subprocess, "run") as run:
            with self.assertRaisesRegex(Rejected, "route-config-changed"):
                session.pack(self.task)
            run.assert_not_called()

    def test_core_pack_response_mismatch_is_not_admitted(self):
        session = runtime.Session(self.config_path)
        original = session.pack(self.task)["native"]
        for field, value in (("profile", "wrong-profile"), ("root", "/wrong-root"),
                             ("request_id", "wrong-task"), ("strict", False)):
            response = {**original, field: value}
            process = subprocess.CompletedProcess([], 0, encode(response))
            with patch.object(runtime.subprocess, "run", return_value=process):
                with self.assertRaisesRegex(Rejected, "core-response-mismatch"):
                    session.pack(self.task)

    def test_route_change_during_collection_cannot_return_success(self):
        session = runtime.Session(self.config_path)
        original = session.pack(self.task)["native"]

        def changed(*args, **kwargs):
            write(self.config_path, self.config_path.read_bytes() + b" ")
            return subprocess.CompletedProcess([], 0, encode(original))

        with patch.object(runtime.subprocess, "run", side_effect=changed):
            with self.assertRaisesRegex(Rejected, "route-config-changed"):
                session.pack(self.task)

    def test_task_file_symlink_is_rejected(self):
        original = self.base / "original.json"
        self.task_path.rename(original)
        self.task_path.symlink_to(original)
        process, result = self.pack_cli()
        self.assertEqual(process.returncode, 1)
        self.assertNotIn("native", result)

    def test_pack_envelope_stays_within_existing_byte_cap(self):
        session = runtime.Session(self.config_path)
        core = session.pack(self.task)["native"]
        process = subprocess.CompletedProcess([], 0, encode(core))
        with patch.object(runtime, "MAX_OUTPUT", len(process.stdout) + 1), \
             patch.object(runtime.subprocess, "run", return_value=process):
            with self.assertRaisesRegex(Rejected, "agent-pack-output-budget"):
                session.pack(self.task)

    def test_incomplete_pack_keeps_native_gaps_and_nonzero_exit(self):
        self.task["task"]["must"].append({"id": "absent", "query": "NotImplementedQzxSourceCollection", "require_full_coverage": True})
        write(self.task_path, encode(self.task))
        process, result = self.pack_cli()
        self.assertEqual((process.returncode, process.stderr), (1, b""))
        self.assertFalse(result["ok"])
        self.assertIn("absent", result["native"]["completion"]["unsatisfied_required_requirement_ids"])
        self.assertTrue(result["native"]["index_freshness"]["current"])

    def test_stale_source_refuses_collection_without_repair(self):
        self.source.write_text("int changed_source() { return 0; }\n")
        before = snapshot(self.base)
        process, result = self.pack_cli()
        self.assertEqual(process.returncode, 1)
        self.assertEqual(result["error"]["code"], "core-admission-rejected")
        self.assertNotIn("native", result)
        self.assertEqual(snapshot(self.base), before)

    def test_task_modes_cannot_be_mixed(self):
        for options in (("question",), ("--status",), ("--metrics-dir", str(self.base / "metrics")),
                        ("--projection", "audit"), ("--root", str(self.fixture.root))):
            process, _ = self.pack_cli(*options)
            self.assertEqual(process.returncode, 2)

    def test_duplicate_json_and_oversized_task_are_rejected_before_retrieval(self):
        for data in (b'{"budget":{},"budget":{}}', b" " * (128 * 1024 + 1)):
            write(self.task_path, data)
            process, result = self.pack_cli()
            self.assertEqual(process.returncode, 1)
            self.assertFalse(result["ok"])
            self.assertNotIn("native", result)


class ReadTests(FixtureTests):
    def setUp(self):
        super().setUp()
        profile = decode(self.fixture.profile_path.read_bytes())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".cpp"], sourceFactExtractors=["language"])
        write(self.fixture.profile_path, encode(profile))
        (self.fixture.root / "src").mkdir()
        self.source = self.fixture.root / "src/pipeline.cpp"
        self.body = "int operation() {\n  return 7;\n}\n"
        self.source.write_text(self.body)
        self.build()
        self.config["project"] = "logical-project-not-profile"
        self.config["profile_sha256"] = sha(self.fixture.profile_path.read_bytes())
        self.save_config()
        outline = runtime.Session(self.config_path).inspect("src/pipeline.cpp")["native"]
        self.request = {"schema_version": "shin.source-read-request.v1", "snapshot_id": outline["snapshot_id"],
                        "record_ids": [row["id"] for row in outline["records"] if row.get("symbol_name") == "operation"]}
        self.request_path = self.base / "read request.json"
        write(self.request_path, encode(self.request))

    def read_cli(self, *options):
        process = subprocess.run([sys.executable, str(ROOT / "tools/shin.py"), "--config", str(self.config_path),
                                  "--read", str(self.request_path), *options], capture_output=True, timeout=30)
        return process, decode(process.stdout) if process.stdout else None

    def test_cli_preserves_exact_native_body_without_mutation(self):
        before = snapshot(self.base)
        process, result = self.read_cli()
        self.assertEqual((process.returncode, process.stderr), (0, b""), result)
        self.assertEqual(result["schema_version"], "shin.agent-read.v2")
        self.assertNotIn("text", result["native"]["evidence"]["bodies"][0])
        process, audit = self.read_cli("--projection", "audit")
        self.assertEqual(process.returncode, 0)
        self.assertEqual(expand_read(result), audit)
        result = expand_read(result)
        self.assertEqual(result["project"], "logical-project-not-profile")
        self.assertEqual(result["native"]["evidence"]["bodies"][0]["text"], self.body)
        self.assertTrue(result["native"]["complete"])
        self.assertIsNone(result["admission"]["ready"])
        self.assertEqual(snapshot(self.base), before)

    def test_v2_file_read_keeps_native_audit_and_requires_matching_schema(self):
        request = {"schema_version": "shin.source-read-request.v2", "snapshot_id": self.request["snapshot_id"],
                   "file_paths": ["src/pipeline.cpp"]}
        write(self.request_path, encode(request))
        before = snapshot(self.base)
        process, result = self.read_cli()
        self.assertEqual((process.returncode, process.stderr), (0, b""), result)
        result = expand_read(result)
        session = runtime.Session(self.config_path)
        expected = subprocess.run(session.core_arguments() + ["read", "--input", "-"], input=encode(request),
                                  capture_output=True, timeout=30)
        self.assertEqual(result["native"], decode(expected.stdout))
        self.assertEqual(result["native"]["schema_version"], "shin.source-read.v2")
        self.assertEqual(result["native"]["evidence"]["bodies"][0]["text"], self.body)
        self.assertEqual(snapshot(self.base), before)
        with patch.object(runtime.subprocess, "run", return_value=expected):
            with self.assertRaisesRegex(Rejected, "core-response-mismatch"):
                session.read(self.request)

    def test_reader_never_loads_optional_artifact_or_model(self):
        self.artifact_dir.mkdir()
        write(self.artifact_dir / "manifest.json", b"invalid unused artifact")
        session = runtime.Session(self.config_path)
        expected = subprocess.run(session.core_arguments() + ["read", "--input", "-"], input=encode(self.request),
                                  capture_output=True, timeout=30)
        self.assertEqual(expected.returncode, 0, expected.stderr)
        with patch.object(runtime, "Artifact") as artifact, patch.object(runtime.model, "load") as model:
            result = session.read(self.request)
            artifact.assert_not_called()
            model.assert_not_called()
        self.assertEqual(result["native"], decode(expected.stdout))

    def test_changed_route_refuses_before_and_after_native_read(self):
        session = runtime.Session(self.config_path)
        original = session.read(self.request)["native"]
        write(self.config_path, self.config_path.read_bytes() + b" ")
        with patch.object(runtime.subprocess, "run") as run:
            with self.assertRaisesRegex(Rejected, "route-config-changed"):
                session.read(self.request)
            run.assert_not_called()
        self.save_config()

        def changed(*args, **kwargs):
            write(self.config_path, self.config_path.read_bytes() + b" ")
            return subprocess.CompletedProcess([], 0, encode(original))

        with patch.object(runtime.subprocess, "run", side_effect=changed):
            with self.assertRaisesRegex(Rejected, "route-config-changed"):
                session.read(self.request)

    def test_mismatched_response_or_stale_freshness_is_rejected(self):
        session = runtime.Session(self.config_path)
        original = session.read(self.request)["native"]
        for field, value in (("profile", "wrong-profile"), ("snapshot_id", "fnv1a64:0000000000000000"),
                             ("schema_version", "wrong.v1"), ("complete", 1), ("evidence", {})):
            process = subprocess.CompletedProcess([], 0, encode({**original, field: value}))
            with patch.object(runtime.subprocess, "run", return_value=process):
                with self.assertRaisesRegex(Rejected, "core-response-mismatch"):
                    session.read(self.request)
        response = deepcopy(original)
        response["index_freshness"]["current"] = False
        with patch.object(runtime.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, encode(response))):
            with self.assertRaisesRegex(Rejected, "core-currentness-rejected"):
                session.read(self.request)

    def test_no_body_on_native_refusal_or_oversized_response(self):
        session = runtime.Session(self.config_path)
        for code, output in ((1, b"private child failure"), (0, b" " * (runtime.MAX_OUTPUT + 1))):
            with patch.object(runtime.subprocess, "run", return_value=subprocess.CompletedProcess([], code, output)):
                with self.assertRaisesRegex(Rejected, "core-admission-rejected"):
                    session.read(self.request)

    def test_incomplete_redacted_read_remains_successful_but_incomplete(self):
        secret = "sk-" + "Q7x9" * 16
        self.source.write_text('int operation() { return "' + secret + '"; } // gitleaks:allow\n')
        self.build()
        session = runtime.Session(self.config_path)
        outline = session.inspect("src/pipeline.cpp")["native"]
        self.assertEqual(outline["records"], [])
        request = {"schema_version": "shin.source-read-request.v2", "snapshot_id": outline["snapshot_id"],
                   "file_paths": ["src/pipeline.cpp"]}
        result = session.read(request)
        self.assertTrue(result["ok"])
        self.assertFalse(result["native"]["complete"])
        self.assertNotIn(secret.encode(), encode(result))

    def test_stale_source_refuses_without_repair(self):
        self.source.write_text(self.body + "// changed\n")
        before = snapshot(self.base)
        process, result = self.read_cli()
        self.assertEqual(process.returncode, 1)
        self.assertFalse(result["ok"])
        self.assertNotIn("native", result)
        self.assertEqual(snapshot(self.base), before)

    def test_wrapper_is_counted_in_transport_budget(self):
        session = runtime.Session(self.config_path)
        core = session.read(self.request)["native"]
        process = subprocess.CompletedProcess([], 0, encode(core))
        with patch.object(runtime, "MAX_OUTPUT", len(process.stdout) + 1), \
             patch.object(runtime.subprocess, "run", return_value=process):
            with self.assertRaisesRegex(Rejected, "agent-read-output-budget"):
                session.read(self.request)

    def test_symlink_duplicate_json_and_excessive_input_are_rejected(self):
        original = self.base / "original.json"
        self.request_path.rename(original)
        self.request_path.symlink_to(original)
        process, result = self.read_cli()
        self.assertEqual(process.returncode, 1)
        self.assertNotIn("native", result)
        self.request_path.unlink()
        for data in (b'{"record_ids":[],"record_ids":[]}', b" " * (128 * 1024 + 1)):
            write(self.request_path, data)
            process, result = self.read_cli()
            self.assertEqual(process.returncode, 1)
            self.assertNotIn("native", result)

    def test_read_mode_rejects_other_commands_or_route_overrides(self):
        for options in (("question",), ("--inspect", "src/pipeline.cpp"), ("--task", str(self.request_path)),
                        ("--status",), ("--metrics-dir", str(self.base / "metrics")),
                        ("--root", str(self.fixture.root)),):
            process, _ = self.read_cli(*options)
            self.assertEqual(process.returncode, 2)


class InspectTests(FixtureTests):
    def inspect_cli(self, path="README.md", *options):
        process = subprocess.run([sys.executable, str(ROOT / "tools/shin.py"), "--config", str(self.config_path),
                                  "--inspect", path, *options], capture_output=True, timeout=30)
        return process, decode(process.stdout) if process.stdout else None

    def test_native_inventory_without_optional_runtime_or_mutation(self):
        self.artifact_dir.mkdir()
        write(self.artifact_dir / "manifest.json", b"invalid unused artifact")
        before = snapshot(self.base)
        session = runtime.Session(self.config_path)
        native = subprocess.run(session.core_arguments() + ["inspect", "README.md", "--require-current"],
                                capture_output=True, timeout=30)
        self.assertEqual(native.returncode, 0, native.stderr)
        with patch.object(runtime, "Artifact") as artifact, patch.object(runtime.model, "load") as model:
            result = session.inspect("README.md")
            artifact.assert_not_called()
            model.assert_not_called()
        self.assertEqual(result["native"], decode(native.stdout))
        process, cli_result = self.inspect_cli()
        self.assertEqual((process.returncode, process.stderr), (0, b""))
        self.assertEqual(cli_result, result)
        self.assertIsNone(result["admission"]["ready"])
        self.assertEqual(result["semantic_status"], "not-used-native-inspect")
        self.assertEqual(snapshot(self.base), before)

    def test_inspection_discovers_symbols_then_same_route_pack_collects(self):
        profile = decode(self.fixture.profile_path.read_bytes())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".cpp"])
        write(self.fixture.profile_path, encode(profile))
        (self.fixture.root / "src").mkdir()
        path = "src/neutral.cpp"
        (self.fixture.root / path).write_text("int prepare_batch() {\n return 1;\n}\n\nint retain_error() {\n return 2;\n}\n")
        self.build()
        self.config.update(profile_sha256=sha(self.fixture.profile_path.read_bytes()), project="different-logical-id")
        self.save_config()
        session = runtime.Session(self.config_path)
        result = session.inspect(path)
        names = [r["symbol_name"] for r in result["native"]["records"] if "symbol_name" in r]
        self.assertEqual(names, ["prepare_batch", "retain_error"])
        task = {"schema_version": "shin.task-request.v1", "request_id": "inspect-follow-up",
                "task": {"objective": "Collect discovered declarations", "must": [
                    {"id": name, "query": path + " " + name, "require_full_coverage": True} for name in names],
                         "should": [], "scope": []}, "evidence_types": ["fact"],
                "budget": {"max_evidence_bytes": 8192, "max_records": 2, "per_query_records": 2, "per_document_records": 2}}
        pack = session.pack(task)
        self.assertTrue(pack["native"]["completion"]["complete"])
        self.assertEqual(len(pack["native"]["evidence"]), 2)
        self.assertEqual(pack["native"]["snapshot_id"], result["native"]["snapshot_id"])

    def test_stale_and_corrupt_publications_refuse_inspection(self):
        self.fixture.write_source("Changed source.\n")
        process, result = self.inspect_cli()
        self.assertEqual(process.returncode, 1)
        self.assertNotIn("native", result)
        self.build()
        with (self.fixture.out / "docs/kb/generated/sections.jsonl").open("ab") as output:
            output.write(b"corrupt\n")
        process, result = self.inspect_cli()
        self.assertEqual(process.returncode, 1)
        self.assertNotIn("native", result)

    def test_invalid_paths_stop_before_child(self):
        session = runtime.Session(self.config_path)
        for path in ("", "/tmp/x", "../x", "src/../x", "src//x", "./x", "-x", "x\n", "x\0", "x\\y", "x" * 4097):
            with patch.object(runtime.subprocess, "run") as run:
                with self.assertRaisesRegex(Rejected, "invalid-inspect-path"):
                    session.inspect(path)
                run.assert_not_called()

    def test_changed_route_before_or_during_inspection_refuses_success(self):
        session = runtime.Session(self.config_path)
        native = session.inspect("README.md")["native"]
        write(self.config_path, self.config_path.read_bytes() + b" ")
        with patch.object(runtime.subprocess, "run") as run:
            with self.assertRaisesRegex(Rejected, "route-config-changed"):
                session.inspect("README.md")
            run.assert_not_called()
        self.save_config()
        def changed(*args, **kwargs):
            write(self.config_path, self.config_path.read_bytes() + b" ")
            return subprocess.CompletedProcess([], 0, encode(native))
        with patch.object(runtime.subprocess, "run", side_effect=changed):
            with self.assertRaisesRegex(Rejected, "route-config-changed"):
                session.inspect("README.md")

    def test_wrong_response_identity_or_freshness_refuses_success(self):
        session = runtime.Session(self.config_path)
        native = session.inspect("README.md")["native"]
        for field, value in (("profile", "wrong"), ("root", "/wrong"), ("path", "other.md"), ("index_mode", "source-scan")):
            process = subprocess.CompletedProcess([], 0, encode({**native, field: value}))
            with patch.object(runtime.subprocess, "run", return_value=process):
                with self.assertRaisesRegex(Rejected, "core-response-mismatch"):
                    session.inspect("README.md")
        bad = deepcopy(native)
        bad["index_freshness"]["snapshot_id"] = "wrong"
        with patch.object(runtime.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, encode(bad))):
            with self.assertRaisesRegex(Rejected, "core-currentness-rejected"):
                session.inspect("README.md")

    def test_inspect_modes_do_not_silently_change_client_or_budget(self):
        for options in (("question",), ("--status",), ("--task", "task.json"), ("--projection", "audit"),
                        ("--metrics-dir", str(self.base / "metrics")), ("--root", str(self.fixture.root))):
            process, _ = self.inspect_cli("README.md", *options)
            self.assertEqual(process.returncode, 2)
        process = subprocess.run([sys.executable, str(ROOT / "tools/shin.py"), "--inspect", "README.md"], capture_output=True)
        self.assertEqual(process.returncode, 2)


class RouteTests(FixtureTests):
    def agent_cli(self, *options, query="cached workspace observation"):
        value = subprocess.run([sys.executable, str(ROOT / "tools/shin.py"), "--config", str(self.config_path),
                                *options, query], capture_output=True, timeout=30)
        return value, decode(value.stdout) if value.stdout else None

    def test_metrics_opt_in_preserves_stdout_and_sources(self):
        from shin_semantic import telemetry
        home = self.base / "metrics"
        telemetry.initialize(home)
        before = snapshot(self.fixture.root)
        plain, _ = self.agent_cli()
        measured, result = self.agent_cli("--metrics-dir", str(home))
        self.assertEqual((measured.returncode, measured.stderr), (0, b""))
        self.assertEqual(measured.stdout, plain.stdout)
        self.assertEqual(snapshot(self.fixture.root), before)
        row = telemetry.report(home)["recent"][0]
        self.assertEqual(row["semantic_status"], "artifact-unavailable-lexical-only")
        self.assertEqual(row["response_bytes"], len(measured.stdout))
        self.assertEqual(row["snapshot_id"], result["native"]["snapshot_id"])
        self.assertIn("core-brief", row["phase_ns"])
        self.assertNotIn("model-load", row["phase_ns"])
        self.assertIsNone(telemetry.report(home)["agent_usefulness"])

    def test_metrics_failure_is_bounded_stderr_not_a_retrieval_failure(self):
        plain, _ = self.agent_cli()
        missing = self.base / "not-initialized"
        measured, _ = self.agent_cli("--metrics-dir", str(missing))
        self.assertEqual(measured.returncode, plain.returncode)
        self.assertEqual(measured.stdout, plain.stdout)
        self.assertEqual(decode(measured.stderr)["code"], "query-metrics-not-confirmed")
        self.assertFalse(missing.exists())

    def test_stale_rejection_has_no_fabricated_success_metrics(self):
        from shin_semantic import telemetry
        home = self.base / "metrics"
        telemetry.initialize(home)
        self.fixture.write_source("Changed source.\n")
        measured, result = self.agent_cli("--metrics-dir", str(home))
        self.assertEqual((measured.returncode, measured.stderr), (1, b""))
        row = telemetry.report(home)["recent"][0]
        self.assertEqual(row["reason"], result["error"]["code"])
        self.assertEqual(row["failed_phase"], "core-brief")
        self.assertFalse(row["ok"])
        self.assertIsNone(row["snapshot_id"])
        self.assertIsNone(row["native_records"])
        self.assertNotIn("model-load", row["phase_ns"])

    def test_invalid_route_is_unrecorded_not_written_with_guessed_identity(self):
        from shin_semantic import telemetry
        home = self.base / "metrics"
        telemetry.initialize(home)
        self.config_path.chmod(0o644)
        measured, result = self.agent_cli("--metrics-dir", str(home))
        self.assertEqual(measured.returncode, 1)
        self.assertEqual(result["error"]["code"], "unsafe-private-file")
        self.assertEqual(decode(measured.stderr)["code"], "query-metrics-not-confirmed")
        self.assertEqual(telemetry.report(home)["retained"], 0)

    def test_agent_entrypoint_keeps_missing_model_fallback_and_admission_boundary(self):
        before = snapshot(self.base)
        value, result = self.agent_cli()
        self.assertEqual((value.returncode, value.stderr), (0, b""))
        self.assertEqual(result["schema_version"], "shin.agent-query.v2")
        self.assertIsNone(result["admission"]["ready"])
        self.assertIsNone(result["navigation"])
        self.assertEqual(result["semantic_status"], "artifact-unavailable-lexical-only")
        self.assertTrue(result["native"]["files"])
        self.assertEqual(snapshot(self.base), before)

    def test_agent_audit_preserves_original_query_envelope(self):
        expected = runtime.Session(self.config_path).query("cached workspace observation")
        value, result = self.agent_cli("--projection", "audit")
        self.assertEqual(value.returncode, 0, value.stderr)
        self.assertEqual(result, expected)

    def test_agent_stale_route_cannot_become_a_compact_success(self):
        self.fixture.write_source("Changed source.\n")
        value, result = self.agent_cli()
        self.assertEqual(value.returncode, 1)
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "core-admission-rejected")
        self.assertNotIn("native", result)

    def test_agent_config_rejects_mixed_route_selection(self):
        for options in (("--root", str(self.fixture.root)), ("--out-dir", str(self.fixture.out)),
                        ("--marker", "docs/adoption.json"), ("--status",)):
            value, _ = self.agent_cli(*options)
            self.assertEqual(value.returncode, 2)

    def test_missing_optional_runtime_preserves_lexical_without_installing(self):
        self.artifact_dir.mkdir()
        self.model_dir.mkdir()
        session = runtime.Session(self.config_path)
        with patch.object(runtime.model, "available", return_value=False), patch.object(runtime, "Artifact") as artifact:
            result = session.query("cached workspace observation")
            artifact.assert_not_called()
        self.assertEqual(result["semantic_status"], "runtime-unavailable-lexical-only")
        self.assertIsNone(result["navigation"])

    def test_missing_artifact_preserves_exact_native_result_and_writes_nothing(self):
        session = runtime.Session(self.config_path)
        before = snapshot(self.base)
        expected = session.lexical("cached workspace observation")
        with patch.object(runtime, "Artifact") as artifact, patch.object(runtime.model, "load") as model:
            result = session.query("cached workspace observation")
            artifact.assert_not_called()
            model.assert_not_called()
        self.assertEqual(result["core"], expected)
        self.assertIsNone(result["navigation"])
        self.assertEqual(result["semantic_status"], "artifact-unavailable-lexical-only")
        self.assertEqual(snapshot(self.base), before)

    def test_source_drift_fails_even_when_optional_channel_is_absent(self):
        session = runtime.Session(self.config_path)
        self.fixture.write_source("Changed source.\n")
        with self.assertRaisesRegex(Rejected, "core-admission-rejected"):
            session.query("cached workspace observation")
        code, result = self.cli()
        self.assertEqual(code, 1)
        self.assertEqual(result["error"]["code"], "core-admission-rejected")

    def test_identical_archived_profile_is_not_a_current_route(self):
        original = self.fixture.profile_path
        directory = self.base / "archived-profiles"
        directory.mkdir()
        archived = directory / original.name
        write(archived, original.read_bytes())
        self.config["profile_path"] = str(archived)
        self.save_config()
        with self.assertRaisesRegex(Rejected, "core-admission-rejected"):
            runtime.Session(self.config_path).query("cached workspace observation")
        self.config["profile_path"] = str(original)
        self.save_config()
        self.assertTrue(runtime.Session(self.config_path).query("cached workspace observation")["ok"])

    def test_profile_core_root_and_route_pin_drift_rejected(self):
        for field, value in (("core_sha256", "0" * 64), ("profile_sha256", "0" * 64), ("root_identity", "1:2")):
            with self.subTest(field=field):
                original = self.config[field]
                self.config[field] = value
                self.save_config()
                with self.assertRaises(Rejected):
                    runtime.Session(self.config_path)
                self.config[field] = original
        self.save_config()
        session = runtime.Session(self.config_path)
        write(self.config_path, self.config_path.read_bytes() + b" ")
        with self.assertRaisesRegex(Rejected, "route-config-changed"):
            session.query("cached workspace observation")

    def test_invalid_queries_cannot_become_core_options(self):
        session = runtime.Session(self.config_path)
        for query in ("", "--source-scan", "\0", "x" * 4097):
            with self.subTest(query=query[:20]), self.assertRaisesRegex(Rejected, "invalid-query"):
                session.query(query)

    def test_private_config_and_duplicate_json_rejected_quietly(self):
        self.config_path.chmod(0o644)
        code, result = self.cli()
        self.assertEqual((code, result["error"]["code"]), (1, "unsafe-private-file"))
        write(self.config_path, b'{"a":1,"a":2}')
        code, result = self.cli()
        self.assertEqual((code, result["error"]["code"]), (1, "duplicate-json-key"))

    def test_exact_owner_path_does_not_require_model_or_artifact(self):
        session = runtime.Session(self.config_path)
        raw = session.lexical("cached workspace observation")
        raw["retrieval_mode"] = "identifier-scoped"
        with patch.object(session, "lexical", return_value=raw), patch.object(runtime, "Artifact") as artifact:
            result = session.query("cached workspace observation")
            artifact.assert_not_called()
        self.assertEqual(result["core"], raw)
        self.assertEqual(result["semantic_status"], "not-needed-exact-or-owner")


class ArtifactTests(FixtureTests):
    def setUp(self):
        if importlib.util.find_spec("numpy") is None:
            self.skipTest("NumPy is optional; run this suite in semantic-pilot-venv for vector admission")
        import numpy as np
        self.np = np
        super().setUp()
        self.artifact_dir.mkdir(mode=0o700)
        generated = self.fixture.out / "docs/kb/generated"
        self.units, _ = project_units({"id": "fixture", "root": str(self.fixture.root)}, generated)
        metadata = [{**{key: row[key] for key in ("project", "path", "start_line", "end_line", "source_sha256", "record_ids")},
                     "text_sha256": sha(row["text"].encode())} for row in self.units]
        write(self.artifact_dir / "units.jsonl", b"".join(encode(row) + b"\n" for row in metadata))
        publication = (generated / "manifest.json").read_bytes()
        write(self.artifact_dir / "publication.json", publication)
        write(self.artifact_dir / "links.jsonl", (generated / "links.jsonl").read_bytes())
        matrix = np.zeros((len(metadata), 384), dtype=np.float32)
        matrix[:, 0] = 1
        self.vector(matrix)
        published = decode(publication)
        self.descriptor = {"schema_version": "shin.semantic-artifact.v1", "projection": "core-bound-range-v1",
            "selection": "code-anchor-source-resolution-v1", "model": deepcopy(MODEL), "units": len(metadata),
            "binding": {**{key: self.config[key] for key in ("root", "project", "root_identity", "profile", "profile_sha256")},
                        "publication_sha256": sha(publication), "snapshot_id": publication_id(publication),
                        "profile_config_hash": published["profile_config_hash"], "generator": published["generator_version"]},
            "files": {}}
        self.seal()
        self.model_dir.mkdir()

    def vector(self, matrix):
        stream = io.BytesIO()
        self.np.save(stream, matrix, allow_pickle=False)
        write(self.artifact_dir / "vectors.npy", stream.getvalue())

    def seal(self):
        self.descriptor["files"] = {name: {"bytes": (self.artifact_dir / name).stat().st_size,
                                           "sha256": sha((self.artifact_dir / name).read_bytes())}
                                    for name in ("units.jsonl", "links.jsonl", "publication.json", "vectors.npy")}
        data = encode(self.descriptor)
        write(self.artifact_dir / "manifest.json", data)
        self.config["artifact_sha256"] = sha(data)
        self.save_config()

    def artifact(self):
        return Artifact(self.artifact_dir, self.config["artifact_sha256"], self.config)

    def fake_model(self):
        vector = self.np.zeros(384, dtype=self.np.float32)
        vector[0] = 1
        class Model:
            def embed(self, queries):
                return iter([vector for _ in queries])
        return Model()

    def test_instrumented_cold_and_reused_model_do_not_change_answers(self):
        from shin_semantic.telemetry import Trace
        with patch.object(runtime.model, "load", return_value=self.fake_model()):
            expected = runtime.Session(self.config_path).query("cached workspace observation")
            session = runtime.Session(self.config_path)
            cold, warm = Trace(), Trace()
            measured = session.query("cached workspace observation", trace=cold)
            self.assertEqual(measured, expected)
            repeated = session.query("cached workspace observation", trace=warm)
        self.assertEqual(repeated, expected)
        self.assertIn("model-load", cold.phases)
        self.assertNotIn("model-load", warm.phases)
        self.assertIn("query-embedding", warm.phases)
        self.assertIn("selection", warm.phases)
        self.assertEqual(warm.record(self.config, repeated, repeated, 100, "compact")["model_state"], "reused")

    def test_captured_matrix_survives_disk_mutation_without_mixing(self):
        artifact = self.artifact()
        original = artifact.matrix.copy()
        write(self.artifact_dir / "vectors.npy", b"corrupt")
        self.np.testing.assert_array_equal(artifact.matrix, original)
        self.assertFalse(artifact.matrix.flags.writeable)
        with self.assertRaises(Rejected):
            self.artifact()

    def test_artifact_permissions_do_not_expose_private_vectors(self):
        self.artifact_dir.chmod(0o755)
        with self.assertRaisesRegex(Rejected, "unsafe-artifact-directory"):
            self.artifact()
        self.artifact_dir.chmod(0o700)
        (self.artifact_dir / "vectors.npy").chmod(0o644)
        with self.assertRaisesRegex(Rejected, "unsafe-private-file"):
            self.artifact()

    def test_vector_shape_dtype_nan_norm_and_trailing_bytes_rejected(self):
        for matrix in (self.np.zeros((1, 385), dtype="f4"), self.np.zeros((1, 384), dtype="f8"),
                       self.np.full((1, 384), float("nan"), dtype="f4"), self.np.zeros((1, 384), dtype="f4")):
            self.vector(matrix)
            self.seal()
            with self.assertRaises(Rejected):
                self.artifact()
        data = io.BytesIO()
        self.np.lib.format.write_array_header_1_0(data, {"shape": (10**12, 384), "fortran_order": False, "descr": "<f4"})
        write(self.artifact_dir / "vectors.npy", data.getvalue())
        self.seal()
        with self.assertRaisesRegex(Rejected, "invalid-vector-layout"):
            self.artifact()
        matrix = self.np.zeros((len(self.units), 384), dtype="f4")
        matrix[:, 0] = 1
        self.vector(matrix)
        path = self.artifact_dir / "vectors.npy"
        write(path, path.read_bytes() + b"trailing")
        self.seal()
        with self.assertRaisesRegex(Rejected, "vector-byte-count"):
            self.artifact()

    def test_artifact_hash_route_links_and_units_rejected(self):
        for name in ("units.jsonl", "vectors.npy", "links.jsonl", "publication.json"):
            path = self.artifact_dir / name
            data = path.read_bytes()
            write(path, data + b"x")
            with self.subTest(name=name), self.assertRaises(Rejected):
                self.artifact()
            write(path, data)
        self.descriptor["binding"]["project"] = "foreign"
        self.seal()
        with self.assertRaisesRegex(Rejected, "artifact-route-mismatch"):
            self.artifact()

    def test_duplicate_traversal_and_source_generation_metadata_rejected(self):
        original = (self.artifact_dir / "units.jsonl").read_bytes()
        row = decode(original.split(b"\n")[0])
        for content in (original + original, encode({**row, "path": "../escape"}) + b"\n",
                        encode({**row, "source_sha256": "bad"}) + b"\n"):
            write(self.artifact_dir / "units.jsonl", content)
            self.seal()
            with self.assertRaises(Rejected):
                self.artifact()

    def test_new_native_generation_rejects_old_semantics_before_inference(self):
        self.fixture.write_source("# Changed document\n\nThis is a new publication.\n")
        self.build()
        with patch.object(runtime.model, "load") as model:
            with self.assertRaisesRegex(Rejected, "semantic-snapshot-mismatch"):
                runtime.Session(self.config_path).query("new publication")
            model.assert_not_called()

    def test_model_absence_is_lexical_but_partial_or_foreign_model_is_error(self):
        self.model_dir.rmdir()
        result = runtime.Session(self.config_path).query("cached workspace observation")
        self.assertEqual(result["semantic_status"], "model-unavailable-lexical-only")
        self.assertIsNone(result["navigation"])
        self.model_dir.mkdir()
        with self.assertRaises((ValueError, OSError)):
            runtime.Session(self.config_path).query("cached workspace observation")
        self.descriptor["model"]["name"] = "foreign"
        self.seal()
        with self.assertRaisesRegex(Rejected, "unsupported-semantic-model"):
            runtime.Session(self.config_path).query("cached workspace observation")

    def test_warm_session_rechecks_core_currentness_and_keeps_immutable_generation(self):
        with patch.object(runtime.model, "load", return_value=self.fake_model()):
            session = runtime.Session(self.config_path)
            first = session.query("cached workspace observation")
            expected = deepcopy(first)
            first["navigation"]["candidates"][0]["record_ids"].append("caller-mutated-result")
            write(self.artifact_dir / "vectors.npy", b"changed after capture")
            second = session.query("cached workspace observation")
            self.assertEqual(expected, second)
            self.fixture.write_source("Changed without publication.\n")
            with self.assertRaisesRegex(Rejected, "core-admission-rejected"):
                session.query("cached workspace observation")

    def test_read_only_single_query_and_no_false_authority(self):
        before = snapshot(self.base)
        with patch.object(runtime.model, "load", return_value=self.fake_model()):
            result = runtime.Session(self.config_path).query("cached workspace observation")
        self.assertEqual(snapshot(self.base), before)
        self.assertEqual(result["semantic_status"], "available")
        navigation = result["navigation"]
        self.assertEqual(navigation["candidates"][0]["path"], "README.md")
        for field in ("semantic_grants_lexical_coverage", "symbol_existence_asserted", "current_source_verified", "pack_completion_asserted"):
            self.assertIs(navigation[field], False)

    def test_real_offline_model_and_cli_on_current_fixture(self):
        if MODEL_DIR is None:
            self.skipTest("supply --model-dir to exercise the provisioned local model")
        real_model = runtime.model.load(MODEL_DIR, MODEL)
        matrix = self.np.array(list(real_model.embed([MODEL["passage_prefix"] + row["text"] for row in self.units])), dtype="f4")
        self.vector(matrix)
        self.config["model_dir"] = str(MODEL_DIR)
        self.seal()
        before = snapshot(self.base)
        code, result = self.cli()
        self.assertEqual(code, 0, result)
        self.assertEqual(result["semantic_status"], "available")
        self.assertEqual(result["navigation"]["candidates"][0]["path"], "README.md")
        self.assertEqual(snapshot(self.base), before)

    def test_repeated_real_model_load_in_one_process_preserves_vectors(self):
        if MODEL_DIR is None:
            self.skipTest("supply --model-dir for repeated local model construction")
        first = runtime.model.load(MODEL_DIR, MODEL)
        second = runtime.model.load(MODEL_DIR, MODEL)
        text = [MODEL["query_prefix"] + "Which tenant and revision own this cached feed?"]
        self.np.testing.assert_array_equal(next(first.embed(text)), next(second.embed(text)))

    def test_model_registration_does_not_adopt_foreign_entries(self):
        if importlib.util.find_spec("fastembed") is None:
            self.skipTest("FastEmbed is optional")
        from fastembed import TextEmbedding
        with patch.object(runtime.model, "_registered_embedding", None), \
             patch.object(TextEmbedding, "add_custom_model", side_effect=ValueError("already registered")):
            with self.assertRaisesRegex(Rejected, "model-registration-conflict"):
                runtime.model._embedding_type()
            self.assertIsNone(runtime.model._registered_embedding)

    def test_model_registration_is_once_for_concurrent_constructors(self):
        if importlib.util.find_spec("fastembed") is None:
            self.skipTest("FastEmbed is optional")
        from concurrent.futures import ThreadPoolExecutor
        from fastembed import TextEmbedding
        with patch.object(runtime.model, "_registered_embedding", None), \
             patch.object(TextEmbedding, "add_custom_model") as register, ThreadPoolExecutor(max_workers=4) as pool:
            values = list(pool.map(lambda _: runtime.model._embedding_type(), range(4)))
            register.assert_called_once()
            self.assertTrue(all(value is TextEmbedding for value in values))

    def test_cached_registration_does_not_skip_model_pin_verification(self):
        if MODEL_DIR is None:
            self.skipTest("supply --model-dir to exercise cached model registration")
        instance = runtime.model.load(MODEL_DIR, MODEL)
        self.assertIsNotNone(instance)
        with patch.object(runtime.model, "read", return_value=b"changed manifest"):
            with self.assertRaisesRegex(Rejected, "model-manifest-changed"):
                runtime.model.load(MODEL_DIR, MODEL)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True, type=Path)
    parser.add_argument("--model-dir", type=Path)
    parser.add_argument("--report", type=Path)
    args, remaining = parser.parse_known_args()
    CORE = args.core.resolve()
    MODEL_DIR = args.model_dir.resolve() if args.model_dir else None
    program = unittest.main(argv=[sys.argv[0], *remaining], exit=False)
    if args.report:
        files = [ROOT / "tools/shin.py", ROOT / "tools/shin_semantic.py",
                 *sorted((ROOT / "integrations/semantic").glob("*.py")), Path(__file__)]
        report = {"schema_version": "shin.semantic-runtime-tests.v1", "ok": program.result.wasSuccessful(),
                  "tests": program.result.testsRun, "skipped": len(program.result.skipped),
                  "failures": len(program.result.failures), "errors": len(program.result.errors),
                  "core_sha256": sha(CORE.read_bytes()), "real_model_requested": MODEL_DIR is not None,
                  "implementation_sha256": {str(path.relative_to(ROOT)): sha(path.read_bytes()) for path in files}}
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_bytes(encode(report) + b"\n")
    raise SystemExit(0 if program.result.wasSuccessful() else 1)
