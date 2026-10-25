#!/usr/bin/env python3
"""Offline boundary and paired-baseline tests; never calls a reasoning provider."""

import argparse
from copy import deepcopy
from pathlib import Path
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from code_context_pair import BaselineBackend, GUIDE, MODEL, PROVIDER, source_rows, audit_messages, check_answer, cohort_policy
from code_context_workflow import Workflow, TOOL_NAME
from test_code_context_workflow import Backend, Clock
from run_code_context_pilot import prepare, SOURCE, QUESTION, support
from run_code_context_hermes import PilotStop, agent_type
from shin_semantic.artifact import Rejected, decode, encode, sha

CORE = None


class PairTests(unittest.TestCase):
    def test_new_cohort_keeps_original_thresholds_but_not_old_prompts(self):
        old, old_sha = cohort_policy("p4-regression")
        new, new_sha = cohort_policy("file-first")
        self.assertEqual(old["acceptance"], new["acceptance"])
        self.assertNotEqual(old_sha, new_sha)
        self.assertEqual(len(new["topics"]), 4)
        self.assertFalse({row["id"] for row in old["topics"]} & {row["id"] for row in new["topics"]})

    def test_queued_quota_yields_one_final_request_without_tools(self):
        class Host:
            def _build_api_kwargs(self, messages, tools_for_api=None):
                return {"tools": tools_for_api, "input": [
                    {"type": "function_call_output", "output": row["content"]}
                    for row in messages if row["role"] == "tool"]}

        with tempfile.TemporaryDirectory() as directory:
            clock = Clock()
            backend = Backend(clock)
            backend.change = lambda value: value["native"].update(oversized="X" * 65536)
            agent = agent_type(Host)()
            agent.flow = Workflow(backend, {"src/flow.cpp"}, clock=clock)
            agent.model, agent.provider, agent.api_mode = MODEL, PROVIDER, "codex_responses"
            agent.question, agent.request_audits, agent.requests = "question", [], []
            agent.output = Path(directory)
            agent.tools = agent.pilot_tools = [{"type": "function", "function": {"name": TOOL_NAME}}]
            agent.pilot_messages = [{"role": "system", "content": GUIDE}, {"role": "user", "content": "question"}]
            call = SimpleNamespace(id="first", function=SimpleNamespace(name=TOOL_NAME,
                arguments=encode({"operation": "query", "argument": "flow"}).decode()))
            queued = SimpleNamespace(content=None, tool_calls=[call, SimpleNamespace(id="second", function=call.function)])
            messages = []
            agent._execute_tool_calls(queued, messages, None)
            self.assertEqual(len(backend.calls), 1)
            self.assertEqual(agent.flow.cancelled_calls, 1)
            result = agent._build_api_kwargs([])
            self.assertEqual(result["tools"], [])
            self.assertEqual([item["output"] for item in result["input"]], agent.flow.trace)
            with self.assertRaisesRegex(PilotStop, "pilot-finalization-not-allowed"):
                agent._build_api_kwargs([])
            with self.assertRaisesRegex(PilotStop, "pilot-agent-called-after-stop"):
                agent._execute_tool_calls(queued, messages, None)
            self.assertEqual(len(backend.calls), 1)

    def test_sources_are_identical_p0_product_bytes_not_reports_tests_or_docs(self):
        rows = [{"path": path, "text": "int f();\n", "sha256": sha(b"int f();\n")} for path in (
            "src/test.cpp", "observer/include/test.h", "observer/tests/oracle.cpp", "docs/machine/rubric.json",
            "AGENTS.md", "shared/observer-protocol/src/protocol.cpp", "shared/observer-protocol/tests/test.cpp")]
        selected = source_rows({"sources": rows})
        self.assertEqual([row["path"] for row in selected], [rows[i]["path"] for i in (0, 1, 5)])
        rows[0]["text"] = "changed"
        with self.assertRaisesRegex(Rejected, "paired-source-pin-mismatch"):
            source_rows({"sources": rows})

    def test_answer_citations_must_come_from_delivered_bodies_not_inventory(self):
        trace = [encode({"native": {"evidence": {"bodies": [{"path": "src/one.cpp", "start_line": 3,
            "end_line": 7, "text": "implementation", "state": "complete"}]}}}).decode()]
        answer = {"answer": "Supported claim", "claims": [{"text": "Claim", "evidence": [
            {"path": "src/one.cpp", "start_line": 3, "end_line": 7}]}], "gaps": [], "tests_executed": False}
        self.assertTrue(check_answer(answer, trace)["citations_delivered"])
        for key, value in (("start_line", 2), ("end_line", 8), ("path", "src/unread.cpp")):
            bad = deepcopy(answer)
            bad["claims"][0]["evidence"][0][key] = value
            with self.assertRaisesRegex(Rejected, "pilot-unread-citation"):
                check_answer(bad, trace)
        answer["tests_executed"] = True
        with self.assertRaisesRegex(Rejected, "invalid-pilot-answer"):
            check_answer(answer, trace)

    def test_request_audit_rejects_appended_trimmed_or_untracked_tool_bytes(self):
        flow = SimpleNamespace(trace=['{"evidence":1}\n'])
        messages = [{"role": "system", "content": GUIDE}, {"role": "user", "content": "question"},
                    {"role": "tool", "content": flow.trace[0]}]
        audit_messages(messages, flow, "question")
        for content in ('{"evidence":1}', '{"evidence":1}\nHost note', "UNTRACKED"):
            messages[-1]["content"] = content
            with self.assertRaisesRegex(Rejected, "pilot-tool-delivery-mismatch"):
                audit_messages(messages, flow, "question")

    def test_hermes_bridge_owns_only_guarded_tool_and_fixed_prompt(self):
        class Host:
            def _build_api_kwargs(self, messages, tools_for_api=None):
                self.sent = messages
                return {"input": [{"type": "function_call_output", "output": row["content"]}
                                  for row in messages if row["role"] == "tool"]}

        agent = agent_type(Host)()
        agent.flow = SimpleNamespace(trace=["guarded\n"], remaining_seconds=lambda: 10, stopped=None)
        agent.model, agent.provider, agent.api_mode = MODEL, PROVIDER, "codex_responses"
        agent.question, agent.request_audits = "question", []
        agent.tools = agent.pilot_tools = [{"type": "function", "function": {"name": TOOL_NAME}}]
        agent.pilot_messages = [{"role": "system", "content": GUIDE}, {"role": "user", "content": "question"},
                                {"role": "tool", "content": "guarded\n"}]
        agent._build_api_kwargs([{"role": "system", "content": "HOST_CONTEXT"}, {"role": "user", "content": "ORACLE"},
                                 {"role": "tool", "content": "guarded\n"}])
        self.assertNotIn("ORACLE", str(agent.sent))
        self.assertNotIn("HOST_CONTEXT", str(agent.sent))
        self.assertEqual(agent.request_audits[0]["tool_bytes"], 8)
        agent.pilot_messages[-1]["content"] = "untracked"
        with self.assertRaisesRegex(PilotStop, "pilot-tool-delivery-mismatch"):
            agent._build_api_kwargs([{"role": "tool", "content": "untracked"}])
        agent.tools = []
        with self.assertRaisesRegex(PilotStop, "pilot-tools-changed"):
            agent._build_api_kwargs([])


