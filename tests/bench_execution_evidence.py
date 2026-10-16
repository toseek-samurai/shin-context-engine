#!/usr/bin/env python3
"""Benchmark the external execution-evidence CLI on a temporary corpus."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import shutil
import subprocess
import sys
import tempfile
import time


TASK_ID = "execution-evidence-benchmark-task"
THREAD_ID = "execution-evidence-benchmark-thread"
PRODUCER = "execution-evidence-benchmark-producer"
FINGERPRINT = "fnv1a64:0000000000000000"
REPORT_CASES = 10_000
MAX_BUNDLE_BYTES = 256 * 1024


class BenchmarkError(RuntimeError):
    pass


def require(value: bool, message: str) -> None:
    if not value:
        raise BenchmarkError(message)


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")


def quantiles(values: list[float | int]) -> dict[str, float | int]:
    ordered = sorted(values)
    require(ordered, "empty benchmark sample")

    def pick(fraction: float) -> float | int:
        return ordered[min(len(ordered) - 1, int(fraction * (len(ordered) - 1) + 0.5))]

    return {"min": ordered[0], "p50": pick(0.50), "p95": pick(0.95), "max": ordered[-1]}


def child_peak_rss_bytes() -> int:
    raw = int(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
    require(raw > 0, "child RSS unavailable")
    require(sys.platform == "darwin" or sys.platform.startswith("linux"), "RSS units are defined only for macOS/Linux")
    return raw if sys.platform == "darwin" else raw * 1024


def snapshot(root: Path) -> dict[str, tuple[str, int, str]]:
    result: dict[str, tuple[str, int, str]] = {}
    for path in [root, *sorted(root.rglob("*"))]:
        rel = "." if path == root else path.relative_to(root).as_posix()
        stat = path.lstat()
        if path.is_symlink():
            result[rel] = ("link:" + os.readlink(path), stat.st_size, "")
        elif path.is_file():
            result[rel] = ("file", stat.st_size, hashlib.sha256(path.read_bytes()).hexdigest())
        elif path.is_dir():
            result[rel] = ("directory", 0, "")
        else:
            result[rel] = ("special", stat.st_size, "")
    return result


def make_tree(root: Path, name: str, count: int, size: int) -> dict[str, object]:
    tree = root / "execution-bench" / name
    tree.mkdir(parents=True)
    block = bytes((65 + index % 26 for index in range(size)))
    for index in range(count):
        (tree / f"case-{index:04d}.bin").write_bytes(block)
    return {"name": name, "path": tree.relative_to(root).as_posix(), "files": count,
            "file_bytes": size, "tree_bytes": count * size, "outside_profile_source_root": True}


def make_report(path: Path) -> int:
    header = b'<?xml version="1.0" encoding="UTF-8"?>\n<testsuite tests="10000" failures="0" errors="0" skipped="0">\n'
    cases = b"".join(f'<testcase name="case-{i:05d}" classname="execution-benchmark"/>\n'.encode("ascii")
                      for i in range(REPORT_CASES))
    payload = header + cases + b"</testsuite>\n"
    path.write_bytes(payload)
    require(payload.count(b"<testcase ") == REPORT_CASES, "JUnit testcase count mismatch")
    require(450 * 1024 <= len(payload) <= 650 * 1024, "JUnit report is not approximately 500 KiB")
    return len(payload)


class Benchmark:
    def __init__(self, tool: Path, source_root: Path, runs: int, scope_runs: int, warmup: int) -> None:
        self.tool = tool.resolve()
        self.source_root = source_root.resolve()
        self.runs, self.scope_runs, self.warmup = runs, scope_runs, warmup
        self.base = Path(tempfile.mkdtemp(prefix="shin-execution-evidence-bench-"))
        self.root, self.profiles = self.base / "consumer", self.base / "profiles"
        self.out, self.artifacts = self.base / "cache" / "index", self.base / "artifacts"
        self.profile_id = "execution-evidence"
        self.clock = int(time.time())
        self.env = dict(os.environ, LC_ALL="C", LANG="C",
                        SHIN_EXECUTION_EVIDENCE_SENTINEL=str(self.base / "unexpected-execution"))
        self.datasets: list[dict[str, object]] = []
        self.report_bytes = 0
        self.small_bytes = 0
        self.before: dict[str, tuple[str, int, str]] = {}

    def __enter__(self) -> "Benchmark":
        fixture = self.source_root / "tests" / "fixtures" / "execution-evidence"
        self.artifacts.mkdir(parents=True)
        shutil.copytree(fixture / "root", self.root)
        self.profiles.mkdir()
        profile_path = self.profiles / "execution-evidence.json"
        shutil.copyfile(fixture / "profiles" / "execution-evidence.json", profile_path)
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        self.datasets = [make_tree(self.root, "selected-128x16k", 128, 16 * 1024),
                         make_tree(self.root, "selected-1024x8k", 1024, 8 * 1024)]
        owned = [*profile["sourceFactRoots"], *profile["rootFiles"]]
        for dataset in self.datasets:
            path = str(dataset["path"])
            require(all(path != parent and not path.startswith(parent + "/") for parent in owned),
                    f"selector overlaps profile source root: {path}")
        self.small_bytes = sum(path.stat().st_size for path in self.root.rglob("*")
                               if path.is_file() and not path.relative_to(self.root).as_posix().startswith("execution-bench/"))
        self.report_bytes = make_report(self.artifacts / "junit-10000.xml")
        self.before = snapshot(self.root)
        result, _, _ = self.call("build")
        require(result.get("schema_version") == "shin.build.v1" and result.get("command") == "build" and
                result.get("ok") is True and result.get("published") is True, "build envelope mismatch")
        return self

    def __exit__(self, *_args: object) -> None:
        shutil.rmtree(self.base, ignore_errors=True)

    def argv(self, command: str, *args: str) -> list[str]:
        return [str(self.tool), "--profile", self.profile_id, "--profile-dir", str(self.profiles),
                "--root", str(self.root.resolve()), "--out-dir", str(self.out), command, *args]

    def call(self, command: str, *args: str) -> tuple[dict[str, object], float, int]:
        started = time.perf_counter()
        process = subprocess.run(self.argv(command, *args), capture_output=True, env=self.env,
                                 cwd=self.source_root, timeout=180)
        elapsed = (time.perf_counter() - started) * 1000
        require(process.returncode == 0, f"{command} exit {process.returncode}: {process.stderr[:300]!r}")
        require(not process.stderr, f"{command} wrote stderr: {process.stderr[:300]!r}")
        try:
            payload = json.loads(process.stdout.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise BenchmarkError(f"{command} did not emit JSON: {error}") from error
        require(isinstance(payload, dict), f"{command} output is not an object")
        return payload, elapsed, len(process.stdout)

    def binding_ok(self, binding: object) -> bool:
        return isinstance(binding, dict) and binding.get("root") == str(self.root.resolve()) and \
            binding.get("profile") == self.profile_id and binding.get("task_id") == TASK_ID and \
            binding.get("thread_id") == THREAD_ID

    def scope_request(self, coverage: str, selector: dict[str, str] | None) -> dict[str, object]:
        return {"schema_version": "shin.execution-input-request.v1", "coverage": coverage,
                "selectors": [] if selector is None else [selector]}

    def validate_scope(self, result: dict[str, object], coverage: str, selector: dict[str, str] | None) -> None:
        expected = [] if selector is None else [selector]
        require(result.get("schema_version") == "shin.execution-inputs.v1" and
                result.get("coverage") == coverage and self.binding_ok(result.get("binding")), "scope mismatch")
        require(result.get("selectors") == expected and isinstance(result.get("observations"), list) and
                len(result["observations"]) == len(expected), "scope selector mismatch")
        if selector is not None:
            observation = result["observations"][0]
            dataset = next(item for item in self.datasets if item["path"] == selector["path"])
            require(observation.get("observation") == "observed" and observation.get("files") == dataset["files"] and
                    observation.get("bytes") == dataset["tree_bytes"], "tree observation mismatch")

    def validate_import(self, result: dict[str, object], selector: dict[str, str]) -> None:
        require(result.get("schema_version") == "shin.receipt-bundle.v1" and result.get("command") == "receipt-import" and
                result.get("ok") is True, "import envelope mismatch")
        receipts = result.get("receipts")
        require(isinstance(receipts, list) and len(receipts) == 1, "import receipt count mismatch")
        receipt = receipts[0]
        inputs, report = receipt.get("inputs"), receipt.get("report")
        require(receipt.get("schema_version") == "shin.execution-receipt.v1" and receipt.get("result") == "pass" and
                self.binding_ok(receipt.get("scope")) and isinstance(inputs, dict) and
                inputs.get("coverage") == "declared-complete" and inputs.get("selectors") == [selector], "receipt scope mismatch")
        summary = report.get("summary") if isinstance(report, dict) else None
        require(isinstance(summary, dict) and report.get("format") == "junit" and
                report.get("bytes") == self.report_bytes and report.get("payload_retained") is False and
                summary.get("total") == REPORT_CASES and summary.get("passed") == REPORT_CASES and
                summary.get("failed") == 0 and summary.get("skipped") == 0, "receipt counters mismatch")

    def validate_check(self, result: dict[str, object]) -> None:
        require(result.get("schema_version") == "shin.receipt-assessment.v1" and result.get("command") == "receipt-check" and
                result.get("ok") is True and result.get("strict") is True and result.get("require_current") is True and
                result.get("index_current") is True, "check envelope mismatch")
        receipts = result.get("receipts")
        require(isinstance(receipts, list) and len(receipts) == 32, "check receipt count mismatch")
        require(all(r.get("producer_trusted") is True and r.get("inputs_current") is True and
                    r.get("scope_current") is True and r.get("narrowed_scope_used") is True and
                    r.get("passed_current") is True for r in receipts), "check assessment mismatch")

    def phase(self, name: str, command: str, args: tuple[str, ...], check, runs: int,
              report_bytes: int, tree_bytes: int) -> tuple[dict[str, object], dict[str, object]]:
        for _ in range(self.warmup):
            result, _, _ = self.call(command, *args)
            check(result)
        wall, outputs, first = [], [], None
        for _ in range(runs):
            result, elapsed, output_bytes = self.call(command, *args)
            check(result)
            first = result if first is None else first
            wall.append(elapsed)
            outputs.append(output_bytes)
        return first, {"name": name, "runs": runs, "warmup": self.warmup, "wall_ms": quantiles(wall),
                       "report_bytes": report_bytes, "tree_bytes": tree_bytes,
                       "output_bytes": quantiles(outputs), "child_peak_rss_bytes": child_peak_rss_bytes()}

    def run(self) -> dict[str, object]:
        phases: list[dict[str, object]] = []
        profile_request = self.artifacts / "profile-request.json"
        write_json(profile_request, self.scope_request("profile", None))
        _, phase = self.phase("capture-full-profile", "receipt-scope",
                              ("--input", str(profile_request), "--task-id", TASK_ID, "--thread-id", THREAD_ID,
                               "--observed-at", str(self.clock), "--require-current"),
                              lambda result: self.validate_scope(result, "profile", None), self.runs, 0, self.small_bytes)
        phases.append(phase)

        for dataset in self.datasets:
            selector = {"path": str(dataset["path"]), "kind": "tree"}
            name = str(dataset["name"])
            request_path = self.artifacts / f"{name}-scope.json"
            inputs_path = self.artifacts / f"{name}-inputs.json"
            write_json(request_path, self.scope_request("declared-complete", selector))
            first, phase = self.phase(f"scope-{name}", "receipt-scope",
                                      ("--input", str(request_path), "--task-id", TASK_ID, "--thread-id", THREAD_ID,
                                       "--observed-at", str(self.clock), "--require-current"),
                                      lambda result: self.validate_scope(result, "declared-complete", selector),
                                      self.scope_runs, 0, int(dataset["tree_bytes"]))
            write_json(inputs_path, first)
            phases.append(phase)

            import_request = {
                "schema_version": "shin.receipt-import-request.v1", "format": "junit",
                "report_path": str(self.artifacts / "junit-10000.xml"), "inputs_path": str(inputs_path),
                "receipt_id": f"{name}-receipt", "producer": PRODUCER, "started_at": self.clock + 1,
                "observed_at": self.clock + 2, "ttl_seconds": 86400, "command_fingerprint": FINGERPRINT,
                "complete": True,
            }
            import_path = self.artifacts / f"{name}-import.json"
            write_json(import_path, import_request)
            first, phase = self.phase(f"receipt-import-{name}", "receipt-import",
                                      ("--input", str(import_path), "--task-id", TASK_ID, "--thread-id", THREAD_ID,
                                       "--observed-at", str(self.clock + 3), "--require-current"),
                                      lambda result: self.validate_import(result, selector), self.runs,
                                      self.report_bytes, int(dataset["tree_bytes"]))
            phases.append(phase)

            receipts = []
            for index in range(32):
                receipt = copy.deepcopy(first["receipts"][0])
                receipt["receipt_id"] = f"{name}-assessment-{index:02d}"
                receipts.append(receipt)
            bundle = {"schema_version": "shin.receipt-bundle.v1", "tool": "shin-context",
                      "command": "receipt-import", "ok": True, "receipts": receipts}
            bundle_path = self.artifacts / f"{name}-32-receipts.json"
            write_json(bundle_path, bundle)
            bundle_bytes = bundle_path.stat().st_size
            require(bundle_bytes <= MAX_BUNDLE_BYTES and len({json.dumps(r["inputs"], sort_keys=True) for r in receipts}) == 1,
                    "assessment bundle is oversized or manifests differ")
            _, phase = self.phase(f"receipt-check-{name}-32", "receipt-check",
                                  ("--input", str(bundle_path), "--task-id", TASK_ID, "--thread-id", THREAD_ID,
                                   "--observed-at", str(self.clock + 3), "--trusted-producer", PRODUCER,
                                   "--require-current", "--strict"), self.validate_check, self.runs,
                                  self.report_bytes, int(dataset["tree_bytes"]))
            phase.update({"receipt_bundle_bytes": bundle_bytes, "assessment_receipts": 32,
                          "identical_captured_manifest": True})
            phases.append(phase)

        require(not (self.base / "unexpected-execution").exists(), "execution sentinel was triggered")
        require(snapshot(self.root) == self.before, "consumer repository was mutated")
        return {"schema_version": "shin.execution-evidence-bench.v1", "command": "bench-execution-evidence",
                "ok": True, "iterations": self.runs, "scope_runs": self.scope_runs, "warmup": self.warmup,
                "platform": {"system": platform.system(), "machine": platform.machine()},
                "artifacts_external": True, "cache_external": True, "consumer_mutated": False,
                "evidence_kind": "synthetic report throughput; no underlying tests were executed by this benchmark",
                "run_plan": {"measured_samples_per_phase": self.runs, "scope_samples_per_tree": self.scope_runs,
                             "warmup_samples_per_phase": self.warmup}, "datasets": self.datasets,
                "junit": {"testcase_records": REPORT_CASES, "report_bytes": self.report_bytes},
                "receipt_bundle_limit_bytes": MAX_BUNDLE_BYTES,
                "child_peak_rss": {"bytes": child_peak_rss_bytes(), "normalized": True,
                                   "platform": "macOS" if sys.platform == "darwin" else "Linux",
                                   "source": "resource.RUSAGE_CHILDREN.ru_maxrss",
                                   "scope": "cumulative child-process high-water, not per-phase allocation or leak proof"}, "phases": phases}


def env_int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, str(default)))
    except ValueError as error:
        raise SystemExit(f"{name} must be an integer") from error


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tool", type=Path)
    parser.add_argument("source_root", type=Path)
    parser.add_argument("--iterations", "--runs", dest="runs", type=int,
                        default=env_int("SHIN_EXECUTION_BENCH_RUNS", 20), help="measured samples per phase")
    parser.add_argument("--scope-runs", type=int,
                        default=env_int("SHIN_EXECUTION_BENCH_SCOPE_RUNS", 10), help="measured selected-tree scope samples")
    parser.add_argument("--warmup", type=int,
                        default=env_int("SHIN_EXECUTION_BENCH_WARMUP", 2), help="discarded validated samples per phase")
    parser.add_argument("--smoke", action="store_true", help="run one measured sample, one scope sample and no warmup")
    args = parser.parse_args(argv)
    if args.smoke:
        args.runs, args.scope_runs, args.warmup = 1, 1, 0
    if not args.tool.is_file() or not os.access(args.tool, os.X_OK):
        parser.error(f"tool is not executable: {args.tool}")
    if not args.source_root.is_dir() or args.runs < 1 or args.scope_runs < 1 or args.warmup < 0:
        parser.error("source_root must exist and run counts must be positive (warmup may be zero)")
    benchmark = Benchmark(args.tool, args.source_root, args.runs, args.scope_runs, args.warmup)
    try:
        with benchmark:
            result = benchmark.run()
    except (BenchmarkError, OSError, subprocess.SubprocessError) as error:
        print(f"execution evidence benchmark failed: {error}", file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(benchmark.base, ignore_errors=True)
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
