#!/usr/bin/env python3
"""Bounded read-only API baseline on the isolated, already provisioned development stand."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import secrets
import subprocess
import sys
import threading
import time

from integration_test import BUILD_ROOT, SERVER_VERSION, IntegrationFailure, IntegrationSuite, under_build


def percentile(values: list[float], fraction: float) -> float:
    return round(sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)], 3)


def workload(suite: IntegrationSuite, requests: int, clients: int) -> dict:
    headers = suite.auth_headers("reader")

    def client(count: int) -> list[float]:
        latencies = []
        for _ in range(count):
            started = time.perf_counter()
            response = suite.expect_status(200, "GET", "/api/v1/projects/visible", headers=headers)
            if response["result"].get("id") != "visible":
                raise IntegrationFailure("benchmark returned an unexpected project")
            latencies.append((time.perf_counter() - started) * 1000)
        return latencies

    started = time.perf_counter()
    with ThreadPoolExecutor(max_workers=clients) as pool:
        batches = pool.map(client, [requests // clients + (i < requests % clients) for i in range(clients)])
        latencies = [latency for batch in batches for latency in batch]
    elapsed = time.perf_counter() - started
    return {
        "requests": requests, "clients": clients, "errors": 0,
        "elapsed_seconds": round(elapsed, 3), "requests_per_second": round(requests / elapsed, 2),
        "p50_ms": percentile(latencies, 0.5), "p95_ms": percentile(latencies, 0.95),
        "p99_ms": percentile(latencies, 0.99), "max_ms": round(max(latencies), 3),
    }


def sample_rss(pid: int, stopped: threading.Event, samples: list[int]) -> None:
    while not stopped.wait(0.5):
        try:
            result = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True,
                                    text=True, timeout=2, check=False)
            if result.returncode == 0:
                samples.append(int(result.stdout.strip()))
        except (OSError, ValueError, subprocess.TimeoutExpired):
            continue


def run(binary: Path, state_root: Path) -> dict:
    suite = IntegrationSuite(binary, state_root, state_root / "reports" / "unused.json")
    suite.require_http_port_free()
    suite.admin_token = suite.read_private_token("dev_admin.token")
    suite.reader_token = suite.read_private_token("dev_reader.token")
    samples: list[int] = []
    stopped = threading.Event()
    sampler = None
    try:
        suite.start_server()
        suite.wait_for_response("/health/ready", 200, headers=suite.auth_headers("admin"))
        workload(suite, 100, 4)
        sampler = threading.Thread(target=sample_rss, args=(suite.server.pid, stopped, samples))
        sampler.start()
        runs = [workload(suite, count, clients) for count, clients in ((100, 1), (1000, 4), (10000, 16))]
    finally:
        stopped.set()
        if sampler is not None:
            sampler.join()
        suite.stop_server(expect_clean=True)
    third = max(1, len(samples) // 3)
    with binary.open("rb") as executable:
        binary_hash = hashlib.file_digest(executable, "sha256").hexdigest()
    return {
        "schema_version": "shin.server-registry-benchmark.v1", "ok": True,
        "server_version": SERVER_VERSION, "binary_sha256": binary_hash,
        "platform": {"system": platform.system(), "machine": platform.machine(), "logical_cpus": os.cpu_count()},
        "scope": "Native registry GET, warm PostgreSQL in Docker, authenticated reader, existing fixture project.",
        "connection_policy": "One HTTP connection per request; this is not a keep-alive or retrieval benchmark.",
        "client": "Python stdlib concurrent HTTP clients; client and Docker overhead are included, not isolated server CPU cost.",
        "runs": runs,
        "server_rss_kib": {
            "samples": len(samples), "max": max(samples, default=None),
            "early_median": percentile(samples[:third], 0.5) if samples else None,
            "late_median": percentile(samples[-third:], 0.5) if samples else None,
            "scope": "Sampled current native process RSS; excludes PostgreSQL and is not a proof of no leaks.",
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=BUILD_ROOT / "server" / "shin-server")
    parser.add_argument("--state-root", type=Path, default=BUILD_ROOT / "server-dev")
    args = parser.parse_args()
    binary, state = args.binary.resolve(), args.state_root.resolve()
    if not under_build(binary) or not under_build(state):
        parser.error("binary and state must be inside this development worktree's build directory")
    try:
        report = run(binary, state)
        path = state / "reports" / f"benchmark-{secrets.token_hex(8)}.json"
        descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(report, output, indent=2)
            output.write("\n")
        print(json.dumps({"ok": True, "report": str(path), "runs": report["runs"], "rss": report["server_rss_kib"]}))
        return 0
    except (IntegrationFailure, OSError, ValueError, subprocess.SubprocessError):
        print(json.dumps({"ok": False, "error": "registry-benchmark-failed"}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
