#!/usr/bin/env python3
"""Three predeclared known-case regressions, never a fresh cohort or retry loop."""

import argparse
import os
from pathlib import Path
import sys

import test_semantic_runtime as support
from code_context_pair import MODEL, PROVIDER, EFFORT, CONTEXT_LENGTH, answer_guide
from run_agent_query_pilot import write
from run_code_context_pair import launch
from shin_semantic.artifact import decode, encode, require, sha

ROOT = Path(__file__).resolve().parents[1]
POLICIES = {
    "quoted-v2": ("answer-grounding-regression-policy.json",
                  "2785f8da28eb306e644042f060c3c0c2b9aa334e399f3ab19b7483cde27c6bd0"),
    "legacy-v1": ("ordinary-answer-regression-policy.json",
                  "05fa0afd3d05d200233f0e59932d4a60c2d2b5024b7177344dbee23092a579c3"),
}


def load_policy(contract):
    require(contract in POLICIES, "unknown-answer-contract")
    filename, digest = POLICIES[contract]
    path = ROOT / "tests/fixtures" / filename
    data = path.read_bytes()
    require(sha(data) == digest, "grounding-policy-changed")
    policy = decode(data)
    require(policy["answer_contract"] == contract, "grounding-contract-mismatch")
    if "guide_sha256" in policy:
        require(sha(answer_guide(contract).encode()) == policy["guide_sha256"], "grounding-guide-changed")
    return path, digest, policy


def run(args):
    policy_path, policy_sha, policy = load_policy(args.answer_contract)
    binding_bytes = (args.prepared / "binding.json").read_bytes()
    binding = decode(binding_bytes)
    require(binding.get("cohort") == "file-first", "grounding-expects-p6b-control")
    require(policy["attempts_per_case"] == 1 and policy["variant"] == "candidate", "grounding-attempt-bound")
    cases = {row["id"]: row for row in binding["cases"]}
    require(len(set(policy["cases"])) == 3 and all(name in cases for name in policy["cases"]), "grounding-case-set")
    source = Path(binding["root"])
    require(all(sha((source / path).read_bytes()) == digest for path, digest in binding["sources"].items()),
            "grounding-source-changed")
    output = args.output.resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    files = [ROOT / "tests" / name for name in (
        "code_context_workflow.py", "code_context_pair.py", "run_code_context_hermes.py", "run_code_context_pair.py",
        "run_code_context_pilot.py", "run_agent_query_pilot.py", "test_semantic_runtime.py", "run_answer_grounding_probe.py")]
    files += [ROOT / "tools/shin.py", ROOT / "tools/shin_semantic.py", policy_path]
    files += sorted((ROOT / "integrations/semantic").glob("*.py"))
    files += [args.hermes_root / name for name in (
        "run_agent.py", "agent/agent_init.py", "agent/conversation_loop.py", "agent/chat_completion_helpers.py",
        "agent/turn_api_request.py", "hermes_cli/runtime_provider.py", "agent/transports/codex.py")]
    pins = {str(path): sha(path.read_bytes()) for path in files}
    write(output / "execution-freeze.json", {
        "schema_version": "shin.answer-grounding-freeze.v1", "policy_sha256": policy_sha,
        "answer_contract": policy["answer_contract"],
        "binding_sha256": sha(binding_bytes), "implementation_sha256": pins,
        "guide_sha256": sha(answer_guide(policy["answer_contract"]).encode()),
        "model": MODEL, "provider": PROVIDER, "effort": EFFORT, "context_limit": CONTEXT_LENGTH,
        "case_order": policy["cases"], "attempts_per_case": 1, "classification": policy["classification"]})
    before = support.snapshot(source)
    results = []
    for name in policy["cases"]:
        require(all(sha(Path(path).read_bytes()) == digest for path, digest in pins.items()), "grounding-runtime-changed")
        task = {"variant": "candidate", "question": cases[name]["question"],
                "route": binding["variants"]["candidate"]["route"], "sources": binding["sources"],
                "python": binding["python"], "answer_contract": policy["answer_contract"]}
        task_path = output / (name + ".task.json")
        write(task_path, task)
        result = launch(args.hermes_python, args.hermes_root, task_path, task_path.with_suffix(""))
        require(before == support.snapshot(source), "grounding-source-mutated")
        receipt = Path(result["output"]) / "receipt.json"
        require(receipt.exists(), "grounding-host-no-receipt")
        data = decode(receipt.read_bytes())
        results.append({"id": name, **result, "failure": data["failure"], "stop_reason": data["stop_reason"],
                        "receipt_sha256": sha(receipt.read_bytes())})
    require(all(sha(Path(path).read_bytes()) == digest for path, digest in pins.items()), "grounding-runtime-changed")
    write(output / "completion.json", {"schema_version": "shin.answer-grounding-completion.v1", "tasks": results,
        "source_unchanged": True, "runtime_pins_unchanged": True,
        "acceptance": "requires-main-session-semantic-source-review-not-a-fresh-quality-gate"})


if __name__ == "__main__":
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--answer-contract", choices=POLICIES, default="quoted-v2")
    for field in ("prepared", "output", "hermes-root", "hermes-python"):
        parser.add_argument("--" + field, type=Path, required=True)
    try:
        run(parser.parse_args())
    except Exception as error:
        print(encode({"ok": False, "error": str(error) if type(error).__name__ == "Rejected"
                      else type(error).__name__}).decode(), file=sys.stderr)
        raise SystemExit(1)
