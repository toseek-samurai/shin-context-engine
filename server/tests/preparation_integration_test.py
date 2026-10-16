#!/usr/bin/env python3
"""Real PostgreSQL and HTTP coverage for immutable preparation jobs."""

from __future__ import annotations

import argparse
from datetime import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import sys
import threading
import time
from typing import Any

from integration_test import BUILD_ROOT, IntegrationFailure, under_build
from activation_checks import ActivationChecks
from preparation_inspection_checks import PreparationInspectionChecks
from preparation_retention_checks import PreparationRetentionChecks
from preparation_preflight_checks import PreparationPreflightChecks
from retrieval_integration_test import (
    DEFAULT_CORE,
    PROJECT_ID,
    REPOSITORY_ROOT,
    ROUTE_ID,
    RetrievalIntegrationSuite,
    validate_candidate_core,
)


DEFAULT_LEDGER_PROBE = REPOSITORY_ROOT / "build" / "server-p2" / "shin-server-preparation-store-tests"
SNAPSHOT_ID = re.compile(r"fnv1a64:[0-9a-f]{16}\Z")
PUBLIC_JOB_FIELDS = {
    "id",
    "project_id",
    "route_id",
    "state",
    "cancel_requested",
    "artifact_snapshot_id",
    "artifact_bytes",
    "reserved_artifact_bytes",
    "reason_code",
    "created_at",
    "updated_at",
}
PRIVATE_JOB_FIELDS = {
    "principal_id",
    "idempotency_key",
    "request_hash",
    "route_fingerprint",
    "expected_snapshot_id",
    "expected_route_revision",
    "worker_id",
    "lease_until",
}


