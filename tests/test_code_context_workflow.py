#!/usr/bin/env python3
"""Delivery-gate tests independent of a model, provider or installed client."""

import argparse
import io
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

import code_context_workflow as workflow
from code_context_workflow import Captured, Workflow, TOOL_NAME, SCHEMAS
from run_code_context_pilot import prepare, serve, RequestInput, SOURCE, QUESTION, query, write, support
from shin_semantic.artifact import Rejected, decode, encode

SID = "fnv1a64:0123456789abcdef"
OTHER = "fnv1a64:1123456789abcdef"
CORE = None


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class Backend:
    def __init__(self, clock):
        self.clock, self.calls, self.change = clock, [], None
        self.seconds = 0.1

    def invoke(self, operation, argument, timeout):
        self.calls.append((operation, argument, timeout))
        self.clock.now += self.seconds
        value = {"schema_version": SCHEMAS[operation], "ok": True,
                 "native": {"snapshot_id": SID, "index_freshness": {
                     "snapshot_id": SID, "current": True, "fully_verified": True},
                     "records": [{"path": "src/flow.cpp", "id": "operation", "excerpt": "bounded evidence"}]}}
        if self.change:
            self.change(value)
        return Captured(encode(value) + b"\n", 0 if value["ok"] else 1, self.seconds)


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.backend = Backend(self.clock)
        self.flow = Workflow(self.backend, {"src/flow.cpp"}, clock=self.clock)

    def call(self, operation="query", argument="Find the flow"):
        return decode(self.flow.invoke(TOOL_NAME, {"operation": operation, "argument": argument}))

    def test_full_workflow_preserves_delivered_bytes_and_snapshot(self):
        self.call()
        self.call("inspect", "src/flow.cpp")
        request = {"schema_version": "shin.source-read-request.v1", "snapshot_id": SID, "record_ids": ["operation"]}
        self.call("read", encode(request).decode())
        receipt = self.flow.receipt()
        self.assertEqual((receipt["calls"], receipt["rounds"], receipt["snapshot_id"]), (3, 1, SID))
        self.assertEqual(receipt["delivered_bytes"], sum(len(item.encode()) for item in self.flow.trace))
        self.assertTrue(all(step["evidence_delivered"] for step in receipt["steps"]))
        self.assertIsNone(receipt["provider_usage"])
        self.assertIsNone(receipt["core_phase_timings"])
        self.assertAlmostEqual(receipt["tool_wall_seconds"], 0.3)

    def test_remaining_budget_includes_itself_and_repeated_responses(self):
        for operation, argument in (("query", "Find flow"), ("inspect", "src/flow.cpp")):
            value = self.call(operation, argument)
            budget = value["budget_remaining"]
            self.assertEqual(budget["response_bytes"], workflow.MAX_RESPONSE_BYTES - self.flow.delivered_bytes)
            self.assertEqual(budget["calls"], 12 - self.flow.calls)
            self.assertEqual(budget["discovery_rounds"], 3 - self.flow.rounds)
            self.assertEqual(budget["reserved_error_bytes"], 512)
        self.assertEqual(self.flow.delivered_bytes, sum(len(row.encode()) for row in self.flow.trace))

    def test_remaining_budget_is_exact_across_decimal_widths(self):
        for limit in (1999, 2000, 9999, 10000, 65536):
            self.setUp()
            with patch.object(workflow, "MAX_RESPONSE_BYTES", limit):
                value = self.call()
                self.assertTrue(value["ok"], value)
                self.assertEqual(value["budget_remaining"]["response_bytes"], limit - self.flow.delivered_bytes)
                self.assertLessEqual(len(self.flow.trace[-1]) - len(self.flow.trace[-1].rstrip()), 5)

    def test_quota_drains_queued_calls_without_backend_or_more_evidence(self):
        self.call()
        self.backend.change = lambda value: value["native"].update(private_marker="X" * 65536)
        result = self.call("inspect", "src/flow.cpp")
        self.assertTrue(result["final_answer_allowed"])
        calls, attempts = len(self.backend.calls), self.flow.calls
        for _ in range(11):
            self.assertEqual(decode(self.flow.cancel_queued()), {"cancelled": True})
        self.assertEqual((len(self.backend.calls), self.flow.calls), (calls, attempts))
        self.assertEqual(self.flow.cancelled_calls, 11)
        self.assertLessEqual(sum(step["delivered_bytes"] for step in self.flow.steps if not step["evidence_delivered"]), 512)
        self.assertEqual(self.flow.delivered_bytes, sum(len(row.encode()) for row in self.flow.trace))
        with self.assertRaisesRegex(Rejected, "pilot-task-already-stopped"):
            self.flow.cancel_queued()
        self.assertNotIn("XXXX", str(self.flow.trace))

    def test_security_refusal_and_expired_quota_never_allow_finalization_drain(self):
        result = self.call("inspect", "src/flow.cpp")
        self.assertFalse(result["final_answer_allowed"])
        with self.assertRaises(Rejected):
            self.flow.cancel_queued()
        self.setUp()
        self.backend.change = lambda value: value["native"].update(private_marker="X" * 65536)
        self.call()
        self.clock.now = 181
        self.assertFalse(self.flow.can_finalize())
        with self.assertRaises(Rejected):
            self.flow.cancel_queued()

    def test_changed_generation_is_discarded_before_delivery_and_stops(self):
        self.call()

        def changed(value):
            value["native"]["snapshot_id"] = value["native"]["index_freshness"]["snapshot_id"] = OTHER
            value["native"]["private_marker"] = "NOT_TO_MODEL"

        self.backend.change = changed
        result = self.call("inspect", "src/flow.cpp")
        self.assertEqual(result["error"]["code"], "pilot-snapshot-changed")
        self.assertTrue(result["restart_needed"])
        self.assertNotIn("NOT_TO_MODEL", self.flow.trace[-1])
        with self.assertRaisesRegex(Rejected, "pilot-task-already-stopped"):
            self.call()
        self.assertEqual(len(self.backend.calls), 2)

    def test_read_rejects_mismatched_requested_snapshot_before_backend(self):
        self.call()
        result = self.call("read", encode({"snapshot_id": OTHER}).decode())
        self.assertEqual(result["error"]["code"], "pilot-snapshot-changed")
        self.assertEqual(len(self.backend.calls), 1)

    def test_large_response_is_refused_without_clipping_or_raw_payload(self):
        self.backend.change = lambda value: value["native"].update(private_marker="X" * 66000)
        result = self.call()
        self.assertEqual(result["error"]["code"], "pilot-response-budget")
        self.assertNotIn("XXXX", self.flow.trace[-1])
        self.assertLess(self.flow.delivered_bytes, 512)
        self.assertGreater(self.flow.steps[0]["captured_bytes"], 65536)

    def test_repeated_results_are_charged_each_time(self):
        self.backend.change = lambda value: value["native"].update(repeated_text="q" * 29000)
        self.call()
        self.call("inspect", "src/flow.cpp")
        result = self.call("inspect", "src/flow.cpp")
        self.assertEqual(result["error"]["code"], "pilot-response-budget")
        self.assertGreater(self.flow.delivered_bytes, 58000)
        self.assertLessEqual(self.flow.delivered_bytes, 65536)

    def test_round_and_inspection_limits_cannot_be_reset_by_other_operations(self):
        for _ in range(3):
            self.call()
        self.assertEqual(self.call()["error"]["code"], "pilot-round-budget")
        self.assertEqual(len(self.backend.calls), 3)
        self.setUp()
        self.call()
        for _ in range(3):
            self.call("inspect", "src/flow.cpp")
        self.call("pack", '{"schema_version":"shin.task-request.v1"}')
        self.assertEqual(self.call("inspect", "src/flow.cpp")["error"]["code"], "pilot-inspection-budget")
        self.assertEqual(len(self.backend.calls), 5)

    def test_call_budget_refuses_thirteenth_backend_invocation(self):
        self.call()
        for _ in range(11):
            self.call("pack", '{"schema_version":"shin.task-request.v1"}')
        self.assertEqual(self.call("pack", "{}")["error"]["code"], "pilot-call-budget")
        self.assertEqual(len(self.backend.calls), 12)
        self.assertEqual((self.flow.receipt()["calls"], self.flow.receipt()["requests"]), (12, 13))
        self.assertEqual(self.flow.receipt()["backend_calls"], 12)

    def test_finish_cannot_hide_expired_task_and_is_idempotent(self):
        self.call()
        self.clock.now = 181
        result = decode(self.flow.close())
        self.assertEqual(result["error"]["code"], "pilot-task-deadline")
        self.assertEqual(self.flow.receipt()["status"], "stopped")
        self.assertEqual(self.flow.receipt()["requests"], 1)
        self.assertIsNone(self.flow.close())
        self.assertEqual(len(self.flow.steps), 2)
        self.setUp()
        self.assertIsNone(self.flow.close())
        self.assertEqual(self.flow.receipt()["status"], "closed")
        with self.assertRaisesRegex(Rejected, "pilot-task-already-stopped"):
            self.call()

    def test_disallowed_input_and_nested_result_paths_are_never_delivered(self):
        self.call()
        self.assertEqual(self.call("inspect", "tests/rubric.json")["error"]["code"], "pilot-corpus-violation")
        self.assertEqual(len(self.backend.calls), 1)
        self.setUp()
        self.backend.change = lambda value: value.update(navigation={"origin": {"path": "tests/rubric.json", "text": "ORACLE"}})
        self.assertEqual(self.call()["error"]["code"], "pilot-corpus-violation")
        self.assertNotIn("ORACLE", self.flow.trace[-1])

    def test_unknown_tools_and_malformed_actions_cannot_reach_backend(self):
        for name, args in (("terminal", {"command": "read secrets"}),
                           (TOOL_NAME, {"operation": [], "argument": "x"}),
                           (TOOL_NAME, {"operation": "query", "argument": "x", "root": "/elsewhere"})):
            self.setUp()
            value = decode(self.flow.invoke(name, args))
            self.assertFalse(value["ok"])
            self.assertEqual(self.backend.calls, [])

    def test_valid_incomplete_pack_is_not_confused_with_refused_admission(self):
        self.call()
        self.backend.change = lambda value: value.update(ok=False)
        result = self.call("pack", '{"schema_version":"shin.task-request.v1"}')
        self.assertFalse(result["ok"])
        self.assertIsNone(self.flow.stopped)
        self.assertEqual(result["native"]["snapshot_id"], SID)
        self.backend.change = lambda value: value.pop("native")
        self.assertEqual(self.call()["error"]["code"], "pilot-admission-rejected")

    def test_deadline_and_actual_backend_time_are_enforced(self):
        self.clock.now = 181
        self.assertEqual(self.call()["error"]["code"], "pilot-task-deadline")
        self.assertEqual(self.backend.calls, [])
        self.setUp()
        self.backend.seconds = 31
        self.assertEqual(self.call()["error"]["code"], "pilot-tool-time-budget")
        self.assertEqual(self.flow.receipt()["tool_wall_seconds"], 31)
        self.setUp()
        self.call()
        self.backend.seconds = 6
        self.assertEqual(self.call("inspect", "src/flow.cpp")["error"]["code"], "pilot-native-time-budget")

    def test_snapshot_and_response_schema_fail_closed(self):
        for update in (lambda value: value.update(schema_version="wrong.v1"),
                       lambda value: value["native"]["index_freshness"].update(fully_verified=False),
                       lambda value: value["native"].update(snapshot_id="not-a-snapshot")):
            self.setUp()
            self.backend.change = update
            self.assertFalse(self.call()["ok"])
            self.assertFalse(self.flow.steps[0]["evidence_delivered"])

    def test_unexpected_exception_does_not_leak_private_diagnostics(self):
        def fail(*args):
            self.clock.now += 2
            raise RuntimeError("PRIVATE_DIAGNOSTIC")

        self.backend.invoke = fail
        self.assertEqual(self.call()["error"]["code"], "pilot-operation-rejected")
        self.assertNotIn("PRIVATE_DIAGNOSTIC", self.flow.trace[-1])
        self.assertEqual(self.flow.receipt()["tool_wall_seconds"], 2)


