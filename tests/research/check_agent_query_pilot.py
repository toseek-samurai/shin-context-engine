#!/usr/bin/env python3
"""Check full Hermes receipts and source reads; human answer review stays separate."""

import argparse
import json
from pathlib import Path
import shlex

import test_semantic_runtime  # Loads the source-owned package without a model.
from shin_semantic.artifact import canonical, decode, read, relative, require, sha


def inspect(session, binding, controls, process, source_text):
    messages = session["messages"]
    calls, replies = {}, {}
    for message in messages:
        for call in message.get("tool_calls") or []:
            calls[call["id"]] = call["function"]
        if message["role"] == "tool":
            replies[message["tool_call_id"]] = decode(message["content"])
    final = next(message["content"] for message in reversed(messages) if message["role"] == "assistant" and message["content"])
    answers = decode(final)["cases"]
    require(len(answers) == len(binding["cases"]) and len({row["id"] for row in answers}) == len(answers), "invalid-pilot-answer-set")
    require({row["id"] for row in answers} == {row["id"] for row in binding["cases"]}, "invalid-pilot-answer-set")
    command_receipts = []
    for identity, call in calls.items():
        if call["name"] == "terminal" and identity in replies:
            command_receipts.append((shlex.split(decode(call["arguments"])["command"]), replies[identity]))
    checks = []
    for case in binding["cases"]:
        matching = [result for argv, result in command_receipts if argv == case["argv"]]
        require(len(matching) == 1, "expected-one-pilot-query")
        result, control = matching[0], controls[case["id"]]
        require(result["exit_code"] == control["exit_code"] and decode(result["output"]) == control["response"],
                "pilot-query-differs-from-independent-control")
        answer = next(row for row in answers if row["id"] == case["id"])
        require(answer["readiness_evaluated"] is False and answer["tests_executed"] is False, "pilot-false-verification-claim")
        payload = control["response"]
        if case["id"] == "stale":
            require(answer["decision"] == "stop" and answer["answer"] is None and answer["evidence"] == []
                    and answer["semantic_status"] is None and answer["error"] == payload["error"]["code"], "pilot-stale-answer")
        else:
            require(answer["decision"] == "answer" and isinstance(answer["answer"], str) and answer["answer"]
                    and answer["semantic_status"] == payload["semantic_status"] and answer["error"] is None
                    and bool(answer["evidence"]), "pilot-answer-status-mismatch")
            for evidence in answer["evidence"]:
                require(relative(evidence["path"]), "pilot-source-path-escape")
                path = str(Path(case["root"]) / evidence["path"])
                text = source_text[path]
                require(type(evidence["start_line"]) is int and type(evidence["end_line"]) is int
                        and 1 <= evidence["start_line"] <= evidence["end_line"] <= len(text.splitlines()), "pilot-invalid-source-range")
                require(any(path in argv and receipt.get("exit_code") == 0 and text.rstrip() in receipt.get("output", "")
                            for argv, receipt in command_receipts), "pilot-source-read-not-observed")
        checks.append({"id": case["id"], "query_matches_control": True, "status_and_scope_correct": True,
                       "cited_sources_read": case["id"] != "stale", "substantive_answer_review": "requires-main-session"})
    require(process["process_exit"] == 0 and process["fixture_unchanged"] is True, "pilot-process-or-mutation-failure")
    return {"schema_version": "shin.agent-query-pilot-check.v1", "ok": True, "session_id": session["id"], "checks": checks,
            "tool_calls": len(calls), "api_calls": session.get("api_call_count"), "fixture_unchanged": True,
            "usage_cumulative": {key: session.get(key) for key in ("input_tokens", "output_tokens", "cache_read_tokens", "cache_write_tokens")},
            "acceptance": "not-decided-human-answer-and-complete-command-review-required"}


def check(output):
    prepared = decode(read(output / "prepared.json", 65536))
    process = decode(read(output / "process.json", 65536))
    trace = read(output / "agent.jsonl", 8 * 1024 * 1024)
    require(sha(trace) == process["trace_sha256"], "pilot-trace-changed")
    events = [decode(line) for line in trace.splitlines() if line]
    session_bytes = read(output / "session.jsonl", 8 * 1024 * 1024)
    session = decode(session_bytes)
    require(session["id"] == events[0]["session_id"] == events[-1]["session_id"]
            and events[-1]["type"] == "result" and events[-1]["exit_code"] == 0, "pilot-session-mismatch")
    binding_bytes = read(canonical(prepared["home"]) / "binding.json", 65536)
    require(sha(binding_bytes) == prepared["binding_sha256"], "pilot-binding-changed")
    binding = decode(binding_bytes)
    controls = {case["id"]: decode(read(output / (case["id"] + "-control.json"), 1024 * 1024)) for case in binding["cases"]}
    source_text = {}
    for case in binding["cases"]:
        for group in controls[case["id"]]["response"].get("native", {}).get("files", []):
            require(relative(group["path"]), "pilot-source-path-escape")
            path = canonical(str(Path(case["root"]) / group["path"]))
            source_text[str(path)] = read(path, 16384).decode()
    result = inspect(session, binding, controls, process, source_text)
    result["session_export_sha256"] = sha(session_bytes)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    check(parser.parse_args().output.resolve())
