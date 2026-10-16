"""CLI/native parity coverage for the extracted session evaluator API."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest


CLI = str(Path(sys.argv.pop(1)).resolve())
BRIDGE = str(Path(sys.argv.pop(1)).resolve())
SOURCE_ROOT = Path(sys.argv.pop(1)).resolve()
FIXTURE = SOURCE_ROOT / "tests" / "fixtures" / "basic"
TASK_ID = "native-session-task"
THREAD_ID = "native-session-thread"
PRODUCER = "fixture-runner"


def compact(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def tree_image(root):
    image = {}
    for path in [root, *sorted(root.rglob("*"))]:
        metadata = path.lstat()
        key = str(path.relative_to(root))
        if stat.S_ISREG(metadata.st_mode):
            content = ("file", hashlib.sha256(path.read_bytes()).hexdigest())
        elif stat.S_ISLNK(metadata.st_mode):
            content = ("symlink", os.readlink(path))
        else:
            content = ("directory",)
        image[key] = (metadata.st_mode, metadata.st_size, metadata.st_mtime_ns,
                      metadata.st_ctime_ns, metadata.st_ino, content)
    return image


class SessionEvaluationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-session-evaluation-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root = self.base / "root"
        self.profiles = self.base / "profiles"
        self.out = self.base / "index"
        self.state = self.base / "state"
        self.state.mkdir()
        shutil.copytree(FIXTURE / "root", self.root)
        shutil.copytree(FIXTURE / "profiles", self.profiles)
        self.profile = "fixture"
        self.run_cli("build", expected=(0,))

    def watched(self):
        return {str(path): tree_image(path) for path in (self.root, self.profiles, self.out)}

    def assert_read_only(self, before):
        after = self.watched()
        changed = [f"{root}/{path}" for root in before for path in sorted(
            before[root].keys() | after[root].keys())
            if before[root].get(path) != after[root].get(path)]
        self.assertEqual(changed, [], f"session evaluation mutated fixture state: {changed[:8]}")

    def cli_argv(self, command, *args):
        return [CLI, "--profile", self.profile, "--profile-dir", str(self.profiles),
                "--root", str(self.root), "--out-dir", str(self.out), command, *args]

    def run_cli(self, command, *args, expected=(0,)):
        process = subprocess.run(self.cli_argv(command, *args), text=True, capture_output=True, timeout=90)
        self.assertIn(process.returncode, expected, process.stderr or process.stdout)
        return process

    def session_draft(self, receipts=None, assertions=None):
        profile = json.loads((self.profiles / "fixture.json").read_text())
        request = profile["packEvalCases"][0]["request"]
        return {
            "schema_version": "shin.session-draft.v1",
            "task_request": request,
            "state": {
                "current_slice": "Evaluate the native session contract.",
                "assertions": assertions if assertions is not None else [{
                    "id": "authority", "kind": "invariant",
                    "text": "The host retains external authority.",
                    "paths": ["docs/architecture.md"], "receipt_ids": []}],
                "changed_paths": ["docs/architecture.md"],
                "identifiers": ["triggerWidget"],
                "next_actions": [{"id": "review", "text": "Review the current fixture contract."}],
            },
            "receipts": receipts or [], "vcs": None, "ttl_seconds": 3600,
        }

    def native(self, command, temperature, raw, *, task=TASK_ID, thread=THREAD_ID, now=1000,
               require_current=False, jobs=1, current_task="-", producers="-", mismatch=None,
               expected=(0,)):
        argv = [BRIDGE, command, temperature, str(self.root), str(self.profiles), str(self.out),
                self.profile, task, thread, str(now), "1" if require_current else "0", str(jobs),
                current_task, producers]
        if mismatch:
            argv.append(mismatch)
        process = subprocess.run(argv, input=raw, text=True, capture_output=True, timeout=90)
        self.assertIn(process.returncode, expected, process.stderr or process.stdout)
        return process

    def canonical_success(self, command, raw, *, now, **kwargs):
        state_path = self.state / f"{command}-{now}.json"
        state_path.write_text(raw)
        input_flag = "--input" if command == "checkpoint" else "--checkpoint"
        cli = self.run_cli(command, input_flag, str(state_path), "--task-id", kwargs.get("task", TASK_ID),
                           "--thread-id", kwargs.get("thread", THREAD_ID), "--observed-at", str(now),
                           *( ["--require-current"] if kwargs.get("require_current", False) else [] ),
                           "--jobs", str(kwargs.get("jobs", 1)),
                           *( ["--task", kwargs["current_task"]] if kwargs.get("current_task", "-") != "-" else [] ),
                           *sum((["--trusted-producer", item] for item in kwargs.get("producers", "-").split(",")
                                  if item != "-"), []), expected=kwargs.get("expected", (0,)))
        cold = self.native(command, "cold", raw, now=now, **kwargs)
        warm = self.native(command, "warm", raw, now=now, **kwargs)
        self.assertEqual(cold.stdout, cli.stdout, f"cold native {command} output differs from CLI")
        self.assertEqual(warm.stdout, cli.stdout, f"warm native {command} output differs from CLI")
        self.assertEqual(cold.stderr, "", cold.stderr)
        self.assertEqual(warm.stderr, "", warm.stderr)
        return json.loads(cli.stdout)

    def canonical_impact(self, raw="-", *, now, require_current=False, producers="-", expected=(0,)):
        args = ["impact"]
        if raw != "-":
            receipt_path = self.state / f"impact-receipts-{now}.json"
            receipt_path.write_text(raw)
            args.extend(["--receipts", str(receipt_path), "--task-id", TASK_ID,
                         "--thread-id", THREAD_ID, "--observed-at", str(now)])
            for producer in producers.split(","):
                if producer != "-":
                    args.extend(["--trusted-producer", producer])
        if require_current:
            args.append("--require-current")
        cli = self.run_cli(*args, expected=expected)
        cold = self.native("impact", "cold", raw, now=now, require_current=require_current,
                           producers=producers, expected=expected)
        warm = self.native("impact", "warm", raw, now=now, require_current=require_current,
                           producers=producers, expected=expected)
        self.assertEqual(cold.stdout, cli.stdout, "cold native impact output differs from CLI")
        self.assertEqual(warm.stdout, cli.stdout, "warm native impact output differs from CLI")
        self.assertEqual(cold.stderr, "", cold.stderr)
        self.assertEqual(warm.stderr, "", warm.stderr)
        return json.loads(cli.stdout)

    def assert_rejected(self, raw, *, jobs=1, producers="-", expected_code=None):
        path = self.state / "rejected.json"
        path.write_text(raw)
        cli = self.run_cli("checkpoint", "--input", str(path), "--task-id", TASK_ID,
                           "--thread-id", THREAD_ID, "--observed-at", "1000", expected=range(1, 256))
        self.assertEqual(cli.stdout, "")
        expected = json.loads(cli.stderr)
        self.assertNotIn("sk-", cli.stderr)
        for temperature in ("cold", "warm"):
            native = self.native("checkpoint", temperature, raw, now=1000, jobs=jobs,
                                 producers=producers, expected=range(1, 256))
            self.assertEqual(native.stdout, "")
            self.assertEqual(native.stderr, cli.stderr,
                             f"{temperature} native parser rejection differs from CLI")
            actual = json.loads(native.stderr)
            self.assertEqual(actual["error"]["category"], "usage")
            self.assertNotIn("sk-", native.stderr)
            if expected_code is not None:
                self.assertEqual(actual["error"]["code"], expected_code)
                self.assertEqual(expected["error"]["code"], expected_code)

    def test_checkpoint_and_resume_current_cold_warm_byte_evidence_parity(self):
        before = self.watched()
        checkpoint = self.canonical_success("checkpoint", compact(self.session_draft()), now=1000,
                                            require_current=True)
        self.assertEqual(checkpoint["schema_version"], "shin.session-checkpoint.v1")
        self.assertTrue(checkpoint["baseline"]["index_current"])
        self.assertTrue(checkpoint["baseline"]["observation_complete"])
        self.assertIn("docs/architecture.md", [row["path"] for row in checkpoint["tracked_sources"]])
        resume = self.canonical_success("resume", compact(checkpoint), now=1020, require_current=True)
        self.assertEqual(resume["schema_version"], "shin.session-resume.v1")
        self.assertEqual(resume["status"], "current")
        self.assertTrue(resume["index_current"])
        self.assertTrue(resume["continuation_ready"])
        self.assert_read_only(before)

    def test_drift_receipt_expiry_untrusted_and_binding_mismatch(self):
        checkpoint = self.canonical_success("checkpoint", compact(self.session_draft()), now=1000,
                                            require_current=True)
        self.root.joinpath("docs/architecture.md").write_text(
            self.root.joinpath("docs/architecture.md").read_text() + "\nNative evaluator drift.\n")
        drift = self.canonical_success("resume", compact(checkpoint), now=1020)
        self.assertEqual(drift["status"], "drifted")
        self.assertFalse(drift["index_current"])
        self.assertTrue(any(row["path"] == "docs/architecture.md" and row["status"] == "changed"
                            for row in drift["checkpoint_drift"]["paths"]))

        self.run_cli("build")
        initial = self.canonical_success("checkpoint", compact(self.session_draft()), now=2000,
                                         require_current=True)
        scope = dict(initial["binding"], snapshot_id=initial["baseline"]["snapshot_id"],
                     worktree_id=initial["baseline"]["worktree_id"])
        receipt = {"schema_version": "shin.execution-receipt.v1", "receipt_id": "gate",
                   "kind": "test", "producer": PRODUCER, "observed_at": 2000, "ttl_seconds": 10,
                   "result": "pass", "scope": scope,
                   "command_fingerprint": "fnv1a64:0000000000000000"}
        with_receipt = self.session_draft(receipts=[receipt], assertions=[{
            "id": "gate", "kind": "completed", "text": "Fixture gate completed.",
            "paths": ["docs/architecture.md"], "receipt_ids": ["gate"]}])
        receipt_checkpoint = self.canonical_success("checkpoint", compact(with_receipt), now=2000,
                                                    require_current=True)
        expired = self.canonical_success("resume", compact(receipt_checkpoint), now=2020)
        assessed = expired["receipts"][0]
        self.assertEqual(assessed["age"], "expired")
        self.assertFalse(assessed["producer_trusted"])
        self.assertFalse(assessed["passed_current"])
        self.assertTrue(expired["review_required"])

        mismatch = self.canonical_success("resume", compact(receipt_checkpoint), now=2001,
                                          thread="other-thread", expected=(1,))
        self.assertEqual(mismatch["status"], "mismatched")
        self.assertIn("thread_id", mismatch["binding_differences"])
        self.assertIsNone(mismatch["continuation_pack"])

    def test_exact_host_policy_does_not_trust_names_or_bypass_applicability(self):
        initial = self.canonical_success("checkpoint", compact(self.session_draft()), now=2000)
        scope = dict(initial["binding"], snapshot_id=initial["baseline"]["snapshot_id"],
                     worktree_id=initial["baseline"]["worktree_id"])
        receipt = {"schema_version": "shin.execution-receipt.v1", "receipt_id": "exact",
                   "kind": "test", "producer": PRODUCER, "observed_at": 2000, "ttl_seconds": 10,
                   "result": "pass", "scope": scope,
                   "command_fingerprint": "fnv1a64:0000000000000000"}
        sibling = dict(receipt, receipt_id="same-producer")
        checkpoint = self.canonical_success("checkpoint", compact(self.session_draft(receipts=[receipt, sibling])), now=2000)
        for temperature in ("cold", "warm"):
            for command in ("resume", "impact"):
                document = checkpoint if command == "resume" else {"schema_version": "shin.receipt-bundle.v1",
                                                                   "receipts": [receipt, sibling]}
                def assess(trusted, now=2001):
                    envelope = compact({"document": document, "trusted_documents": trusted})
                    return json.loads(self.native("host-" + command, temperature, envelope, now=now).stdout)["receipts"]
                rows = assess([compact(receipt)])
                self.assertTrue(rows[0]["producer_trusted"])
                self.assertTrue(rows[0]["passed_current"])
                self.assertEqual(rows[0]["trust_source"], "explicit-host-receipt-policy")
                self.assertFalse(rows[0]["command_independently_verified"])
                self.assertFalse(rows[1]["producer_trusted"])
                self.assertFalse(assess([compact(dict(receipt, result="fail"))])[0]["producer_trusted"])
                expired = assess([compact(receipt)], now=2020)[0]
                self.assertTrue(expired["producer_trusted"])
                self.assertFalse(expired["usable"])
                self.assertEqual(expired["age"], "expired")
        envelope = {"document": checkpoint, "trusted_documents": [compact(receipt)]}
        path = self.root / "docs/architecture.md"
        path.write_text(path.read_text() + "\nChanged after the test.\n")
        drift = json.loads(self.native("host-resume", "warm", compact(envelope), now=2001).stdout)["receipts"][0]
        self.assertTrue(drift["producer_trusted"])
        self.assertFalse(drift["usable"])
        for policy in ([str(i) for i in range(33)], ["x" * (256 * 1024 + 1)]):
            rejected = self.native("host-resume", "cold", compact(dict(envelope, trusted_documents=policy)),
                                   expected=range(1, 256))
            self.assertEqual(json.loads(rejected.stderr)["error"]["code"], "invalid-trusted-receipts")
        rejected = self.native("host-checkpoint", "cold",
                               compact({"document": self.session_draft(), "trusted_documents": [compact(receipt)]}),
                               expected=range(1, 256))
        self.assertNotEqual(rejected.returncode, 0)

    def test_typed_options_and_strict_document_rejections(self):
        draft = compact(self.session_draft())
        for jobs in (-1, 9):
            with self.subTest(jobs=jobs):
                native = self.native("checkpoint", "cold", draft, jobs=jobs, expected=range(1, 256))
                self.assertEqual(native.stdout, "")
                self.assertEqual(json.loads(native.stderr)["error"]["code"], "invalid-option-value")
        producers = ",".join(f"runner-{index}" for index in range(17))
        native = self.native("resume", "cold", draft, producers=producers, expected=range(1, 256))
        self.assertEqual(native.stdout, "")
        self.assertEqual(json.loads(native.stderr)["error"]["code"], "invalid-trusted-producer")
        invalid_time = self.native("checkpoint", "cold", draft, now=-1, expected=range(1, 256))
        self.assertEqual(json.loads(invalid_time.stderr)["error"]["code"], "invalid-session-time")
        invalid_identity = self.native("checkpoint", "cold", draft, task="", expected=range(1, 256))
        self.assertEqual(json.loads(invalid_identity.stderr)["error"]["code"], "invalid-session-identity")

        deep = '{"a":' * 33 + "null" + "}" * 33
        secret = "sk-" + "Ab9z" * 12
        cases = [("duplicate", '{"schema_version":"a","schema_version":"b"}', "duplicate-session-field"),
                 ("deep", deep, "session-input-too-deep"),
                 ("bytes", " " * (256 * 1024 + 1), "session-input-too-large"),
                 ("secret", compact({"value": secret}), "secret-like-session-content")]
        for name, raw, code in cases:
            with self.subTest(document=name):
                self.assert_rejected(raw, expected_code=code)

    def test_loaded_snapshot_scope_rejections_and_shared_snapshot_concurrency(self):
        raw = compact(self.session_draft())
        for mismatch in ("root", "profile", "profile-config", "out"):
            with self.subTest(scope=mismatch):
                native = self.native("checkpoint", "warm", raw, mismatch=mismatch, expected=range(1, 256))
                self.assertEqual(native.stdout, "")
                error = json.loads(native.stderr)
                self.assertEqual(error["error"]["code"], "session-snapshot-binding-mismatch")

        initial = self.canonical_success("checkpoint", raw, now=3000, require_current=True)
        copied_profile = self.native("checkpoint", "warm", raw, now=3000, require_current=True,
                                     mismatch="profile-copy")
        self.assertEqual(json.loads(copied_profile.stdout), initial)
        scope = dict(initial["binding"], snapshot_id=initial["baseline"]["snapshot_id"],
                     worktree_id=initial["baseline"]["worktree_id"])
        receipt = {"schema_version": "shin.execution-receipt.v1", "receipt_id": "shared",
                   "kind": "test", "producer": PRODUCER, "observed_at": 3000, "ttl_seconds": 60,
                   "result": "pass", "scope": scope,
                   "command_fingerprint": "fnv1a64:0000000000000000"}
        checkpoint = self.canonical_success(
            "checkpoint", compact(self.session_draft(receipts=[receipt], assertions=[{
                "id": "shared", "kind": "completed", "text": "Shared fixture gate completed.",
                "paths": ["docs/architecture.md"], "receipt_ids": ["shared"]}])),
            now=3000, require_current=True)
        parallel = self.native("parallel-resume", "warm", compact(checkpoint), now=3010,
                               require_current=True, producers=PRODUCER)
        envelope = json.loads(parallel.stdout)
        self.assertEqual(envelope["schema_version"], "shin.session-evaluation-fixture.v1")
        self.assertEqual(len(envelope["outputs"]), 8)
        expected = self.canonical_success("resume", compact(checkpoint), now=3010,
                                          require_current=True, producers=PRODUCER)
        self.assertTrue(all(output == expected for output in envelope["outputs"]),
                        "shared snapshot produced divergent evaluator outputs")

    def test_loaded_snapshot_replacement_cannot_claim_current_new_generation(self):
        checkpoint = self.canonical_success("checkpoint", compact(self.session_draft()), now=4000,
                                            require_current=True)
        control = self.state / "replacement-control"
        control.mkdir()
        argv = [BRIDGE, "staged-resume", "warm", str(self.root), str(self.profiles), str(self.out),
                self.profile, TASK_ID, THREAD_ID, "4010", "1", "1", "-", "-", str(control)]
        process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True)
        self.addCleanup(self.stop_process, process)
        assert process.stdin is not None
        process.stdin.write(compact(checkpoint))
        process.stdin.close()
        process.stdin = None
        deadline = time.monotonic() + 30
        while not (control / "first-complete").exists():
            self.assertLess(time.monotonic(), deadline, "native fixture did not release its first call")
            time.sleep(0.01)
        architecture = self.root / "docs" / "architecture.md"
        architecture.write_text(architecture.read_text() + "\nReplacement generation evidence.\n")
        self.run_cli("build")
        (control / "continue").write_text("continue\n")
        stdout, stderr = process.communicate(timeout=90)
        self.assertEqual(process.returncode, 1, stderr or stdout)
        self.assertEqual(stderr, "", stderr)
        outputs = json.loads(stdout)["outputs"]
        self.assertTrue(outputs[0]["index_current"])
        self.assertFalse(outputs[1]["index_current"])
        self.assertFalse(outputs[1]["continuation_ready"])
        self.assertTrue(outputs[1]["same_snapshot"])
        self.assertFalse(outputs[1]["same_worktree"])
        self.assertEqual(outputs[1]["snapshot_id"], outputs[0]["snapshot_id"])

    @staticmethod
    def stop_process(process):
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=5)

    def test_impact_default_drift_and_receipt_ttl_cold_warm_parity(self):
        current = self.canonical_impact(now=5000, require_current=True)
        self.assertEqual(current["schema_version"], "shin.impact-pack.v1")
        self.assertTrue(current["index_current"])
        self.assertEqual(current["coverage"]["max_evidence_bytes"], 32768)

        architecture = self.root / "docs" / "architecture.md"
        architecture.write_text(architecture.read_text() + "\nImpact evaluator drift.\n")
        drift = self.canonical_impact(now=5010)
        self.assertFalse(drift["index_current"])
        self.assertTrue(any(item["path"] == "docs/architecture.md" for item in drift["reread"]))

        self.run_cli("build")
        scoped = self.canonical_impact(now=5020, require_current=True)
        scope = {"root": str(self.root.resolve()), "profile": self.profile,
                 "profile_config_hash": scoped["worktree"]["profile_config_hash"],
                 "task_id": TASK_ID, "thread_id": THREAD_ID,
                 "worktree_id": scoped["worktree"]["worktree_id"], "snapshot_id": scoped["snapshot_id"]}
        receipt = {"schema_version": "shin.execution-receipt.v1", "receipt_id": "impact-ttl",
                   "kind": "test", "producer": PRODUCER, "observed_at": 5020, "ttl_seconds": 10,
                   "result": "pass", "scope": scope,
                   "command_fingerprint": "fnv1a64:0000000000000000"}
        bundle = compact({"schema_version": "shin.receipt-bundle.v1", "tool": "shin-context",
                          "command": "receipt-import", "ok": True, "receipts": [receipt]})
        expired = self.canonical_impact(bundle, now=5040, require_current=True, producers=PRODUCER)
        assessed = expired["receipts"][0]
        self.assertTrue(assessed["producer_trusted"])
        self.assertEqual(assessed["age"], "expired")
        self.assertFalse(assessed["passed_current"])


if __name__ == "__main__":
    if len(sys.argv) != 1:
        raise SystemExit("usage: run_session_evaluation_tests.py <shin-context> <native-fixture> <source-root>")
    unittest.main(verbosity=2)
