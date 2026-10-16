#!/usr/bin/env python3
"""Bounded R2 outage probes on the existing isolated PostgreSQL stand."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from integration_test import BUILD_ROOT, IntegrationFailure, IntegrationSuite, under_build
from observer_ingestion_integration_test import load_observer_fixture_module


BUDGETS = {"database_response_ms": 4000, "server_shutdown_ms": 10000,
           "local_command_ms": 3000, "outbox_entries": 4, "outbox_bytes": 65536}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class FaultBudgetSuite(IntegrationSuite):
    def __init__(self, binary, observer, core, state_root, report):
        super().__init__(binary, state_root, report)
        self.observer = observer
        self.core = core
        self.observations = []
        self.paused = False
        self.fixture_module = load_observer_fixture_module()

    def observe(self, name, passed, **details):
        self.observations.append({"name": name, "ok": bool(passed), **details})

    def native(self, command):
        environment = {"PATH": "", "LC_ALL": "C"}
        for key in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "ASAN_SYMBOLIZER_PATH"):
            if key in os.environ:
                environment[key] = os.environ[key]
        started = time.monotonic()
        result = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, env=environment, timeout=BUDGETS["local_command_ms"] / 1000)
        elapsed = (time.monotonic() - started) * 1000
        if result.returncode or result.stderr or elapsed > BUDGETS["local_command_ms"]:
            raise IntegrationFailure("local native fixture command failed")
        value = json.loads(result.stdout)
        if value.get("ok") is not True:
            raise IntegrationFailure("local native fixture returned a failed envelope")
        return value, elapsed

    def sample(self, fixture):
        result, elapsed = self.native([str(self.observer), "sample", "--config", str(fixture.config)])
        self.fixture_module.assert_observer_envelope(result, expected_ok=True)
        return self.fixture_module.state_from_result(result["result"]), elapsed

    def core_arguments(self, fixture):
        return [str(self.core), "--root", str(fixture.root), "--profile", "observer-fixture",
                "--profile-dir", str(fixture.profiles), "--out-dir", str(fixture.out)]

    def prepare_local(self, parent):
        fixture = self.fixture_module.Fixture(parent, "offline")
        config = fixture.config_data()
        config["outbox"] = {"enabled": True, "max_entries": BUDGETS["outbox_entries"],
                            "max_bytes": BUDGETS["outbox_bytes"]}
        fixture.write_config(config)
        for index in range(BUDGETS["outbox_entries"] + 2):
            fixture.write_source(f"Offline fixture observation {index}.\n")
            state, _ = self.sample(fixture)
        queue = state["outbox"]
        self.observe("outbox-saturation-preserves-unacknowledged-prefix",
                     len(queue["entries"]) == BUDGETS["outbox_entries"] and
                     queue["accepted_sequence"] == BUDGETS["outbox_entries"] and
                     queue["acknowledged_sequence"] == 0 and queue["rejected_observations"] >= 2 and
                     queue["payload_bytes"] <= BUDGETS["outbox_bytes"],
                     retained=len(queue["entries"]), rejected=queue["rejected_observations"])
        self.native([*self.core_arguments(fixture), "build"])
        return fixture, queue["entries"]

    def local_continuation(self, fixture, pending, condition):
        before = self.fixture_module.tree_metadata(fixture.root)
        manifest = fixture.out / "docs/kb/generated/manifest.json"
        before_index = digest(manifest)
        state, sample_ms = self.sample(fixture)
        result, brief_ms = self.native([*self.core_arguments(fixture), "brief", "Offline fixture observation",
                                       "--projection", "compact", "--require-current"])
        self.observe("local-continuation-" + condition,
                     state["outbox"]["entries"] == pending and state["outbox"]["acknowledged_sequence"] == 0 and
                     result.get("index_freshness", {}).get("current") is True and
                     before == self.fixture_module.tree_metadata(fixture.root) and before_index == digest(manifest),
                     observer_ms=round(sample_ms, 3), core_ms=round(brief_ms, 3),
                     command_budget_ms=BUDGETS["local_command_ms"])

    def unavailable_response(self, name):
        started = time.monotonic()
        status, code = None, None
        try:
            status, response = self.request("GET", "/health/ready", headers=self.auth_headers("admin"))
            code = response.get("error", {}).get("code")
        except IntegrationFailure:
            pass
        elapsed = (time.monotonic() - started) * 1000
        self.observe(name, status == 503 and code == "database-unavailable" and
                     elapsed <= BUDGETS["database_response_ms"],
                     http_status=status, error_code=code, elapsed_ms=round(elapsed, 3),
                     budget_ms=BUDGETS["database_response_ms"])

    def paused_database(self, fixture, pending):
        self.expect_status(200, "GET", "/health/ready", headers=self.auth_headers("admin"))
        self.compose("pause", "postgres")
        self.paused = True
        process = self.server
        try:
            self.local_continuation(fixture, pending, "database-paused")
            self.unavailable_response("paused-database-response-deadline")
            started = time.monotonic()
            process.terminate()
            code = None
            try:
                code = process.wait(timeout=BUDGETS["server_shutdown_ms"] / 1000)
            except subprocess.TimeoutExpired:
                pass
            elapsed = (time.monotonic() - started) * 1000
            self.observe("paused-database-server-shutdown", code == 0 and elapsed <= BUDGETS["server_shutdown_ms"],
                         exit_code=code, elapsed_ms=round(elapsed, 3),
                         budget_ms=BUDGETS["server_shutdown_ms"])
        finally:
            self.compose("unpause", "postgres")
            self.paused = False
            # Fault release belongs to cleanup, not successful deadline proof.
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            self.server = None
        self.start_server()
        self.wait_for_response("/health/ready", 200, headers=self.auth_headers("admin"))
        self.observe("server-restarts-after-fault-release", True)

    def run(self):
        self.record("version", self.check_version)
        self.dev_command("init")
        self.dev_command("up")
        self.wait_for_postgres()
        self.require_http_port_free()
        self.reset_schema()
        self.migrate()
        self.admin_token = self.read_private_token("dev_admin.token")
        self.reader_token = self.read_private_token("dev_reader.token")
        self.start_server()
        self.wait_for_response("/health/ready", 200, headers=self.auth_headers("admin"))
        with tempfile.TemporaryDirectory(prefix="shin-r2-fault-") as directory:
            fixture, pending = self.prepare_local(Path(directory).resolve())
            self.compose("stop", "postgres")
            self.local_continuation(fixture, pending, "database-stopped")
            self.expect_status(200, "GET", "/health/live")
            self.unavailable_response("stopped-database-response-deadline")
            self.compose("start", "postgres")
            self.wait_for_postgres()
            self.wait_for_response("/health/ready", 200, headers=self.auth_headers("admin"))
            self.paused_database(fixture, pending)
        self.stop_server(expect_clean=True)

    def cleanup(self):
        try:
            if self.paused:
                self.compose("unpause", "postgres")
                self.paused = False
        finally:
            try:
                self.stop_server(expect_clean=False)
            finally:
                self.compose("stop", "postgres")

    def report(self, failure):
        identities = {}
        for name, path in (("server", self.binary), ("observer", self.observer), ("core", self.core)):
            identities[name] = {"path": str(path), "sha256": digest(path)}
        value = {"schema_version": "shin.fault-budget-report.v1", "ok": failure is None and
                 all(item["ok"] for item in self.observations), "budgets": BUDGETS,
                 "binaries": identities, "checks": self.observations, "failure": failure,
                 "boundary": "One isolated local PostgreSQL stand and tiny external fixture; no throughput, RSS, remote deployment or full R2/R3 qualification."}
        with self.report_path.open("x") as stream:
            json.dump(value, stream, indent=2)
            stream.write("\n")
        return value["ok"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "observer", "core", "report"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--state-root", type=Path, default=BUILD_ROOT / "server-dev")
    args = parser.parse_args()
    if not under_build(args.state_root) or not under_build(args.report) or args.report.exists():
        parser.error("state/report must remain under this development build; report must be new")
    for binary in (args.binary, args.observer, args.core):
        if not binary.is_absolute() or not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error("binaries must be absolute executable paths")
    suite = FaultBudgetSuite(args.binary, args.observer, args.core, args.state_root, args.report)
    failure = None
    try:
        suite.run()
    except Exception as error:
        failure = type(error).__name__
    finally:
        try:
            suite.cleanup()
        except Exception as error:
            failure = failure or type(error).__name__
    ok = suite.report(failure)
    print(json.dumps({"ok": ok, "checks": len(suite.observations),
                      "failed": [row["name"] for row in suite.observations if not row["ok"]],
                      "failure": failure}))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
