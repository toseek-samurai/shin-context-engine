#!/usr/bin/env python3
"""Measure Store readiness cost on the explicitly owned isolated PostgreSQL stand."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import secrets
import stat
import subprocess
import sys
from typing import Any

from integration_test import HOST, PORT, IntegrationFailure, under_build
from stand_guard import StandGuard, require_http_port_available


PROBE_SCHEMA = "shin.server-store-cost-probe.v1"
REPORT_SCHEMA = "shin.server-store-cost-benchmark.v1"
MAX_REPETITIONS = 100
MAX_PROBE_OUTPUT_BYTES = 2 * 1024 * 1024


class BenchmarkFailure(RuntimeError):
    pass


def bounded_count(value: str, *, name: str, minimum: int, maximum: int) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"{name} must be an integer") from error
    if not minimum <= parsed <= maximum:
        raise argparse.ArgumentTypeError(f"{name} must be between {minimum} and {maximum}")
    return parsed


def percentile(samples: list[float], fraction: float) -> float:
    return round(sorted(samples)[max(0, math.ceil(len(samples) * fraction) - 1)], 3)


def summary(samples: list[float]) -> dict[str, float | int]:
    if not samples:
        raise BenchmarkFailure("probe returned no timing samples")
    return {
        "samples": len(samples),
        "total_ms": round(sum(samples), 3),
        "p50_ms": percentile(samples, 0.50),
        "p95_ms": percentile(samples, 0.95),
    }


def resolve_without_symlink_components(path: Path, description: str, *, allow_missing_leaf: bool = False) -> Path:
    candidate = Path(os.path.abspath(path))
    current = Path(candidate.anchor)
    components = candidate.parts[1:]
    for index, component in enumerate(components):
        current /= component
        try:
            entry = current.lstat()
        except FileNotFoundError as error:
            if allow_missing_leaf and index == len(components) - 1:
                break
            raise BenchmarkFailure(f"{description} is unavailable") from error
        except OSError as error:
            raise BenchmarkFailure(f"{description} is unavailable") from error
        if stat.S_ISLNK(entry.st_mode):
            raise BenchmarkFailure(f"{description} contains a symbolic link")
    return candidate.resolve(strict=False)


def require_private_directory(path: Path, description: str) -> None:
    try:
        entry = path.lstat()
    except OSError as error:
        raise BenchmarkFailure(f"{description} is unavailable") from error
    if (stat.S_ISLNK(entry.st_mode) or not stat.S_ISDIR(entry.st_mode) or entry.st_uid != os.geteuid() or
            stat.S_IMODE(entry.st_mode) != 0o700):
        raise BenchmarkFailure(f"{description} is not a private owned directory")


def require_private_file(path: Path, description: str) -> None:
    try:
        entry = path.lstat()
    except OSError as error:
        raise BenchmarkFailure(f"{description} is unavailable") from error
    if (stat.S_ISLNK(entry.st_mode) or not stat.S_ISREG(entry.st_mode) or entry.st_uid != os.geteuid() or
            stat.S_IMODE(entry.st_mode) != 0o600):
        raise BenchmarkFailure(f"{description} is not a private owned regular file")


def require_executable_file(path: Path, description: str) -> None:
    try:
        entry = path.lstat()
    except OSError as error:
        raise BenchmarkFailure(f"{description} is unavailable") from error
    if stat.S_ISLNK(entry.st_mode) or not stat.S_ISREG(entry.st_mode) or not os.access(path, os.X_OK):
        raise BenchmarkFailure(f"{description} is not an executable regular file")


def require_child(path: Path, parent: Path, description: str) -> None:
    try:
        path.relative_to(parent)
    except ValueError as error:
        raise BenchmarkFailure(f"{description} is outside its explicitly owned parent") from error


def parse_probe_output(output: bytes, *, iterations: int, warmup: int) -> tuple[float, list[float], list[float]]:
    if len(output) > MAX_PROBE_OUTPUT_BYTES:
        raise BenchmarkFailure("native probe output exceeds the bounded machine JSON limit")

    def reject_duplicate_object_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, item in pairs:
            if key in result:
                raise ValueError("duplicate JSON object key")
            result[key] = item
        return result

    def reject_nonfinite_json(_: str) -> None:
        raise ValueError("non-finite JSON number")

    try:
        value = json.loads(output.decode("utf-8"), object_pairs_hook=reject_duplicate_object_keys,
                           parse_constant=reject_nonfinite_json)
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
        raise BenchmarkFailure("native probe did not return machine JSON") from error
    expected_fields = {
        "schema_version", "tool", "ok", "iterations", "warmup", "cold_first_readiness_ms",
        "warm_pooled_readiness_ms", "raw_select1_lower_bound_ms",
    }
    if (not isinstance(value, dict) or set(value) != expected_fields or value.get("schema_version") != PROBE_SCHEMA or
            value.get("tool") != "shin-server-store-cost-probe" or value.get("ok") is not True):
        raise BenchmarkFailure("native probe returned an invalid result")
    if (type(value.get("iterations")) is not int or type(value.get("warmup")) is not int or
            value["iterations"] != iterations or value["warmup"] != warmup):
        raise BenchmarkFailure("native probe did not preserve requested bounds")

    def timing(name: str) -> float:
        candidate = value.get(name)
        if type(candidate) not in (float, int) or not math.isfinite(float(candidate)) or candidate < 0:
            raise BenchmarkFailure("native probe returned an invalid timing")
        return float(candidate)

    def timings(name: str) -> list[float]:
        candidate = value.get(name)
        if not isinstance(candidate, list) or len(candidate) != iterations:
            raise BenchmarkFailure("native probe returned an invalid sample count")
        values = []
        for item in candidate:
            if type(item) not in (float, int) or not math.isfinite(float(item)) or item < 0:
                raise BenchmarkFailure("native probe returned an invalid timing sample")
            values.append(float(item))
        return values

    return timing("cold_first_readiness_ms"), timings("warm_pooled_readiness_ms"), timings("raw_select1_lower_bound_ms")


def execute_probe(command: list[str], *, iterations: int, warmup: int) -> tuple[float, list[float], list[float]]:
    try:
        completed = subprocess.run(
            command,
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=False,
            timeout=600,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise BenchmarkFailure("native probe could not complete") from error
    if completed.returncode != 0 or completed.stderr:
        raise BenchmarkFailure("native probe failed")
    return parse_probe_output(completed.stdout, iterations=iterations, warmup=warmup)


def write_report(path: Path, report: dict[str, Any]) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(report, handle, sort_keys=True, separators=(",", ":"))
        handle.write("\n")


def binary_sha256(path: Path) -> str:
    with path.open("rb") as executable:
        return hashlib.file_digest(executable, "sha256").hexdigest()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path, help="test-only native store cost probe below build")
    parser.add_argument("--state-root", required=True, type=Path, help="explicit isolated development state below build")
    parser.add_argument("--config", required=True, type=Path, help="explicit runtime config below the owned state root")
    parser.add_argument("--iterations", type=lambda value: bounded_count(value, name="--iterations", minimum=1, maximum=10000), default=100)
    parser.add_argument("--warmup", type=lambda value: bounded_count(value, name="--warmup", minimum=1, maximum=10000), default=10)
    parser.add_argument("--repetitions", type=lambda value: bounded_count(value, name="--repetitions", minimum=1, maximum=MAX_REPETITIONS), default=5)
    parser.add_argument("--report", type=Path, help="new machine JSON path below the owned state root")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    try:
        probe = resolve_without_symlink_components(args.probe, "native probe")
        state_root = resolve_without_symlink_components(args.state_root, "state root")
        config = resolve_without_symlink_components(args.config, "runtime config")
        report_input = args.report if args.report else state_root / "reports" / f"store-cost-{secrets.token_hex(8)}.json"
        report_path = resolve_without_symlink_components(report_input, "report", allow_missing_leaf=True)
        if not under_build(probe) or not under_build(state_root):
            raise BenchmarkFailure("probe and state root must be below this worktree's build directory")
        require_executable_file(probe, "native probe")
        require_private_directory(state_root, "state root")
        require_private_file(config, "runtime config")
        require_child(config, state_root, "runtime config")
        require_child(report_path, state_root, "report")
        if report_path.exists():
            raise BenchmarkFailure("report path already exists")
        require_private_directory(report_path.parent, "report directory")

        # Keep the lock alive across port inspection and every probe repetition.
        guard_alive = StandGuard()
        try:
            try:
                require_http_port_available(HOST, PORT)
            except OSError as error:
                raise BenchmarkFailure("native Server listener is active; stop it before measuring Store directly") from error

            command = [
                str(probe), "--config", str(config), "--iterations", str(args.iterations), "--warmup", str(args.warmup),
            ]
            initial_binary_sha256 = binary_sha256(probe)
            cold_samples: list[float] = []
            warm_samples: list[float] = []
            raw_samples: list[float] = []
            for _ in range(args.repetitions):
                cold, warm, raw = execute_probe(command, iterations=args.iterations, warmup=args.warmup)
                cold_samples.append(cold)
                warm_samples.extend(warm)
                raw_samples.extend(raw)

            if binary_sha256(probe) != initial_binary_sha256:
                raise BenchmarkFailure("native probe changed during measurement")
            raw_summary = summary(raw_samples)
            report = {
                "schema_version": REPORT_SCHEMA,
                "tool": "shin-server-store-cost-benchmark",
                "ok": True,
                "execution": {
                    "probe_binary_sha256": initial_binary_sha256,
                    "argv": command,
                    "repetitions": args.repetitions,
                    "iterations_per_repetition": args.iterations,
                    "warmup_per_repetition": args.warmup,
                },
                "measurements": {
                    "cold_first_readiness_ms": summary(cold_samples),
                    "warm_pooled_readiness_ms": summary(warm_samples),
                    "raw_select1_lower_bound_ms": {**raw_summary, "baseline_total_ms": raw_summary["total_ms"]},
                },
                "boundaries": [
                    "Store measurements use the current public Store::readiness() with the supplied runtime configuration; no schema layout is hard-coded.",
                    "The raw libpqxx SELECT 1 baseline uses an already-connected separate session and excludes connection setup.",
                    "SELECT 1 is a lower-bound comparison, not isolated schema cost or a production throughput measurement.",
                    "The driver does not enable pg_stat_statements or any PostgreSQL extension and does not start, stop, migrate, or reset the stand.",
                    "Results are local development measurements, not a production optimization or capacity claim.",
                ],
            }
            write_report(report_path, report)
            print(json.dumps({"ok": True, "report": str(report_path), "repetitions": args.repetitions}))
            return 0
        finally:
            del guard_alive
    except (BenchmarkFailure, IntegrationFailure, OSError, ValueError, subprocess.SubprocessError):
        print("error: store cost benchmark failed", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
