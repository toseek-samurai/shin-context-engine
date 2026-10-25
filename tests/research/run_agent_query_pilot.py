#!/usr/bin/env python3
"""One bounded fresh-agent workflow on isolated source-owned query fixtures."""

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

import test_semantic_runtime as fixtures
from shin_semantic.artifact import decode, encode, root_identity, sha
from shin_semantic.producer import Producer

ROOT = Path(__file__).resolve().parents[2]
POLICY = """# 0001: Public feed exposure

Status: accepted

## Decision

The administrative catalog and public feed routes are separate views.
Creating a feed prepares its catalog entry; it does not publish public content.

Only feeds containing at least one published entry are exposed in the public
catalog. Empty feeds have no public route, even when an administrator can see
their catalog entry. An entry that is still a draft does not count as published.

The feed cache is partitioned by the current implementation in src/feed.ts.
This decision does not claim that the implementation has been executed.
"""
SOURCE = """export function feedCacheKey(tenantId: string, revision: number) {
  return ['feed-cache', tenantId, revision] as const;
}

export function hasPublicFeed(entries: { published: boolean }[]) {
  return entries.some(entry => entry.published);
}
"""
GUIDE = """# Agent query pilot

Read binding.json. Each case provides an exact argv for the source-owned query
entrypoint and its allowed source root. Execute that argv without changing its
route, interpreter, options or question. Queries do not build or repair indexes.

A successful shin.agent-query.v1 has native.files groups: path, records and
excerpts. Each record retains global rank and grading; excerpt_ref indexes that
file's excerpts. range.kind=document-anchor is not the source span of its
summary. Read the source document or substantive section before answering.
source-span and section-span still require source inspection. Navigation is a
separate path hint, not native evidence or proof that a symbol exists.

admission.kind=core-current-only and ready=null mean that full adoption/readiness
was NOT evaluated. current-source admission is not executed tests. Missing
optional runtime may leave valid native results; report that limitation without
installing anything. If ok=false or the query exits nonzero, stop that case and
report the error: do not bypass the gate with another route, direct source answer,
rebuild or repair. Never infer missing answers from another case.

Only read GUIDE.md, binding.json and returned source paths inside a case's root.
Do not inspect project/test implementation, other repositories, global settings,
credentials, or the maintainer's output files. The query argv itself may read
its pinned runtime/index/model. Ordinary shell/JSON tools may inspect responses
and allowed source files. No writes, installs, downloads, service management,
network commands, model switching, delegation, or test/source execution.

Return one JSON object with a cases array. For each supplied id include decision
(answer or stop), concise answer (null on stop), semantic_status (null if absent),
error (null or error code), readiness_evaluated (boolean), tests_executed (boolean),
and evidence (relative path/start_line/end_line actually read; empty on stop).
Interpret the results rather than echoing this guide. No prose outside the JSON.
"""


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    fixtures.write(path, value.encode() if isinstance(value, str) else encode(value) + b"\n")


def query(argv):
    environment = {"PATH": "", "LC_ALL": "C"}
    symbolizer = os.environ.get("ASAN_SYMBOLIZER_PATH")
    if symbolizer:
        if not Path(symbolizer).is_absolute():
            raise ValueError("Pilot sanitizer symbolizer must be an absolute path")
        environment["ASAN_SYMBOLIZER_PATH"] = symbolizer
    result = subprocess.run(argv, capture_output=True, timeout=60, env=environment)
    if result.stderr:
        raise ValueError("Query emitted unexpected diagnostics")
    return {"exit_code": result.returncode, "response": decode(result.stdout), "bytes": len(result.stdout)}


