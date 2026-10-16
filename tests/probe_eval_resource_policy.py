"""Compare policies against a read-only live project with a private registry/index."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary, root, output = args.binary.resolve(), args.root.resolve(), args.output.resolve()
    if output == root or output.is_relative_to(root) or root.is_relative_to(output):
        parser.error("output must be disjoint from the consumer root")
    original = args.profile.read_bytes()
    profile = json.loads(original)
    output.mkdir(parents=True, exist_ok=False)
    registry = output / "profiles"
    registry.mkdir()
    private_profile = registry / (profile["id"] + ".json")
    prefix = [str(binary), "--profile", profile["id"], "--profile-dir", str(registry),
              "--root", str(root), "--out-dir", str(output / "index")]

    def run(policy, command, *flags):
        process = subprocess.run([*prefix, command, *flags], capture_output=True, timeout=180)
        value = json.loads(process.stdout or process.stderr)
        (output / f"{policy}-{command}.json").write_bytes(process.stdout or process.stderr)
        if process.returncode not in (0, 1) or value.get("command") != command:
            raise RuntimeError(f"{policy} {command} failed: {process.returncode}")
        return value, process.returncode

    observations = {}
    for policy in ("strict", "advisory"):
        profile["evalResourcePolicy"] = policy
        private_profile.write_text(json.dumps(profile))
        built, code = run(policy, "build")
        assert code == 0, built
        evaluation, code = run(policy, "eval", "--strict", "--require-current")
        readiness, ready_code = run(policy, "ready", "--strict")
        observations[policy] = {"eval": evaluation, "exit": code, "ready_exit": ready_code,
                                "ready": readiness["ready"]}

    def evidence(rows):
        rows = copy.deepcopy(rows)
        for row in rows:
            for query in [row, *row["query_variants"]]:
                for field in ("passed", "query_passed", "resource_policy"):
                    query.pop(field)
        return rows

    strict, advisory = observations["strict"], observations["advisory"]
    identical = evidence(strict["eval"]["cases"]) == evidence(advisory["eval"]["cases"])
    assert identical, "policy changed retrieval or quality diagnostics"
    assert args.profile.read_bytes() == original, "consumer profile changed"
    report = {"schema_version": "shin.eval-resource-policy-probe.v1", "evidence_identical": identical,
              "consumer_profile_unchanged": True, "canonical_index_touched": False,
              "corpus_copied": False, "root": str(root),
              "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "profile_sha256": hashlib.sha256(original).hexdigest(),
              "scope": "private-profile-and-index; no consumer adoption assertion",
              "policies": {policy: {"exit": item["exit"], "ready_exit": item["ready_exit"],
                  "ready": item["ready"], "summary": item["eval"]["summary"],
                  "query_summary": item["eval"]["query_summary"],
                  "quality_ok": item["eval"]["quality_ok"], "resource_check": item["eval"]["resource_check"],
                  "warnings": item["eval"]["warnings"]} for policy, item in observations.items()}}
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
