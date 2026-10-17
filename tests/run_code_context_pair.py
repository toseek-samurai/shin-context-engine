#!/usr/bin/env python3
"""Prepare an evaluator-free same-source pair; run each declared task once."""

import argparse
from copy import deepcopy
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import tempfile
import time

import test_semantic_runtime as support
from code_context_pair import MODEL, PROVIDER, EFFORT, CONTEXT_LENGTH, SOURCE_PREFIXES, source_rows, GUIDE, tool_schema, cohort_policy
from run_agent_query_pilot import write, query
from run_code_context_pilot import prepare as prepare_smoke, QUESTION
from shin_semantic.artifact import decode, encode, publication_id, require, root_identity, sha

ROOT = Path(__file__).resolve().parents[1]


def launch(hermes_python, hermes_root, task_path, output):
    bootstrap = ("import sys,runpy; sys.dont_write_bytecode=True; "
                 "sys.path.insert(0,sys.argv.pop(1)); import hermes_bootstrap; "
                 "sys.path.insert(0,sys.argv.pop(1)); runpy.run_module('run_code_context_hermes',run_name='__main__')")
    argv = [str(hermes_python), "-I", "-c", bootstrap, str(hermes_root), str(ROOT / "tests"),
            "--task", str(task_path), "--output", str(output)]
    started = time.monotonic()
    with output.with_suffix(".stdout").open("xb") as stdout, output.with_suffix(".stderr").open("xb") as stderr:
        process = subprocess.Popen(argv, stdout=stdout, stderr=stderr, stdin=subprocess.DEVNULL, start_new_session=True,
                                   cwd=task_path.parent)
        try:
            code = process.wait(timeout=210)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            code = 124
    status = {"process_exit": code, "process_seconds": round(time.monotonic() - started, 3), "output": str(output)}
    write(output.with_suffix(".process.json"), status)
    print(encode(status).decode(), flush=True)
    return status