def prepare(core, exporter, model, plain_python, output):
    output.mkdir(parents=True, exist_ok=False)
    output.chmod(0o700)
    home = Path(tempfile.mkdtemp(prefix="shin-agent-query-pilot-")).resolve()
    if any((parent / ".git").exists() for parent in (home, *home.parents)):
        raise ValueError("Pilot fixtures must be outside Git ancestry")
    bindings = {}
    for name in ("current", "stale"):
        fixture = fixtures.fixture_support.Fixture(home, name)
        fixture.write_source("# Feed project\n\nPublic feed visibility and cache partitioning.\n")
        write(fixture.root / "docs/decisions/0001-public-feeds.md", POLICY)
        write(fixture.root / "src/feed.ts", SOURCE)
        profile = decode(fixture.profile_path.read_bytes())
        profile.update(rootFiles=["README.md", "docs/decisions/0001-public-feeds.md"], sourceFactRoots=["src"],
                       sourceFactExtensions=[".ts"], sourceFactExtractors=["language"])
        write(fixture.profile_path, profile)
        prefix = [str(core), "--root", str(fixture.root), "--profile", "observer-fixture", "--profile-dir", str(fixture.profiles),
                  "--out-dir", str(fixture.out)]
        built = query([*prefix, "build"])
        if built["exit_code"] or built["response"].get("ok") is not True:
            raise ValueError("Fixture publication failed")
        route = {"core_binary": str(core), "core_sha256": sha(core.read_bytes()), "root": str(fixture.root),
                 "root_identity": root_identity(fixture.root), "project": "query-pilot", "profile": "observer-fixture",
                 "profile_path": str(fixture.profile_path), "profile_sha256": sha(fixture.profile_path.read_bytes()),
                 "out_dir": str(fixture.out), "model_dir": str(model)}
        config = {"schema_version": "shin.semantic-build.v1", "route": route, "exporter_binary": str(exporter),
                  "exporter_sha256": sha(exporter.read_bytes()), "store_dir": str(fixture.base / "semantic")}
        path = fixture.base / "build.json"
        write(path, config)
        produced = Producer(path).build()
        write(output / (name + "-producer.json"), produced)
        bindings[name] = {"root": str(fixture.root), "config": produced["route"]}
        if name == "stale":
            fixture.write_source("# Feed project\n\nAn unpublished local change.\n")
    cases = []
    for identity, name, interpreter, question in (
        ("normal", "current", sys.executable, "When is a feed exposed in the public catalog?"),
        ("exact", "current", plain_python, "feedCacheKey"),
        ("optional-runtime-absent", "current", plain_python, "When is a feed exposed in the public catalog?"),
        ("stale", "stale", sys.executable, "When is a feed exposed in the public catalog?"),
    ):
        binding = bindings[name]
        cases.append({"id": identity, "root": binding["root"], "argv": [str(interpreter), str(ROOT / "tools/shin.py"),
                      "--config", binding["config"], question]})
    write(home / "GUIDE.md", GUIDE)
    write(home / "binding.json", {"schema_version": "shin.agent-query-pilot-binding.v1", "cases": cases})
    expected = {"normal": "available", "exact": "not-needed-exact-or-owner",
                "optional-runtime-absent": "runtime-unavailable-lexical-only"}
    controls = {}
    for case in cases:
        result = query(case["argv"])
        payload = result["response"]
        if case["id"] == "stale":
            passed = result["exit_code"] == 1 and payload.get("error", {}).get("code") == "core-admission-rejected"
        else:
            passed = (result["exit_code"] == 0 and payload.get("semantic_status") == expected[case["id"]]
                      and payload["admission"]["ready"] is None and bool(payload["native"]["files"]))
        if not passed:
            write(output / "failed-control.json", {"id": case["id"], **result})
            raise ValueError("Pilot preflight rejected: " + case["id"])
        write(output / (case["id"] + "-control.json"), result)
        controls[case["id"]] = {"passed": True, "bytes": result["bytes"]}
    write(output / "prepared.json", {"home": str(home), "controls": controls, "binding_sha256": sha((home / "binding.json").read_bytes())})
    return home, cases


def run_agent(hermes, home, output):
    prompt = ("Run the four read-only Shin agent-query acceptance cases in binding.json. "
              "First read GUIDE.md and binding.json. Follow only their bounded fixture permissions. "
              "Do not modify any files. Verify returned evidence in the allowed source before answering. "
              "A fresh run is intentional: do not resume another session. Return the requested JSON.")
    write(home / "prompt.txt", prompt)
    before = fixtures.snapshot(home)
    command = [str(hermes), "chat", "--safe-mode", "--oneshot", "--format", "stream-json", "--provider", "openai-codex",
               "--model", "gpt-6-sol", "--reasoning", "high", "--toolsets", "terminal", "--max-turns", "16",
               "--run-budget", "180", "--in", str(home), "--query-file", str(home / "prompt.txt")]
    started = time.monotonic()
    with (output / "agent.jsonl").open("xb") as trace, (output / "agent.stderr").open("xb") as errors:
        process = subprocess.Popen(command, cwd=home, stdout=trace, stderr=errors, stdin=subprocess.DEVNULL, start_new_session=True)
        try:
            code = process.wait(timeout=210)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
            code = 124
    after = fixtures.snapshot(home)
    result = {"schema_version": "shin.agent-query-pilot-process.v1", "process_exit": code,
              "seconds": round(time.monotonic() - started, 3), "fixture_unchanged": before == after,
              "trace_sha256": sha((output / "agent.jsonl").read_bytes()), "model": "gpt-6-sol", "provider": "openai-codex",
              "reasoning": "high", "max_turns": 16, "run_budget_seconds": 180,
              "agent_acceptance": "not-decided-main-session-trace-and-answer-review-required",
              "boundary": "One neutral fixture, not installed integration or malicious-agent filesystem containment"}
    write(output / "process.json", result)
    print(json.dumps(result), flush=True)
    if code or before != after:
        raise ValueError("Agent process or fixture-preservation check failed; retain the original trace")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ("core", "exporter", "model-dir", "plain-python", "output"):
        parser.add_argument("--" + field, type=Path, required=True)
    parser.add_argument("--hermes", type=Path)
    args = parser.parse_args()
    home, _ = prepare(args.core.resolve(), args.exporter.resolve(), args.model_dir.resolve(), args.plain_python.resolve(), args.output.resolve())
    print(json.dumps({"prepared": str(home), "controls": 4}), flush=True)
    if args.hermes:
        run_agent(args.hermes.resolve(), home, args.output.resolve())
