#!/usr/bin/env python3
"""Aggregate the provisioned Shin Server companion verification suites.

This runner owns no PostgreSQL, Docker, or native-process cleanup. Existing
harnesses retain their lifecycle and shared-stand guard responsibilities.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass
from typing import Callable, Sequence


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
REPORT_SCHEMA = "shin.companion-check-report.v1"
MAX_LOG_BYTES = 64 * 1024
MAX_REPORT_BYTES = 8 * 1024 * 1024

# Keep the inherited stand guard's scope unchanged: holding it through child
# harnesses would make their own non-reentrant acquisition fail.
sys.path.insert(0, str(REPOSITORY_ROOT / "server" / "tests"))
from stand_guard import StandGuard  # noqa: E402


class CompanionCheckError(RuntimeError):
    pass


@dataclass(frozen=True)
class BinaryIdentity:
    path: Path
    sha256: str

    def report_value(self) -> dict[str, str]:
        return {"path": str(self.path), "sha256": self.sha256}


@dataclass(frozen=True)
class Inputs:
    build_root: Path
    server_binary: Path
    observer_binary: Path
    core_binary: Path
    ledger_probe: Path
    activation_store_probe: Path
    preflight_observation_probe: Path
    publication_review_probe: Path
    retrieval_journal_probe: Path
    json_codec_probe: Path
    server_build_root: Path
    observer_build_root: Path
    state_root: Path
    report_root: Path
    lane: str


@dataclass(frozen=True)
class Suite:
    name: str
    command: tuple[str, ...]
    report_name: str
    report_kind: str
    covered_scopes: tuple[str, ...]


Runner = Callable[[Sequence[str]], subprocess.CompletedProcess[str]]


def under(path: Path, parent: Path) -> bool:
    try:
        path.resolve(strict=False).relative_to(parent.resolve(strict=False))
    except ValueError:
        return False
    return True


def require_private_directory(path: Path, label: str) -> Path:
    if not path.is_absolute():
        raise CompanionCheckError(f"{label} must be absolute")
    try:
        initial = path.lstat()
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise CompanionCheckError(f"{label} must be an existing directory") from error
    entry = resolved.lstat()
    if stat.S_ISLNK(initial.st_mode) or stat.S_ISLNK(entry.st_mode) or not stat.S_ISDIR(entry.st_mode):
        raise CompanionCheckError(f"{label} must be an existing directory")
    if entry.st_uid != os.geteuid() or stat.S_IMODE(entry.st_mode) != 0o700:
        raise CompanionCheckError(f"{label} must be an owner-only directory")
    return resolved


def require_directory(path: Path, label: str) -> Path:
    if not path.is_absolute():
        raise CompanionCheckError(f"{label} must be absolute")
    try:
        initial = path.lstat()
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise CompanionCheckError(f"{label} must be an existing directory") from error
    entry = resolved.lstat()
    if stat.S_ISLNK(initial.st_mode) or stat.S_ISLNK(entry.st_mode) or not stat.S_ISDIR(entry.st_mode):
        raise CompanionCheckError(f"{label} must be an existing directory")
    return resolved


def require_executable(path: Path, label: str, parent: Path, name: str) -> Path:
    if not path.is_absolute():
        raise CompanionCheckError(f"{label} must be absolute")
    try:
        initial = path.lstat()
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise CompanionCheckError(f"{label} must be an executable regular file") from error
    entry = resolved.lstat()
    if stat.S_ISLNK(initial.st_mode) or stat.S_ISLNK(entry.st_mode) or not stat.S_ISREG(entry.st_mode) or not os.access(resolved, os.X_OK):
        raise CompanionCheckError(f"{label} must be an executable regular file")
    if resolved.name != name or resolved.parent != parent:
        raise CompanionCheckError(f"{label} must be exactly {parent / name}")
    return resolved


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as input_file:
        for block in iter(lambda: input_file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def binary_identity(path: Path) -> BinaryIdentity:
    return BinaryIdentity(path=path, sha256=sha256(path))


def successful_check(value: object) -> bool:
    if not isinstance(value, dict) or set(value) != {"name", "ok", "elapsed_ms"}:
        return False
    duration = value["elapsed_ms"]
    return (isinstance(value["name"], str) and bool(value["name"].strip()) and value["ok"] is True and
            type(duration) in (int, float) and duration >= 0 and (type(duration) is int or math.isfinite(duration)))


def cmake_cache_value(build_root: Path, name: str) -> str | None:
    cache = build_root / "CMakeCache.txt"
    try:
        lines = cache.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise CompanionCheckError(f"{build_root} has no readable CMakeCache.txt") from error
    prefix = f"{name}:"
    for line in lines:
        if line.startswith(prefix) and "=" in line:
            return line.split("=", 1)[1]
    return None


def require_cache_flag(build_root: Path, name: str, expected: str) -> None:
    if cmake_cache_value(build_root, name) != expected:
        raise CompanionCheckError(f"{build_root} must set {name}={expected}")


def require_sanitizer_environment() -> None:
    def options(name: str) -> dict[str, str]:
        result = {}
        for field in os.environ.get(name, "").split(":"):
            if not field:
                continue
            key, separator, value = field.partition("=")
            if not separator or not key or key in result:
                raise CompanionCheckError(f"{name} has malformed or duplicate options")
            result[key] = value
        return result

    asan, ubsan = options("ASAN_OPTIONS"), options("UBSAN_OPTIONS")
    if asan.get("detect_leaks") != "1" or asan.get("halt_on_error") != "1":
        raise CompanionCheckError("sanitizer lane requires ASAN_OPTIONS detect_leaks=1 and halt_on_error=1")
    if ubsan.get("halt_on_error") != "1":
        raise CompanionCheckError("sanitizer lane requires UBSAN_OPTIONS halt_on_error=1")
    # Observer fixtures deliberately clear PATH to verify native independence.
    symbolizer = Path(os.environ.get("ASAN_SYMBOLIZER_PATH", ""))
    if not symbolizer.is_absolute() or not symbolizer.is_file() or not os.access(symbolizer, os.X_OK):
        raise CompanionCheckError("sanitizer lane requires ASAN_SYMBOLIZER_PATH to name an absolute executable from the matching LLVM toolchain")


def validate_inputs(args: argparse.Namespace, *, repository_root: Path = REPOSITORY_ROOT) -> Inputs:
    build_root = (repository_root / "build").resolve(strict=False)
    server_build_root = require_directory(Path(args.server_build_root), "--server-build-root")
    observer_build_root = require_directory(Path(args.observer_build_root), "--observer-build-root")
    core_root = require_directory(Path(args.core_root), "--core-root")
    state_root = require_private_directory(Path(args.state_root), "--state-root")
    report_root = require_private_directory(Path(args.report_root), "--report-root")
    for label, path in (("--server-build-root", server_build_root), ("--observer-build-root", observer_build_root),
                        ("--core-root", core_root), ("--state-root", state_root)):
        if not under(path, build_root):
            raise CompanionCheckError(f"{label} must be below this worktree build directory")
    if not under(report_root, state_root):
        raise CompanionCheckError("--report-root must be below --state-root")
    server_binary = require_executable(Path(args.server_binary), "--server-binary", server_build_root, "shin-server")
    observer_binary = require_executable(Path(args.observer_binary), "--observer-binary", observer_build_root, "shin-observer")
    core_binary = require_executable(core_root / "shin-context", "--core-root", core_root, "shin-context")
    ledger_probe = require_executable(Path(args.ledger_probe), "--ledger-probe", server_build_root,
                                      "shin-server-preparation-store-tests")
    activation_store_probe = require_executable(Path(args.activation_store_probe), "--activation-store-probe",
                                                 server_build_root, "shin-server-activation-store-tests")
    preflight_observation_probe = require_executable(Path(args.preflight_observation_probe),
                                                     "--preflight-observation-probe", server_build_root,
                                                     "shin-server-preflight-observation-tests")
    publication_review_probe = require_executable(server_build_root / "shin-server-publication-review-tests",
                                                   "publication review probe", server_build_root,
                                                   "shin-server-publication-review-tests")
    retrieval_journal_probe = require_executable(server_build_root / "shin-server-retrieval-journal-store-probe",
                                                 "retrieval journal probe", server_build_root,
                                                 "shin-server-retrieval-journal-store-probe")
    json_codec_probe = require_executable(server_build_root / "shin-server-json-codec-probe",
                                          "JSON codec probe", server_build_root, "shin-server-json-codec-probe")
    if args.lane == "sanitizer":
        require_cache_flag(server_build_root, "SHIN_SERVER_SANITIZERS", "ON")
        require_cache_flag(observer_build_root, "SHIN_OBSERVER_SANITIZERS", "ON")
        require_sanitizer_environment()
    else:
        require_cache_flag(server_build_root, "SHIN_SERVER_SANITIZERS", "OFF")
        require_cache_flag(observer_build_root, "SHIN_OBSERVER_SANITIZERS", "OFF")
    return Inputs(build_root, server_binary, observer_binary, core_binary, ledger_probe, activation_store_probe,
                  preflight_observation_probe, publication_review_probe, retrieval_journal_probe, json_codec_probe, server_build_root,
                  observer_build_root, state_root, report_root, args.lane)


class CompanionLock:
    def __init__(self, path: Path) -> None:
        self._fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            entry = os.fstat(self._fd)
            if not stat.S_ISREG(entry.st_mode) or entry.st_uid != os.geteuid() or entry.st_nlink != 1 or stat.S_IMODE(entry.st_mode) != 0o600:
                raise CompanionCheckError("companion aggregate lock is not a private regular file")
            try:
                fcntl.flock(self._fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise CompanionCheckError("another companion aggregate already owns the development stand") from error
        except BaseException:
            os.close(self._fd)
            raise

    def __enter__(self) -> "CompanionLock":
        return self

    def __exit__(self, *_: object) -> None:
        os.close(self._fd)


class CompanionCheck:
    def __init__(self, inputs: Inputs, *, runner: Runner | None = None,
                 stand_guard_factory: Callable[[], object] = StandGuard) -> None:
        self.inputs = inputs
        self.runner = runner or self.default_runner
        self.stand_guard_factory = stand_guard_factory
        self.identities = {name: binary_identity(path) for name, path in {
            "server": inputs.server_binary,
            "observer": inputs.observer_binary,
            "core": inputs.core_binary,
            "ledger_probe": inputs.ledger_probe,
            "activation_store_probe": inputs.activation_store_probe,
            "preflight_observation_probe": inputs.preflight_observation_probe,
            "publication_review_probe": inputs.publication_review_probe,
            "retrieval_journal_probe": inputs.retrieval_journal_probe,
            "json_codec_probe": inputs.json_codec_probe,
        }.items()}
        self.run_id = uuid.uuid4().hex
        self.run_root = self.inputs.report_root / f"companion-{self.run_id}"
        self.run_root.mkdir(mode=0o700)
        self.checks: list[dict[str, object]] = []

    @staticmethod
    def default_runner(command: Sequence[str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(list(command), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, check=False, cwd=REPOSITORY_ROOT)

    def suites(self) -> list[Suite]:
        from operations_budget_integration_test import DEFAULT_ACCEPTANCE
        report = lambda name: str(self.run_root / name)
        python, server, observer, core, state = sys.executable, str(self.inputs.server_binary), str(self.inputs.observer_binary), str(self.inputs.core_binary), str(self.inputs.state_root)
        return [
            Suite("server-ctest", ("ctest", "--test-dir", str(self.inputs.server_build_root), "--no-tests=error", "--output-on-failure", "--output-junit", report("server-ctest.junit.xml")), "server-ctest.junit.xml", "junit", ("server-native-unit",)),
            Suite("observer-ctest", ("ctest", "--test-dir", str(self.inputs.observer_build_root), "--no-tests=error", "--output-on-failure", "--output-junit", report("observer-ctest.junit.xml")), "observer-ctest.junit.xml", "junit", ("observer-native-unit",)),
            Suite("server-publication", (python, "server/tests/publication_integration_test.py", "--binary", server, "--core", core, "--ledger-probe", str(self.inputs.ledger_probe), "--state-root", state, "--load", "--load-requests", "128", "--report", report("server-publication.json")), "server-publication.json", "server", ("server-foundation", "server-retrieval", "server-candidate-lifecycle", "server-publication-proposals", "publication-history-retirement")),
            Suite("server-audit-history", (python, "server/tests/audit_history_integration_test.py", "--binary", server, "--state-root", state, "--load-events", "16384", "--report", report("server-audit-history.json")), "server-audit-history.json", "server", ("server-audit-history",)),
            Suite("server-observer-history", (python, "server/tests/observer_reclamation_integration_test.py", "--binary", server, "--observer", observer, "--state-root", state, "--load-requests", "4096", "--report", report("server-observer-history.json")), "server-observer-history.json", "server", ("observer-ingestion", "observer-history", "observer-reconciliation", "observer-registry-reclamation")),
            Suite("server-recovery", (python, "server/tests/recovery_integration_test.py", "--binary", server, "--observer", observer, "--state-root", state, "--report", report("server-recovery.json")), "server-recovery.json", "recovery", ("server-recovery", "observer-reviewed-re-enrollment")),
            Suite("observer-integration", (python, "observer/tests/integration_test.py", "--observer", observer, "--core", core, "--report", report("observer-integration.json")), "observer-integration.json", "observer", ("observer-local", "observer-outbox-archive")),
            Suite("observer-delivery", (python, "observer/tests/delivery_integration_test.py", "--observer", observer, "--core", core, "--report", report("observer-delivery.json")), "observer-delivery.json", "delivery", ("observer-delivery",)),
            Suite("headless-workflow", (python, "server/tests/headless_workflow_test.py", "--server", server, "--observer", observer, "--core", core, "--state-root", state, "--report", report("headless-workflow.json"), "--backup-restore"), "headless-workflow.json", "headless", ("agent-continuity", "offline-core-observer", "clean-database-artifact-restore")),
            Suite("fault-budgets", (python, "server/tests/fault_budget_integration_test.py", "--binary", server, "--observer", observer, "--core", core, "--state-root", state, "--report", report("fault-budgets.json")), "fault-budgets.json", "faults", ("database-outage-deadlines", "offline-saturated-outbox",)),
            Suite("operations-budgets", (python, "server/tests/operations_budget_integration_test.py", "--binary", server, "--core", core, "--state-root", state, "--lane", self.inputs.lane, "--acceptance", DEFAULT_ACCEPTANCE, "--report", report("operations-budgets.json")), "operations-budgets.json", "operations", ("combined-background-load", "http-saturation-shutdown", "current-rss-descriptors",)),
        ]

    def assert_identities_unchanged(self) -> None:
        for name, expected in self.identities.items():
            try:
                observed = binary_identity(expected.path)
            except OSError as error:
                raise CompanionCheckError(f"{name} binary disappeared during companion verification") from error
            if observed != expected:
                raise CompanionCheckError(f"{name} binary identity changed during companion verification")

    @staticmethod
    def validate_junit(path: Path) -> int:
        try:
            root = ElementTree.parse(path).getroot()
        except (ElementTree.ParseError, OSError) as error:
            raise CompanionCheckError("CTest did not write a valid JUnit report") from error
        for suite in root.iter():
            if suite.tag.rsplit("}", 1)[-1] not in {"testsuite", "testsuites"}:
                continue
            for name in ("failures", "errors", "skipped", "disabled"):
                try:
                    count = int(suite.attrib.get(name, "0"))
                except ValueError as error:
                    raise CompanionCheckError("CTest JUnit report has invalid outcome counters") from error
                if count != 0:
                    raise CompanionCheckError("CTest JUnit report has failed or skipped outcome counters")
        cases = [element for element in root.iter() if element.tag.rsplit("}", 1)[-1] == "testcase"]
        if not cases:
            raise CompanionCheckError("CTest JUnit report has no test cases")
        for case in cases:
            status = case.attrib.get("status", "run").lower()
            if status not in {"run", "passed"} or {element.tag.rsplit("}", 1)[-1] for element in case} & {"failure", "error", "skipped"}:
                raise CompanionCheckError("CTest JUnit report contains failed or skipped test cases")
        return len(cases)

    @staticmethod
    def validate_json(path: Path, kind: str, *, lane: str | None = None) -> int:
        def object_without_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
            result: dict[str, object] = {}
            for name, value in pairs:
                if name in result:
                    raise ValueError("duplicate JSON object key")
                result[name] = value
            return result
        try:
            def invalid_constant(_: str) -> None:
                raise ValueError("non-finite JSON constant")
            value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=object_without_duplicates,
                               parse_constant=invalid_constant)
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            raise CompanionCheckError("mandatory harness report is not valid JSON") from error
        if not isinstance(value, dict) or value.get("ok") is not True:
            raise CompanionCheckError("mandatory harness report did not succeed")
        if kind == "operations":
            from operations_budget_integration_test import DEFAULT_ACCEPTANCE, validate_report
            if lane is not None and value.get("lane") != lane:
                raise CompanionCheckError("operations report does not match the requested build lane")
            try:
                return validate_report(value, expected_acceptance=DEFAULT_ACCEPTANCE)
            except (ValueError, TypeError, KeyError) as error:
                raise CompanionCheckError("operations report exceeds its fixed bounds or lacks required proof") from error
        if kind == "faults":
            expected = {
                "outbox-saturation-preserves-unacknowledged-prefix",
                "local-continuation-database-stopped", "local-continuation-database-paused",
                "stopped-database-response-deadline", "paused-database-response-deadline",
                "paused-database-server-shutdown", "server-restarts-after-fault-release",
            }
            budgets = {"database_response_ms": 4000, "server_shutdown_ms": 10000,
                       "local_command_ms": 3000, "outbox_entries": 4, "outbox_bytes": 65536}
            checks = value.get("checks")
            if (value.get("schema_version") != "shin.fault-budget-report.v1" or
                    value.get("failure") is not None or value.get("budgets") != budgets or
                    not isinstance(checks, list) or len(checks) != len(expected) or
                    any(not isinstance(check, dict) or check.get("ok") is not True or not isinstance(check.get("name"), str) or
                        check.get("name") not in expected or "skipped" in check or "status" in check for check in checks) or
                    {check["name"] for check in checks} != expected):
                raise CompanionCheckError("fault report lacks the fixed budgets or required successful probes")
            def within(actual: object, limit: int) -> bool:
                return type(actual) in (int, float) and 0 <= actual <= limit and math.isfinite(actual)
            for check in checks:
                name = check["name"]
                if name.endswith("response-deadline"):
                    valid = (check.get("http_status") == 503 and check.get("error_code") == "database-unavailable" and
                             check.get("budget_ms") == budgets["database_response_ms"] and
                             within(check.get("elapsed_ms"), budgets["database_response_ms"]))
                elif name == "paused-database-server-shutdown":
                    valid = (type(check.get("exit_code")) is int and check["exit_code"] == 0 and
                             check.get("budget_ms") == budgets["server_shutdown_ms"] and
                             within(check.get("elapsed_ms"), budgets["server_shutdown_ms"]))
                elif name.startswith("local-continuation-"):
                    valid = (check.get("command_budget_ms") == budgets["local_command_ms"] and
                             all(within(check.get(key), budgets["local_command_ms"]) for key in ("observer_ms", "core_ms")))
                elif name == "outbox-saturation-preserves-unacknowledged-prefix":
                    valid = (check.get("retained") == budgets["outbox_entries"] and
                             type(check.get("rejected")) is int and check["rejected"] >= 2)
                else:
                    valid = True
                if not valid:
                    raise CompanionCheckError("fault report exceeds its operating budget or hides a failed outcome")
            return len(checks)
        if kind == "server":
            schema, checks = "shin.server-integration-report.v1", value.get("checks")
        elif kind == "recovery":
            schema, checks = "shin.server-recovery-integration-report.v1", value.get("checks")
            if not isinstance(checks, list) or not any(isinstance(case, dict) and
                    case.get("name") == "old-observer-archive-reviewed-same-workspace-enrollment" for case in checks):
                raise CompanionCheckError("recovery report lacks archived same-workspace re-enrollment")
        elif kind == "observer":
            schema, checks = "shin.observer-integration-report.v1", value.get("cases")
            if not isinstance(checks, list) or not any(isinstance(case, dict) and
                    case.get("name") == "outbox-archive-disposition" for case in checks):
                raise CompanionCheckError("Observer report lacks explicit outbox archive disposition")
        elif kind == "headless":
            schema, phases = "shin.headless-workflow-report.v1", value.get("phases")
            if (value.get("proof_kind") != "scripted-fresh-process-replay" or
                    value.get("outside_git") is not True or value.get("model_agent_proof") is not False or
                    value.get("failure") is not None or not isinstance(phases, list) or
                    any(not isinstance(phase, dict) for phase in phases) or
                    [phase.get("phase") for phase in phases] != ["capture", "return", "offline"]):
                raise CompanionCheckError("headless workflow report lacks the required isolated phases")
            restore = value.get("backup_restore")
            if not isinstance(restore, dict):
                raise CompanionCheckError("headless workflow report lacks the mandatory backup/restore proof")
            phases = [*phases, {**restore, "phase": "backup-restore"}]
            checks = []
            for phase in phases:
                cases = phase.get("checks")
                if (phase.get("ok") is not True or phase.get("failure") is not None or
                        not isinstance(cases, list) or not cases or
                        any(not isinstance(case, str) or not case.strip() or
                            case.lower() in {"failed", "skipped"} for case in cases)):
                    raise CompanionCheckError("headless workflow phase failed or contains no checks")
                checks.extend(phase["phase"] + ":" + case for case in cases)
        elif kind == "delivery":
            checks, metrics = value.get("checks"), value.get("metrics")
            if set(value) != {"ok", "checks", "metrics"} or not isinstance(metrics, dict) or not isinstance(checks, list) or metrics.get("check_count") != len(checks) or metrics.get("passed_count") != len(checks):
                raise CompanionCheckError("delivery harness report schema is not accepted")
            schema = None
        else:
            raise CompanionCheckError("unknown harness report contract")
        if schema is not None and value.get("schema_version") != schema:
            raise CompanionCheckError("mandatory harness report schema is not accepted")
        if not isinstance(checks, list) or not checks:
            raise CompanionCheckError("mandatory harness report has no checks")
        if kind in {"server", "headless"}:
            if not all(isinstance(check, str) and check and check.lower() not in {"failed", "skipped"} for check in checks) or len(set(checks)) != len(checks):
                raise CompanionCheckError("server harness report has invalid checks")
        elif not all(successful_check(check) for check in checks) or len({check["name"] for check in checks}) != len(checks):
            raise CompanionCheckError("observer harness report has failed or skipped checks")
        return len(checks)

    def validate_report(self, suite: Suite, started_ns: int) -> int:
        path = self.run_root / suite.report_name
        try:
            entry = path.lstat()
        except OSError as error:
            raise CompanionCheckError(f"{suite.name} did not write its mandatory report") from error
        if stat.S_ISLNK(entry.st_mode) or not stat.S_ISREG(entry.st_mode) or entry.st_size == 0 or entry.st_size > MAX_REPORT_BYTES or entry.st_mtime_ns < started_ns:
            raise CompanionCheckError(f"{suite.name} report is missing, unsafe, empty, or stale")
        return self.validate_junit(path) if suite.report_kind == "junit" else self.validate_json(path, suite.report_kind, lane=self.inputs.lane)

    def write_text(self, path: Path, value: str) -> None:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            output.write(value[:MAX_LOG_BYTES])

    def run_suite(self, suite: Suite) -> None:
        started_ns = time.time_ns()
        report_path = self.run_root / suite.report_name
        if report_path.exists() or report_path.is_symlink():
            self.checks.append({"name": suite.name, "command": list(suite.command), "ok": False,
                                "error": "mandatory report path already exists"})
            raise CompanionCheckError(f"{suite.name} report path must be absent before launch")
        try:
            completed = self.runner(suite.command)
        except (OSError, subprocess.SubprocessError) as error:
            self.checks.append({"name": suite.name, "command": list(suite.command), "ok": False,
                                "error": f"could not run mandatory suite: {type(error).__name__}"})
            raise CompanionCheckError(f"could not run mandatory suite {suite.name}") from error
        stdout_path, stderr_path = self.run_root / f"{suite.name}.stdout.log", self.run_root / f"{suite.name}.stderr.log"
        self.write_text(stdout_path, completed.stdout or "")
        self.write_text(stderr_path, completed.stderr or "")
        result: dict[str, object] = {"name": suite.name, "command": list(suite.command), "covered_scopes": list(suite.covered_scopes), "ok": False, "exit_code": completed.returncode, "stdout": str(stdout_path), "stderr": str(stderr_path)}
        try:
            if completed.returncode != 0:
                raise CompanionCheckError(f"{suite.name} failed with exit status {completed.returncode}")
            self.assert_identities_unchanged()
            result["check_count"] = self.validate_report(suite, started_ns)
            result["ok"] = True
        except CompanionCheckError as error:
            result["error"] = str(error)
            self.checks.append(result)
            raise
        self.checks.append(result)

    def write_aggregate(self, ok: bool, error: str | None = None) -> Path:
        report: dict[str, object] = {"schema_version": REPORT_SCHEMA, "ok": ok, "lane": self.inputs.lane,
            "run_id": self.run_id, "report_root": str(self.run_root),
            "binary_identities": {name: identity.report_value() for name, identity in self.identities.items()},
            "covered_scopes": [scope for suite in self.suites() for scope in suite.covered_scopes], "checks": self.checks}
        if error is not None:
            report["error"] = error
        target = self.run_root / "aggregate.json"
        descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(report, output, sort_keys=True, separators=(",", ":"))
            output.write("\n")
        return target

    def run(self) -> Path:
        with CompanionLock(self.inputs.build_root / "companion-check.lock"):
            try:
                guard = self.stand_guard_factory()
            except RuntimeError as error:
                raise CompanionCheckError("development PostgreSQL stand is already held") from error
            del guard
            try:
                for suite in self.suites():
                    self.run_suite(suite)
                self.assert_identities_unchanged()
            except CompanionCheckError as error:
                report = self.write_aggregate(False, str(error))
                raise CompanionCheckError(f"{error}; aggregate report: {report}") from error
            return self.write_aggregate(True)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-binary", required=True)
    parser.add_argument("--observer-binary", required=True)
    parser.add_argument("--core-root", required=True)
    parser.add_argument("--ledger-probe", required=True)
    parser.add_argument("--activation-store-probe", required=True)
    parser.add_argument("--preflight-observation-probe", required=True)
    parser.add_argument("--server-build-root", required=True)
    parser.add_argument("--observer-build-root", required=True)
    parser.add_argument("--state-root", required=True)
    parser.add_argument("--report-root", required=True)
    parser.add_argument("--lane", choices=("release", "sanitizer"), required=True)
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    try:
        report = CompanionCheck(validate_inputs(parse_args(argv))).run()
    except CompanionCheckError as error:
        print(f"error: companion check failed: {error}", file=sys.stderr)
        return 1
    print(f"companion check passed; aggregate report: {report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