def prepare(args):
    frozen = args.freeze.read_bytes()
    require(sha(frozen) == args.freeze_sha256, "p0-freeze-changed")
    freeze = decode(frozen)
    policy_bytes = (ROOT / "tests/fixtures/code-context-pilot-policy.json").read_bytes()
    require(sha(policy_bytes) == freeze["policy_sha256"], "p0-policy-changed")
    rows = source_rows(freeze)
    output = args.output.resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    home = Path(tempfile.mkdtemp(prefix="shin-code-context-pair-")).resolve()
    require(not any((parent / ".git").exists() for parent in (home, *home.parents)), "paired-root-inside-git")
    source = home / "source"
    source.mkdir(mode=0o700)
    sources = {row["path"]: row["sha256"] for row in rows}
    for row in rows:
        write(source / row["path"], row["text"])
    profile = deepcopy(freeze["profile"])
    profile.update(rootFiles=[], documentIncludePrefixes=[], sourceFactRoots=[p.rstrip("/") for p in SOURCE_PREFIXES],
                   sourceFactExtensions=[".cpp", ".h"], agentStartPaths=[], evalCases=[], packEvalCases=[], bootstrapOnly=True)
    profile_path = home / "profiles" / "shin.json"
    write(profile_path, profile)
    # The existing native scanner examines every full file before any baseline raw read.
    screen = home / "screen-input.jsonl"
    write(screen, "".join(encode({"index": i, "path": row["path"], "text": row["text"], "start_line": 1,
         "end_line": len(row["text"].splitlines()), "source_content_hash": publication_id(row["text"].encode())}).decode() + "\n"
         for i, row in enumerate(rows)))
    result = subprocess.run([str(args.candidate_exporter.resolve()), "--redact-model-input", str(source), str(screen)],
                            capture_output=True, timeout=30)
    require(result.returncode == 0 and not result.stderr, "paired-native-screen-refused")
    screened = [decode(line) for line in result.stdout.splitlines()]
    require(screened[-1] == {"type": "complete", "records": len(rows)} and len(screened) == len(rows) + 1,
            "paired-native-screen-incomplete")
    require(all(row["redacted"] is False and row["text"] == rows[i]["text"] for i, row in enumerate(screened[:-1])),
            "paired-source-needs-redaction-review")
    screen.unlink()
    variants = {}
    for variant in ("baseline", "candidate"):
        core, exporter = getattr(args, variant).resolve(), getattr(args, variant + "_exporter").resolve()
        if variant == "baseline":
            require(sha(core.read_bytes()) == freeze["core_sha256"], "p0-baseline-changed")
        index = home / variant / "index"
        prefix = [str(core), "--root", str(source), "--profile", "shin", "--profile-dir", str(profile_path.parent),
                  "--out-dir", str(index)]
        for command in (["build"], ["doctor", "--strict"]):
            checked = query(prefix + command)
            write(output / variant / (command[0] + ".json"), checked)
            require(checked["exit_code"] == 0 and checked["response"]["ok"] is True, "paired-native-preflight-failed")
        route = {"core_binary": str(core), "core_sha256": sha(core.read_bytes()), "root": str(source),
                 "root_identity": root_identity(source), "project": "shin", "profile": "shin",
                 "profile_path": str(profile_path), "profile_sha256": sha(profile_path.read_bytes()),
                 "out_dir": str(index), "model_dir": str(args.model_dir.resolve())}
        config = home / variant / "build.json"
        store = home / variant / "semantic"
        write(config, {"schema_version": "shin.semantic-build.v1", "route": route,
            "exporter_binary": str(exporter), "exporter_sha256": sha(exporter.read_bytes()), "store_dir": str(store)})
        built = subprocess.run([str(args.python.absolute()), str(ROOT / "tools/shin_semantic_build.py"), "--config", str(config)],
            capture_output=True, timeout=1800, env={"PATH": "", "LC_ALL": "C", "PYTHONDONTWRITEBYTECODE": "1"})
        require(not built.stderr, "paired-semantic-diagnostics")
        result = {"exit_code": built.returncode, "response": decode(built.stdout)}
        write(output / variant / "producer.json", result)
        require(result["exit_code"] == 0 and result["response"]["ok"] is True, "paired-semantic-preparation-failed")
        variants[variant] = {"route": str(store / "current-route.json"), "core_sha256": route["core_sha256"],
            "exporter_sha256": sha(exporter.read_bytes()), "snapshot_id": result["response"]["snapshot_id"],
            "artifact_sha256": result["response"]["artifact_sha256"]}
    policy, case_policy_sha = cohort_policy(args.cohort)
    cases = [{"id": topic["id"] + "-" + language, "question": topic[language], "group": "new", "language": language}
             for topic in policy["topics"] for language in ("english", "russian")]
    cases += [{"id": "writer-" + language, "question": policy["writerControl"][language],
               "group": "writer", "language": language} for language in ("english", "russian")]
    require(len(cases) == 10, "paired-case-count")
    binding = {"schema_version": "shin.code-context-pair.v1", "freeze_sha256": args.freeze_sha256,
        "policy_sha256": freeze["policy_sha256"], "cohort": args.cohort, "case_policy_sha256": case_policy_sha,
        "root": str(source), "sources": sources, "variants": variants,
        "profile_path": str(profile_path), "profile_sha256": sha(profile_path.read_bytes()),
        "source_bytes": sum(len(row["text"].encode()) for row in rows), "cases": cases,
        "model": MODEL, "provider": PROVIDER, "effort": EFFORT, "context_limit": CONTEXT_LENGTH,
        "python": str(args.python.absolute()), "native_full_file_screen": "passed",
        "guide_sha256": sha(GUIDE.encode()), "tool_schema_sha256": {v: sha(encode(tool_schema(v))) for v in variants},
        "scope": "same-source product C++ subset of P0 for both variants; all docs, reports, tests and tools excluded before model runs; not old full-profile retrieval scoring"}
    write(output / "binding.json", binding)
    print(encode({"prepared": str(output), "files": len(sources), "bytes": binding["source_bytes"], "tasks": len(cases)}).decode())


