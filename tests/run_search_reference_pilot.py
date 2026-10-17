#!/usr/bin/env python3
"""Small file-localization pilot, not an agent-quality or universal benchmark."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def execute(argv, path):
    start = time.monotonic()
    process = subprocess.run(argv, capture_output=True, text=True, timeout=180)
    elapsed = (time.monotonic() - start) * 1000
    path.write_text(process.stdout)
    path.with_suffix(".stderr").write_text(process.stderr)
    if process.returncode:
        raise RuntimeError(f"Command failed ({process.returncode}); see {path}")
    return json.loads(process.stdout), elapsed, len(process.stdout.encode())


def main():
    parser = argparse.ArgumentParser()
    for name in ("root", "cases", "output", "shin", "jbcontext"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--repository", required=True)
    args = parser.parse_args()
    args.root = args.root.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    cases = json.loads(args.cases.read_text())
    registry = args.output / "profiles"
    registry.mkdir()
    profile = {"schemaVersion": "shin.profile.v1", "id": "pilot",
               "sourceLabel": "Frozen source-localization pilot", "rootFiles": ["README.md", "AGENTS.md"],
               "sourceExtensions": [".md", ".json"],
               "documentIncludePrefixes": ["README.md", "AGENTS.md", "docs/machine"],
               "documentExcludePrefixes": ["docs/kb"],
               "sourceFactRoots": ["src", "observer/src", "observer/include"],
               "sourceFactExtensions": [".cpp", ".h"], "sourceFactExtractors": ["language"],
               "defaultTopic": "source", "evalCases": [], "topicRules": []}
    (registry / "pilot.json").write_text(json.dumps(profile))
    prefix = [str(args.shin.resolve()), "--root", str(args.root), "--profile", "pilot",
              "--profile-dir", str(registry), "--out-dir", str(args.output / "index")]
    build, _, _ = execute([*prefix, "build"], args.output / "build.json")
    if not build.get("ok"):
        raise RuntimeError("Shin publication failed")
    rows = []
    for case in cases:
        paths = case["expected_paths"]
        for path in paths:
            if not (args.root / path).is_file():
                raise RuntimeError("Missing expected source: " + path)
        for engine in ("shin", "jbcontext"):
            argv = ([*prefix, "brief", case["query"], "--projection", "compact", "--limit", "5",
                     "--per-doc", "2", "--require-current"] if engine == "shin" else
                    [str(args.jbcontext.resolve()), "search", "--project-path", str(args.root),
                     "--git-remote-url", args.repository, "--revision", args.revision,
                     "--limit", "5", "--json-output", case["query"]])
            result, elapsed, size = execute(argv, args.output / f"{case['id']}-{engine}.json")
            if engine == "shin":
                records = result.get("evidence", [])
                found = [item["path"] for item in records]
                valid = all(1 <= item["start_line"] <= item["end_line"] <=
                            len((args.root / item["path"]).read_text().splitlines()) for item in records)
            else:
                if result.get("revision") != args.revision:
                    raise RuntimeError("Unexpected reference index revision")
                records = result.get("results", [])
                found = [item["result"]["sourcePosition"]["relativePath"] for item in records]
                valid = all(item["content"] in (args.root / path).read_text()
                            for item, path in zip(records, found))
            rank = next((i + 1 for i, path in enumerate(found) if path in paths), None)
            if not records:
                valid = None
            row = {"id": case["id"], "engine": engine, "query": case["query"],
                   "expected_paths": paths, "paths": found, "first_evidence_rank": rank,
                   "citation_coordinates_or_text_valid": valid, "wall_ms": elapsed,
                   "response_bytes": size, "records": len(records)}
            rows.append(row)
            print(json.dumps(row), flush=True)
    report = {"revision": args.revision, "root": str(args.root), "rows": rows,
              "cases_sha256": hashlib.sha256(args.cases.read_bytes()).hexdigest(),
              "limits": f"{len(cases)} hand-authored file-localization queries, one sample each, no LLM answering or token accounting. Different engine enrollment/chunking and transport; latency is descriptive, not a controlled speed claim. Empty results do not prove source absence.",
              "summary": {engine: {"queries": sum(r["engine"] == engine for r in rows),
                                   "expected_file_in_5": sum(r["engine"] == engine and r["first_evidence_rank"] is not None for r in rows)}
                          for engine in ("shin", "jbcontext")}}
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"]))


if __name__ == "__main__":
    main()
