"""Hermetic integration coverage for Shin 1.24 execution evidence commands.

The runner deliberately treats Core as read-only. It creates capture/import/check
artifacts outside a temporary consumer, and checks only documented envelope
properties until the parent implementation assigns stable error codes.
"""

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
SOURCE_ROOT = Path(sys.argv.pop(1)).resolve()
FIXTURE = Path(__file__).resolve().parent
REPORTS = FIXTURE / "reports"
SECRET = "execution-evidence-test-secret"
TASK_ID = "execution-task"
THREAD_ID = "execution-thread"
PRODUCER = "fixture-runner"
FINGERPRINT = "fnv1a64:0000000000000000"


def write_json(path, value):
    path.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n")


def file_tree(root):
    if not root.exists() and not root.is_symlink():
        return {}
    state = {}
    for path in [root, *sorted(root.rglob("*"))]:
        metadata = path.lstat()
        relative = str(path.relative_to(root))
        if stat.S_ISLNK(metadata.st_mode):
            content = ("symlink", os.readlink(path))
        elif stat.S_ISREG(metadata.st_mode):
            content = ("file", hashlib.sha256(path.read_bytes()).hexdigest())
        else:
            content = ("directory",)
        state[relative] = (metadata.st_mode, metadata.st_size, metadata.st_mtime_ns,
                           metadata.st_ctime_ns, metadata.st_ino, content)
    return state


class ExecutionEvidenceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-execution-evidence-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "consumer"
        self.profiles = self.base / "profiles"
        self.out = self.base / "index"
        self.artifacts = self.base / "artifacts"
        self.artifacts.mkdir()
        shutil.copytree(FIXTURE / "root", self.root)
        shutil.copytree(FIXTURE / "profiles", self.profiles)
        profile_paths = sorted(self.profiles.glob("*.json"))
        self.assertEqual(len(profile_paths), 1)
        self.profile_path = profile_paths[0]
        self.profile = json.loads(self.profile_path.read_text())
        self.profile_id = self.profile["id"]
        self.sentinel = self.root / "execution-sentinel"
        self.environment = dict(os.environ, LC_ALL="C",
                                SHIN_EXECUTION_EVIDENCE_SENTINEL=str(self.sentinel))
        self.build()

    def argv(self, *args, root=None, profiles=None, out=None):
        return [TOOL, "--profile", self.profile_id, "--profile-dir", str(profiles or self.profiles),
                "--root", str(root or self.root), "--out-dir", str(out or self.out), *args]

    def build(self, *, root=None, profiles=None, out=None, profile_id=None):
        args = self.argv("build", root=root, profiles=profiles, out=out)
        if profile_id is not None:
            args[args.index("--profile") + 1] = profile_id
        process = subprocess.run(args, capture_output=True, text=True, env=self.environment, timeout=90)
        self.assertEqual(process.returncode, 0, process.stderr or process.stdout)

    def watched_state(self):
        return {str(path): file_tree(path) for path in (self.root, self.profiles, self.out)}

    def assert_read_only(self, before):
        after = self.watched_state()
        changed = []
        for root in before:
            for path in sorted(before[root].keys() | after[root].keys()):
                if before[root].get(path) != after[root].get(path):
                    changed.append(str(Path(root) / path))
        self.assertFalse(changed, f"execution evidence command wrote consumer/index state: {changed[:8]}")
        self.assertFalse(self.sentinel.exists(), "execution evidence command executed a consumer sentinel")

    def invoke(self, command, *args, expected=(0,), root=None, profiles=None, out=None, profile_id=None):
        before = self.watched_state() if root is None and profiles is None and out is None else None
        argv = self.argv(command, *args, root=root, profiles=profiles, out=out)
        if profile_id is not None:
            argv[argv.index("--profile") + 1] = profile_id
        process = subprocess.run(argv, capture_output=True, text=True, env=self.environment, timeout=90)
        if before is not None:
            self.assert_read_only(before)
        self.assertIn(process.returncode, expected, process.stderr or process.stdout)
        return process

    def reject(self, command, *args, error_code=None, **kwargs):
        process = self.invoke(command, *args, expected=tuple(range(1, 256)), **kwargs)
        self.assertEqual(process.stdout, "", "a rejected operation emitted a receipt or assessment")
        self.assertNotIn(SECRET, process.stderr)
        try:
            error = json.loads(process.stderr)
        except json.JSONDecodeError:
            self.fail(f"rejected operation did not emit a structured error: {process.stderr}")
        self.assertEqual(error.get("schema_version"), "shin.error.v1")
        self.assertFalse(error.get("ok", True))
        if error_code is not None:
            self.assertEqual(error["error"]["code"], error_code)
        return error

    def json_command(self, command, *args, expected=(0,), **kwargs):
        process = self.invoke(command, *args, expected=expected, **kwargs)
        self.assertEqual(process.stderr, "", process.stderr)
        self.assertNotIn(SECRET, process.stdout)
        return json.loads(process.stdout)

    def scope_request(self, coverage, selectors):
        return {"schema_version": "shin.execution-input-request.v1", "coverage": coverage,
                "selectors": selectors}

    def capture(self, coverage, selectors, name="inputs", now=1000, task_id=TASK_ID, thread_id=THREAD_ID):
        request_path = self.artifacts / f"{name}-request.json"
        write_json(request_path, self.scope_request(coverage, selectors))
        result = self.json_command("receipt-scope", "--input", str(request_path), "--task-id", task_id,
                                   "--thread-id", thread_id, "--observed-at", str(now))
        self.assertEqual(result["schema_version"], "shin.execution-inputs.v1")
        self.assertEqual(result["captured_at"], now)
        self.assertEqual(result["coverage"], coverage)
        self.assertEqual(result["binding"]["root"], str(self.root.resolve()))
        self.assertEqual(result["binding"]["profile"], self.profile_id)
        self.assertEqual(result["binding"]["task_id"], task_id)
        self.assertEqual(result["binding"]["thread_id"], thread_id)
        self.assertTrue(result["inputs_id"].startswith("fnv1a64:"))
        self.assertEqual(len(result["observations"]), len(selectors))
        for observation in result["observations"]:
            self.assertIn(observation["observation"], {"observed", "missing", "unobserved"})
            if observation["observation"] == "missing":
                self.assertIsNone(observation["content_hash"])
            else:
                self.assertTrue(observation["content_hash"].startswith("fnv1a64:"))
            self.assertNotIn("inventory", observation)
            self.assertNotIn("file_hashes", observation)
            self.assertNotIn("content", observation)
        path = self.artifacts / f"{name}.json"
        write_json(path, result)
        return result, path

    def import_report(self, inputs_path, report_name="junit-pass.xml", *, receipt_id="receipt-pass",
                      report_format="junit", started_at=1001, observed_at=1002, now=1003,
                      complete=True, producer=PRODUCER, task_id=TASK_ID, thread_id=THREAD_ID):
        report_path = self.artifacts / report_name
        shutil.copyfile(REPORTS / report_name, report_path)
        request = {
            "schema_version": "shin.receipt-import-request.v1",
            "format": report_format,
            "report_path": str(report_path),
            "inputs_path": str(inputs_path),
            "receipt_id": receipt_id,
            "producer": producer,
            "started_at": started_at,
            "observed_at": observed_at,
            "ttl_seconds": 60,
            "command_fingerprint": FINGERPRINT,
            "complete": complete,
        }
        request_path = self.artifacts / f"{receipt_id}-import.json"
        write_json(request_path, request)
        result = self.json_command("receipt-import", "--input", str(request_path), "--task-id", task_id,
                                   "--thread-id", thread_id, "--observed-at", str(now))
        self.assertEqual(result["schema_version"], "shin.receipt-bundle.v1")
        self.assertTrue(result["ok"])
        self.assertEqual(len(result["receipts"]), 1)
        receipt = result["receipts"][0]
        self.assertEqual(receipt["schema_version"], "shin.execution-receipt.v1")
        self.assertEqual(receipt["receipt_id"], receipt_id)
        self.assertEqual(receipt["producer"], producer)
        self.assertEqual(receipt["started_at"], started_at)
        self.assertEqual(receipt["observed_at"], observed_at)
        self.assertEqual(receipt["inputs"]["schema_version"], "shin.execution-inputs.v1")
        self.assertEqual(receipt["report"]["format"], report_format)
        self.assertTrue(receipt["report"]["payload_retained"] is False)
        self.assertGreater(receipt["report"]["bytes"], 0)
        self.assertTrue(receipt["report"]["content_hash"].startswith("fnv1a64:"))
        rendered = json.dumps(result, sort_keys=True)
        for forbidden in (SECRET, str(report_path), "passes-one", "failure body"):
            self.assertNotIn(forbidden, rendered)
        bundle_path = self.artifacts / f"{receipt_id}-bundle.json"
        write_json(bundle_path, result)
        return result, bundle_path

    def check(self, bundle_path, *options, now=1003, expected=(0,), receipt_count=1,
              task_id=TASK_ID, thread_id=THREAD_ID, **kwargs):
        result = self.json_command("receipt-check", "--input", str(bundle_path), "--task-id", task_id,
                                   "--thread-id", thread_id, "--observed-at", str(now), *options,
                                   expected=expected, **kwargs)
        self.assertEqual(result["schema_version"], "shin.receipt-assessment.v1")
        self.assertEqual(result["observed_at"], now)
        self.assertIn("index_current", result)
        self.assertIn("strict", result)
        self.assertIn("require_current", result)
        self.assertEqual(len(result["receipts"]), receipt_count)
        return result

    def assess(self, result):
        receipt = result["receipts"][0]
        for field in ("inputs_current", "changed_selectors", "narrowed_scope_used", "passed_current"):
            self.assertIn(field, receipt)
        return receipt

    def test_capture_receipt_scope_file_tree_and_tree_membership_drift(self):
        _, file_inputs = self.capture("partial", [{"path": "src/selected.ts", "kind": "file"}], "file")
        manifest, tree_inputs = self.capture("declared-complete", [{"path": "src/nested", "kind": "tree"}],
                                             "tree")
        self.assertEqual(manifest["selectors"], [{"path": "src/nested", "kind": "tree"}])
        bundle, bundle_path = self.import_report(tree_inputs, receipt_id="tree-receipt")
        baseline = self.assess(self.check(bundle_path, "--trusted-producer", PRODUCER))
        self.assertTrue(baseline["inputs_current"])
        self.assertTrue(baseline["passed_current"])

        added = self.root / "src/nested/added.ts"
        added.write_text("export const added = 3;\n")
        changed = self.assess(self.check(bundle_path, "--trusted-producer", PRODUCER))
        self.assertFalse(changed["inputs_current"])
        self.assertTrue(changed["changed_selectors"])
        self.assertFalse(changed["passed_current"])

        added.unlink()
        (self.root / "src/nested/value.ts").unlink()
        removed = self.assess(self.check(bundle_path, "--trusted-producer", PRODUCER))
        self.assertFalse(removed["inputs_current"])
        self.assertTrue(removed["changed_selectors"])
        self.assertFalse(removed["passed_current"])
        self.assertEqual(bundle["receipts"][0]["inputs"]["inputs_id"], manifest["inputs_id"])
        self.assertTrue(file_inputs.exists())

    def test_capture_missing_selector_tracks_later_file_creation_as_drift(self):
        manifest, inputs = self.capture("declared-complete", [{"path": "src/generated.ts", "kind": "file"}],
                                        "missing")
        self.assertEqual(manifest["observations"][0]["observation"], "missing")
        _, bundle = self.import_report(inputs, receipt_id="missing-receipt")
        baseline = self.assess(self.check(bundle, "--trusted-producer", PRODUCER))
        self.assertTrue(baseline["inputs_current"])
        self.assertTrue(baseline["passed_current"])

        (self.root / "src/generated.ts").write_text("export const generated = true;\n")
        created = self.assess(self.check(bundle, "--trusted-producer", PRODUCER))
        self.assertFalse(created["inputs_current"])
        self.assertTrue(created["changed_selectors"])
        self.assertFalse(created["passed_current"])

    def test_capture_rejects_overlap_symlink_rootescape_and_selector_caps(self):
        cases = {
            "overlap": (self.scope_request("partial", [{"path": "src", "kind": "tree"},
                                                            {"path": "src/selected.ts", "kind": "file"}]),
                        "overlapping-execution-selectors"),
            "root": (self.scope_request("partial", [{"path": ".", "kind": "tree"}]),
                     "invalid-execution-selector"),
            "rootescape": (self.scope_request("partial", [{"path": "../outside.ts", "kind": "file"}]),
                           "invalid-execution-selector"),
            "profile-with-selector": (self.scope_request("profile", [{"path": "src/selected.ts", "kind": "file"}]),
                                      "invalid-execution-selectors"),
            "partial-without-selector": (self.scope_request("partial", []), "invalid-execution-selectors"),
        }
        outside = self.artifacts / "outside.ts"
        outside.write_text("export const outside = true;\n")
        (self.root / "src/link.ts").symlink_to(outside)
        cases["symlink"] = (self.scope_request("partial", [{"path": "src/link.ts", "kind": "file"}]),
                            "unsafe-execution-selector")
        selectors = [{"path": f"execution-inputs/cap-{index}.ts", "kind": "file"} for index in range(33)]
        cases["selector-cap"] = (self.scope_request("partial", selectors), "invalid-execution-selectors")
        for name, (request, error_code) in cases.items():
            with self.subTest(name=name):
                path = self.artifacts / f"{name}.json"
                write_json(path, request)
                self.reject("receipt-scope", "--input", str(path), "--task-id", TASK_ID,
                            "--thread-id", THREAD_ID, "--observed-at", "1000", error_code=error_code)
        inside_request = self.root / "receipt-scope-request.json"
        write_json(inside_request, self.scope_request("partial", [{"path": "src/selected.ts", "kind": "file"}]))
        self.reject("receipt-scope", "--input", str(inside_request), "--task-id", TASK_ID,
                    "--thread-id", THREAD_ID, "--observed-at", "1000")
        duplicate = self.artifacts / "duplicate-scope.json"
        duplicate.write_text('{"schema_version":"shin.execution-input-request.v1",'
                             '"schema_version":"shin.execution-input-request.v1"}')
        self.reject("receipt-scope", "--input", str(duplicate), "--task-id", TASK_ID,
                    "--thread-id", THREAD_ID, "--observed-at", "1000")

    def test_capture_rejects_file_tree_depth_and_byte_caps(self):
        def reject_cap(name, prepare, error_code):
            case_root = self.root / "execution-inputs" / name
            case_root.mkdir(parents=True)
            try:
                selector = prepare(case_root)
                path = self.artifacts / f"cap-{name}.json"
                write_json(path, self.scope_request("partial", [selector]))
                self.reject("receipt-scope", "--input", str(path), "--task-id", TASK_ID,
                            "--thread-id", THREAD_ID, "--observed-at", "1000", error_code=error_code)
            finally:
                shutil.rmtree(case_root)

        def file_over_cap(case_root):
            (case_root / "value.ts").write_bytes(b"x" * (16 * 1024 * 1024 + 1))
            return {"path": "execution-inputs/file-bytes/value.ts", "kind": "file"}

        def total_over_cap(case_root):
            for index in range(5):
                (case_root / f"{index}.ts").write_bytes(b"x" * (13 * 1024 * 1024))
            return {"path": "execution-inputs/total-bytes", "kind": "tree"}

        def depth_over_cap(case_root):
            current = case_root
            for index in range(33):
                current /= f"depth{index}"
            current.mkdir(parents=True)
            (current / "value.ts").write_text("export const tooDeep = true;\n")
            return {"path": "execution-inputs/depth", "kind": "tree"}

        def files_over_cap(case_root):
            for index in range(8193):
                (case_root / f"{index}.ts").write_text("x\n")
            return {"path": "execution-inputs/file-count", "kind": "tree"}

        for name, prepare, error_code in (
            ("file-bytes", file_over_cap, "execution-input-too-large"),
            ("total-bytes", total_over_cap, "execution-input-too-large"),
            ("depth", depth_over_cap, "execution-input-budget"),
            ("file-count", files_over_cap, "execution-input-budget"),
        ):
            with self.subTest(name=name):
                reject_cap(name, prepare, error_code)

    def test_imports_junit_and_playwright_pass_fail_empty_skipped_and_flaky_without_payloads(self):
        _, inputs = self.capture("profile", [], "reports")
        cases = [
            ("junit-pass.xml", "junit", "pass"), ("junit-fail.xml", "junit", "fail"),
            ("junit-empty.xml", "junit", None), ("junit-skipped.xml", "junit", "skipped"),
            ("junit-flaky.xml", "junit", None), ("playwright-pass.json", "playwright", "pass"),
            ("playwright-fail.json", "playwright", "fail"), ("playwright-empty.json", "playwright", None),
            ("playwright-skipped.json", "playwright", "skipped"),
            ("playwright-flaky.json", "playwright", None),
        ]
        for index, (report, report_format, expected_result) in enumerate(cases):
            with self.subTest(report=report):
                bundle, bundle_path = self.import_report(
                    inputs, report, receipt_id=f"report-{index}", report_format=report_format)
                result = bundle["receipts"][0]["result"]
                if expected_result is None:
                    self.assertIn(result, {"fail", "unknown", "skipped"})
                else:
                    self.assertEqual(result, expected_result)
                assessment = self.assess(self.check(bundle_path, "--trusted-producer", PRODUCER))
                self.assertEqual(assessment["passed_current"], result == "pass")

    def test_import_rejects_unsafe_artifacts_malformed_and_corrupt_manifests(self):
        _, inputs = self.capture("partial", [{"path": "src/selected.ts", "kind": "file"}], "unsafe")
        report_path = self.artifacts / "junit-pass.xml"
        shutil.copyfile(REPORTS / "junit-pass.xml", report_path)
        base_request = {
            "schema_version": "shin.receipt-import-request.v1", "format": "junit",
            "report_path": str(report_path), "inputs_path": str(inputs), "receipt_id": "unsafe-receipt",
            "producer": PRODUCER, "started_at": 1001, "observed_at": 1002, "ttl_seconds": 60,
            "command_fingerprint": FINGERPRINT, "complete": True,
        }
        cases = {
            "incomplete": {**base_request, "complete": False},
            "bad-format": {**base_request, "format": "unknown"},
            "missing-field": {key: value for key, value in base_request.items() if key != "producer"},
        }
        inside_inputs = self.root / "inputs.json"
        shutil.copyfile(inputs, inside_inputs)
        cases["inside-inputs"] = {**base_request, "inputs_path": str(inside_inputs)}
        inputs_link = self.artifacts / "inputs-link.json"
        inputs_link.symlink_to(inputs)
        cases["symlink-inputs"] = {**base_request, "inputs_path": str(inputs_link)}
        link = self.artifacts / "report-link.xml"
        link.symlink_to(report_path)
        cases["symlink-report"] = {**base_request, "report_path": str(link)}
        corrupt = json.loads(inputs.read_text())
        corrupt["inputs_id"] = "fnv1a64:0000000000000001"
        corrupt_path = self.artifacts / "corrupt-inputs.json"
        write_json(corrupt_path, corrupt)
        cases["corrupt-inputs"] = {**base_request, "inputs_path": str(corrupt_path)}
        for field in ("inputs_id", "observations"):
            missing = json.loads(inputs.read_text())
            missing.pop(field)
            missing_path = self.artifacts / f"missing-{field}.json"
            write_json(missing_path, missing)
            cases[f"missing-{field}"] = {**base_request, "inputs_path": str(missing_path)}
        for name, request in cases.items():
            with self.subTest(name=name):
                path = self.artifacts / f"unsafe-{name}.json"
                write_json(path, request)
                self.reject("receipt-import", "--input", str(path), "--task-id", TASK_ID,
                            "--thread-id", THREAD_ID, "--observed-at", "1003")

        inside_request = self.root / "receipt-import-request.json"
        write_json(inside_request, base_request)
        self.reject("receipt-import", "--input", str(inside_request), "--task-id", TASK_ID,
                    "--thread-id", THREAD_ID, "--observed-at", "1003")

        duplicate = self.artifacts / "duplicate-import.json"
        duplicate.write_text('{"schema_version":"shin.receipt-import-request.v1",'
                             '"schema_version":"shin.receipt-import-request.v1"}')
        self.reject("receipt-import", "--input", str(duplicate), "--task-id", TASK_ID,
                    "--thread-id", THREAD_ID, "--observed-at", "1003")
        malformed = self.artifacts / "malformed-inputs.json"
        malformed.write_text('{"schema_version":"shin.execution-inputs.v1",')
        request = {**base_request, "inputs_path": str(malformed)}
        path = self.artifacts / "malformed-input-request.json"
        write_json(path, request)
        self.reject("receipt-import", "--input", str(path), "--task-id", TASK_ID,
                    "--thread-id", THREAD_ID, "--observed-at", "1003")

        inside_report = self.root / "test-output/junit-pass.xml"
        inside_report.parent.mkdir()
        shutil.copyfile(REPORTS / "junit-pass.xml", inside_report)
        allowed = {**base_request, "report_path": str(inside_report), "receipt_id": "inside-report"}
        allowed_path = self.artifacts / "inside-report-request.json"
        write_json(allowed_path, allowed)
        result = self.json_command("receipt-import", "--input", str(allowed_path), "--task-id", TASK_ID,
                                   "--thread-id", THREAD_ID, "--observed-at", "1003")
        self.assertEqual(result["receipts"][0]["receipt_id"], "inside-report")

    def test_option_values_that_look_like_flags_remain_bound_values(self):
        task_id = "--strict"
        thread_id = "--jobs"
        producer = "--strict"
        _, inputs = self.capture("partial", [{"path": "src/selected.ts", "kind": "file"}], "flag-values",
                                 task_id=task_id, thread_id=thread_id)
        _, bundle = self.import_report(inputs, receipt_id="flag-values", producer=producer,
                                       task_id=task_id, thread_id=thread_id)
        receipt = self.assess(self.check(bundle, "--trusted-producer", producer, task_id=task_id,
                                          thread_id=thread_id))
        self.assertTrue(receipt["passed_current"])

    def test_import_rejects_invalid_temporal_order_and_source_drift_before_or_during_import(self):
        _, inputs = self.capture("partial", [{"path": "src/selected.ts", "kind": "file"}], "timing")
        invalid_times = [
            ("captured-after-started", 999, 1002, 1003),
            ("started-after-observed", 1002, 1001, 1003),
            ("observed-after-importer", 1001, 1004, 1003),
        ]
        for name, started, observed, now in invalid_times:
            with self.subTest(name=name):
                request = {
                    "schema_version": "shin.receipt-import-request.v1", "format": "junit",
                    "report_path": str(REPORTS / "junit-pass.xml"), "inputs_path": str(inputs),
                    "receipt_id": name, "producer": PRODUCER, "started_at": started,
                    "observed_at": observed, "ttl_seconds": 60, "command_fingerprint": FINGERPRINT,
                    "complete": True,
                }
                path = self.artifacts / f"{name}.json"
                write_json(path, request)
                self.reject("receipt-import", "--input", str(path), "--task-id", TASK_ID,
                            "--thread-id", THREAD_ID, "--observed-at", str(now))

        (self.root / "src/selected.ts").write_text("export const selectedValue = 9;\n")
        request = {
            "schema_version": "shin.receipt-import-request.v1", "format": "junit",
            "report_path": str(REPORTS / "junit-pass.xml"), "inputs_path": str(inputs),
            "receipt_id": "pre-drift", "producer": PRODUCER, "started_at": 1001,
            "observed_at": 1002, "ttl_seconds": 60, "command_fingerprint": FINGERPRINT, "complete": True,
        }
        path = self.artifacts / "pre-drift.json"
        write_json(path, request)
        self.reject("receipt-import", "--input", str(path), "--task-id", TASK_ID,
                    "--thread-id", THREAD_ID, "--observed-at", "1003")

        (self.root / "src/selected.ts").write_text("export const selectedValue = 1;\n")
        _, current_inputs = self.capture("partial", [{"path": "src/selected.ts", "kind": "file"}], "post-drift")
        large_report = self.artifacts / "large-junit.xml"
        large_report.write_text("<testsuite tests=\"1\"><testcase name=\"x\"><system-out>" +
                                "x" * (3 * 1024 * 1024) + "</system-out></testcase></testsuite>")
        concurrent = {**request, "inputs_path": str(current_inputs), "report_path": str(large_report),
                      "receipt_id": "post-drift"}
        concurrent_path = self.artifacts / "post-drift.json"
        write_json(concurrent_path, concurrent)
        stop = threading.Event()

        def mutate_source():
            value = 2
            while not stop.is_set():
                (self.root / "src/selected.ts").write_text(f"export const selectedValue = {value};\n")
                value = 1 if value == 2 else 2

        thread = threading.Thread(target=mutate_source)
        thread.start()
        try:
            process = subprocess.run(self.argv("receipt-import", "--input", str(concurrent_path),
                                               "--task-id", TASK_ID, "--thread-id", THREAD_ID,
                                               "--observed-at", "1003"), capture_output=True, text=True,
                                     env=self.environment, timeout=90)
            self.assertNotEqual(process.returncode, 0, process.stderr or process.stdout)
            self.assertEqual(process.stdout, "", "concurrent source drift emitted a receipt")
            self.assertNotIn(SECRET, process.stderr)
            self.assertFalse(self.sentinel.exists(), "import executed a consumer sentinel")
        finally:
            stop.set()
            thread.join(timeout=5)

    def test_declared_complete_narrows_only_current_exact_selectors_and_partial_does_not(self):
        _, declared_inputs = self.capture("declared-complete", [{"path": "src/selected.ts", "kind": "file"}],
                                          "declared")
        _, declared_bundle = self.import_report(declared_inputs, receipt_id="declared")
        baseline = self.assess(self.check(declared_bundle, "--trusted-producer", PRODUCER))
        self.assertTrue(baseline["passed_current"])

        (self.root / "docs/unselected.md").write_text("# Changed Documentation\n")
        docs_drift = self.assess(self.check(declared_bundle, "--trusted-producer", PRODUCER))
        self.assertTrue(docs_drift["inputs_current"])
        self.assertTrue(docs_drift["narrowed_scope_used"])
        self.assertTrue(docs_drift["passed_current"])

        (self.root / "src/selected.ts").write_text("export const selectedValue = 3;\n")
        selected_drift = self.assess(self.check(declared_bundle, "--trusted-producer", PRODUCER))
        self.assertFalse(selected_drift["inputs_current"])
        self.assertTrue(selected_drift["changed_selectors"])
        self.assertFalse(selected_drift["passed_current"])

        (self.root / "src/selected.ts").write_text("export const selectedValue = 1;\n")
        _, partial_inputs = self.capture("partial", [{"path": "src/selected.ts", "kind": "file"}], "partial")
        _, partial_bundle = self.import_report(partial_inputs, receipt_id="partial")
        (self.root / "docs/unselected.md").write_text("# Changed Again\n")
        partial_drift = self.assess(self.check(partial_bundle, "--trusted-producer", PRODUCER))
        self.assertTrue(partial_drift["inputs_current"])
        self.assertFalse(partial_drift["narrowed_scope_used"])
        self.assertFalse(partial_drift["passed_current"])

    def test_receipt_check_trust_time_binding_strict_and_require_current_are_independent(self):
        _, inputs = self.capture("declared-complete", [{"path": "src/selected.ts", "kind": "file"}], "check")
        _, bundle = self.import_report(inputs, receipt_id="check-receipt")
        bundle_link = self.artifacts / "check-receipt-link.json"
        bundle_link.symlink_to(bundle)
        self.reject("receipt-check", "--input", str(bundle_link), "--task-id", TASK_ID,
                    "--thread-id", THREAD_ID, "--observed-at", "1003")
        for name, change in (
            ("retained-report-payload", lambda receipt: receipt["report"].update({"payload_retained": True})),
            ("contradictory-report-summary", lambda receipt: receipt["report"]["summary"].update({"passed": 0})),
        ):
            with self.subTest(name=name):
                tampered = json.loads(bundle.read_text())
                change(tampered["receipts"][0])
                tampered_path = self.artifacts / f"{name}.json"
                write_json(tampered_path, tampered)
                self.reject("receipt-check", "--input", str(tampered_path), "--task-id", TASK_ID,
                            "--thread-id", THREAD_ID, "--observed-at", "1003",
                            error_code="invalid-receipt-report")
        untrusted = self.assess(self.check(bundle))
        self.assertFalse(untrusted["passed_current"])
        trusted = self.assess(self.check(bundle, "--trusted-producer", PRODUCER))
        self.assertTrue(trusted["passed_current"])
        self.assertFalse(self.check(bundle, "--strict", expected=(1,))["ok"])
        self.assertTrue(self.check(bundle, "--trusted-producer", PRODUCER, "--strict")["ok"])
        _, failed_bundle = self.import_report(inputs, "junit-fail.xml", receipt_id="check-failed")
        mixed = json.loads(bundle.read_text())
        mixed["receipts"].extend(json.loads(failed_bundle.read_text())["receipts"])
        mixed_path = self.artifacts / "mixed-bundle.json"
        write_json(mixed_path, mixed)
        mixed_assessment = self.check(mixed_path, "--trusted-producer", PRODUCER, "--strict", expected=(1,),
                                      receipt_count=2)
        self.assertFalse(mixed_assessment["ok"])
        self.assertEqual([receipt["passed_current"] for receipt in mixed_assessment["receipts"]], [True, False])
        self.assertFalse(self.assess(self.check(bundle, "--trusted-producer", PRODUCER, now=1001))["passed_current"])
        self.assertFalse(self.assess(self.check(bundle, "--trusted-producer", PRODUCER, now=1062))["passed_current"])

        mismatched_task = self.json_command("receipt-check", "--input", str(bundle), "--task-id", "other-task",
                                            "--thread-id", THREAD_ID, "--observed-at", "1003",
                                            "--trusted-producer", PRODUCER)
        self.assertFalse(mismatched_task["receipts"][0]["passed_current"])

        other_root = self.base / "other-consumer"
        other_out = self.base / "other-index"
        shutil.copytree(FIXTURE / "root", other_root)
        self.build(root=other_root, out=other_out)
        root_result = self.json_command("receipt-check", "--input", str(bundle), "--task-id", TASK_ID,
                                        "--thread-id", THREAD_ID, "--observed-at", "1003",
                                        "--trusted-producer", PRODUCER, root=other_root, out=other_out)
        self.assertFalse(root_result["receipts"][0]["passed_current"])

        other_profiles = self.base / "other-profiles"
        shutil.copytree(self.profiles, other_profiles)
        changed_profile = other_profiles / "other-profile.json"
        (other_profiles / self.profile_path.name).rename(changed_profile)
        value = json.loads(changed_profile.read_text())
        value["id"] = "other-profile"
        write_json(changed_profile, value)
        profile_out = self.base / "profile-index"
        self.build(profiles=other_profiles, out=profile_out, profile_id="other-profile")
        profile_result = self.json_command("receipt-check", "--input", str(bundle), "--task-id", TASK_ID,
                                           "--thread-id", THREAD_ID, "--observed-at", "1003",
                                           "--trusted-producer", PRODUCER, profiles=other_profiles,
                                           out=profile_out, profile_id="other-profile")
        self.assertFalse(profile_result["receipts"][0]["passed_current"])

        (self.root / "docs/unselected.md").write_text("# Stale Index\n")
        stale = self.check(bundle, "--trusted-producer", PRODUCER)
        self.assertTrue(self.assess(stale)["passed_current"])
        self.reject("receipt-check", "--input", str(bundle), "--task-id", TASK_ID, "--thread-id", THREAD_ID,
                    "--observed-at", "1003", "--trusted-producer", PRODUCER, "--require-current",
                    error_code="execution-index-not-current")
        duplicate_bundle = self.artifacts / "duplicate-bundle.json"
        duplicate_bundle.write_text('{"schema_version":"shin.receipt-bundle.v1",'
                                    '"schema_version":"shin.receipt-bundle.v1"}')
        self.reject("receipt-check", "--input", str(duplicate_bundle), "--task-id", TASK_ID,
                    "--thread-id", THREAD_ID, "--observed-at", "1003")


    def test_offline_host_persists_typed_documents_and_preserves_checkpoint_on_failure(self):
        self.environment = {key: value for key, value in self.environment.items()
                            if not key.startswith("GIT_")}
        self.environment["PATH"] = ""
        self.artifacts.chmod(0o700)
        self.build()

        def persist(process, schema, destination):
            # Host policy, not a new Core writer or an HTTP envelope assumption.
            if process.returncode:
                return False
            payload = json.loads(process.stdout)
            if payload.get("schema_version") != schema or payload.get("ok") is False:
                return False
            descriptor, temporary = tempfile.mkstemp(dir=destination.parent)
            try:
                with os.fdopen(descriptor, "w") as output:
                    output.write(process.stdout)
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temporary, destination)
            finally:
                Path(temporary).unlink(missing_ok=True)
            self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o600)
            return True

        task = ["--task-id", TASK_ID, "--thread-id", THREAD_ID]
        scope_path = self.artifacts / "host-scope.json"
        write_json(scope_path, self.scope_request("profile", []))
        scope = self.invoke("receipt-scope", "--input", str(scope_path), *task,
                            "--observed-at", "1000", "--require-current")
        self.assertNotIn("ok", json.loads(scope.stdout))
        inputs_path = self.artifacts / "host-inputs.json"
        self.assertTrue(persist(scope, "shin.execution-inputs.v1", inputs_path))

        xml = '<testsuite tests="1" failures="0" errors="0" skipped="0"><testcase name="exists"/></testsuite>'
        command = [sys.executable, "-c",
                   "from pathlib import Path; import sys; "
                   "assert Path(sys.argv[1]).is_file(); "
                   f"print({xml!r})",
                   str(self.root / "src/selected.ts")]
        run = subprocess.run(command, capture_output=True, text=True, env=self.environment, timeout=10)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = self.artifacts / "host-report.xml"
        report.write_text(run.stdout)
        fingerprint = lambda argv: "sha256:" + hashlib.sha256(
            json.dumps(argv, ensure_ascii=False, separators=(",", ":")).encode("utf-8")).hexdigest()
        self.assertEqual(" ".join(["a b", "c"]), " ".join(["a", "b c"]))
        self.assertNotEqual(fingerprint(["a b", "c"]), fingerprint(["a", "b c"]))
        request = self.artifacts / "host-import.json"
        write_json(request, {"schema_version": "shin.receipt-import-request.v1", "format": "junit",
                            "report_path": str(report), "inputs_path": str(inputs_path),
                            "receipt_id": "host-check", "producer": PRODUCER,
                            "started_at": 1001, "observed_at": 1002, "ttl_seconds": 60,
                            "command_fingerprint": fingerprint(command), "complete": True})
        imported = self.invoke("receipt-import", "--input", str(request), *task, "--observed-at", "1003")
        bundle_path = self.artifacts / "host-bundle.json"
        self.assertTrue(persist(imported, "shin.receipt-bundle.v1", bundle_path))
        bundle = json.loads(bundle_path.read_text())
        self.assertEqual(bundle["receipts"][0]["command_fingerprint"], fingerprint(command))
        self.assertFalse(self.assess(self.check(bundle_path))["passed_current"])
        self.assertTrue(self.assess(self.check(bundle_path, "--trusted-producer", PRODUCER))["passed_current"])

        draft_path = self.artifacts / "host-draft.json"
        write_json(draft_path, {"schema_version": "shin.session-draft.v1",
                              "task_request": self.profile["packEvalCases"][0]["request"],
                              "state": {"current_slice": "Offline host workflow", "assertions": [],
                                        "changed_paths": [], "identifiers": [], "next_actions": []},
                              "receipts": bundle["receipts"], "vcs": None, "ttl_seconds": 3600})
        capture = ["checkpoint", "--input", str(draft_path), *task, "--observed-at", "1003", "--require-current"]
        checkpoint = self.invoke(*capture)
        self.assertNotIn("ok", json.loads(checkpoint.stdout))
        checkpoint_path = self.artifacts / "host-checkpoint.json"
        self.assertTrue(persist(checkpoint, "shin.session-checkpoint.v1", checkpoint_path))
        saved = checkpoint_path.read_bytes()
        self.assertFalse(persist(scope, "shin.session-checkpoint.v1", checkpoint_path))
        self.assertEqual(checkpoint_path.read_bytes(), saved)
        resumed = self.json_command("resume", "--checkpoint", str(checkpoint_path), *task,
                                    "--observed-at", "1003", "--require-current",
                                    "--trusted-producer", PRODUCER)
        self.assertTrue(resumed["continuation_ready"])
        self.assertTrue(resumed["receipts"][0]["passed_current"])
        expired = self.json_command("resume", "--checkpoint", str(checkpoint_path), *task,
                                    "--observed-at", "1063", "--trusted-producer", PRODUCER)
        self.assertFalse(expired["receipts"][0]["usable"])

        (self.root / "src/selected.ts").write_text("// source changed after the authorized check\n")
        rejected = self.invoke(*capture, expected=tuple(range(1, 256)))
        self.assertFalse(persist(rejected, "shin.session-checkpoint.v1", checkpoint_path))
        self.assertEqual(checkpoint_path.read_bytes(), saved)
        stale = self.json_command("resume", "--checkpoint", str(checkpoint_path), *task,
                                 "--observed-at", "1003", "--require-current",
                                 "--trusted-producer", PRODUCER, expected=(1,))
        self.assertFalse(stale["continuation_ready"])
        self.assertFalse(stale["receipts"][0]["usable"])

    def test_resume_reuses_bundle_assessment_after_declared_scope_drift(self):
        _, inputs = self.capture("declared-complete", [{"path": "src/selected.ts", "kind": "file"}], "resume")
        bundle, bundle_path = self.import_report(inputs, receipt_id="resume-receipt")
        receipt = bundle["receipts"][0]
        draft = {
            "schema_version": "shin.session-draft.v1",
            "task_request": self.profile["packEvalCases"][0]["request"],
            "state": {
                "current_slice": "Execution evidence resume fixture",
                "assertions": [{"id": "receipt", "kind": "completed", "text": "External test passed.",
                                "paths": ["src/selected.ts"], "receipt_ids": [receipt["receipt_id"]]}],
                "changed_paths": ["src/selected.ts"], "identifiers": [], "next_actions": []
            },
            "receipts": bundle["receipts"], "vcs": None, "ttl_seconds": 3600,
        }
        draft_path = self.artifacts / "session-draft.json"
        write_json(draft_path, draft)
        checkpoint = self.json_command("checkpoint", "--input", str(draft_path), "--task-id", TASK_ID,
                                       "--thread-id", THREAD_ID, "--observed-at", "1003", "--require-current")
        checkpoint_path = self.artifacts / "checkpoint.json"
        write_json(checkpoint_path, checkpoint)
        checked = self.assess(self.check(bundle_path, "--trusted-producer", PRODUCER))
        resumed = self.json_command("resume", "--checkpoint", str(checkpoint_path), "--task-id", TASK_ID,
                                    "--thread-id", THREAD_ID, "--observed-at", "1003",
                                    "--trusted-producer", PRODUCER)
        resumed_receipt = resumed["receipts"][0]
        for field in ("inputs_current", "changed_selectors", "narrowed_scope_used", "passed_current"):
            self.assertEqual(resumed_receipt[field], checked[field])

        (self.root / "docs/unselected.md").write_text("# Resume Documentation Drift\n")
        checked_drift = self.assess(self.check(bundle_path, "--trusted-producer", PRODUCER))
        resumed_drift = self.json_command("resume", "--checkpoint", str(checkpoint_path), "--task-id", TASK_ID,
                                          "--thread-id", THREAD_ID, "--observed-at", "1003",
                                          "--trusted-producer", PRODUCER, expected=(0, 1))
        for field in ("inputs_current", "changed_selectors", "narrowed_scope_used", "passed_current"):
            self.assertEqual(resumed_drift["receipts"][0][field], checked_drift[field])

    def test_impact_keeps_receipts_separate_from_static_tests_and_never_executes(self):
        _, inputs = self.capture("profile", [], "impact")
        _, bundle = self.import_report(inputs, receipt_id="impact-receipt")
        result = self.json_command("impact", "--receipts", str(bundle), "--task-id", TASK_ID,
                                   "--thread-id", THREAD_ID, "--trusted-producer", PRODUCER,
                                   "--observed-at", "1003")
        self.assertEqual(result["schema_version"], "shin.impact-pack.v1")
        self.assertEqual(result["receipts_observed_at"], 1003)
        self.assertIn("receipts", result)
        self.assertEqual(len(result["receipts"]), 1)
        self.assertTrue(result["policy"]["read_only"])
        self.assertFalse(result["policy"]["actions_executed"])
        for review in result["tests_to_review"]:
            self.assertEqual(review["execution"], "not-run")
            self.assertTrue(review["requires_review"])


if __name__ == "__main__":
    unittest.main()
