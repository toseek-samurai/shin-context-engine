#!/usr/bin/env python3
"""Fixed R2 combined-load/resource qualification on the guarded local stand."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
import os
from pathlib import Path
import select
import socket
import subprocess
import threading
import time

from integration_test import BUILD_ROOT, HOST, PORT, IntegrationFailure, under_build
from retrieval_integration_test import RetrievalIntegrationSuite, PROJECT_ID, ROUTE_ID
from retrieval_journal_checks import RetrievalJournalChecks


# Frozen before the first native run. These are local fixture budgets, not SLOs.
POLICY = {
    "workers": 2, "queue_limit": 4, "clients": 4, "rounds": 3,
    "requests_per_client": 12, "overload_probes": 8, "saturation_cycles": 2,
    "request_ms": 4000, "overload_close_ms": 1000, "shutdown_ms": 10000,
    "restart_ms": 10000, "background_progress_ms": 10000,
    "sample_interval_ms": 250, "max_sample_gap_ms": 2000,
    "max_fds": 96, "settled_fd_growth": 4,
    "release_rss_bytes": 268435456, "release_rss_growth_bytes": 33554432,
    "sanitizer_rss_bytes": 805306368, "sanitizer_rss_growth_bytes": 134217728,
}
MEMORY_ACCEPTANCE = {
    "release": "current-rss-and-settled-growth",
    "sanitizer": "default-instrumentation-and-current-rss;settled-growth-is-diagnostic",
    "descriptors": "absolute-and-settled-growth-in-both-lanes",
}
ACCEPTANCES = ("original", "split-memory")
DEFAULT_ACCEPTANCE = "split-memory"


def report_schema(acceptance):
    return "shin.operations-budget-report.v2" if acceptance == "split-memory" else "shin.operations-budget-report.v1"


def report_policy(acceptance):
    return {**POLICY, "memory_acceptance": MEMORY_ACCEPTANCE} if acceptance == "split-memory" else POLICY


def validate_instrumentation(lane, instrumentation):
    if not isinstance(instrumentation, dict):
        raise ValueError("instrumentation evidence is missing")
    if instrumentation.get("server_sanitizers") != ("ON" if lane == "sanitizer" else "OFF"):
        raise ValueError("server build cache does not match the requested lane")
    if lane == "release":
        return
    # Keep ASan's default quarantine/redzones/poisoning. Only required safety
    # options and optional exit diagnostics are admitted for this qualification.
    for name, required, diagnostics in (
        ("ASAN_OPTIONS", {"detect_leaks": "1", "halt_on_error": "1"}, {"atexit": "1", "print_stats": "1"}),
        ("UBSAN_OPTIONS", {"halt_on_error": "1"}, {"print_stacktrace": "1"}),
        ("LSAN_OPTIONS", {}, {}),
    ):
        value = instrumentation.get(name)
        if value is not None and not isinstance(value, str):
            raise ValueError("invalid sanitizer options")
        options = {}
        for field in (value or "").split(":"):
            if not field:
                continue
            key, separator, setting = field.partition("=")
            if not separator or key in options or {**required, **diagnostics}.get(key) != setting:
                raise ValueError("sanitizer qualification refuses overridden defaults or ambiguous options")
            options[key] = setting
        if any(options.get(key) != setting for key, setting in required.items()):
            raise ValueError("required sanitizer safety option is missing")


def memory_measurement(lane, acceptance, baseline, settled):
    growth = max(row["rss_bytes"] for row in settled) - baseline["rss_bytes"]
    return {"measured_rss_growth_bytes": growth,
            "rss_growth_gate_applied": lane == "release" or acceptance == "original",
            "original_rss_growth_within_budget": growth <= POLICY[lane + "_rss_growth_bytes"]}
WRITER = "/api/v1/telemetry/retrieval/journal/writer"
KNOWLEDGE = "/api/v1/knowledge-maintenance"
CHECKS = {"http-saturation-1", "http-saturation-2", "mixed-requests-and-background-progress",
          "combined-database-unavailable", "journal-pending-is-bounded", "shutdown-http-saturated",
          "loaded-paused-shutdown", "shutdown-does-not-fabricate-ack",
          "restart-preserves-identity-and-activation", "read-workload-preserves-source-and-artifacts",
          "resource-bounds"}


def validate_report(value, *, expected_acceptance=None):
    """Recompute numeric gates; a producer's ok flag alone is not acceptance."""
    def require(condition):
        if not condition:
            raise ValueError("operations report lacks required proof or exceeds its fixed budget")
    def within(actual, limit):
        return type(actual) in (int, float) and math.isfinite(actual) and 0 <= actual <= limit
    acceptance = next((name for name in ACCEPTANCES if value.get("schema_version") == report_schema(name)), None)
    require(acceptance is not None and (expected_acceptance is None or acceptance == expected_acceptance))
    require(value.get("ok") is True and
            value.get("failure") is None and value.get("policy") == report_policy(acceptance) and
            value.get("lane") in {"release", "sanitizer"} and value.get("sample_errors") == [])
    if acceptance == "split-memory":
        validate_instrumentation(value["lane"], value.get("instrumentation"))
    rows = value.get("checks")
    require(isinstance(rows, list) and len(rows) == len(CHECKS))
    for row in rows:
        require(isinstance(row, dict) and row.get("ok") is True and "skipped" not in row and
                ("status" not in row or row.get("name") == "shutdown-does-not-fabricate-ack"))
    require({row.get("name") for row in rows} == CHECKS)
    checks = {row["name"]: row for row in rows}
    for name in ("http-saturation-1", "http-saturation-2", "shutdown-http-saturated"):
        row = checks[name]
        require(row.get("held") == 6 and row.get("closed") == 8 and isinstance(row.get("close_ms"), list) and
                len(row["close_ms"]) == 8 and all(within(ms, POLICY["overload_close_ms"]) for ms in row["close_ms"]))
    row = checks["mixed-requests-and-background-progress"]
    require(row.get("requests") == 144 and all(within(row.get(key), POLICY["request_ms"])
            for key in ("maximum_ms", "p50_ms", "p95_ms")) and
            type(row.get("health_checks")) is int and row["health_checks"] > 0 and
            type(row.get("journal_exports")) is int and row["journal_exports"] > 0)
    row = checks["combined-database-unavailable"]
    require(row.get("http_status") == 503 and row.get("error_code") in {"database-unavailable", "store-busy"} and
            within(row.get("elapsed_ms"), POLICY["request_ms"]))
    row = checks["journal-pending-is-bounded"]
    require(within(row.get("pending_records"), 256) and row["pending_records"] > 0 and
            within(row.get("pending_bytes"), 4 * 1024 * 1024) and row["pending_bytes"] > 0)
    row = checks["loaded-paused-shutdown"]
    require(type(row.get("exit_code")) is int and row["exit_code"] == 0 and within(row.get("elapsed_ms"), POLICY["shutdown_ms"]))
    row = checks["shutdown-does-not-fabricate-ack"].get("status")
    require(isinstance(row, dict) and row.get("shutdown_prefix_acknowledged") is False and
            row.get("all_traffic_complete") is False and row.get("pending_batches") == 1)
    require(within(checks["restart-preserves-identity-and-activation"].get("elapsed_ms"), POLICY["restart_ms"]))
    samples = value.get("samples")
    require(isinstance(samples, list) and len(samples) >= 4)
    lane = value["lane"]
    for row in samples:
        require(isinstance(row, dict) and within(row.get("rss_bytes"), POLICY[lane + "_rss_bytes"]) and
                row["rss_bytes"] > 0 and within(row.get("fds"), POLICY["max_fds"]) and row["fds"] > 0 and
                within(row.get("elapsed_ms"), 3600000))
    gaps = [right["elapsed_ms"] - left["elapsed_ms"] for left, right in zip(samples, samples[1:])]
    require(all(within(gap, POLICY["max_sample_gap_ms"]) for gap in gaps))
    row = checks["resource-bounds"]
    baseline, settled = row.get("baseline"), row.get("settled")
    require(isinstance(baseline, dict) and isinstance(settled, list) and len(settled) == POLICY["rounds"])
    for sample in [baseline, *settled]:
        require(isinstance(sample, dict) and within(sample.get("fds"), POLICY["max_fds"]) and sample["fds"] > 0 and
                within(sample.get("rss_bytes"), POLICY[lane + "_rss_bytes"]) and sample["rss_bytes"] > 0)
    measurement = memory_measurement(lane, acceptance, baseline, settled)
    if acceptance == "split-memory":
        require(row.get("memory") == measurement)
    require((not measurement["rss_growth_gate_applied"] or measurement["original_rss_growth_within_budget"]) and
            max(sample["fds"] for sample in settled) - baseline["fds"] <= POLICY["settled_fd_growth"])
    return len(rows)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(roots):
    return {str(path): digest(path) for root in roots for path in sorted(root.rglob("*")) if path.is_file()}