class BaselineTests(unittest.TestCase):
    def setUp(self):
        if CORE is None:
            self.skipTest("requires explicit --core")
        self.temp = tempfile.TemporaryDirectory(prefix="shin-p4-tests-")
        self.addCleanup(self.temp.cleanup)
        self.output = Path(self.temp.name).resolve() / "fixture"
        flow, self.fixture, _ = prepare(CORE, Path(sys.executable), self.output)
        self.addCleanup(shutil.rmtree, self.fixture.base.parent)
        binding = decode((self.output / "binding.json").read_bytes())
        self.backend = BaselineBackend(Path(binding["route"]), Path(sys.executable), flow.backend.scratch, binding["source_sha256"])
        self.flow = Workflow(self.backend, set(binding["source_sha256"]))
        self.call("query", "feed cache")

    def call(self, operation, argument):
        return decode(self.flow.invoke(TOOL_NAME, {"operation": operation, "argument": argument}))

    def request(self, **fields):
        return encode({"snapshot_id": self.flow.snapshot_id, "path": "src/feed.ts", **fields}).decode()

    def test_full_file_read_is_available_without_a_matching_declaration(self):
        before = support.snapshot(self.fixture.root), support.snapshot(self.fixture.out)
        result = self.call("read", self.request())
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["schema_version"], "shin.agent-read.v2")
        self.assertEqual(support.expand_read(result)["native"]["evidence"]["bodies"][0]["text"], SOURCE)
        self.assertEqual(before, (support.snapshot(self.fixture.root), support.snapshot(self.fixture.out)))
        self.assertEqual(self.flow.calls, 2)

    def test_raw_range_is_exact_and_invalid_ranges_are_not_clamped(self):
        result = self.call("read", self.request(start_line=1, end_line=3))
        self.assertTrue(result["ok"], result)
        self.assertEqual(support.expand_read(result)["native"]["evidence"]["bodies"][0]["text"],
                         "".join(SOURCE.splitlines(keepends=True)[:3]))
        result = self.call("read", self.request(start_line=0, end_line=10000))
        self.assertEqual(result["error"]["code"], "invalid-baseline-range")

    def test_path_and_source_binding_refuse_without_raw_text(self):
        result = self.call("read", self.request(path="../oracle.json"))
        self.assertEqual(result["error"]["code"], "pilot-corpus-violation")
        self.flow = Workflow(self.backend, {"src/feed.ts"})
        self.call("query", "feed cache")
        self.backend.safe_sources["src/feed.ts"] = "0" * 64
        result = self.call("read", self.request())
        self.assertEqual(result["error"]["code"], "baseline-screened-source-changed")
        self.assertNotIn("feed-cache", self.flow.trace[-1])

    def test_parent_interrupt_also_reaps_cli_not_only_subprocess_timeout(self):
        import code_context_workflow as workflow
        processes = []
        original = workflow.subprocess.Popen

        def launch(*args, **kwargs):
            process = original(*args, **kwargs)
            processes.append(process)
            process.communicate = lambda **kwargs: (_ for _ in ()).throw(KeyboardInterrupt())
            return process

        with patch.object(workflow.subprocess, "Popen", side_effect=launch):
            with self.assertRaises(KeyboardInterrupt):
                self.backend.invoke("query", QUESTION, 5)
        self.assertEqual(len(processes), 1)
        self.assertIsNotNone(processes[0].poll())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--core", type=Path)
    args, remaining = parser.parse_known_args()
    CORE = args.core.resolve() if args.core else None
    unittest.main(argv=[sys.argv[0], *remaining])
