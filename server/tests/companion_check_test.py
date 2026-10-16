#!/usr/bin/env python3
"""Focused regression tests for the companion verification aggregate."""

from __future__ import annotations

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import companion_check
from operations_budget_integration_test import CHECKS as OPERATIONS_CHECKS, DEFAULT_ACCEPTANCE
from operations_budget_integration_test import memory_measurement, report_policy, report_schema, validate_report as validate_operations


class NoopStandGuard:
    pass


class Fixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.build = root / "build"
        self.server_build = self.build / "server"
        self.observer_build = self.build / "observer"
        self.core_root = self.build / "core"
        self.state = self.build / "server-dev"
        self.reports = self.state / "reports"
        for path in (self.server_build, self.observer_build, self.core_root, self.reports):
            path.mkdir(parents=True, exist_ok=True)
        for path in (self.state, self.reports):
            path.chmod(0o700)
        self.server = self.executable(self.server_build / "shin-server")
        self.observer = self.executable(self.observer_build / "shin-observer")
        self.core = self.executable(self.core_root / "shin-context")
        self.ledger = self.executable(self.server_build / "shin-server-preparation-store-tests")
        self.activation = self.executable(self.server_build / "shin-server-activation-store-tests")
        self.preflight = self.executable(self.server_build / "shin-server-preflight-observation-tests")
        self.publication_review = self.executable(self.server_build / "shin-server-publication-review-tests")
        self.retrieval_journal = self.executable(self.server_build / "shin-server-retrieval-journal-store-probe")
        self.json_codec = self.executable(self.server_build / "shin-server-json-codec-probe")
        self.write_caches()

    @staticmethod
    def executable(path: Path) -> Path:
        path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        path.chmod(0o700)
        return path

    def args(self, **overrides: str) -> argparse.Namespace:
        values = {
            "server_binary": str(self.server), "observer_binary": str(self.observer), "core_root": str(self.core_root),
            "ledger_probe": str(self.ledger), "server_build_root": str(self.server_build),
            "activation_store_probe": str(self.activation), "preflight_observation_probe": str(self.preflight),
            "observer_build_root": str(self.observer_build), "state_root": str(self.state),
            "report_root": str(self.reports), "lane": "release",
        }
        values.update(overrides)
        return argparse.Namespace(**values)

    def write_caches(self, *, server: str = "OFF", observer: str = "OFF") -> None:
        (self.server_build / "CMakeCache.txt").write_text(f"SHIN_SERVER_SANITIZERS:BOOL={server}\n", encoding="utf-8")
        (self.observer_build / "CMakeCache.txt").write_text(f"SHIN_OBSERVER_SANITIZERS:BOOL={observer}\n", encoding="utf-8")


def fault_report():
    checks = [
        {"name": "outbox-saturation-preserves-unacknowledged-prefix", "ok": True, "retained": 4, "rejected": 2},
        *[{"name": "local-continuation-database-" + state, "ok": True,
           "observer_ms": 20, "core_ms": 10, "command_budget_ms": 3000} for state in ("stopped", "paused")],
        *[{"name": state + "-database-response-deadline", "ok": True, "http_status": 503,
           "error_code": "database-unavailable", "elapsed_ms": 3000, "budget_ms": 4000} for state in ("stopped", "paused")],
        {"name": "paused-database-server-shutdown", "ok": True, "exit_code": 0, "elapsed_ms": 100, "budget_ms": 10000},
        {"name": "server-restarts-after-fault-release", "ok": True},
    ]
    return {"schema_version": "shin.fault-budget-report.v1", "ok": True, "failure": None, "checks": checks,
            "budgets": {"database_response_ms": 4000, "server_shutdown_ms": 10000,
                        "local_command_ms": 3000, "outbox_entries": 4, "outbox_bytes": 65536}}