def resources(pid):
    # ps reports current resident KiB, not ru_maxrss/high-water or virtual size.
    rss = int(subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)], timeout=2)) * 1024
    proc = Path(f"/proc/{pid}/fd")
    if proc.is_dir():
        fds = len(list(proc.iterdir()))
    else:
        raw = subprocess.check_output(["lsof", "-nP", "-a", "-p", str(pid), "-Ff"], timeout=2).decode()
        fds = sum(line.startswith("f") and line[1:].isdigit() for line in raw.splitlines())
    return {"rss_bytes": rss, "fds": fds}


class OperationsBudgetSuite(RetrievalIntegrationSuite):
    def __init__(self, binary, core, state_root, report, lane, acceptance=DEFAULT_ACCEPTANCE):
        super().__init__(binary, core, state_root, report)
        self.lane = lane
        self.acceptance = acceptance
        self.instrumentation = {key: os.environ.get(key) for key in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS")}
        self.observations = []
        self.samples = []
        self.sample_errors = []
        self.sockets = []
        self.paused = False
        self.sampling_stop = threading.Event()
        self.sampler = None
        self.timings = []
        self.shutdown_log = None
        self.provisioned = False
        self.harness_sha256 = digest(Path(__file__))

    def observe(self, name, passed, **details):
        self.observations.append({"name": name, "ok": bool(passed), **details})
        if not passed:
            raise IntegrationFailure(name)

    def result(self, path):
        return self.expect_status(200, "GET", path, headers=self.auth_headers("admin"))["result"]

    def await_result(self, path, predicate):
        deadline = time.monotonic() + POLICY["background_progress_ms"] / 1000
        while time.monotonic() < deadline:
            value = self.result(path)
            if predicate(value):
                return value
            time.sleep(0.05)
        self.observe("background-progress-deadline", False, path=path, last_status=value)

    def begin_sampling(self):
        pid = self.server.pid
        def sample():
            while not self.sampling_stop.is_set():
                try:
                    self.samples.append({"elapsed_ms": (time.monotonic() - self.started_at) * 1000,
                                         **resources(pid)})
                except Exception as error:
                    self.sample_errors.append(type(error).__name__)
                    return
                self.sampling_stop.wait(POLICY["sample_interval_ms"] / 1000)
        self.sampler = threading.Thread(target=sample)
        self.sampler.start()

    def end_sampling(self):
        self.sampling_stop.set()
        if self.sampler:
            self.sampler.join(timeout=4)
            if self.sampler.is_alive():
                raise IntegrationFailure("resource-sampler-did-not-stop")
            self.sampler = None

    def configure_workload(self):
        self.configure_retrieval()
        for path in (self.runtime_config_path, self.migrator_config_path):
            config = json.loads(path.read_text())
            config.update(workers=POLICY["workers"], queue_limit=POLICY["queue_limit"])
            config["knowledge"] = {"enabled": True, "principal_id": "dev-admin",
                                   "route_ids": [ROUTE_ID, "other-retrieval"], "interval_seconds": 1,
                                   "max_age_seconds": 3, "prepare_on_drift": False}
            config["journal_writer"] = {"enabled": True, "principal_id": "dev-admin",
                                        "interval_ms": 100, "max_retry_ms": 400, "flush_on_shutdown": True}
            self.replace_private_json(path, config)

    def mixed_round(self):
        def client(client_id):
            samples = []
            for index in range(POLICY["requests_per_client"]):
                started = time.monotonic()
                operation = (client_id + index) % 3
                if operation == 0:
                    status, value = self.post_retrieval(PROJECT_ID, "brief", {"query": "snapshot admission evidence"})
                    if status != 200 or value.get("ok") is not True:
                        raise IntegrationFailure("mixed-retrieval-failed")
                else:
                    self.result(KNOWLEDGE if operation == 1 else WRITER)
                samples.append((time.monotonic() - started) * 1000)
                time.sleep(0.025)
            return samples
        with ThreadPoolExecutor(max_workers=POLICY["clients"]) as pool:
            for values in pool.map(client, range(POLICY["clients"])):
                self.timings.extend(values)

    def close_peers(self):
        for peer in self.sockets:
            peer.close()
        self.sockets.clear()

    def saturate(self):
        # Incomplete headers occupy workers without a Store lease. The remaining
        # accepted sockets wait in cpp-httplib's bounded queue, not the TCP backlog.
        for _ in range(POLICY["workers"] + POLICY["queue_limit"]):
            peer = socket.create_connection((HOST, PORT), timeout=1)
            self.sockets.append(peer)
            peer.sendall(b"GET /health/live HTTP/1.1\r\nHost: 127.0.0.1:19125\r\n")
            time.sleep(0.025)
        durations = []
        for _ in range(POLICY["overload_probes"]):
            started = time.monotonic()
            with socket.create_connection((HOST, PORT), timeout=1) as peer:
                try:
                    closed = peer.recv(1) == b""
                except ConnectionResetError:
                    closed = True
            durations.append((time.monotonic() - started) * 1000)
            if not closed:
                raise IntegrationFailure("overload-was-not-closed")
        # Every admitted partial connection must still be held; a reset is not
        # mistaken for proof that six connections reached the worker/queue pair.
        if select.select(self.sockets, [], [], 0)[0]:
            raise IntegrationFailure("admitted-partial-peer-closed-early")
        if max(durations) > POLICY["overload_close_ms"]:
            raise IntegrationFailure("overload-close-deadline")
        return {"held": len(self.sockets), "closed": len(durations), "close_ms": durations}

    def shutdown_under_fault(self):
        # Reuse the existing controlled failed-insert fixture. Admission failures
        # before telemetry begins cannot create a journal tail on their own.
        with RetrievalJournalChecks(self).failing_attempt_insert():
            status, _ = self.post_retrieval(PROJECT_ID, "brief", {"query": "snapshot admission evidence"})
            if status != 200:
                raise IntegrationFailure("journal-tail-setup-failed")
            self.await_result(WRITER, lambda value: value["pending_batches"] == 1 and value["failures"] > 0)
            try:
                self.paused_shutdown()
            finally:
                if self.paused:
                    self.compose("unpause", "postgres")
                    self.paused = False

    def paused_shutdown(self):
        self.compose("pause", "postgres")
        self.paused = True
        started = time.monotonic()
        status, response = self.post_retrieval(PROJECT_ID, "brief", {"query": "snapshot admission evidence"})
        elapsed = (time.monotonic() - started) * 1000
        code = response.get("error", {}).get("code")
        self.observe("combined-database-unavailable", status == 503 and
                     code in {"database-unavailable", "store-busy"} and elapsed <= POLICY["request_ms"],
                     http_status=status, error_code=code, elapsed_ms=elapsed)
        writer = self.await_result(WRITER, lambda value: value["pending_batches"] == 1)
        self.observe("journal-pending-is-bounded", writer["pending_records"] <= 256 and
                     writer["pending_bytes"] <= 4 * 1024 * 1024 and writer["all_traffic_complete"] is False,
                     pending_records=writer["pending_records"], pending_bytes=writer["pending_bytes"])
        pressure = self.saturate()
        self.observe("shutdown-http-saturated", True, **pressure)
        self.end_sampling()
        process, log = self.server, self.server_log
        self.shutdown_log = log
        started = time.monotonic()
        process.terminate()
        code = None
        try:
            code = process.wait(timeout=POLICY["shutdown_ms"] / 1000)
        except subprocess.TimeoutExpired:
            pass
        elapsed = (time.monotonic() - started) * 1000
        # Do not release the peers or database until the bounded-stop verdict.
        self.observe("loaded-paused-shutdown", code == 0 and elapsed <= POLICY["shutdown_ms"],
                     exit_code=code, elapsed_ms=elapsed)
        self.server = None
        events = [json.loads(line) for line in log.read_text().splitlines() if line.startswith("{")]
        stopped = [event["status"] for event in events if event.get("event") == "journal-writer-stopped"]
        self.observe("shutdown-does-not-fabricate-ack", len(stopped) == 1 and
                     stopped[0]["shutdown_prefix_acknowledged"] is False and stopped[0]["pending_batches"] == 1 and
                     stopped[0]["all_traffic_complete"] is False, status=stopped[0] if stopped else None)
        self.close_peers()
        self.compose("unpause", "postgres")
        self.paused = False

    def run(self):
        self.active_check = "instrumentation"
        if self.acceptance == "split-memory":
            cache = (self.binary.parent / "CMakeCache.txt").read_text().splitlines()
            flags = [line.partition("=")[2] for line in cache if line.startswith("SHIN_SERVER_SANITIZERS:BOOL=")]
            if len(flags) != 1:
                raise IntegrationFailure("server-sanitizer-build-flag-missing-or-ambiguous")
            self.instrumentation["server_sanitizers"] = flags[0]
            validate_instrumentation(self.lane, self.instrumentation)
        self.active_check = "setup"
        self.check_version()
        self.dev_command("init")
        self.provisioned = True
        self.dev_command("up")
        self.wait_for_postgres()
        self.require_http_port_free()
        self.reset_schema()
        self.migrate()
        self.admin_token = self.read_private_token("dev_admin.token")
        self.reader_token = self.read_private_token("dev_reader.token")
        self.prepare_retrieval_fixture()
        self.configure_workload()
        self.start_server()
        for project, root in ((PROJECT_ID, self.fixture_root), ("other", self.other_root)):
            self.assert_register(project, "r2-register-" + project)
            self.assert_workspace_register(project, "primary", root, "r2-workspace-" + project)
        self.seed_managed_retrieval_routes()
        self.await_result(KNOWLEDGE, lambda value: value["checks_completed"] >= 2)
        self.mixed_round()  # Warm caches before measuring retained growth.
        self.await_result(WRITER, lambda value: value["pending_batches"] == 0 and value["export_successes"] > 0)
        protected_roots = [self.fixture_root, self.other_root, self.profile_dir, self.managed_state_dir]
        before = inventory(protected_roots)
        activation = self.activation(PROJECT_ID, ROUTE_ID)
        identity = self.server_id
        baseline = resources(self.server.pid)
        health_before = self.result(KNOWLEDGE)
        writer_before = self.result(WRITER)
        self.timings.clear()
        self.begin_sampling()
        self.active_check = "mixed-workload"
        settled = []
        for cycle in range(POLICY["rounds"]):
            self.mixed_round()
            if cycle < POLICY["saturation_cycles"]:
                pressure = self.saturate()
                self.observe(f"http-saturation-{cycle + 1}", True, **pressure)
                self.close_peers()
                self.wait_for_response("/health/live", 200)
            settled.append(resources(self.server.pid))
        health = self.await_result(KNOWLEDGE, lambda value: value["checks_completed"] > health_before["checks_completed"])
        writer = self.await_result(WRITER, lambda value: value["pending_batches"] == 0 and
                                   value["export_successes"] > writer_before["export_successes"])
        self.observe("mixed-requests-and-background-progress", len(self.timings) == 144 and max(self.timings) <= POLICY["request_ms"],
                     requests=len(self.timings), p50_ms=self.percentile_ms(self.timings, 50),
                     p95_ms=self.percentile_ms(self.timings, 95), maximum_ms=max(self.timings),
                     health_checks=health["checks_completed"] - health_before["checks_completed"],
                     journal_exports=writer["export_successes"] - writer_before["export_successes"])
        self.active_check = "combined-shutdown"
        self.shutdown_under_fault()
        self.active_check = "restart-and-preservation"
        started = time.monotonic()
        self.start_server()
        self.expect_status(200, "GET", "/health/ready", headers=self.auth_headers("admin"))
        elapsed = (time.monotonic() - started) * 1000
        self.observe("restart-preserves-identity-and-activation", elapsed <= POLICY["restart_ms"] and
                     identity == self.server_id and activation == self.activation(PROJECT_ID, ROUTE_ID), elapsed_ms=elapsed)
        self.mixed_round()
        self.observe("read-workload-preserves-source-and-artifacts", before == inventory(protected_roots))
        gaps = [right["elapsed_ms"] - left["elapsed_ms"] for left, right in zip(self.samples, self.samples[1:])]
        memory = memory_measurement(self.lane, self.acceptance, baseline, settled)
        self.observe("resource-bounds", not self.sample_errors and len(self.samples) >= 4 and
                     max(gaps) <= POLICY["max_sample_gap_ms"] and
                     max(row["rss_bytes"] for row in self.samples) <= POLICY[self.lane + "_rss_bytes"] and
                     max(row["fds"] for row in self.samples) <= POLICY["max_fds"] and
                     (not memory["rss_growth_gate_applied"] or memory["original_rss_growth_within_budget"]) and
                     max(row["fds"] for row in settled) - baseline["fds"] <= POLICY["settled_fd_growth"],
                     baseline=baseline, settled=settled, maximum_sample_gap_ms=max(gaps, default=0), memory=memory)
        self.stop_server(expect_clean=True)
        self.active_check = None

    def cleanup(self):
        self.end_sampling()
        self.close_peers()
        try:
            if self.paused:
                self.compose("unpause", "postgres")
                self.paused = False
        finally:
            try:
                self.stop_server(expect_clean=False)
            finally:
                self.restore_configs()
                if self.provisioned:
                    self.compose("stop", "postgres")

    def report(self, failure):
        value = {"schema_version": report_schema(self.acceptance), "ok": failure is None,
                 "policy": report_policy(self.acceptance), "lane": self.lane, "checks": self.observations, "failure": failure,
                 "harness_sha256": self.harness_sha256,
                 "instrumentation": self.instrumentation,
                 "shutdown_log": str(self.shutdown_log) if self.shutdown_log else None,
                 "binaries": {name: {"path": str(path), "sha256": digest(path)}
                              for name, path in (("server", self.binary), ("core", self.core))},
                 "samples": self.samples, "sample_errors": self.sample_errors,
                 "boundary": "Fixed tiny local fixture and sampled current RSS/FDs, not high-water RSS, throughput, a total request deadline, remote certification or R3 soak."}
        if failure is None:
            try:
                validate_report(value)
            except (ValueError, TypeError, KeyError):
                value["ok"] = False
                value["failure"] = {"type": "invalid-acceptance-report", "stage": "report"}
        descriptor = os.open(self.report_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w") as stream:
            json.dump(value, stream, indent=2)
            stream.write("\n")
        return value["ok"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "core", "report"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--state-root", type=Path, default=BUILD_ROOT / "server-dev")
    parser.add_argument("--lane", choices=("release", "sanitizer"), required=True)
    parser.add_argument("--acceptance", choices=ACCEPTANCES, default=DEFAULT_ACCEPTANCE)
    args = parser.parse_args()
    if not under_build(args.state_root) or not under_build(args.report) or args.report.exists():
        parser.error("state/report must remain under this development build; report must be new")
    for binary in (args.binary, args.core):
        if not binary.is_absolute() or not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error("binaries must be absolute executable paths")
    suite = OperationsBudgetSuite(args.binary, args.core, args.state_root, args.report, args.lane, args.acceptance)
    failure = None
    try:
        suite.run()
    except Exception as error:
        failure = {"type": type(error).__name__, "stage": suite.active_check}
    finally:
        try:
            suite.cleanup()
        except Exception as error:
            failure = failure or {"type": type(error).__name__, "stage": "cleanup"}
    ok = suite.report(failure)
    print(json.dumps({"ok": ok, "checks": len(suite.observations), "failure": failure,
                      "failed": [row["name"] for row in suite.observations if not row["ok"]]}))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