def run(args):
    output = args.output.resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    binding = decode((args.prepared / "binding.json").read_bytes())
    _, case_policy_sha = cohort_policy(binding.get("cohort", "p4-regression"))
    require(case_policy_sha == binding.get("case_policy_sha256", binding["policy_sha256"]), "paired-case-policy-changed")
    require(sha(GUIDE.encode()) == binding["guide_sha256"] and
            all(sha(encode(tool_schema(v))) == binding["tool_schema_sha256"][v] for v in binding["variants"]),
            "paired-guide-changed")
    source = Path(binding["root"])
    require(all(sha((source / path).read_bytes()) == digest for path, digest in binding["sources"].items()),
            "paired-source-changed-before-run")
    files = [ROOT / "tests" / name for name in ("code_context_workflow.py", "code_context_pair.py", "run_code_context_pair.py",
                                                "run_code_context_hermes.py", "run_code_context_pilot.py",
                                                "run_agent_query_pilot.py", "test_semantic_runtime.py")]
    files += [ROOT / "tools/shin.py", ROOT / "tools/shin_semantic.py"]
    files += sorted((ROOT / "integrations/semantic").glob("*.py"))
    files += [ROOT / "tests/fixtures" / name for name in ("code-context-pilot-policy.json", "file-first-pilot-policy.json")]
    pins = {str(path): sha(path.read_bytes()) for path in files}
    hermes_files = [args.hermes_root / path for path in ("run_agent.py", "agent/agent_init.py", "agent/conversation_loop.py",
                   "agent/chat_completion_helpers.py", "agent/turn_api_request.py", "hermes_cli/runtime_provider.py",
                   "agent/transports/codex.py")]
    pins.update({str(path): sha(path.read_bytes()) for path in hermes_files})
    write(output / "execution-freeze.json", {"binding_sha256": sha((args.prepared / "binding.json").read_bytes()),
        "implementation_sha256": pins, "host": platform.platform(), "python": sys.version,
        "hermes_python": str(args.hermes_python), "hermes_root": str(args.hermes_root),
        "model": MODEL, "provider": PROVIDER, "effort": EFFORT, "context_limit": CONTEXT_LENGTH,
        "order": "alternate baseline/candidate first by case; fresh process per task; no quality retries"})
    before = support.snapshot(source)
    for index, case in enumerate(binding["cases"]):
        for variant in (("baseline", "candidate") if index % 2 == 0 else ("candidate", "baseline")):
            require(all(sha(Path(path).read_bytes()) == digest for path, digest in pins.items()), "paired-runtime-changed")
            task = {"variant": variant, "question": case["question"], "route": binding["variants"][variant]["route"],
                    "sources": binding["sources"], "python": binding["python"]}
            task_path = output / (case["id"] + "-" + variant + ".task.json")
            write(task_path, task)
            result = launch(args.hermes_python, args.hermes_root, task_path, task_path.with_suffix(""))
            require(before == support.snapshot(source), "paired-corpus-mutated")
            receipt_path = Path(result["output"]) / "receipt.json"
            require(receipt_path.exists(), "paired-host-did-not-produce-receipt")
            receipt = decode(receipt_path.read_bytes())
            if receipt["failure"] in ("pilot-request-audit-failed", "pilot-system-context-mismatch",
                                      "pilot-user-context-mismatch", "pilot-tool-delivery-mismatch",
                                      "pilot-wire-delivery-mismatch", "pilot-provider-mismatch", "pilot-model-turn-incomplete"):
                raise ValueError("paired-transport-failure-stop-before-scoring")
    write(output / "completion.json", {"tasks": len(binding["cases"]) * 2, "source_unchanged": True,
        "acceptance": "requires-main-session-source-and-answer-review", "binding_sha256": sha((args.prepared / "binding.json").read_bytes())})


def smoke(args):
    output = args.output.resolve()
    flow, fixture, before = prepare_smoke(args.candidate.resolve(), args.python.absolute(), output)
    binding = decode((output / "binding.json").read_bytes())
    task = {"variant": args.variant, "question": QUESTION, "route": binding["route"],
            "sources": binding["source_sha256"], "python": str(args.python.absolute())}
    task_path = output / "task.json"
    write(task_path, task)
    result = launch(args.hermes_python, args.hermes_root, task_path, output / "agent")
    require(before == (support.snapshot(fixture.root), support.snapshot(fixture.out)), "smoke-corpus-mutated")
    return result["process_exit"]


if __name__ == "__main__":
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prep = commands.add_parser("prepare")
    prep.add_argument("--freeze-sha256", required=True, help="Reviewed SHA-256 from the new local source freeze")
    prep.add_argument("--cohort", choices=("p4-regression", "file-first"), required=True)
    for name in ("freeze", "baseline", "candidate", "baseline-exporter", "candidate-exporter", "model-dir", "python", "output"):
        prep.add_argument("--" + name, type=Path, required=True)
    execute = commands.add_parser("run")
    execute.add_argument("--prepared", type=Path, required=True)
    probe = commands.add_parser("smoke")
    probe.add_argument("--candidate", type=Path, required=True)
    probe.add_argument("--python", type=Path, required=True)
    probe.add_argument("--variant", choices=("baseline", "candidate"), default="candidate")
    for sub in (execute, probe):
        for name in ("hermes-python", "hermes-root", "output"):
            sub.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        code = {"prepare": prepare, "run": run, "smoke": smoke}[args.command](args)
        raise SystemExit(code or 0)
    except Exception as error:
        print(encode({"ok": False, "error": str(error) if type(error).__name__ == "Rejected" else type(error).__name__}).decode())
        raise SystemExit(1)
