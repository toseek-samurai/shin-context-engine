#!/usr/bin/env python3
"""Bounded self-use receipt for an already prepared, explicit Observer route."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--ce", type=Path, required=True)
parser.add_argument("--config", type=Path, required=True)
parser.add_argument("--home", type=Path, required=True)
parser.add_argument("--report", type=Path, required=True)
args = parser.parse_args()
config = json.loads(args.config.read_text())
root = Path(config["root"])


def canonical_fingerprints():
    files = [args.config]
    for key in ["state_dir", "out_dir"]:
        files.extend(path for path in Path(config[key]).rglob("*") if path.is_file())
    return {str(path): {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                        "mtime_ns": path.stat().st_mtime_ns} for path in files}


def run(command):
    started = time.perf_counter()
    result = subprocess.run([str(args.ce.resolve()), *command, "--root", str(root), "--home", str(args.home)],
                            capture_output=True, text=True, timeout=120)
    elapsed = (time.perf_counter() - started) * 1000
    if result.returncode or result.stderr:
        raise RuntimeError(result.stdout + result.stderr)
    response = json.loads(result.stdout)
    assert response["local_ready"] and response["canonical_ready"]
    return response, elapsed, len(result.stdout.encode())


before = canonical_fingerprints()
attached, attach_ms, _ = run(["attach", "--observer-config", str(args.config)])
queries = [
    ("init attach existing route must not rebuild canonical index", "external-route-read-only"),
    ("where is the private source cache for an attached project stored", 'workspace / "source-cache"'),
]
receipts = []
for query, expected in queries:
    response, elapsed, size = run(["query", query])
    view = response["source_context"]
    # Source-view body segments are JSON-quoted; decode the already returned
    # text only, without reading extra source into the delivered evidence.
    bodies = []
    for line in view.splitlines():
        if line.startswith("TEXT "):
            bodies.append("")
        elif match := re.fullmatch(r"L[0-9]+\+([0-9]+) (.*)", line):
            assert bodies and len(bodies[-1].encode()) == int(match[1])
            bodies[-1] += json.loads(match[2])
    receipts.append({"query": query, "elapsed_ms": elapsed, "response_bytes": size,
                     "expected_excerpt": expected, "excerpt_delivered": any(expected in body for body in bodies),
                     "document_paths": response["document_context"]["ranked_paths"],
                     "snapshot_id": response["document_context"]["snapshot_id"],
                     "source_view": view})
after = canonical_fingerprints()
report = {"schema_version": "shin.attached-self-probe.v1", "ok": before == after,
          "version": attached["version"], "core_version": attached["core_version"],
          "profile": attached["profile"], "attach_ms": attach_ms,
          "canonical_files_unchanged": before == after, "canonical_file_count": len(before),
          "queries": receipts, "agent_outcome_assessed": False, "model_called": False}
with args.report.open("x") as output:
    json.dump(report, output, ensure_ascii=False, indent=2)
    output.write("\n")
print(json.dumps({key: value for key, value in report.items() if key != "queries"}))
print(json.dumps([{key: value for key, value in item.items() if key != "source_view"} for item in receipts]))
raise SystemExit(0 if report["ok"] else 1)