class InputTests(unittest.TestCase):
    def setUp(self):
        read_fd, self.writer = os.pipe()
        self.stream = os.fdopen(read_fd, "rb", buffering=0)
        self.addCleanup(self.stream.close)
        self.addCleanup(os.close, self.writer)
        self.reader = RequestInput(self.stream)

    def test_idle_and_partial_lines_obey_deadline(self):
        for prefix in (b"", b'{"name":'):
            with self.subTest(prefix=prefix), patch.object(workflow, "TASK_SECONDS", 0.03):
                flow = Workflow(None, {"src/flow.cpp"})
                os.write(self.writer, prefix)
                self.assertEqual(self.reader.readline(flow), b"")
                self.assertEqual(decode(flow.close())["error"]["code"], "pilot-task-deadline")

    def test_buffered_lines_are_not_lost(self):
        os.write(self.writer, b"one\ntwo\n")
        flow = Workflow(None, {"src/flow.cpp"})
        self.assertEqual(self.reader.readline(flow), b"one\n")
        self.assertEqual(self.reader.readline(flow), b"two\n")


class CliWorkflowTests(unittest.TestCase):
    def setUp(self):
        if CORE is None:
            self.skipTest("requires explicit --core")
        self.temp = tempfile.TemporaryDirectory(prefix="shin-p3-tests-")
        self.addCleanup(self.temp.cleanup)
        self.output = Path(self.temp.name).resolve() / "receipt"
        self.flow, self.fixture, self.before = prepare(CORE, Path(sys.executable), self.output)
        self.addCleanup(shutil.rmtree, self.fixture.base.parent)

    def call(self, operation="query", argument=QUESTION):
        return decode(self.flow.invoke(TOOL_NAME, {"operation": operation, "argument": argument}))

    def rebuild(self):
        result = query(self.flow.backend.session.core_arguments() + ["build"])
        self.assertEqual(result["exit_code"], 0, result)

    def test_query_inspect_read_exact_bodies_with_truthful_optional_fallback(self):
        discovered = self.call()
        self.assertTrue(discovered["ok"], discovered)
        self.assertEqual(discovered["semantic_status"], "artifact-unavailable-lexical-only")
        # Exercise a second question, not an evaluator-supplied path or symbol.
        discovered = self.call(argument="feed cache")
        paths = [item["path"] for item in discovered["native"]["files"] if item["path"].endswith(".ts")]
        self.assertEqual(len(paths), 1)
        outline = self.call("inspect", paths[0])["native"]
        request = {"schema_version": "shin.source-read-request.v1", "snapshot_id": outline["snapshot_id"],
                   "record_ids": [row["id"] for row in outline["records"]]}
        result = self.call("read", encode(request).decode())
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["schema_version"], "shin.agent-read.v2")
        native = support.expand_read(result)["native"]
        self.assertTrue(native["complete"])
        self.assertEqual(len(native["evidence"]["records"]), 4)
        self.assertEqual(len(native["evidence"]["bodies"]), 2)
        lines = SOURCE.splitlines(keepends=True)
        for body in native["evidence"]["bodies"]:
            self.assertEqual(body["text"], "".join(lines[body["start_line"] - 1:body["end_line"]]))
        self.assertEqual(self.before, (support.snapshot(self.fixture.root), support.snapshot(self.fixture.out)))
        self.assertEqual(list(self.flow.backend.scratch.iterdir()), [])
        self.flow.close()
        self.assertEqual(self.flow.receipt()["status"], "closed")
        self.assertEqual(self.flow.receipt()["backend_calls"], 4)
        self.assertEqual(self.flow.receipt()["rounds"], 2)
        self.assertEqual(self.flow.delivered_bytes, sum(len(item.encode()) for item in self.flow.trace))

    def test_query_then_file_read_without_inventory(self):
        found = self.call(argument="feed cache")
        paths = [item["path"] for item in found["native"]["files"] if item["path"].endswith(".ts")]
        self.assertEqual(len(paths), 1)
        result = self.call("read", encode({"schema_version": "shin.source-read-request.v2",
            "snapshot_id": found["native"]["snapshot_id"], "file_paths": paths}).decode())
        self.assertTrue(result["native"]["complete"], result)
        self.assertEqual(support.expand_read(result)["native"]["evidence"]["bodies"][0]["text"], SOURCE)
        self.assertEqual(self.flow.receipt()["backend_calls"], 2)
        self.assertEqual(self.before, (support.snapshot(self.fixture.root), support.snapshot(self.fixture.out)))

    def test_stale_source_refuses_without_repair_or_body(self):
        self.call()
        write(self.fixture.root / "src/feed.ts", SOURCE + "// PRIVATE_NEW_SOURCE\n")
        before = support.snapshot(self.fixture.root), support.snapshot(self.fixture.out)
        result = self.call("inspect", "src/feed.ts")
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "pilot-admission-rejected")
        self.assertNotIn("PRIVATE_NEW_SOURCE", self.flow.trace[-1])
        self.assertEqual(before, (support.snapshot(self.fixture.root), support.snapshot(self.fixture.out)))

    def test_new_publication_cannot_mix_generations(self):
        self.call()
        original = self.flow.snapshot_id
        write(self.fixture.root / "src/feed.ts", SOURCE + "// new publication\n")
        self.rebuild()
        result = self.call("inspect", "src/feed.ts")
        self.assertEqual(result["error"]["code"], "pilot-snapshot-changed")
        self.assertTrue(result["restart_needed"])
        self.assertEqual(self.flow.snapshot_id, original)
        self.assertNotIn("records", result)

    def test_changed_route_stops_before_cli(self):
        self.call()
        path = self.flow.backend.session.path
        path.write_bytes(path.read_bytes() + b" ")
        result = self.call("inspect", "src/feed.ts")
        self.assertEqual(result["error"]["code"], "pilot-route-changed")
        self.assertTrue(result["restart_needed"])
        self.assertIsNone(self.flow.steps[-1]["cli_wall_seconds"])

    def test_cli_timeout_reaps_child_and_removes_request(self):
        processes = []
        original = workflow.subprocess.Popen

        def capture(*args, **kwargs):
            process = original(*args, **kwargs)
            processes.append(process)
            return process

        with patch.object(workflow.subprocess, "Popen", side_effect=capture):
            with self.assertRaisesRegex(Rejected, "pilot-tool-timeout"):
                self.flow.backend.invoke("read", {"schema_version": "shin.source-read-request.v1"}, 0.000001)
        self.assertEqual(len(processes), 1)
        self.assertIsNotNone(processes[0].poll())
        self.assertEqual(list(self.flow.backend.scratch.iterdir()), [])

    def test_indexed_but_undeclared_document_never_reaches_agent(self):
        write(self.fixture.root / "README.md", "# Evaluator rubric\n\nORACLE_PRIVATE_RUBRIC\n")
        self.rebuild()
        self.flow = Workflow(self.flow.backend, {"src/feed.ts"})
        result = self.call(argument="ORACLE_PRIVATE_RUBRIC")
        self.assertEqual(result["error"]["code"], "pilot-corpus-violation")
        self.assertNotIn("ORACLE_PRIVATE_RUBRIC", self.flow.trace[-1])

    def test_incomplete_pack_delivers_native_gap_not_false_success(self):
        self.call()
        task = {"schema_version": "shin.task-request.v1", "request_id": "gap",
            "task": {"objective": "Find an absent operation", "scope": [], "should": [],
                     "must": [{"id": "gap", "query": "UNMATCHABLE_MISSING_SYMBOL", "require_full_coverage": True}]},
            "evidence_types": ["fact"], "budget": {"max_evidence_bytes": 8192, "max_records": 3,
                                                   "per_query_records": 3, "per_document_records": 3}}
        result = self.call("pack", encode(task).decode())
        self.assertFalse(result["ok"])
        self.assertIn("native", result)
        self.assertFalse(result["native"]["completion"]["complete"])
        self.assertIsNone(self.flow.stopped)

    def run_broker(self, data):
        with tempfile.TemporaryFile() as stream:
            stream.write(data)
            stream.seek(0)
            output = io.StringIO()
            code = serve(self.flow, self.fixture, self.before, self.output, stream, output)
        return code, [decode(line.encode()) for line in output.getvalue().splitlines()]

    def test_json_lines_broker_preserves_tool_bytes_and_private_receipt(self):
        request = {"name": TOOL_NAME, "arguments": {"operation": "query", "argument": QUESTION}}
        code, delivered = self.run_broker(encode(request) + b'\n{"finish":true}\n')
        self.assertEqual(code, 0, delivered)
        self.assertEqual([item.get("type", item.get("schema_version")) for item in delivered],
                         ["ready", "shin.agent-query.v2", "closed"])
        receipt = decode((self.output / "receipt.json").read_bytes())
        self.assertEqual(receipt["delivered_bytes"], len(self.flow.trace[0].encode()))
        self.assertTrue(receipt["source_and_index_unchanged"])
        self.assertEqual(receipt["status"], "closed")
        for name in ("binding.json", "receipt.json", "trace.json"):
            self.assertEqual((self.output / name).stat().st_mode & 0o777, 0o600)

    def test_finish_after_deadline_is_not_success(self):
        self.flow.started -= 181
        code, delivered = self.run_broker(b'{"finish":true}\n')
        self.assertEqual(code, 1)
        self.assertEqual(delivered[-1]["receipt"]["stop_reason"], "pilot-task-deadline")
        self.assertFalse(delivered[-1]["ok"])

    def test_malformed_and_oversized_requests_do_not_run_cli(self):
        for data in (b'{"name":', b" " * (256 * 1024 + 1)):
            with self.subTest(bytes=len(data)):
                self.flow = Workflow(self.flow.backend, {"README.md", "src/feed.ts"})
                code, delivered = self.run_broker(data)
                self.assertEqual(code, 1)
                self.assertEqual(delivered[-1]["receipt"]["backend_calls"], 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--core", type=Path)
    args, remaining = parser.parse_known_args()
    CORE = args.core.resolve() if args.core else None
    unittest.main(argv=[sys.argv[0], *remaining])
