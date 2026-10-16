#!/usr/bin/env python3
"""Paired, network-deniable query-metrics smoke test on Shin itself only."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CASES = [
    ("semantic-en", "Where does the server prepare source and publish an immutable context snapshot?", "available"),
    ("semantic-telemetry", "How does the server measure retrieval latency and keep diagnostics out of project evidence?", "available"),
    ("cpp-symbol", "PreparationService::prepare", "runtime-unavailable-lexical-only"),
    ("owner", "server/src/preparation_service.cpp", "not-needed-exact-or-owner"),
    ("runtime-absent", "Where does the server prepare source and publish an immutable context snapshot?",
     "runtime-unavailable-lexical-only"),
]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, sort_keys=True, indent=2)
        stream.write("\n")
    path.chmod(0o600)


def execute(args):
    started = time.perf_counter_ns()
    result = subprocess.run([str(v) for v in args], capture_output=True, timeout=60,
                            env={"PATH": "", "LC_ALL": "C", "PYTHONDONTWRITEBYTECODE": "1"})
    elapsed = time.perf_counter_ns() - started
    if result.returncode or result.stderr:
        raise ValueError("Pilot command rejected; inspect the explicit local route and metrics home")
    return result.stdout, elapsed


def run(config, metrics, plain_python, output):
    route = json.loads(config.read_bytes())
    if route["root"] != str(ROOT):
        raise ValueError("This pilot enrolls only the native Shin checkout")
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    report_command = [plain_python, ROOT / "tools/shin_query_metrics.py", "report", "--dir", metrics]
    initial = json.loads(execute(report_command)[0])
    config_hash = digest(config)
    cases = []
    for index, (name, query, status) in enumerate(CASES):
        python = plain_python if name in ("runtime-absent", "cpp-symbol", "owner") else sys.executable
        base = [python, ROOT / "tools/shin.py", "--config", config, query]
        variants = [("off", base), ("on", [*base, "--metrics-dir", metrics])]
        if index % 2:
            variants.reverse()
        captured = {}
        for mode, argv in variants:
            data, wall_ns = execute(argv)
            value = json.loads(data)
            write(output / f"{name}-{mode}.json", value)
            if value.get("ok") is not True or value.get("semantic_status") != status:
                raise ValueError("Unexpected controlled query outcome: " + name)
            if value["admission"]["ready"] is not None:
                raise ValueError("Native development query claimed adoption readiness")
            captured[mode] = (data, wall_ns)
        if captured["off"][0] != captured["on"][0]:
            raise ValueError("Instrumentation changed the complete response: " + name)
        cases.append({"case": name, "semantic_status": status, "byte_identical_response": True,
                      "off_wall_ns": captured["off"][1], "on_wall_ns": captured["on"][1]})
    report = json.loads(execute(report_command)[0])
    if report["appended"] - initial["appended"] != len(CASES) or digest(config) != config_hash:
        raise ValueError("Unexpected journal count or route mutation")
    write(output / "metrics.json", report)
    files = [ROOT / "tools/shin.py", ROOT / "tools/shin_semantic.py", ROOT / "tools/shin_query_metrics.py",
             *sorted((ROOT / "integrations/semantic").glob("*.py"))]
    receipt = {"schema_version": "shin.local-query-metrics-pilot.v1", "ok": True,
               "scope": "five paired Shin-only queries; not answer-quality or performance qualification",
               "cases": cases, "recorded": len(CASES), "route_sha256": config_hash,
               "core_sha256": digest(Path(route["core_binary"])),
               "implementation_sha256": {str(path.relative_to(ROOT)): digest(path) for path in files}}
    write(output / "report.json", receipt)
    print(json.dumps(receipt))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ("config", "metrics-dir", "plain-python", "output"):
        parser.add_argument("--" + field, required=True, type=Path)
    args = parser.parse_args()
    run(args.config.absolute(), args.metrics_dir.absolute(), args.plain_python.absolute(), args.output.absolute())