def operations_report(acceptance=DEFAULT_ACCEPTANCE, lane="release"):
    rows = {name: {"name": name, "ok": True} for name in OPERATIONS_CHECKS}
    for name in ("http-saturation-1", "http-saturation-2", "shutdown-http-saturated"):
        rows[name].update(held=6, closed=8, close_ms=[1.0] * 8)
    rows["mixed-requests-and-background-progress"].update(requests=144, p50_ms=1, p95_ms=2, maximum_ms=3,
                                                       health_checks=2, journal_exports=3)
    rows["combined-database-unavailable"].update(http_status=503, error_code="store-busy", elapsed_ms=501)
    rows["journal-pending-is-bounded"].update(pending_records=1, pending_bytes=1000)
    rows["loaded-paused-shutdown"].update(exit_code=0, elapsed_ms=6000)
    rows["shutdown-does-not-fabricate-ack"]["status"] = {
        "shutdown_prefix_acknowledged": False, "all_traffic_complete": False, "pending_batches": 1}
    rows["restart-preserves-identity-and-activation"]["elapsed_ms"] = 100
    rows["resource-bounds"].update(baseline={"rss_bytes": 1000000, "fds": 20},
                                  settled=[{"rss_bytes": 1100000, "fds": 20}] * 3)
    resources = rows["resource-bounds"]
    resources["memory"] = memory_measurement(lane, acceptance, resources["baseline"], resources["settled"])
    return {"schema_version": report_schema(acceptance), "ok": True, "failure": None,
            "checks": list(rows.values()), "policy": copy.deepcopy(report_policy(acceptance)), "lane": lane,
            "instrumentation": {"server_sanitizers": "ON" if lane == "sanitizer" else "OFF",
                                "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1",
                                "LSAN_OPTIONS": None},
            "samples": [{"elapsed_ms": index * 250, "rss_bytes": 1000000, "fds": 20} for index in range(4)],
            "sample_errors": []}


class ReportRunner:
    def __init__(self, *, malformed: bool = False, missing: bool = False, failed: bool = False,
                 stale: bool = False, mutate: Path | None = None) -> None:
        self.malformed, self.missing, self.failed, self.stale, self.mutate = malformed, missing, failed, stale, mutate

    def __call__(self, command: list[str]) -> subprocess.CompletedProcess[str]:
        report = Path(command[command.index("--output-junit") + 1] if "--output-junit" in command else command[command.index("--report") + 1])
        if self.mutate is not None:
            self.mutate.write_text("changed", encoding="utf-8")
            self.mutate.chmod(0o700)
        if not self.missing:
            if self.malformed:
                report.write_text("not a report", encoding="utf-8")
            elif report.suffix == ".xml":
                report.write_text("<testsuite><testcase name='fresh'/></testsuite>", encoding="utf-8")
            elif "recovery" in report.name:
                report.write_text(json.dumps({"schema_version": "shin.server-recovery-integration-report.v1", "ok": not self.failed,
                                              "checks": [{"name": "old-observer-archive-reviewed-same-workspace-enrollment", "ok": not self.failed, "elapsed_ms": 1.0}]}), encoding="utf-8")
            elif "observer-integration" in report.name:
                report.write_text(json.dumps({"schema_version": "shin.observer-integration-report.v1", "ok": not self.failed, "cases": [{"name": "outbox-archive-disposition", "ok": not self.failed, "elapsed_ms": 1.0}]}), encoding="utf-8")
            elif "observer-delivery" in report.name:
                report.write_text(json.dumps({"ok": not self.failed, "checks": [{"name": "fresh", "ok": not self.failed, "elapsed_ms": 1.0}], "metrics": {"check_count": 1, "passed_count": 1}}), encoding="utf-8")
            elif "headless-workflow" in report.name:
                report.write_text(json.dumps({"schema_version": "shin.headless-workflow-report.v1", "ok": not self.failed,
                    "proof_kind": "scripted-fresh-process-replay", "outside_git": True, "model_agent_proof": False,
                    "backup_restore": {"ok": not self.failed, "checks": ["fresh"]},
                    "phases": [{"phase": phase, "ok": not self.failed, "checks": ["fresh"]}
                               for phase in ("capture", "return", "offline")]}), encoding="utf-8")
            elif "fault-budgets" in report.name:
                report.write_text(json.dumps({**fault_report(), "ok": not self.failed}), encoding="utf-8")
            elif "operations-budgets" in report.name:
                acceptance = command[command.index("--acceptance") + 1]
                report.write_text(json.dumps({**operations_report(acceptance), "ok": not self.failed}), encoding="utf-8")
            else:
                report.write_text(json.dumps({"schema_version": "shin.server-integration-report.v1", "ok": not self.failed, "checks": ["fresh"]}), encoding="utf-8")
            if self.stale:
                old = time.time_ns() - 10_000_000_000
                os.utime(report, ns=(old, old))
        return subprocess.CompletedProcess(command, 0, "", "")


class CompanionCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.fixture = Fixture(Path(self.temporary.name))

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def gate(self, runner: ReportRunner, args: argparse.Namespace | None = None) -> companion_check.CompanionCheck:
        inputs = companion_check.validate_inputs(args or self.fixture.args(), repository_root=self.fixture.root)
        return companion_check.CompanionCheck(inputs, runner=runner, stand_guard_factory=NoopStandGuard)

    def test_accepts_fresh_nonempty_reports_and_records_commands_and_identities(self) -> None:
        value = json.loads(self.gate(ReportRunner()).run().read_text(encoding="utf-8"))
        self.assertTrue(value["ok"])
        self.assertEqual(value["schema_version"], companion_check.REPORT_SCHEMA)
        self.assertTrue(value["checks"])
        self.assertTrue(all(check["ok"] and check["command"] for check in value["checks"]))
        self.assertEqual(value["binary_identities"]["server"]["path"], str(self.fixture.server.resolve()))
        self.assertIn("activation_store_probe", value["binary_identities"])
        self.assertIn("preflight_observation_probe", value["binary_identities"])
        self.assertIn("retrieval_journal_probe", value["binary_identities"])
        self.assertIn("json_codec_probe", value["binary_identities"])
        self.assertTrue(all("--no-tests=error" in check["command"] for check in value["checks"][:2]))

    def test_validates_every_direct_report_kind(self) -> None:
        gate = self.gate(ReportRunner())
        self.assertIn("headless-workflow", {suite.name for suite in gate.suites()})
        self.assertIn("fault-budgets", {suite.name for suite in gate.suites()})
        self.assertIn("operations-budgets", {suite.name for suite in gate.suites()})
        operations = next(suite for suite in gate.suites() if suite.name == "operations-budgets")
        self.assertEqual(operations.command[operations.command.index("--acceptance") + 1], "split-memory")
        for suite in gate.suites():
            with self.subTest(suite=suite.name):
                gate.runner(suite.command)
                self.assertEqual(gate.validate_report(suite, 0), {
                    "headless-workflow": 4, "fault-budgets": 7, "operations-budgets": 11}.get(suite.name, 1))

    def test_operations_report_recomputes_bounds_and_rejects_incomplete_proof(self):
        path = self.fixture.reports / "operations-budgets.json"
        path.write_text(json.dumps(operations_report()))
        self.assertEqual(companion_check.CompanionCheck.validate_json(path, "operations"), 11)
        with self.assertRaises(companion_check.CompanionCheckError):
            companion_check.CompanionCheck.validate_json(path, "operations", lane="sanitizer")
        for mutation in ("missing", "skip", "failed", "policy", "shutdown", "rss", "fds", "gap", "nan",
                         "sampling-error", "growth", "no-background", "false-ack", "overload", "empty-samples"):
            report = operations_report()
            rows = {row["name"]: row for row in report["checks"]}
            if mutation == "missing": report["checks"].pop()
            elif mutation == "skip": report["checks"][0]["skipped"] = True
            elif mutation == "failed": report["checks"][0]["ok"] = False
            elif mutation == "policy": report["policy"]["shutdown_ms"] += 1
            elif mutation == "shutdown": rows["loaded-paused-shutdown"]["elapsed_ms"] = 10001
            elif mutation == "rss": report["samples"][0]["rss_bytes"] = 268435457
            elif mutation == "fds": report["samples"][0]["fds"] = 97
            elif mutation == "gap": report["samples"][-1]["elapsed_ms"] = 5000
            elif mutation == "nan": report["samples"][0]["rss_bytes"] = float("nan")
            elif mutation == "sampling-error": report["sample_errors"] = ["TimeoutExpired"]
            elif mutation == "growth": rows["resource-bounds"]["settled"][0]["rss_bytes"] = 50000000
            elif mutation == "no-background": rows["mixed-requests-and-background-progress"]["health_checks"] = 0
            elif mutation == "false-ack": rows["shutdown-does-not-fabricate-ack"]["status"]["shutdown_prefix_acknowledged"] = True
            elif mutation == "overload": rows["http-saturation-1"]["close_ms"][0] = 1001
            else: report["samples"] = []
            path.write_text(json.dumps(report))
            with self.subTest(mutation=mutation), self.assertRaises(companion_check.CompanionCheckError):
                companion_check.CompanionCheck.validate_json(path, "operations")

    def test_fault_report_rejects_missing_skipped_and_over_budget_checks(self) -> None:
        path = self.fixture.reports / "fault-budgets.json"
        base = fault_report()
        path.write_text(json.dumps(base))
        self.assertEqual(companion_check.CompanionCheck.validate_json(path, "faults"), 7)
        for mutation in ("missing", "skipped", "failed", "deadline", "budget", "unknown", "local", "shutdown", "rejected"):
            report = fault_report()
            if mutation == "missing": report["checks"].pop()
            elif mutation == "skipped": report["checks"][0]["skipped"] = True
            elif mutation == "failed": report["checks"][0]["ok"] = False
            elif mutation == "deadline": report["checks"][3]["elapsed_ms"] = 4001
            elif mutation == "budget": report["budgets"]["database_response_ms"] = 5000
            elif mutation == "unknown": report["checks"][3]["http_status"] = None
            elif mutation == "local": report["checks"][1]["observer_ms"] = 3001
            elif mutation == "shutdown": report["checks"][5]["exit_code"] = None
            else: report["checks"][0]["rejected"] = 0
            path.write_text(json.dumps(report))
            with self.subTest(mutation=mutation), self.assertRaises(companion_check.CompanionCheckError):
                companion_check.CompanionCheck.validate_json(path, "faults")

    def test_split_memory_preserves_original_failure_and_measured_growth(self):
        for acceptance, lane, accepted in (("original", "sanitizer", False),
                                            ("split-memory", "sanitizer", True),
                                            ("split-memory", "release", False)):
            report = operations_report(acceptance, lane)
            resource = next(row for row in report["checks"] if row["name"] == "resource-bounds")
            for row in resource["settled"]:
                row["rss_bytes"] = 180000000
            resource["memory"] = memory_measurement(lane, acceptance, resource["baseline"], resource["settled"])
            self.assertFalse(resource["memory"]["original_rss_growth_within_budget"])
            with self.subTest(acceptance=acceptance, lane=lane):
                if accepted:
                    self.assertEqual(validate_operations(report), 11)
                    resource["memory"]["measured_rss_growth_bytes"] = 0
                    with self.assertRaises(ValueError):
                        validate_operations(report)
                else:
                    with self.assertRaises(ValueError):
                        validate_operations(report)
        with self.assertRaises(ValueError):
            validate_operations(operations_report("split-memory"), expected_acceptance="original")
        with self.assertRaises(ValueError):
            validate_operations(operations_report("original"), expected_acceptance="split-memory")

    def test_split_memory_refuses_weakened_or_missing_sanitizer_evidence(self):
        base = operations_report("split-memory", "sanitizer")
        self.assertEqual(validate_operations(base), 11)
        for key, setting in (
            ("server_sanitizers", "OFF"), ("ASAN_OPTIONS", None), ("ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1"),
            ("UBSAN_OPTIONS", "halt_on_error=0"), ("LSAN_OPTIONS", "suppressions=/tmp/ignored"),
            ("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1:quarantine_size_mb=64"),
            ("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1:detect_leaks=1"),
            ("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1:start_deactivated=1"),
        ):
            report = copy.deepcopy(base)
            report["instrumentation"][key] = setting
            with self.subTest(key=key, setting=setting), self.assertRaises(ValueError):
                validate_operations(report)
        for field, value in (("rss_bytes", 805306369), ("fds", 97)):
            report = copy.deepcopy(base)
            report["samples"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_operations(report)
        report = copy.deepcopy(base)
        report["instrumentation"]["ASAN_OPTIONS"] += ":atexit=1:print_stats=1"
        self.assertEqual(validate_operations(report), 11)

    def test_companion_requires_approved_memory_acceptance_and_rejects_legacy_report(self):
        path = self.fixture.reports / "operations-budgets.json"
        self.assertEqual(DEFAULT_ACCEPTANCE, "split-memory")
        path.write_text(json.dumps(operations_report()))
        self.assertEqual(companion_check.CompanionCheck.validate_json(path, "operations", lane="release"), 11)
        path.write_text(json.dumps(operations_report("original")))
        with self.assertRaises(companion_check.CompanionCheckError):
            companion_check.CompanionCheck.validate_json(path, "operations", lane="release")

    def test_rejects_malformed_missing_and_failed_reports(self) -> None:
        for runner in (ReportRunner(malformed=True), ReportRunner(missing=True), ReportRunner(failed=True)):
            with self.subTest(runner=runner.__dict__), self.assertRaises(companion_check.CompanionCheckError):
                self.gate(runner).run()

    def test_rejects_poisoned_stale_reports(self) -> None:
        stale = self.fixture.reports / "companion-old"
        stale.mkdir(mode=0o700)
        (stale / "server-foundation.json").write_text('{"ok":true,"checks":["poison"]}', encoding="utf-8")
        with self.assertRaises(companion_check.CompanionCheckError):
            self.gate(ReportRunner(missing=True)).run()
        with self.assertRaises(companion_check.CompanionCheckError):
            self.gate(ReportRunner(stale=True)).run()

    def test_recovery_report_requires_structured_successful_checks(self) -> None:
        path = self.fixture.reports / "recovery.json"
        name = "old-observer-archive-reviewed-same-workspace-enrollment"
        report = {"schema_version": "shin.server-recovery-integration-report.v1", "ok": True,
                  "checks": [{"name": name, "ok": True, "elapsed_ms": 1.0}]}
        path.write_text(json.dumps(report), encoding="utf-8")
        self.assertEqual(companion_check.CompanionCheck.validate_json(path, "recovery"), 1)
        for checks in (["unstructured"], [{"name": "offline-recovery", "ok": True, "elapsed_ms": 1.0}],
                       [{"name": name, "ok": False}],
                       [{"name": name, "ok": True, "elapsed_ms": 1.0, "status": "skipped"}],
                       [{"name": name, "ok": True, "elapsed_ms": 1.0, "skipped": True}],
                       [{"name": name, "ok": True, "elapsed_ms": float("nan")}],
                       [{"name": name, "ok": True, "elapsed_ms": True}]):
            with self.subTest(checks=checks):
                path.write_text(json.dumps({**report, "checks": checks}), encoding="utf-8")
                with self.assertRaises(companion_check.CompanionCheckError):
                    companion_check.CompanionCheck.validate_json(path, "recovery")

    def test_observer_requires_archive_disposition_proof(self) -> None:
        path = self.fixture.reports / "observer.json"
        base = {"schema_version": "shin.observer-integration-report.v1", "ok": True}
        for case in ({"name": "old-local-sampling", "ok": True, "elapsed_ms": 1.0},
                     {"name": "outbox-archive-disposition", "ok": False, "elapsed_ms": 1.0},
                     {"name": "outbox-archive-disposition", "ok": True, "elapsed_ms": 1.0, "status": "skipped"}):
            path.write_text(json.dumps({**base, "cases": [case]}), encoding="utf-8")
            with self.subTest(case=case), self.assertRaises(companion_check.CompanionCheckError):
                companion_check.CompanionCheck.validate_json(path, "observer")

    def test_headless_report_requires_all_successful_outside_git_phases(self) -> None:
        path = self.fixture.reports / "headless-workflow.json"
        base = {"schema_version": "shin.headless-workflow-report.v1", "ok": True,
                "backup_restore": {"ok": True, "checks": ["restored"]},
                "proof_kind": "scripted-fresh-process-replay", "outside_git": True, "model_agent_proof": False,
                "phases": [{"phase": phase, "ok": True, "checks": ["fresh"]}
                           for phase in ("capture", "return", "offline")]}
        path.write_text(json.dumps(base), encoding="utf-8")
        self.assertEqual(companion_check.CompanionCheck.validate_json(path, "headless"), 4)
        invalid = [{**base, "outside_git": False}, {**base, "failure": "cleanup failed"},
                   {**base, "phases": base["phases"][:2]}, {**base, "phases": None},
                   {**base, "proof_kind": "model-driven-fixture-with-independent-oracle"}]
        invalid.extend({**base, "backup_restore": restore} for restore in
                       (None, {}, {"ok": False, "checks": ["restored"]},
                        {"ok": True, "checks": []}, {"ok": True, "checks": ["skipped"]},
                        {"ok": True, "checks": ["restored"], "failure": "cleanup failed"}))
        for change in ({"ok": False}, {"checks": []}, {"checks": ["skipped"]},
                       {"checks": ["same", "same"]}, {"checks": [None]}):
            invalid.append({**base, "phases": [*base["phases"][:2], {**base["phases"][2], **change}]})
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(companion_check.CompanionCheckError):
                path.write_text(json.dumps(value), encoding="utf-8")
                companion_check.CompanionCheck.validate_json(path, "headless")

    def test_rejects_existing_and_oversized_report_paths_before_parse(self) -> None:
        gate = self.gate(ReportRunner())
        suite = gate.suites()[0]
        report = gate.run_root / suite.report_name
        report.write_text("seeded", encoding="utf-8")
        with self.assertRaises(companion_check.CompanionCheckError):
            gate.run()
        oversized = self.gate(ReportRunner())
        report = oversized.run_root / oversized.suites()[0].report_name
        with report.open("wb") as output:
            output.truncate(companion_check.MAX_REPORT_BYTES + 1)
        with self.assertRaises(companion_check.CompanionCheckError):
            oversized.validate_report(oversized.suites()[0], 0)

    def test_rejects_junit_counters_unknown_status_and_invalid_json_checks(self) -> None:
        gate = self.gate(ReportRunner())
        junit = gate.run_root / "bad.junit.xml"
        for content in (
            "<testsuite failures='1'><testcase name='bad'/></testsuite>",
            "<testsuite errors='1'><testcase name='bad'/></testsuite>",
            "<testsuite skipped='1'><testcase name='bad'/></testsuite>",
            "<testsuite disabled='1'><testcase name='bad'/></testsuite>",
            "<testsuite><testcase name='bad' status='unknown'/></testsuite>",
        ):
            junit.write_text(content, encoding="utf-8")
            with self.subTest(content=content), self.assertRaises(companion_check.CompanionCheckError):
                gate.validate_junit(junit)
        invalid_reports = (
            ("server.json", "server", '{"schema_version":"shin.server-integration-report.v1","ok":true,"checks":["same","same"]}'),
            ("recovery.json", "recovery", '{"schema_version":"shin.server-recovery-integration-report.v1","ok":true,"checks":[""]}'),
            ("observer.json", "observer", '{"schema_version":"shin.observer-integration-report.v1","ok":true,"cases":[{"name":"failed","ok":false}]}'),
            ("observer-skipped.json", "observer", '{"schema_version":"shin.observer-integration-report.v1","ok":true,"cases":[{"name":"skipped","ok":true,"status":"skipped"}]}'),
            ("delivery.json", "delivery", '{"ok":true,"checks":[{"name":"same","ok":true},{"name":"same","ok":true}],"metrics":{"check_count":2,"passed_count":2}}'),
            ("duplicate.json", "server", '{"ok":true,"ok":true,"schema_version":"shin.server-integration-report.v1","checks":["fresh"]}'),
        )
        for name, kind, content in invalid_reports:
            report = gate.run_root / name
            report.write_text(content, encoding="utf-8")
            with self.subTest(name=name), self.assertRaises(companion_check.CompanionCheckError):
                gate.validate_json(report, kind)

    def test_rejects_wrong_binary_path_and_changed_identity(self) -> None:
        wrong = self.fixture.build / "wrong" / "shin-server"
        wrong.parent.mkdir(mode=0o700)
        self.fixture.executable(wrong)
        with self.assertRaises(companion_check.CompanionCheckError):
            companion_check.validate_inputs(self.fixture.args(server_binary=str(wrong)), repository_root=self.fixture.root)
        with self.assertRaises(companion_check.CompanionCheckError):
            self.gate(ReportRunner(mutate=self.fixture.server)).run()

    def test_rejects_busy_stand_and_companion_overlap(self) -> None:
        def busy_guard() -> object:
            raise RuntimeError("held")
        inputs = companion_check.validate_inputs(self.fixture.args(), repository_root=self.fixture.root)
        with self.assertRaises(companion_check.CompanionCheckError):
            companion_check.CompanionCheck(inputs, runner=ReportRunner(), stand_guard_factory=busy_guard).run()
        other_state = self.fixture.build / "server-dev-other"
        other_reports = other_state / "reports"
        other_reports.mkdir(parents=True)
        other_state.chmod(0o700)
        other_reports.chmod(0o700)
        other_inputs = self.fixture.args(state_root=str(other_state), report_root=str(other_reports))
        with companion_check.CompanionLock(inputs.build_root / "companion-check.lock"):
            with self.assertRaises(companion_check.CompanionCheckError):
                self.gate(ReportRunner(), other_inputs).run()

    def test_sanitizer_lane_requires_cache_flags_and_environment(self) -> None:
        sanitizer_args = self.fixture.args(lane="sanitizer")
        with self.assertRaises(companion_check.CompanionCheckError):
            companion_check.validate_inputs(sanitizer_args, repository_root=self.fixture.root)
        self.fixture.write_caches(server="ON", observer="ON")
        with mock.patch.dict(os.environ, {"ASAN_OPTIONS": "", "UBSAN_OPTIONS": ""}), self.assertRaises(companion_check.CompanionCheckError):
            companion_check.validate_inputs(sanitizer_args, repository_root=self.fixture.root)
        symbolizer = self.fixture.executable(self.fixture.root / "llvm-symbolizer")
        with mock.patch.dict(os.environ, {"ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1",
                                         "ASAN_SYMBOLIZER_PATH": str(symbolizer)}):
            companion_check.validate_inputs(sanitizer_args, repository_root=self.fixture.root)
        with self.assertRaises(companion_check.CompanionCheckError):
            companion_check.validate_inputs(self.fixture.args(), repository_root=self.fixture.root)

    def test_sanitizer_lane_rejects_unusable_symbolizer_before_running_suites(self) -> None:
        self.fixture.write_caches(server="ON", observer="ON")
        non_executable = self.fixture.root / "not-executable"
        non_executable.write_text("not a tool")
        non_executable.chmod(0o600)
        for value in ("", "llvm-symbolizer", str(self.fixture.root / "missing"),
                      str(self.fixture.root), str(non_executable)):
            with self.subTest(symbolizer=value), mock.patch.dict(os.environ, {
                "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1",
                "ASAN_SYMBOLIZER_PATH": value,
            }):
                with self.assertRaisesRegex(companion_check.CompanionCheckError, "ASAN_SYMBOLIZER_PATH"):
                    companion_check.validate_inputs(self.fixture.args(lane="sanitizer"), repository_root=self.fixture.root)

    def test_rejects_ambiguous_sanitizer_options(self) -> None:
        self.fixture.write_caches(server="ON", observer="ON")
        for asan, ubsan in (("detect_leaks=1:halt_on_error=1:detect_leaks=0", "halt_on_error=1"),
                            ("detect_leaks=1:halt_on_error=1", "halt_on_error=1:halt_on_error=0")):
            with self.subTest(asan=asan, ubsan=ubsan), mock.patch.dict(os.environ, {"ASAN_OPTIONS": asan, "UBSAN_OPTIONS": ubsan}):
                with self.assertRaises(companion_check.CompanionCheckError):
                    companion_check.validate_inputs(self.fixture.args(lane="sanitizer"), repository_root=self.fixture.root)

    def test_default_runner_uses_repository_root(self) -> None:
        completed = subprocess.CompletedProcess(["command"], 0, "", "")
        with mock.patch.object(companion_check.subprocess, "run", return_value=completed) as run:
            self.assertIs(companion_check.CompanionCheck.default_runner(("command",)), completed)
        self.assertEqual(run.call_args.kwargs["cwd"], companion_check.REPOSITORY_ROOT)


if __name__ == "__main__":
    unittest.main()
