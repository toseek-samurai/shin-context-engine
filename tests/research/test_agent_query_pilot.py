#!/usr/bin/env python3
"""Negative controls for the independent, non-certifying pilot checker."""

import json
import subprocess
import unittest
from unittest.mock import patch

from check_agent_query_pilot import inspect
import run_agent_query_pilot as runner
from shin_semantic.artifact import Rejected


class PilotEnvironmentTests(unittest.TestCase):
    def test_explicit_symbolizer_does_not_widen_environment(self):
        with patch.dict(runner.os.environ, {"ASAN_SYMBOLIZER_PATH": "/tools/llvm-symbolizer",
                        "PATH": "/unexpected", "PRIVATE_TEST_SETTING": "not-forwarded"}, clear=True), \
             patch.object(runner.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, b'{}', b'')) as run:
            runner.query(["/test-core"])
            self.assertEqual(run.call_args.kwargs["env"], {
                "PATH": "", "LC_ALL": "C", "ASAN_SYMBOLIZER_PATH": "/tools/llvm-symbolizer"})

    def test_relative_symbolizer_and_diagnostics_still_refuse(self):
        with patch.dict(runner.os.environ, {"ASAN_SYMBOLIZER_PATH": "relative"}, clear=True), \
             patch.object(runner.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "absolute path"):
                runner.query(["/test-core"])
            run.assert_not_called()
        with patch.dict(runner.os.environ, {}, clear=True), \
             patch.object(runner.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, b'{}', b'diagnostic')):
            with self.assertRaisesRegex(ValueError, "unexpected diagnostics"):
                runner.query(["/test-core"])


class PilotCheckTests(unittest.TestCase):
    def fixture(self, stale=False):
        identity = "stale" if stale else "normal"
        argv = ["/python", "/shin.py", "--config", "/route.json", "question"]
        response = {"error": {"code": "core-admission-rejected"}} if stale else {"semantic_status": "available"}
        code = 1 if stale else 0
        answer = {"id": identity, "decision": "stop" if stale else "answer", "answer": None if stale else "A source-backed statement.",
                  "error": "core-admission-rejected" if stale else None, "semantic_status": None if stale else "available",
                  "readiness_evaluated": False, "tests_executed": False,
                  "evidence": [] if stale else [{"path": "rule.md", "start_line": 1, "end_line": 1}]}
        session = {"id": "fixture", "messages": [
            {"role": "assistant", "content": "", "tool_calls": [
                {"id": "q", "function": {"name": "terminal", "arguments": json.dumps({"command": "/python /shin.py --config /route.json question"})}}]},
            {"role": "tool", "tool_call_id": "q", "content": json.dumps({"exit_code": code, "output": json.dumps(response)})},
            {"role": "assistant", "content": "", "tool_calls": [
                {"id": "read", "function": {"name": "terminal", "arguments": json.dumps({"command": "more /source/rule.md"})}}]},
            {"role": "tool", "tool_call_id": "read", "content": json.dumps({"exit_code": 0, "output": "Rule.\n"})},
            {"role": "assistant", "content": json.dumps({"cases": [answer]})}]}
        binding = {"cases": [{"id": identity, "argv": argv, "root": "/source"}]}
        controls = {identity: {"exit_code": code, "response": response}}
        process = {"process_exit": 0, "fixture_unchanged": True}
        return session, binding, controls, process, {"/source/rule.md": "Rule.\n"}

    def test_checks_pass_without_self_certifying_answer_truth(self):
        result = inspect(*self.fixture())
        self.assertTrue(result["ok"])
        self.assertIn("not-decided", result["acceptance"])

    def test_wrong_exit_or_tool_response_is_not_a_pass(self):
        for change in ({"exit_code": 1, "output": '{"semantic_status":"available"}'},
                       {"exit_code": 0, "output": '{"semantic_status":"changed"}'}):
            args = self.fixture()
            args[0]["messages"][1]["content"] = json.dumps(change)
            with self.assertRaises(Rejected):
                inspect(*args)

    def test_correct_answer_without_source_read_is_rejected(self):
        args = self.fixture()
        args[0]["messages"][3]["content"] = '{"exit_code":0,"output":"different file"}'
        with self.assertRaisesRegex(Rejected, "pilot-source-read-not-observed"):
            inspect(*args)

    def test_invalid_coordinates_and_false_scope_claims_are_rejected(self):
        for field in ("coordinates", "tests_executed", "readiness_evaluated"):
            args = self.fixture()
            final = json.loads(args[0]["messages"][-1]["content"])
            if field == "coordinates":
                final["cases"][0]["evidence"][0]["end_line"] = 99
            else:
                final["cases"][0][field] = True
            args[0]["messages"][-1]["content"] = json.dumps(final)
            with self.assertRaises(Rejected):
                inspect(*args)

    def test_stale_must_not_return_an_answer(self):
        args = self.fixture(stale=True)
        self.assertTrue(inspect(*args)["ok"])
        final = json.loads(args[0]["messages"][-1]["content"])
        final["cases"][0]["answer"] = "A guess from the previous case."
        args[0]["messages"][-1]["content"] = json.dumps(final)
        with self.assertRaisesRegex(Rejected, "pilot-stale-answer"):
            inspect(*args)

    def test_mutated_fixture_or_failed_agent_process_is_rejected(self):
        for patch in ({"fixture_unchanged": False}, {"process_exit": 124}):
            args = self.fixture()
            args[3].update(patch)
            with self.assertRaises(Rejected):
                inspect(*args)


if __name__ == "__main__":
    unittest.main()