class PreparationIntegrationSuite(ActivationChecks, PreparationPreflightChecks, PreparationRetentionChecks,
                                 PreparationInspectionChecks, RetrievalIntegrationSuite):
    def __init__(self, binary: Path, core: Path, state_root: Path, report_path: Path, ledger_probe: Path | None) -> None:
        super().__init__(binary, core, state_root, report_path)
        self.ledger_probe = ledger_probe
        self.preparation_root: Path | None = None
        self.preparation_measurement: dict[str, Any] = {"status": "not-run"}
        self.inspection_measurement: dict[str, Any] = {"status": "not-run"}
        self.retention_measurement: dict[str, Any] = {"status": "not-run"}
        self.preflight_measurement: dict[str, Any] = {"status": "not-run"}
        self.activation_measurement: dict[str, Any] = {"status": "not-run"}

    def run_other_cli_json(self, *arguments: str) -> dict[str, Any]:
        completed = self.run_process(
            [
                str(self.core),
                "--profile",
                "generic-docs",
                "--profile-dir",
                str(self.profile_dir),
                "--root",
                str(self.other_root),
                "--out-dir",
                str(self.other_out_dir),
                *arguments,
            ],
            "candidate Core preparation fixture",
            timeout=45.0,
        )
        try:
            result = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise IntegrationFailure("candidate Core preparation fixture did not return JSON") from error
        if not isinstance(result, dict) or result.get("ok") is not True:
            raise IntegrationFailure("candidate Core preparation fixture did not succeed")
        return result

    def check_native_ledger(self) -> None:
        if self.ledger_probe is None:
            return
        self.run_process(
            [str(self.ledger_probe), str(self.runtime_config_path.resolve())],
            "native preparation ledger probe",
            timeout=30.0,
        )

    def check_native_activation_ledger(self) -> None:
        if self.ledger_probe is None:
            return
        probe = self.ledger_probe.with_name("shin-server-activation-store-tests")
        if not probe.is_file() or not os.access(probe, os.X_OK):
            raise IntegrationFailure("native activation ledger probe is unavailable")
        self.run_process([str(probe), str(self.runtime_config_path.resolve())],
                         "native activation ledger probe", timeout=30.0)

    def reset_preparation_rows(self) -> None:
        if self.scalar("SELECT to_regclass('shin_server.preparations') IS NOT NULL") != "t":
            raise IntegrationFailure("preparation migration is not available to the controlled migrator setup")
        for table in ("active_routes", "activation_receipts"):
            if self.scalar(f"SELECT to_regclass('shin_server.{table}') IS NOT NULL") != "t":
                raise IntegrationFailure("activation migration is not available to the controlled migrator setup")
        self.psql("TRUNCATE TABLE shin_server.audit_events, shin_server.active_routes, shin_server.activation_receipts, "
                  "shin_server.publication_heads, shin_server.publication_resolutions, shin_server.publication_proposals, "
                  "shin_server.publication_withdrawals, "
                  "shin_server.preparation_retention, shin_server.preparations RESTART IDENTITY")
        self.psql("UPDATE shin_server.publication_history SET history_epoch=0,retired_proposals=0,last_retention=NULL; "
                  "UPDATE shin_server.audit_history SET retired_through=0,retired_events=0,last_retention=NULL")

    def prepare_private_preparation_root(self) -> None:
        root = self.state_root / "preparations" / f"preparation-integration-{secrets.token_hex(8)}"
        parent = root.parent
        parent_entry = parent.lstat()
        if stat.S_ISLNK(parent_entry.st_mode) or not stat.S_ISDIR(parent_entry.st_mode) or stat.S_IMODE(parent_entry.st_mode) != 0o700:
            raise IntegrationFailure("shared preparation parent is not a private directory")
        root.mkdir(mode=0o700)
        os.chmod(root, 0o700)
        self.preparation_root = root.resolve(strict=True)

    def cleanup_private_preparation_root(self) -> None:
        if self.preparation_root is None or not self.preparation_root.exists():
            return
        expected_parent = (self.state_root / "preparations").resolve(strict=True)
        try:
            relative = self.preparation_root.relative_to(expected_parent)
        except ValueError as error:
            raise IntegrationFailure("refusing to remove a preparation root outside the private test parent") from error
        if len(relative.parts) != 1 or not relative.name.startswith("preparation-integration-"):
            raise IntegrationFailure("refusing to remove a preparation root not owned by this test")
        entry = self.preparation_root.lstat()
        if stat.S_ISLNK(entry.st_mode) or not stat.S_ISDIR(entry.st_mode):
            raise IntegrationFailure("preparation root changed type before cleanup")
        shutil.rmtree(self.preparation_root)
        self.preparation_root = None

    def configure_preparations(self) -> None:
        if self.preparation_root is None:
            raise IntegrationFailure("private preparation root was not initialized")
        for path in (self.runtime_config_path, self.migrator_config_path):
            config = json.loads(path.read_text(encoding="utf-8"))
            config["preparations"] = {
                "enabled": True,
                "state_dir": str(self.preparation_root),
                "max_retained_jobs": 16,
                "lease_seconds": 3,
            }
            config["retrieval"]["max_generation_bytes"] = 64 * 1024 * 1024
            self.replace_private_json(path, config)

    def post_preparation(
        self,
        project_id: str,
        route_id: str,
        expected_snapshot_id: str | None,
        key: str,
        *,
        role: str = "admin",
    ) -> tuple[int, dict[str, Any]]:
        headers = {
            **self.auth_headers(role),
            "Content-Type": "application/json",
            "Idempotency-Key": key,
        }
        return self.request(
            "POST",
            f"/api/v1/projects/{project_id}/routes/{route_id}/preparations",
            headers=headers,
            body=json.dumps({"expected_snapshot_id": expected_snapshot_id}, separators=(",", ":")).encode("utf-8"),
        )

    def get_job(self, job_id: int, *, role: str = "admin", expected_status: int = 200) -> dict[str, Any]:
        response = self.expect_status(
            expected_status,
            "GET",
            f"/api/v1/preparations/{job_id}",
            headers=self.auth_headers(role),
        )
        return response.get("result", {})

    def assert_public_job(self, job: Any, *, project_id: str, route_id: str) -> dict[str, Any]:
        if not isinstance(job, dict) or set(job) != PUBLIC_JOB_FIELDS:
            raise IntegrationFailure("preparation response does not match the public job contract")
        if PRIVATE_JOB_FIELDS.intersection(job):
            raise IntegrationFailure("preparation response exposed private lease or credential fields")
        if not isinstance(job["id"], int) or job["id"] <= 0:
            raise IntegrationFailure("preparation response has an invalid job identity")
        if job["project_id"] != project_id or job["route_id"] != route_id:
            raise IntegrationFailure("preparation response crossed its project or route boundary")
        if job["state"] not in {"queued", "running", "prepared", "failed", "cancelled"}:
            raise IntegrationFailure("preparation response has an invalid state")
        if type(job["cancel_requested"]) is not bool:
            raise IntegrationFailure("preparation response has an invalid cancellation flag")
        if job["artifact_snapshot_id"] is not None and not (
            isinstance(job["artifact_snapshot_id"], str) and SNAPSHOT_ID.fullmatch(job["artifact_snapshot_id"])
        ):
            raise IntegrationFailure("preparation response has an invalid artifact snapshot identity")
        if job["artifact_bytes"] is not None and (type(job["artifact_bytes"]) is not int or job["artifact_bytes"] < 0):
            raise IntegrationFailure("preparation response has invalid artifact bytes")
        if type(job["reserved_artifact_bytes"]) is not int or not 0 < job["reserved_artifact_bytes"] <= 1073741824:
            raise IntegrationFailure("preparation response has an invalid artifact reservation")
        if job["reason_code"] is not None and not isinstance(job["reason_code"], str):
            raise IntegrationFailure("preparation response has an invalid reason code")
        rendered = json.dumps(job, sort_keys=True)
        forbidden_values = [str(self.fixture_root), str(self.out_dir)]
        if self.preparation_root is not None:
            forbidden_values.append(str(self.preparation_root))
        if any(value in rendered for value in forbidden_values):
            raise IntegrationFailure("preparation response leaked a local path")
        return job

    @staticmethod
    def tree_digest(root: Path) -> str:
        digest = hashlib.sha256()
        for path in sorted(root.rglob("*")):
            entry = path.lstat()
            if stat.S_ISLNK(entry.st_mode):
                raise IntegrationFailure("preparation fixture unexpectedly contains a symbolic link")
            if not stat.S_ISREG(entry.st_mode):
                continue
            relative = path.relative_to(root).as_posix().encode("utf-8")
            digest.update(len(relative).to_bytes(4, "big"))
            digest.update(relative)
            content = path.read_bytes()
            digest.update(len(content).to_bytes(8, "big"))
            digest.update(content)
        return digest.hexdigest()

    def current_snapshot_id(self) -> str | None:
        active = self.activation(PROJECT_ID, ROUTE_ID)
        snapshot_id = active["snapshot_id"]
        if snapshot_id is not None and not SNAPSHOT_ID.fullmatch(snapshot_id):
            raise IntegrationFailure("managed route returned an invalid active snapshot identity")
        return snapshot_id

    def wait_for_terminal(self, job_id: int, state: str, reason_code: str | None = None,
                          *, project_id: str = PROJECT_ID, route_id: str = ROUTE_ID) -> dict[str, Any]:
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            job = self.assert_public_job(self.get_job(job_id), project_id=project_id, route_id=route_id)
            if job["state"] == state:
                if reason_code is not None and job["reason_code"] != reason_code:
                    raise IntegrationFailure("preparation completed with the wrong safe reason code")
                return job
            if job["state"] in {"prepared", "failed", "cancelled"}:
                raise IntegrationFailure(f"preparation ended as {job['state']}/{job['reason_code']}, expected {state}")
            time.sleep(0.1)
        raise IntegrationFailure("preparation did not reach its expected terminal state")

    def wait_for_state(self, job_id: int, state: str, timeout: float = 5.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            job = self.assert_public_job(self.get_job(job_id), project_id=PROJECT_ID, route_id=ROUTE_ID)
            if job["state"] == state:
                return job
            if job["state"] in {"prepared", "failed", "cancelled"}:
                raise IntegrationFailure("preparation reached a terminal state before the controlled phase")
            time.sleep(0.05)
        raise IntegrationFailure("preparation did not enter the controlled phase")

    def kill_owned_server(self) -> None:
        if self.server is None:
            raise IntegrationFailure("owned server process is not running")
        process = self.server
        self.server = None
        process.kill()
        try:
            process.wait(timeout=5.0)
        except Exception as error:
            raise IntegrationFailure("owned server process did not exit after SIGKILL") from error

    def check_worker_status_and_access(self) -> None:
        self.expect_status(403, "GET", "/api/v1/preparation-worker", headers=self.auth_headers("reader"))
        result = self.expect_status(200, "GET", "/api/v1/preparation-worker", headers=self.auth_headers("admin")).get("result")
        if not isinstance(result, dict) or result.get("enabled") is not True or not isinstance(result.get("state"), str):
            raise IntegrationFailure("preparation worker did not report enabled state")
        if result.get("activation_supported") is not True:
            raise IntegrationFailure("preparation worker did not report active-generation support")
        if result.get("last_error_code") is not None and not isinstance(result["last_error_code"], str):
            raise IntegrationFailure("preparation worker returned an invalid safe error code")

    def check_prepare_api_and_artifact(self) -> None:
        snapshot_id = self.current_snapshot_id()
        canonical_output = self.tree_digest(self.out_dir)
        source_tree = self.tree_digest(self.fixture_root)
        status, response = self.post_preparation(PROJECT_ID, ROUTE_ID, snapshot_id, "prepare-visible")
        if status != 202:
            raise IntegrationFailure("new preparation did not return HTTP 202")
        result = response.get("result")
        if not isinstance(result, dict) or result.get("replayed") is not False:
            raise IntegrationFailure("new preparation response did not mark a non-replayed job")
        job = self.assert_public_job(result.get("job"), project_id=PROJECT_ID, route_id=ROUTE_ID)
        job_id = job["id"]

        replay_status, replay_response = self.post_preparation(PROJECT_ID, ROUTE_ID, snapshot_id, "prepare-visible")
        if replay_status != 200:
            raise IntegrationFailure("preparation replay did not return HTTP 200")
        replay = replay_response.get("result")
        if not isinstance(replay, dict) or replay.get("replayed") is not True:
            raise IntegrationFailure("preparation replay marker is missing")
        replay_job = self.assert_public_job(replay.get("job"), project_id=PROJECT_ID, route_id=ROUTE_ID)
        if replay_job["id"] != job_id:
            raise IntegrationFailure("preparation replay changed its job identity")

        prepared = self.wait_for_terminal(job_id, "prepared")
        if prepared["artifact_snapshot_id"] is None or prepared["artifact_bytes"] is None:
            raise IntegrationFailure("prepared job did not retain its artifact metadata")
        if self.tree_digest(self.out_dir) != canonical_output or self.tree_digest(self.fixture_root) != source_tree:
            raise IntegrationFailure("preparation changed canonical output or source bytes")
        if self.preparation_root is None:
            raise IntegrationFailure("private preparation root is unavailable")
        artifact_dir = self._inspection_artifact(job_id)
        for command in (
            ["check"],
            ["doctor", "--strict"],
            ["brief", "preparation candidate", "--projection", "compact", "--require-current"],
        ):
            self.run_process(
                [
                    str(self.core),
                    "--profile",
                    "generic-docs",
                    "--profile-dir",
                    str(self.profile_dir),
                    "--root",
                    str(self.fixture_root),
                    "--out-dir",
                    str(artifact_dir),
                    *command,
                ],
                "independent prepared artifact verification",
                timeout=45.0,
            )

        page = self.expect_status(200, "GET", "/api/v1/preparations?limit=1", headers=self.auth_headers("admin")).get("result")
        if not isinstance(page, dict) or not isinstance(page.get("items"), list) or "next_cursor" not in page:
            raise IntegrationFailure("preparation list response is malformed")
        for item in page["items"]:
            self.assert_public_job(item, project_id=item.get("project_id"), route_id=item.get("route_id"))
        self.assert_public_job(self.get_job(job_id), project_id=PROJECT_ID, route_id=ROUTE_ID)
        self.assert_public_job(self.get_job(job_id, role="reader"), project_id=PROJECT_ID, route_id=ROUTE_ID)

        self.expect_status(
            403,
            "POST",
            f"/api/v1/preparations/{job_id}/cancel",
            headers={**self.auth_headers("reader"), "Content-Type": "application/json"},
            body=b"{}",
        )
        cancel = self.expect_status(
            200,
            "POST",
            f"/api/v1/preparations/{job_id}/cancel",
            headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
            body=b"{}",
        ).get("result")
        terminal = self.assert_public_job(cancel, project_id=PROJECT_ID, route_id=ROUTE_ID)
        if terminal["state"] != "prepared":
            raise IntegrationFailure("cancelling a prepared job changed its terminal state")

        self.stop_server(expect_clean=True)
        self.start_server()
        restarted = self.assert_public_job(self.get_job(job_id), project_id=PROJECT_ID, route_id=ROUTE_ID)
        if restarted["state"] != "prepared" or restarted["artifact_snapshot_id"] != prepared["artifact_snapshot_id"]:
            raise IntegrationFailure("prepared job did not survive a clean restart")
        events = self.psql(f"SELECT action FROM shin_server.audit_events WHERE preparation_id = {job_id} ORDER BY id").splitlines()
        if events != ["preparation.queued", "preparation.running", "preparation.prepared"]:
            raise IntegrationFailure("preparation audit lost its identity, order or idempotency")

    def check_enqueue_audit_rollback(self) -> None:
        snapshot_id = self.current_snapshot_id()
        self.psql("REVOKE INSERT ON shin_server.audit_events FROM shin_runtime")
        try:
            status, _ = self.post_preparation(PROJECT_ID, ROUTE_ID, snapshot_id, "prepare-audit-rollback")
            if status != 503:
                raise IntegrationFailure("preparation without audit authority did not fail")
        finally:
            self.psql("GRANT INSERT ON shin_server.audit_events TO shin_runtime")
        if self.scalar("SELECT count(*) FROM shin_server.preparations WHERE idempotency_key = 'prepare-audit-rollback'") != "0":
            raise IntegrationFailure("failed preparation transaction retained a job or disk reservation")

    def check_scope_and_base_change(self) -> None:
        snapshot_id = self.current_snapshot_id()
        denied_status, _ = self.post_preparation(PROJECT_ID, ROUTE_ID, snapshot_id, "prepare-reader", role="reader")
        if denied_status != 403:
            raise IntegrationFailure("reader preparation creation was not forbidden")

        other_snapshot = self.activation("other", "other-retrieval")["snapshot_id"]
        status, response = self.post_preparation("other", "other-retrieval", other_snapshot, "prepare-other")
        if status != 202:
            raise IntegrationFailure("other preparation was not accepted")
        other = self.assert_public_job(response.get("result", {}).get("job"), project_id="other", route_id="other-retrieval")
        self.get_job(other["id"], role="reader", expected_status=404)

        wrong_snapshot = "fnv1a64:ffffffffffffffff"
        if wrong_snapshot == snapshot_id:
            wrong_snapshot = "fnv1a64:0000000000000000"
        baseline_output = self.tree_digest(self.out_dir)
        baseline_source = self.tree_digest(self.fixture_root)
        status, response = self.post_preparation(PROJECT_ID, ROUTE_ID, wrong_snapshot, "prepare-wrong-base")
        if status != 202:
            raise IntegrationFailure("wrong-base preparation was not accepted for asynchronous validation")
        wrong_base = self.assert_public_job(response.get("result", {}).get("job"), project_id=PROJECT_ID, route_id=ROUTE_ID)
        failed = self.wait_for_terminal(wrong_base["id"], "failed", "preparation-base-changed")
        if failed["artifact_snapshot_id"] is not None or self.tree_digest(self.out_dir) != baseline_output or self.tree_digest(self.fixture_root) != baseline_source:
            raise IntegrationFailure("wrong-base preparation changed an active artifact or source")

    def check_activation_compare_and_swap_fencing(self) -> None:
        base = self.activation(PROJECT_ID, ROUTE_ID)
        first = self._prepare_activation_candidate(base["snapshot_id"], base["revision"], "prepare-cas-first")
        second = self._prepare_activation_candidate(base["snapshot_id"], base["revision"], "prepare-cas-second")
        first_outcome: list[tuple[int, dict[str, Any]]] = []
        first_failures: list[BaseException] = []

        def activate_first() -> None:
            try:
                first_outcome.append(self._post_activation(PROJECT_ID, ROUTE_ID, first["id"], base["revision"]))
            except BaseException as error:
                first_failures.append(error)

        publication = os.open(
            self._inspection_artifact(first["id"]) / "docs" / "kb" / ".shin-context.lock",
            os.O_RDONLY | os.O_NOFOLLOW,
        )
        lifetime = os.open(self._inspection_lock(first["id"]), os.O_RDONLY | os.O_NOFOLLOW)
        worker = threading.Thread(target=activate_first)
        try:
            fcntl.flock(publication, fcntl.LOCK_EX)
            worker.start()
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                try:
                    fcntl.flock(lifetime, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    break
                else:
                    fcntl.flock(lifetime, fcntl.LOCK_UN)
                time.sleep(0.02)
            else:
                raise IntegrationFailure("first activation did not occupy the shared candidate-inspection slot")
            busy_status, busy = self._post_activation(PROJECT_ID, ROUTE_ID, second["id"], base["revision"])
            if busy_status != 503 or busy.get("error", {}).get("code") != "preparation-inspection-busy":
                raise IntegrationFailure("concurrent activation did not reject the second candidate through the shared inspection slot")
        finally:
            fcntl.flock(publication, fcntl.LOCK_UN)
            os.close(publication)
            os.close(lifetime)
            if worker.ident is not None:
                worker.join(timeout=10.0)
        if worker.is_alive() or first_failures or len(first_outcome) != 1 or first_outcome[0][0] != 200:
            raise IntegrationFailure("first activation did not complete after releasing its publication barrier")
        active = self.activation(PROJECT_ID, ROUTE_ID)
        if active["revision"] != base["revision"] + 1 or active["preparation_id"] != first["id"]:
            raise IntegrationFailure("concurrent activation did not persist the winning candidate")
        retry_status, retry = self._post_activation(PROJECT_ID, ROUTE_ID, second["id"], base["revision"])
        if retry_status != 409 or retry.get("error", {}).get("code") != "activation-conflict":
            raise IntegrationFailure("inspection-slot refusal was not fenced by a later activation compare-and-swap")
        status, response = self._post_activation(PROJECT_ID, ROUTE_ID, None, active["revision"])
        if status != 200:
            raise IntegrationFailure("concurrent activation fixture could not be explicitly deactivated")
        self._release_candidate(first)
        self._release_candidate(second)

    def check_preparation_base_revision_restart_fencing(self) -> None:
        base = self.activation(PROJECT_ID, ROUTE_ID)
        stale = self._prepare_activation_candidate(base["snapshot_id"], base["revision"], "prepare-restart-stale")
        replacement = self._prepare_activation_candidate(base["snapshot_id"], base["revision"], "prepare-restart-replacement")
        status, response = self._post_activation(PROJECT_ID, ROUTE_ID, replacement["id"], base["revision"])
        if status != 200:
            raise IntegrationFailure("base-revision replacement activation was not accepted")
        active = self.activation(PROJECT_ID, ROUTE_ID)
        self.stop_server(expect_clean=True)
        self.start_server()
        if self.activation(PROJECT_ID, ROUTE_ID) != active:
            raise IntegrationFailure("restart lost the active base used to fence stale preparations")
        status, response = self._post_activation(PROJECT_ID, ROUTE_ID, stale["id"], base["revision"])
        if status != 409 or response.get("error", {}).get("code") not in {"activation-preflight-failed", "activation-conflict"}:
            raise IntegrationFailure("stale preparation was not fenced by its captured route revision")
        if self.activation(PROJECT_ID, ROUTE_ID) != active:
            raise IntegrationFailure("stale preparation activation changed the durable active generation")
        status, response = self._post_activation(PROJECT_ID, ROUTE_ID, None, active["revision"])
        if status != 200:
            raise IntegrationFailure("base-revision fencing fixture could not be explicitly deactivated")
        self._release_candidate(stale)

    def check_strict_doctor_failure(self) -> None:
        fixture = self.fixture_root / "docs" / "credential-fixture.md"
        marker = secrets.token_hex(24)
        self.write_fixture_file(fixture, "# Credential Fixture\n\napi_key = " + marker + "\n")
        try:
            self.run_cli_json("build")
            snapshot_id = self.current_snapshot_id()
            canonical_output = self.tree_digest(self.out_dir)
            source_tree = self.tree_digest(self.fixture_root)
            status, response = self.post_preparation(PROJECT_ID, ROUTE_ID, snapshot_id, "prepare-doctor-failure")
            if status != 202:
                raise IntegrationFailure("credential-like preparation was not accepted for worker validation")
            job = self.assert_public_job(response.get("result", {}).get("job"), project_id=PROJECT_ID, route_id=ROUTE_ID)
            failed = self.wait_for_terminal(job["id"], "failed", "preparation-doctor-failed")
            rendered = json.dumps(failed, sort_keys=True)
            if marker in rendered or failed["artifact_snapshot_id"] is not None:
                raise IntegrationFailure("doctor failure exposed credential-like content or activated an artifact")
            if self.tree_digest(self.out_dir) != canonical_output or self.tree_digest(self.fixture_root) != source_tree:
                raise IntegrationFailure("doctor failure changed canonical output or source bytes")
        finally:
            fixture.unlink(missing_ok=True)
            self.run_cli_json("build")

    def check_preparation_baseline(self) -> None:
        snapshot_id = self.current_snapshot_id()
        output = self.tree_digest(self.out_dir)
        samples = []
        payload_bytes = 0
        for index in range(8):
            status, response = self.post_preparation(PROJECT_ID, ROUTE_ID, snapshot_id, f"prepare-measure-{index}")
            if status != 202:
                raise IntegrationFailure("measured preparation was not accepted")
            completed = self.wait_for_terminal(response["result"]["job"]["id"], "prepared")
            start = datetime.fromisoformat(completed["created_at"].replace("Z", "+00:00"))
            finish = datetime.fromisoformat(completed["updated_at"].replace("Z", "+00:00"))
            samples.append((finish - start).total_seconds() * 1000)
            payload_bytes = completed["artifact_bytes"]
        if self.tree_digest(self.out_dir) != output:
            raise IntegrationFailure("measured preparations changed the active publication")
        self.preparation_measurement = {
            "status": "completed", "samples": len(samples), "payload_bytes": payload_bytes,
            "p50_ms": self.percentile_ms(samples, 50), "p95_ms": self.percentile_ms(samples, 95),
            "scope": "PostgreSQL enqueue-to-terminal timestamps; eight sequential small-fixture jobs, not throughput or soak",
        }

    def check_artifact_budget_and_initial_base(self) -> None:
        snapshot_id = self.current_snapshot_id()
        self.stop_server(expect_clean=True)
        original = json.loads(self.runtime_config_path.read_text(encoding="utf-8"))
        smaller = json.loads(json.dumps(original))
        payload_size = sum(path.stat().st_size for path in (self.out_dir / "docs/kb/generated").iterdir() if path.is_file())
        smaller["retrieval"]["max_generation_bytes"] = payload_size - 1
        smaller["retrieval"]["max_file_bytes"] = payload_size - 1
        self.replace_private_json(self.runtime_config_path, smaller)
        self.start_server()
        status, response = self.post_preparation(PROJECT_ID, ROUTE_ID, snapshot_id, "prepare-small-budget")
        if status != 202:
            raise IntegrationFailure("bounded artifact preparation was not accepted")
        job_id = response["result"]["job"]["id"]
        self.wait_for_terminal(job_id, "failed", "preparation-artifact-too-large")
        if self.preparation_root is None or self._inspection_artifact(job_id).exists():
            raise IntegrationFailure("over-budget preparation wrote an artifact")
        self.stop_server(expect_clean=True)
        other = next(route for route in original["retrieval"]["routes"] if route["project_id"] == "other")
        original["retrieval"]["routes"].append({**other, "id": "initial-route"})
        self.replace_private_json(self.runtime_config_path, original)
        self.start_server()
        status, response = self.post_preparation("other", "initial-route", None, "prepare-initial-base")
        if status != 202:
            raise IntegrationFailure("initial no-publication preparation was not accepted")
        self.wait_for_terminal(response["result"]["job"]["id"], "prepared", project_id="other", route_id="initial-route")
        initial = self.activation("other", "initial-route")
        if initial["revision"] != 0 or initial["preparation_id"] is not None or initial["snapshot_id"] is not None:
            raise IntegrationFailure("initial preparation implicitly published a managed route")
        status, overflow = self.post_preparation("other", "initial-route", None, "prepare-capacity-overflow")
        if status != 409 or overflow.get("error", {}).get("code") != "preparation-capacity":
            raise IntegrationFailure("retained job capacity was not enforced")

    def write_report(self, *, ok: bool, failure: str | None = None) -> None:
        super().write_report(ok=ok, failure=failure)
        report = json.loads(self.report_path.read_text(encoding="utf-8"))
        report["preparation_measurement"] = self.preparation_measurement
        report["inspection_measurement"] = self.inspection_measurement
        report["retention_measurement"] = self.retention_measurement
        report["preflight_measurement"] = self.preflight_measurement
        report["activation_measurement"] = self.activation_measurement
        self.replace_private_json(self.report_path, report)

    def run(self) -> None:
        super().run()
        try:
            self.record("preparation-native-controlled-reset", self.reset_preparation_rows)
            self.record("preparation-native-ledger", self.check_native_ledger)
            self.record("activation-native-controlled-reset", self.reset_preparation_rows)
            self.record("activation-native-ledger", self.check_native_activation_ledger)
            self.record("preparation-controlled-reset", self.reset_preparation_rows)
            self.prepare_retrieval_fixture()
            self.prepare_private_preparation_root()
            self.configure_retrieval()
            self.configure_preparations()
            self.start_server()
            self.record("preparation-worker-status-and-access", self.check_worker_status_and_access)
            self.record("preparation-api-artifact-and-restart", self.check_prepare_api_and_artifact)
            self.record("candidate-inspection-success", self.check_candidate_inspection_success)
            self.record("candidate-inspection-source-drift", self.check_candidate_inspection_source_drift)
            self.record("candidate-inspection-base-change", self.check_candidate_inspection_base_change)
            self.record("candidate-inspection-artifact-integrity", self.check_candidate_inspection_artifact_integrity)
            self.record("candidate-inspection-safety-and-route", self.check_candidate_inspection_safety_and_route)
            self.record("candidate-inspection-ledger-mismatch", self.check_candidate_inspection_ledger_mismatch)
            self.record("candidate-inspection-lock-safety", self.check_candidate_inspection_lock_safety)
            self.record("candidate-inspection-nonprepared-states", self.check_candidate_inspection_nonprepared_states)
            self.record("candidate-inspection-scope-and-query-guards", self.check_candidate_inspection_scope_and_query_guards)
            self.record("candidate-inspection-global-admission", self.check_candidate_inspection_global_admission)
            self.record("candidate-inspection-small-fixture-baseline", self.check_candidate_inspection_baseline)
            self.record("preparation-enqueue-audit-rollback", self.check_enqueue_audit_rollback)
            self.record("preparation-scope-and-base-change", self.check_scope_and_base_change)
            self.record("activation-concurrent-compare-and-swap", self.check_activation_compare_and_swap_fencing)
            self.record("preparation-base-revision-restart-fencing", self.check_preparation_base_revision_restart_fencing)
            self.record("preparation-strict-doctor-failure", self.check_strict_doctor_failure)
            self.record("preparation-small-fixture-baseline", self.check_preparation_baseline)
            self.record("preparation-artifact-budget-and-initial-base", self.check_artifact_budget_and_initial_base)
            self.record("candidate-retention-clean-prepared", self.check_candidate_retention_clean_prepared_at_capacity)
            self.record("candidate-retention-plan-and-tree-safety", self.check_candidate_retention_plan_change_and_tree_safety)
            self.record("candidate-retention-busy-writer", self.check_candidate_retention_busy_writer)
            self.record("candidate-retention-active-states", self.check_candidate_retention_active_states)
            self.record("candidate-retention-cancelled-and-expired-fences", self.check_candidate_retention_cancelled_and_expired_fences)
            self.record("candidate-retention-audit-rollback", self.check_candidate_retention_audit_rollback)
            self.record("candidate-retention-crash-recovery", self.check_candidate_retention_crash_recovery)
            self.record("candidate-retention-namespace-and-request-guards", self.check_candidate_retention_namespace_and_request_guards)
            self.record("candidate-retention-small-fixture-baseline", self.check_candidate_retention_baseline)
            self.stop_server(expect_clean=True)
            self.record("preflight-controlled-reset", self.reset_preparation_rows)
            self.run_preflight_checks()
            self.stop_server(expect_clean=True)
            self.record("activation-controlled-reset", self.reset_preparation_rows)
            self.run_activation_checks()
            self.stop_server(expect_clean=True)
        finally:
            self.stop_server(expect_clean=False)
            self.restore_configs()
            self.cleanup_private_preparation_root()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="native shin-server executable")
    parser.add_argument("--core", default=str(DEFAULT_CORE), help="candidate shin-context executable")
    parser.add_argument("--ledger-probe", default=str(DEFAULT_LEDGER_PROBE), help="optional native preparation Store probe")
    parser.add_argument("--state-root", default=str(BUILD_ROOT / "server-dev"), help="existing isolated server development state")
    parser.add_argument("--report", help="new report path below this worktree's build directory")
    return parser.parse_args(argv)


def optional_probe(value: str) -> Path | None:
    if not value:
        return None
    probe = Path(value).resolve(strict=False)
    if not under_build(probe) or not probe.is_file() or not os.access(probe, os.X_OK):
        raise IntegrationFailure("ledger probe must be an executable below this worktree build directory")
    return probe


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    binary = Path(args.binary).resolve(strict=False)
    core = Path(args.core).resolve(strict=False)
    state_root = Path(args.state_root).resolve(strict=False)
    report_path = Path(args.report).resolve(strict=False) if args.report else state_root / "reports" / f"preparation-integration-{secrets.token_hex(8)}.json"
    try:
        ledger_probe = optional_probe(args.ledger_probe)
    except IntegrationFailure:
        print("error: invalid ledger probe", file=sys.stderr)
        return 2
    if not binary.is_file() or not os.access(binary, os.X_OK):
        print("error: --binary must name an executable native shin-server", file=sys.stderr)
        return 2
    if not core.is_file() or not os.access(core, os.X_OK) or not validate_candidate_core(core):
        print("error: --core must name this worktree's current candidate shin-context", file=sys.stderr)
        return 2
    if not under_build(state_root) or not under_build(report_path) or report_path.exists():
        print("error: state root and report must be new paths below this worktree build directory", file=sys.stderr)
        return 2

    suite = PreparationIntegrationSuite(binary, core, state_root, report_path, ledger_probe)
    try:
        suite.run()
    except Exception as error:
        suite.stop_server(expect_clean=False)
        if isinstance(error, IntegrationFailure):
            suite.failure_assertion = str(error)
        try:
            suite.write_report(ok=False, failure=type(error).__name__)
        except OSError:
            pass
        print(f"error: preparation integration test failed: {type(error).__name__}", file=sys.stderr)
        return 1
    suite.write_report(ok=True)
    print("preparation integration test passed; compact report written below the isolated build state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
