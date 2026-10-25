"""Portable optional E2d parity fixture; all artifacts go to a new explicit directory."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def run(core, oracle, bench, destination):
    base = Path(destination).resolve()
    base.mkdir(parents=True, exist_ok=False)
    root, profiles, out = base / "root", base / "profiles", base / "index"
    (root / "docs").mkdir(parents=True)
    profiles.mkdir()
    (root / "README.md").write_text("# Evaluation fixture\n\nNeutral bounded adaptive retrieval proof.\n")
    docs = {
        "authorization": "Endpoint authorization operator guidance token checks.",
        "storage": "Storage fencing snapshot consistency recovery runbook.",
        "publication": "Published endpoint policy rollout operator controls.",
        "transport": "Transport queue batch concurrency cancellation lifecycle.",
    }
    for name, body in docs.items():
        (root / "docs" / (name + ".md")).write_text(
            f"# {name.title()}\n\nStatus: active\n\n## Current contract\n\n{body}\n\n## Examples\n\n{body}\n")
    cases = [
        {"id": "auth", "query": "authorization operator", "queryVariants": ["endpoint authorization"], "mustPaths": ["docs/authorization.md"]},
        {"id": "storage", "query": "snapshot recovery fencing", "mustPaths": ["docs/storage.md"]},
        {"id": "owner-path", "query": "docs/publication.md operator", "mustPaths": ["docs/publication.md"]},
        {"id": "broad", "query": "endpoint authorization storage recovery lifecycle", "minCoveredGroups": 2},
        {"id": "single", "query": "transport", "mustPaths": ["docs/transport.md"]},
        {"id": "missing-required", "query": "quasarx nonexistent", "mustPaths": ["docs/authorization.md"]},
        {"id": "resource-refusal", "query": "operator", "maxCandidateRows": 0},
        {"id": "forbidden-return", "query": "storage", "mustNotPaths": ["docs/storage.md"]},
        {"id": "variant-recovery", "query": "queue batch", "queryVariants": ["transport lifecycle"], "mustPaths": ["docs/transport.md"]},
    ]
    profile = {"schemaVersion": "shin.profile.v1", "id": "neutral", "sourceLabel": "Neutral experimental fixture",
        "rootFiles": ["README.md"], "sourceExtensions": [".md"], "agentStartPaths": ["README.md"],
        "defaultTopic": "fixture", "topicRules": [], "evalCases": cases, "packEvalCases": []}
    (profiles / "neutral.json").write_text(json.dumps(profile, indent=2) + "\n")

    def invoke(label, args):
        result = subprocess.run(args, capture_output=True, text=True, timeout=120)
        (base / (label + ".stdout")).write_text(result.stdout)
        (base / (label + ".stderr")).write_text(result.stderr)
        if result.returncode:
            raise RuntimeError(f"{label} failed: {result.returncode}: {result.stderr}")
        return result.stdout

    invoke("build", [core, "--root", str(root), "--out-dir", str(out), "--profile-dir", str(profiles), "--profile", "neutral", "build"])
    inputs = [str(root), str(out), str(profiles), "neutral"]
    oracle_path = base / "oracle.jsonl"
    oracle_path.write_text(invoke("oracle", [oracle, *inputs]))

    def identities():
        return {str(p.relative_to(base)): hashlib.sha256(p.read_bytes()).hexdigest()
            for directory in (root, profiles, out / "docs/kb/generated")
            for p in directory.rglob("*") if p.is_file()}

    before = identities()
    for size, order in ((1, "baseline-first"), (8, "candidate-first")):
        result = json.loads(invoke(f"batch-{size}", [bench, *inputs, order, str(size), str(oracle_path)]))
        assert result["ok"] and result["external_oracle_checked"] and result["pack_parity"]
        assert not result["quality_passed"]
        assert (result["exact_eval_cases"], result["exact_eval_queries"], result["exact_brief_queries"]) == (9, 11, 11)
        assert result["summary"]["failed_case_ids"] == ["missing-required", "resource-refusal", "forbidden-return"]
        assert result["summary"]["resource_failed_queries"] == 1
        assert all(lane["batch_metrics"]["pending_count"] == 0 and lane["batch_metrics"]["error_replays"] == 0 for lane in result["runs"])
        assert identities() == before, "fixture source/profile/generated index changed"
    (base / "input-sha256.json").write_text(json.dumps(before, indent=2) + "\n")
    print(json.dumps({"ok": True, "cases": 9, "queries": 11, "preserved_failures": 3, "batches": [1, 8]}))


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("usage: eval_batch_fixture.py CORE ORACLE BENCH NEW_EXTERNAL_DIRECTORY")
    run(*(str(Path(value).resolve()) for value in sys.argv[1:]))
