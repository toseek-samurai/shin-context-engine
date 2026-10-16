#!/usr/bin/env python3
"""HTTP checks for the read-only prepared-candidate inspection route.

This module deliberately does not import ``preparation_integration_test``.  The
main preparation suite supplies its HTTP, PostgreSQL, Core and fixture helpers
by subclassing this mixin after ``PreparationIntegrationSuite``.
"""

from __future__ import annotations

import fcntl
import hashlib
import json
import os
from pathlib import Path
import secrets
import stat
import threading
import time
from typing import Any

from integration_test import IntegrationFailure
from retrieval_integration_test import PROJECT_ID, ROUTE_ID


_PUBLIC_JOB_FIELDS = {
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
_CHECK_NAMES = ("route", "writer", "artifact", "ledger", "doctor", "source", "base")
_NOT_ASSESSED = "not-assessed"
_PASSING_CHECKS = {
    "route": "matched",
    "writer": "quiescent",
    "artifact": "verified",
    "ledger": "matched",
    "doctor": "passed",
    "source": "current",
    "base": "matched",
}


class PreparationInspectionChecks:
    """Checks mixed into ``PreparationIntegrationSuite`` by the main runner."""

    def _inspection_job(self) -> dict[str, Any]:
        value = self.scalar(
            "SELECT id FROM shin_server.preparations "
            f"WHERE project_id = '{PROJECT_ID}' AND route_id = '{ROUTE_ID}' AND state = 'prepared' "
            "ORDER BY id DESC LIMIT 1"
        )
        if not value.isdigit() or int(value) <= 0:
            raise IntegrationFailure("candidate inspection requires the visible prepared job from the preparation check")
        return self.assert_public_job(self.get_job(int(value)), project_id=PROJECT_ID, route_id=ROUTE_ID)

    def _inspection_artifact(self, job_id: int) -> Path:
        if self.preparation_root is None:
            raise IntegrationFailure("candidate inspection has no private preparation state directory")
        key = self.scalar("SELECT artifact_key FROM shin_server.preparations WHERE id = " + str(job_id))
        return self.preparation_root / f"job-{job_id}-{key}"

    def _inspection_lock(self, job_id: int) -> Path:
        if self.preparation_root is None:
            raise IntegrationFailure("candidate inspection has no private preparation state directory")
        return self._inspection_artifact(job_id).with_name("." + self._inspection_artifact(job_id).name + ".lock")

    @staticmethod
    def _sql_text(value: str) -> str:
        return "'" + value.replace("'", "''") + "'"

    @staticmethod
    def _tree_fingerprint(root: Path) -> str:
        """Fingerprint names, types, modes and bytes without following links."""
        if not root.exists():
            return "missing"
        digest = hashlib.sha256()
        for path in [root, *sorted(root.rglob("*"))]:
            entry = path.lstat()
            relative = "." if path == root else path.relative_to(root).as_posix()
            digest.update(relative.encode("utf-8"))
            digest.update(b"\0")
            digest.update(str(stat.S_IFMT(entry.st_mode)).encode("ascii"))
            digest.update(b"\0")
            digest.update(oct(stat.S_IMODE(entry.st_mode)).encode("ascii"))
            digest.update(b"\0")
            if stat.S_ISLNK(entry.st_mode):
                digest.update(os.readlink(path).encode("utf-8"))
            elif stat.S_ISREG(entry.st_mode):
                digest.update(path.read_bytes())
            digest.update(b"\0")
        return digest.hexdigest()

    def _inspection_database_state(self, job_id: int) -> dict[str, str]:
        return {
            "job": self.scalar(
                "SELECT row_to_json(preparation)::text FROM "
                "(SELECT * FROM shin_server.preparations WHERE id = " + str(job_id) + ") AS preparation"
            ),
            "audit": self.scalar(
                "SELECT COALESCE(jsonb_agg(event ORDER BY id)::text, '[]') FROM "
                "(SELECT * FROM shin_server.audit_events WHERE preparation_id = " + str(job_id) + ") AS event"
            ),
        }

    def _inspection_state(self, job_id: int) -> dict[str, Any]:
        artifact = self._inspection_artifact(job_id)
        return {
            "artifact": self._tree_fingerprint(artifact),
            "source": self._tree_fingerprint(self.fixture_root),
            "active": self.activation(PROJECT_ID, ROUTE_ID),
            "database": self._inspection_database_state(job_id),
        }

    def _assert_inspection_state(self, job_id: int, expected: dict[str, Any]) -> None:
        actual = self._inspection_state(job_id)
        if actual != expected:
            changed = ",".join(key for key in expected if actual.get(key) != expected[key])
            raise IntegrationFailure("candidate inspection changed tracked fixture state: " + changed)

    def _get_inspection(self, job_id: int, *, role: str = "admin") -> tuple[dict[str, Any], dict[str, Any]]:
        envelope = self.expect_status(
            200,
            "GET",
            f"/api/v1/preparations/{job_id}/inspection",
            headers=self.auth_headers(role),
        )
        report = envelope.get("result")
        if not isinstance(report, dict):
            raise IntegrationFailure("candidate inspection response has no report object")
        return envelope, report

    @staticmethod
    def _checks(**values: str) -> dict[str, str]:
        result = {name: _NOT_ASSESSED for name in _CHECK_NAMES}
        result.update(values)
        return result

    def _assert_safe_report(self, report: dict[str, Any], forbidden: tuple[str, ...] = ()) -> None:
        rendered = json.dumps(report, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        private_values = [str(self.fixture_root), str(self.out_dir)]
        if self.preparation_root is not None:
            private_values.append(str(self.preparation_root))
        private_values.extend((self.admin_token, self.reader_token, *forbidden))
        if any(value and value in rendered for value in private_values):
            raise IntegrationFailure("candidate inspection exposed a path, secret or inspected file content")

        def visit(value: Any) -> None:
            if isinstance(value, dict):
                for key, child in value.items():
                    if key.lower() in {"path", "paths", "message", "content", "detail", "exception"}:
                        raise IntegrationFailure("candidate inspection exposed an unsafe diagnostic field")
                    visit(child)
            elif isinstance(value, list):
                for child in value:
                    visit(child)

        visit(report)

    def _assert_report(
        self,
        report: dict[str, Any],
        job: dict[str, Any],
        *,
        checks: dict[str, str],
        reason_codes: list[str],
        candidate_verified: bool,
    ) -> None:
        expected_fields = {
            "schema_version",
            "job",
            "scope",
            "started_at",
            "finished_at",
            "checks",
            "candidate_verified",
            "activation_supported",
            "activation_ready",
            "project_quality_assessed",
            "mutations_performed",
            "artifact_snapshot_id",
            "artifact_bytes",
            "reason_codes",
        }
        if set(report) != expected_fields:
            raise IntegrationFailure("candidate inspection report does not have the exact public schema")
        if report["schema_version"] != "shin.preparation-inspection.v1" or report["scope"] != "candidate-observation":
            raise IntegrationFailure("candidate inspection report has the wrong schema identity or scope")
        if not isinstance(report["job"], dict) or set(report["job"]) != _PUBLIC_JOB_FIELDS or report["job"] != job:
            raise IntegrationFailure("candidate inspection did not return exactly the public inspected job")
        if report["checks"] != checks or report["reason_codes"] != reason_codes:
            raise IntegrationFailure("candidate inspection reported an unexpected check state or reason code")
        if report["candidate_verified"] is not candidate_verified:
            raise IntegrationFailure("candidate inspection has an invalid verification summary")
        if report["activation_supported"] is not True or report["activation_ready"] is not False:
            raise IntegrationFailure("candidate inspection reported an invalid activation capability boundary")
        for name in ("project_quality_assessed", "mutations_performed"):
            if report[name] is not False:
                raise IntegrationFailure("candidate inspection claimed unsupported quality or mutation behavior")
        for name in ("started_at", "finished_at"):
            value = report[name]
            if not isinstance(value, str) or not value.endswith("Z"):
                raise IntegrationFailure("candidate inspection did not render a UTC observation timestamp")
            try:
                time.strptime(value, "%Y-%m-%dT%H:%M:%S%z")
            except ValueError:
                # Fractional UTC timestamps are valid and expected from the server clock.
                try:
                    from datetime import datetime
                    datetime.fromisoformat(value.replace("Z", "+00:00"))
                except ValueError as error:
                    raise IntegrationFailure("candidate inspection returned an invalid UTC timestamp") from error
        if report["started_at"] > report["finished_at"]:
            raise IntegrationFailure("candidate inspection observation finished before it started")
        if candidate_verified:
            if report["artifact_snapshot_id"] != job["artifact_snapshot_id"] or report["artifact_bytes"] != job["artifact_bytes"]:
                raise IntegrationFailure("verified candidate inspection did not bind artifact metadata to the ledger")
        elif report["artifact_snapshot_id"] is not None and not isinstance(report["artifact_snapshot_id"], str):
            raise IntegrationFailure("candidate inspection has an invalid optional artifact snapshot identity")
        elif report["artifact_bytes"] is not None and (type(report["artifact_bytes"]) is not int or report["artifact_bytes"] < 0):
            raise IntegrationFailure("candidate inspection has an invalid optional artifact byte count")
        self._assert_safe_report(report)

    def _assert_observation(
        self,
        job: dict[str, Any],
        *,
        checks: dict[str, str],
        reason_codes: list[str],
        candidate_verified: bool = False,
        role: str = "admin",
    ) -> dict[str, Any]:
        _, report = self._get_inspection(job["id"], role=role)
        self._assert_report(
            report,
            job,
            checks=checks,
            reason_codes=reason_codes,
            candidate_verified=candidate_verified,
        )
        return report

    @staticmethod
    def _first_regular_file(root: Path) -> Path:
        for path in sorted(root.rglob("*")):
            if path.is_file() and not path.is_symlink():
                return path
        raise IntegrationFailure("candidate inspection fixture has no regular file to tamper")

    def _assert_existing_lock(self, job_id: int) -> Path:
        lock_path = self._inspection_lock(job_id)
        entry = lock_path.lstat()
        if stat.S_ISLNK(entry.st_mode) or not stat.S_ISREG(entry.st_mode) or stat.S_IMODE(entry.st_mode) != 0o600:
            raise IntegrationFailure("prepared candidate inspection lock is not a private regular file")
        if entry.st_size != 0:
            raise IntegrationFailure("prepared candidate inspection lock is not empty")
        return lock_path

    def check_candidate_inspection_success(self) -> None:
        job = self._inspection_job()
        baseline = self._inspection_state(job["id"])
        for role in ("admin", "reader", "admin"):
            self._assert_observation(
                job,
                checks=dict(_PASSING_CHECKS),
                reason_codes=[],
                candidate_verified=True,
                role=role,
            )
            self._assert_inspection_state(job["id"], baseline)

        self.stop_server(expect_clean=True)
        self.start_server()
        job = self._inspection_job()
        self._assert_observation(job, checks=dict(_PASSING_CHECKS), reason_codes=[], candidate_verified=True)
        self._assert_inspection_state(job["id"], baseline)

    def check_candidate_inspection_source_drift(self) -> None:
        job = self._inspection_job()
        baseline = self._inspection_state(job["id"])
        source = self._first_regular_file(self.fixture_root)
        original = source.read_bytes()
        marker = b"\ninspection-source-drift\n"
        try:
            source.write_bytes(original + marker)
            self._assert_observation(
                job,
                checks=self._checks(**{**_PASSING_CHECKS, "source": "drifted"}),
                reason_codes=["candidate-source-drifted"],
            )
        finally:
            source.write_bytes(original)
        self._assert_inspection_state(job["id"], baseline)

    def check_candidate_inspection_base_change(self) -> None:
        job = self._inspection_job()
        baseline = self._inspection_state(job["id"])
        base = self.activation(PROJECT_ID, ROUTE_ID)
        source = self._first_regular_file(self.fixture_root)
        original = source.read_bytes()
        try:
            source.write_bytes(original + b"\ninspection-base-change\n")
            self.run_cli_json("build")
            source.write_bytes(original)
        finally:
            source.write_bytes(original)
            self.run_cli_json("build")
        if self.activation(PROJECT_ID, ROUTE_ID) != base:
            raise IntegrationFailure("plain CLI rebuild changed the database-managed preparation base")
        self._assert_observation(job, checks=dict(_PASSING_CHECKS), reason_codes=[], candidate_verified=True)

        status, response = self.post_preparation(PROJECT_ID, ROUTE_ID, base["snapshot_id"], "inspection-base-replacement")
        if status != 202:
            raise IntegrationFailure("controlled replacement candidate was not accepted")
        replacement = self.wait_for_terminal(response["result"]["job"]["id"], "prepared")
        status, response = self._post_activation(PROJECT_ID, ROUTE_ID, replacement["id"], base["revision"])
        if status != 200:
            raise IntegrationFailure("controlled replacement activation was not accepted")
        self._assert_observation(
            job,
            checks=self._checks(**{**_PASSING_CHECKS, "base": "changed"}),
            reason_codes=["preparation-base-changed"],
        )
        active = self.activation(PROJECT_ID, ROUTE_ID)
        status, response = self._post_activation(PROJECT_ID, ROUTE_ID, None, active["revision"])
        if status != 200:
            raise IntegrationFailure("controlled replacement deactivation was not accepted")
        self._release_candidate(replacement)
        inactive = self.activation(PROJECT_ID, ROUTE_ID)
        status, response = self.post_preparation(PROJECT_ID, ROUTE_ID, None, "inspection-base-followup")
        if status != 202:
            raise IntegrationFailure("post-deactivation inspection candidate was not accepted")
        self.wait_for_terminal(response["result"]["job"]["id"], "prepared")
        if self.activation(PROJECT_ID, ROUTE_ID) != inactive:
            raise IntegrationFailure("post-deactivation candidate preparation changed the active base")

    def check_candidate_inspection_artifact_integrity(self) -> None:
        job = self._inspection_job()
        artifact = self._inspection_artifact(job["id"])
        baseline = self._inspection_state(job["id"])
        hidden = artifact.with_name(f".{artifact.name}.inspection-hidden")
        if hidden.exists():
            raise IntegrationFailure("candidate inspection temporary artifact path already exists")
        try:
            artifact.rename(hidden)
            self._assert_observation(
                job,
                checks=self._checks(route="matched", writer="quiescent", artifact="missing"),
                reason_codes=["candidate-artifact-missing"],
            )
        finally:
            if hidden.exists():
                hidden.rename(artifact)
        self._assert_inspection_state(job["id"], baseline)

        target = artifact / "docs" / "kb" / "generated" / "manifest.json"
        target_entry = target.lstat()
        if stat.S_ISLNK(target_entry.st_mode) or not stat.S_ISREG(target_entry.st_mode):
            raise IntegrationFailure("prepared candidate has no regular generated manifest to corrupt")
        original = target.read_bytes()
        extra = artifact / ".inspection-unknown-entry"
        link = artifact / ".inspection-symlink"
        invalid_checks = self._checks(route="matched", writer="quiescent", artifact="invalid")
        try:
            target.write_bytes(original + b"\ninspection-corruption\n")
            self._assert_observation(job, checks=invalid_checks, reason_codes=["candidate-artifact-invalid"])
        finally:
            target.write_bytes(original)
        self._assert_inspection_state(job["id"], baseline)

        try:
            descriptor = os.open(extra, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "wb") as handle:
                handle.write(b"inspection-extra\n")
            self._assert_observation(job, checks=invalid_checks, reason_codes=["candidate-artifact-invalid"])
        finally:
            extra.unlink(missing_ok=True)
        self._assert_inspection_state(job["id"], baseline)

        try:
            link.symlink_to(target.name)
            self._assert_observation(job, checks=invalid_checks, reason_codes=["candidate-artifact-invalid"])
        finally:
            link.unlink(missing_ok=True)
        self._assert_inspection_state(job["id"], baseline)

        saved = artifact.with_name(f".{artifact.name}.inspection-saved-manifest")
        if saved.exists():
            raise IntegrationFailure("candidate inspection saved manifest already exists")
        try:
            target.rename(saved)
            target.symlink_to(saved)
            self._assert_observation(job, checks=invalid_checks, reason_codes=["candidate-artifact-invalid"])
            target.unlink()
            os.link(saved, target)
            self._assert_observation(job, checks=invalid_checks, reason_codes=["candidate-artifact-invalid"])
            target.unlink()
            os.mkfifo(target, 0o600)
            self._assert_observation(job, checks=invalid_checks, reason_codes=["candidate-artifact-invalid"])
        finally:
            target.unlink(missing_ok=True)
            if saved.exists():
                saved.rename(target)
        self._assert_inspection_state(job["id"], baseline)

    def check_candidate_inspection_safety_and_route(self) -> None:
        job = self._inspection_job()
        baseline = self._inspection_state(job["id"])
        source = self.fixture_root / "docs" / "inspection-credential.md"
        marker = secrets.token_hex(24)
        try:
            with source.open("x", encoding="utf-8") as handle:
                handle.write("# Credential\n\napi_key = " + marker + "\n")
            before_inspection = self._inspection_state(job["id"])
            report = self._assert_observation(job,
                checks={**_PASSING_CHECKS, "doctor": "failed", "source": "drifted"},
                reason_codes=["candidate-doctor-failed", "candidate-source-drifted"])
            self._assert_safe_report(report, (marker,))
            self._assert_inspection_state(job["id"], before_inspection)
        finally:
            source.unlink(missing_ok=True)
        fingerprint = self.scalar("SELECT route_fingerprint FROM shin_server.preparations WHERE id = " + str(job["id"]))
        replacement = "0" * 64 if fingerprint != "0" * 64 else "1" * 64
        try:
            self.psql("UPDATE shin_server.preparations SET route_fingerprint = " + self._sql_text(replacement) +
                      " WHERE id = " + str(job["id"]))
            self._assert_observation(job, checks=self._checks(route="changed"), reason_codes=["preparation-route-changed"])
        finally:
            self.psql("UPDATE shin_server.preparations SET route_fingerprint = " + self._sql_text(fingerprint) +
                      " WHERE id = " + str(job["id"]))
        self._assert_inspection_state(job["id"], baseline)
        hidden_root = self.fixture_root.with_name(self.fixture_root.name + "-inspection-hidden")
        if hidden_root.exists():
            raise IntegrationFailure("candidate inspection hidden project root already exists")
        try:
            self.fixture_root.rename(hidden_root)
            self._assert_observation(job, checks=self._checks(route="unavailable"), reason_codes=["route-input-unavailable"])
        finally:
            if hidden_root.exists():
                hidden_root.rename(self.fixture_root)
        self._assert_inspection_state(job["id"], baseline)

    def check_candidate_inspection_ledger_mismatch(self) -> None:
        job = self._inspection_job()
        baseline = self._inspection_state(job["id"])
        replacement = "fnv1a64:0000000000000000"
        if replacement == job["artifact_snapshot_id"]:
            replacement = "fnv1a64:0000000000000001"
        try:
            self.psql(
                "UPDATE shin_server.preparations SET artifact_snapshot_id = " + self._sql_text(replacement) +
                " WHERE id = " + str(job["id"])
            )
            tampered = self.assert_public_job(self.get_job(job["id"]), project_id=PROJECT_ID, route_id=ROUTE_ID)
            self._assert_observation(
                tampered,
                checks=self._checks(**{**_PASSING_CHECKS, "ledger": "mismatch"}),
                reason_codes=["candidate-ledger-mismatch"],
            )
        finally:
            self.psql(
                "UPDATE shin_server.preparations SET artifact_snapshot_id = " + self._sql_text(job["artifact_snapshot_id"]) +
                " WHERE id = " + str(job["id"])
            )
        self._assert_inspection_state(job["id"], baseline)

    def check_candidate_inspection_lock_safety(self) -> None:
        job = self._inspection_job()
        baseline = self._inspection_state(job["id"])
        lock_path = self._assert_existing_lock(job["id"])
        descriptor: int | None = None
        try:
            descriptor = os.open(lock_path, os.O_RDONLY | os.O_NOFOLLOW)
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            started = time.perf_counter_ns()
            self._assert_observation(
                job,
                checks=self._checks(route="matched", writer="busy"),
                reason_codes=["candidate-artifact-busy"],
            )
            if (time.perf_counter_ns() - started) / 1_000_000 > 1000:
                raise IntegrationFailure("busy candidate inspection did not return immediately")
        finally:
            if descriptor is not None:
                fcntl.flock(descriptor, fcntl.LOCK_UN)
                os.close(descriptor)
        self._assert_inspection_state(job["id"], baseline)

        original_mode = stat.S_IMODE(lock_path.lstat().st_mode)
        try:
            os.chmod(lock_path, 0o644)
            self._assert_observation(
                job,
                checks=self._checks(route="matched", writer="unavailable"),
                reason_codes=["candidate-lock-unsafe"],
            )
        finally:
            os.chmod(lock_path, original_mode)
        self._assert_inspection_state(job["id"], baseline)

        hidden = lock_path.with_name(f".{lock_path.name}.inspection-hidden")
        if hidden.exists():
            raise IntegrationFailure("candidate inspection temporary lock path already exists")
        try:
            lock_path.rename(hidden)
            self._assert_observation(
                job,
                checks=self._checks(route="matched", writer="unavailable"),
                reason_codes=["candidate-lock-missing"],
            )
            if lock_path.exists():
                raise IntegrationFailure("candidate inspection recreated a missing writer lock")
        finally:
            if hidden.exists():
                hidden.rename(lock_path)
        self._assert_inspection_state(job["id"], baseline)

    def check_candidate_inspection_nonprepared_states(self) -> None:
        job = self._inspection_job()
        baseline = self._inspection_state(job["id"])
        worker_id = self.scalar("SELECT worker_id FROM shin_server.preparations WHERE id = " + str(job["id"]))
        if not worker_id:
            raise IntegrationFailure("prepared candidate has no worker identity to restore after state checks")
        self.stop_server(expect_clean=True)
        config = json.loads(self.runtime_config_path.read_text(encoding="utf-8"))
        disabled = json.loads(json.dumps(config))
        disabled["preparations"]["enabled"] = False
        self.replace_private_json(self.runtime_config_path, disabled)
        try:
            self.start_server()
            states = {
                "queued": "state = 'queued', cancel_requested = FALSE, worker_id = NULL, lease_until = NULL, "
                "artifact_snapshot_id = NULL, artifact_bytes = NULL, reason_code = NULL",
                "running": "state = 'running', cancel_requested = FALSE, lease_until = clock_timestamp() + interval '60 seconds', "
                "artifact_snapshot_id = NULL, artifact_bytes = NULL, reason_code = NULL",
                "failed": "state = 'failed', cancel_requested = FALSE, lease_until = NULL, artifact_snapshot_id = NULL, "
                "artifact_bytes = NULL, reason_code = 'inspection-test-failed'",
                "cancelled": "state = 'cancelled', cancel_requested = TRUE, lease_until = NULL, artifact_snapshot_id = NULL, "
                "artifact_bytes = NULL, reason_code = 'inspection-test-cancelled'",
            }
            for state, assignment in states.items():
                self.psql("UPDATE shin_server.preparations SET " + assignment + " WHERE id = " + str(job["id"]))
                current = self.assert_public_job(self.get_job(job["id"]), project_id=PROJECT_ID, route_id=ROUTE_ID)
                if current["state"] != state:
                    raise IntegrationFailure("controlled non-prepared candidate state was not retained")
                unchanged = self._inspection_state(job["id"])
                self._assert_observation(
                    current,
                    checks=self._checks(),
                    reason_codes=["candidate-not-prepared"],
                )
                self._assert_inspection_state(job["id"], unchanged)
                self.psql(
                    "UPDATE shin_server.preparations SET state = 'prepared', cancel_requested = FALSE, worker_id = " +
                    self._sql_text(worker_id) + ", lease_until = NULL, artifact_snapshot_id = " +
                    self._sql_text(job["artifact_snapshot_id"]) + ", artifact_bytes = " + str(job["artifact_bytes"]) +
                    ", reason_code = NULL WHERE id = " + str(job["id"])
                )
        finally:
            self.psql(
                "UPDATE shin_server.preparations SET state = 'prepared', cancel_requested = FALSE, worker_id = " +
                self._sql_text(worker_id) + ", lease_until = NULL, artifact_snapshot_id = " +
                self._sql_text(job["artifact_snapshot_id"]) + ", artifact_bytes = " + str(job["artifact_bytes"]) +
                ", reason_code = NULL WHERE id = " + str(job["id"])
            )
            self.stop_server(expect_clean=True)
            self.replace_private_json(self.runtime_config_path, config)
            self.start_server()
        self._assert_inspection_state(job["id"], baseline)

    def check_candidate_inspection_scope_and_query_guards(self) -> None:
        job = self._inspection_job()
        artifact = self._inspection_artifact(job["id"])
        baseline = self._inspection_state(job["id"])
        self.expect_status(404, "GET", "/api/v1/preparations/999999999/inspection", headers=self.auth_headers("admin"))
        self._assert_inspection_state(job["id"], baseline)
        hidden = artifact.with_name(f".{artifact.name}.inspection-scope-hidden")
        if hidden.exists():
            raise IntegrationFailure("candidate inspection scope temporary artifact path already exists")
        try:
            artifact.rename(hidden)
            self.psql("UPDATE shin_server.preparations SET project_id = 'other' WHERE id = " + str(job["id"]))
            self.expect_status(404, "GET", f"/api/v1/preparations/{job['id']}/inspection", headers=self.auth_headers("reader"))
            if artifact.exists():
                raise IntegrationFailure("denied candidate inspection recreated or touched the unavailable artifact")
        finally:
            self.psql("UPDATE shin_server.preparations SET project_id = " + self._sql_text(PROJECT_ID) +
                      " WHERE id = " + str(job["id"]))
            if hidden.exists():
                hidden.rename(artifact)
        self._assert_inspection_state(job["id"], baseline)

        for query in ("?unexpected=1", "?unexpected=1&unexpected=2", "?%75nexpected=1"):
            self.expect_status(
                400,
                "GET",
                f"/api/v1/preparations/{job['id']}/inspection{query}",
                headers=self.auth_headers("admin"),
            )
        self._assert_inspection_state(job["id"], baseline)

    def check_candidate_inspection_global_admission(self) -> None:
        job = self._inspection_job()
        baseline = self._inspection_state(job["id"])
        results: list[tuple[int, dict[str, Any]]] = []
        failures: list[BaseException] = []

        def inspect() -> None:
            try:
                result = self.request(
                    "GET",
                    f"/api/v1/preparations/{job['id']}/inspection",
                    headers=self.auth_headers("admin"),
                )
                results.append(result)
            except BaseException as error:
                failures.append(error)

        publication = os.open(self._inspection_artifact(job["id"]) / "docs/kb/.shin-context.lock", os.O_RDONLY | os.O_NOFOLLOW)
        lifetime = os.open(self._inspection_lock(job["id"]), os.O_RDONLY | os.O_NOFOLLOW)
        thread = threading.Thread(target=inspect)
        try:
            fcntl.flock(publication, fcntl.LOCK_EX)
            thread.start()
            deadline = time.monotonic() + 5
            admitted = False
            while time.monotonic() < deadline:
                try:
                    fcntl.flock(lifetime, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    admitted = True
                    break
                else:
                    fcntl.flock(lifetime, fcntl.LOCK_UN)
                time.sleep(0.02)
            if not admitted:
                raise IntegrationFailure("first inspection did not reach its held artifact boundary")
            results.append(self.request("GET", f"/api/v1/preparations/{job['id']}/inspection",
                                        headers=self.auth_headers("admin")))
        finally:
            fcntl.flock(publication, fcntl.LOCK_UN)
            os.close(publication)
            os.close(lifetime)
            if thread.ident is not None:
                thread.join(timeout=10.0)
        if thread.is_alive() or failures:
            raise IntegrationFailure("concurrent candidate inspection requests did not complete cleanly")
        statuses = [status for status, _ in results]
        if 200 not in statuses or 503 not in statuses:
            raise IntegrationFailure("candidate inspection did not enforce one global inspection admission slot")
        for status, envelope in results:
            if status == 200:
                report = envelope.get("result")
                if not isinstance(report, dict):
                    raise IntegrationFailure("admitted candidate inspection returned no report")
                self._assert_report(report, job, checks=dict(_PASSING_CHECKS), reason_codes=[], candidate_verified=True)
            elif status != 503 or envelope.get("error", {}).get("code") != "preparation-inspection-busy":
                raise IntegrationFailure("concurrent candidate inspection returned an unsafe admission response")
        self._assert_inspection_state(job["id"], baseline)

    def check_candidate_inspection_baseline(self) -> None:
        job = self._inspection_job()
        baseline = self._inspection_state(job["id"])
        durations: list[float] = []
        response_bytes: list[float] = []
        for _ in range(20):
            started = time.perf_counter_ns()
            envelope, report = self._get_inspection(job["id"])
            durations.append((time.perf_counter_ns() - started) / 1_000_000)
            response_bytes.append(float(len(json.dumps(envelope, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8"))))
            self._assert_report(report, job, checks=dict(_PASSING_CHECKS), reason_codes=[], candidate_verified=True)
            self._assert_inspection_state(job["id"], baseline)
        self.inspection_measurement = {
            "status": "completed",
            "samples": len(durations),
            "total_http_p50_ms": self.percentile_ms(durations, 50),
            "total_http_p95_ms": self.percentile_ms(durations, 95),
            "response_bytes_p50": self.percentile_ms(response_bytes, 50),
            "response_bytes_p95": self.percentile_ms(response_bytes, 95),
            "scope": "Twenty sequential unchanged prepared-candidate HTTP observations; total HTTP time and compact response bytes, not throughput or soak.",
        }
