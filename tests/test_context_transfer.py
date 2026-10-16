#!/usr/bin/env python3
"""Portable behavior tests on one frozen mixed-domain fixture, not model grading."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile

from reproject_semantic_identifiers import publication_fingerprint

ROOT = Path(__file__).resolve().parents[1]
POLICY = ROOT / "tests/fixtures/context-transfer-v1.json"
POLICY_SHA = "12699b940157dc74c71f8a642231aaa4d8eb1cc8a2ef0c1198fe6817dc0af156"


def check(value, label):
    if not value:
        raise RuntimeError(label)


def invoke(command):
    env = {"PATH": "", "LC_ALL": "C"}
    # Keep tool discovery disabled without stripping sanitizer diagnostics.
    for key in ("ASAN_SYMBOLIZER_PATH", "ASAN_OPTIONS", "UBSAN_OPTIONS"):
        if key in os.environ:
            env[key] = os.environ[key]
    result = subprocess.run([str(v) for v in command], capture_output=True, timeout=30,
                            env=env)
    check(result.returncode == 0 and not result.stderr and len(result.stdout) <= 1024 * 1024, "native-call-refused")
    value = json.loads(result.stdout)
    check(value["ok"] is True, "native-error")
    return value


def run(core, discovery):
    core_hash = hashlib.sha256(core.read_bytes()).hexdigest()
    discovery_hash = hashlib.sha256(discovery.read_bytes()).hexdigest()
    raw_policy = POLICY.read_bytes()
    check(hashlib.sha256(raw_policy).hexdigest() == POLICY_SHA, "frozen-policy-changed")
    policy = json.loads(raw_policy)
    check(policy["schema_version"] == "shin.context-transfer.v1", "policy-schema")
    spec = importlib.util.spec_from_file_location("observer_fixture", ROOT / "observer/tests/integration_test.py")
    fixture_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture_module)
    rows = []
    with tempfile.TemporaryDirectory(prefix="shin-context-transfer-") as temporary:
        f = fixture_module.Fixture(Path(temporary).resolve(), "workspace")
        for relative, content in policy["files"].items():
            path = Path(relative)
            check(not path.is_absolute() and ".." not in path.parts, "fixture-path")
            target = f.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
        profile = json.loads(f.profile_path.read_text())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".cpp", ".py", ".go", ".rs", ".ts", ".tsx", ".sql", ".yaml"])
        f.profile_path.write_text(json.dumps(profile))
        route = [core, "--root", f.root, "--out-dir", f.out, "--profile-dir", f.profiles, "--profile", "observer-fixture"]
        invoke([*route, "build"])
        source_cases = [c for c in policy["cases"] if c["kind"] != "document"]
        queries = f.base / "queries.json"
        queries.write_text(json.dumps([c["query"] for c in source_cases]))
        source = invoke([discovery, f.root, f.out, f.profiles, "observer-fixture", queries])
        check(source["schema_version"] == "shin.source-candidates.v1" and
              source["root"] == str(f.root) and source["profile"] == "observer-fixture" and
              source["index_freshness"]["current"] is True and source["index_freshness"]["fully_verified"] is True and
              len(source["results"]) == len(source_cases), "source-binding")
        by_id = dict(zip((c["id"] for c in source_cases), source["results"]))
        for case in policy["cases"]:
            kind = case["kind"]
            row = {"id": case["id"], "kind": kind, "query": case["query"], "required": kind != "observation"}
            if kind == "document":
                brief = invoke([*route, "brief", case["query"], "--projection", "compact", "--require-current"])
                check(brief["snapshot_id"] == source["snapshot_id"] and brief["index_freshness"]["current"] is True,
                      "document-binding")
                row["passed"] = case["expected_path"] in [p["path"] for p in brief["verification_paths"]]
                row["source_read_verified"] = case["clause"] in (f.root / case["expected_path"]).read_text()
                row["passed"] = row["passed"] and row["source_read_verified"]
            else:
                result = by_id[case["id"]]
                check(result["query"] == case["query"], "query-binding")
                candidates = result["candidates"]
                row["candidate_paths"] = [c["path"] for c in candidates]
                row["origins"] = sorted({c["range_origin"] for c in candidates})
                for candidate in candidates:
                    path = Path(candidate["path"])
                    check(not path.is_absolute() and ".." not in path.parts, "candidate-path")
                    content = (f.root / path).read_bytes()
                    part = content[candidate["byte_start"]:candidate["byte_end"]]
                    check(publication_fingerprint(content) == candidate["source_content_hash"] and
                          publication_fingerprint(part) == candidate["span_hash"], "source-span-binding")
                if kind in ("direct", "observation"):
                    row["passed"] = case["expected_path"] in row["candidate_paths"]
                    if kind == "direct":
                        row["clause_in_returned_span"] = any(case["clause"].encode() in
                            (f.root / c["path"]).read_bytes()[c["byte_start"]:c["byte_end"]]
                            for c in candidates if c["path"] == case["expected_path"])
                        row["passed"] = row["passed"] and row["clause_in_returned_span"]
                elif kind == "negative":
                    row["passed"] = not candidates
                else:
                    # Fixed extra navigation budget, chosen without oracle paths.
                    seeds = list(dict.fromkeys(row["candidate_paths"]))[:2]
                    links = []
                    for seed in seeds:
                        inspected = invoke([*route, "inspect", seed, "--require-current"])
                        check(inspected["snapshot_id"] == source["snapshot_id"] and inspected["path"] == seed and
                              inspected["index_freshness"]["current"] is True, "navigation-binding")
                        links.extend(inspected["relations"]["items"])
                    targets = [link["target_path"] for link in links if link["direction"] == "outgoing" and link["status"] == "resolved"]
                    row.update(seeds=seeds, declared_targets=targets, additional_inspections=len(seeds))
                    row["passed"] = case["expected_owner"] in seeds
                    if kind == "linked":
                        row["passed"] = row["passed"] and case["expected_path"] in targets and case["clause"] in (f.root / case["expected_path"]).read_text()
                    else:
                        row["passed"] = row["passed"] and not set(case["forbidden_targets"]) & set(targets)
            rows.append(row)
        check(not (f.root / ".git").exists(), "fixture-must-not-require-git")
        for path, text in policy["files"].items():
            check((f.root / path).read_text() == text, "source-mutated")
    check(POLICY.read_bytes() == raw_policy, "policy-drift")
    check(hashlib.sha256(core.read_bytes()).hexdigest() == core_hash and
          hashlib.sha256(discovery.read_bytes()).hexdigest() == discovery_hash, "binary-drift")
    return {"schema_version": "shin.context-transfer-report.v1", "classification": policy["classification"],
            "ok": all(r["passed"] for r in rows if r["required"]), "model_runs": 0,
            "policy_sha256": hashlib.sha256(raw_policy).hexdigest(),
            "core_sha256": core_hash, "discovery_sha256": discovery_hash,
            "required_cases": sum(r["required"] for r in rows),
            "passed_required": sum(r["required"] and r["passed"] for r in rows),
            "observations": [r for r in rows if not r["required"]], "cases": rows}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--discovery", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    os.umask(0o077)
    result = run(args.core.resolve(strict=True), args.discovery.resolve(strict=True))
    if args.report:
        with args.report.open("x") as output:
            json.dump(result, output, sort_keys=True)
            output.write("\n")
    print(json.dumps(result, sort_keys=True))
    raise SystemExit(0 if result["ok"] else 1)
