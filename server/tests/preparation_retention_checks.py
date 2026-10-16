#!/usr/bin/env python3
"""HTTP retention checks mixed into the preparation integration suite.

This module deliberately does not import ``preparation_integration_test``.
The owning suite supplies the HTTP, PostgreSQL, lock and fixture helpers.
"""

from __future__ import annotations

import fcntl
import http.client
import json
import os
from pathlib import Path
import re
import stat
import threading
import time
from typing import Any

from integration_test import HOST, PORT, IntegrationFailure
from retrieval_integration_test import PROJECT_ID, ROUTE_ID


_RETENTION_FIELDS = {
    "schema_version",
    "job_id",
    "state",
    "plan_id",
    "can_apply",
    "reservation_held",
    "artifact_bytes",
    "entries",
    "reason_codes",
    "mutation_performed",
    "automatic_execution",
}
_PLAN_ID = re.compile(r"[0-9a-f]{64}\Z")


class PreparationRetentionChecks:
    """Explicit, durable candidate-artifact retention coverage."""

    def _retention_get(self, job_id: int, *, role: str = "admin", expected_status: int = 200) -> dict[str, Any]:
        envelope = self.expect_status(
            expected_status,
            "GET",
            f"/api/v1/preparations/{job_id}/retention",
            headers=self.auth_headers(role),
        )
        report = envelope.get("result")
        if expected_status == 200:
            return self._assert_retention_report(report, job_id)
        return report

    def _retention_post(
        self,
        job_id: int,
        plan_id: str,
        *,
        role: str = "admin",
        target: str | None = None,
        payload: Any | None = None,
    ) -> tuple[int, dict[str, Any]]:
        body = {"expected_plan_id": plan_id} if payload is None else payload
        return self.request(
            "POST",
            target or f"/api/v1/preparations/{job_id}/retention",
            headers={**self.auth_headers(role), "Content-Type": "application/json"},
            body=json.dumps(body, separators=(",", ":")).encode("utf-8"),
        )

    @staticmethod
    def _assert_error(status: int, response: dict[str, Any], expected_status: int, code: str) -> None:
        if status != expected_status or response.get("error", {}).get("code") != code:
            raise IntegrationFailure(
                f"retention expected HTTP {expected_status}/{code}, got HTTP {status}/"
                f"{response.get('error', {}).get('code')}"
            )

    def _assert_retention_report(self, report: Any, job_id: int) -> dict[str, Any]:
        if not isinstance(report, dict) or set(report) != _RETENTION_FIELDS:
            raise IntegrationFailure("retention response does not match the exact public contract")
        if report["schema_version"] != "shin.candidate-retention.v1" or report["job_id"] != job_id:
            raise IntegrationFailure("retention response has an invalid schema identity or job identity")
        if report["state"] not in {"retained", "intent", "released"}:
            raise IntegrationFailure("retention response has an invalid retention state")
        if report["plan_id"] is not None and (not isinstance(report["plan_id"], str) or not _PLAN_ID.fullmatch(report["plan_id"])):
            raise IntegrationFailure("retention response has an invalid plan identity")
        for name in ("can_apply", "reservation_held", "mutation_performed", "automatic_execution"):
            if type(report[name]) is not bool:
                raise IntegrationFailure("retention response has an invalid boolean field")
        for name in ("artifact_bytes", "entries"):
            if report[name] is not None and (type(report[name]) is not int or report[name] < 0):
                raise IntegrationFailure("retention response has an invalid optional count")
        if not isinstance(report["reason_codes"], list) or not all(isinstance(code, str) for code in report["reason_codes"]):
            raise IntegrationFailure("retention response has invalid safe reason codes")
        if report["automatic_execution"] is not False:
            raise IntegrationFailure("retention response claimed automatic cleanup")
        rendered = json.dumps(report, sort_keys=True, separators=(",", ":"))
        private_values = [str(self.fixture_root), str(self.out_dir), self.admin_token, self.reader_token]
        if self.preparation_root is not None:
            private_values.append(str(self.preparation_root))
        if any(value and value in rendered for value in private_values):
            raise IntegrationFailure("retention response exposed a path or credential")
        return report

    def _retention_job(self) -> dict[str, Any]:
        value = self.scalar(
            "SELECT id FROM shin_server.preparations "
            "WHERE project_id = 'visible' AND route_id = 'visible-retrieval' AND state = 'prepared' "
            "AND NOT EXISTS (SELECT 1 FROM shin_server.preparation_retention AS retention "
            "WHERE retention.preparation_id = preparations.id AND retention.state = 'released') "
            "ORDER BY id ASC LIMIT 1"
        )
        if not value.isdigit() or int(value) <= 0:
            raise IntegrationFailure("retention requires an unreleased prepared candidate")
        return self.assert_public_job(self.get_job(int(value)), project_id=PROJECT_ID, route_id=ROUTE_ID)

    def _retention_state(self, job_id: int) -> dict[str, str]:
        artifact = self._inspection_artifact(job_id)
        lock = self._inspection_lock(job_id)
        return {
            "artifact": self._tree_fingerprint(artifact),
            "lock": self._tree_fingerprint(lock),
            "job": self.scalar(
                "SELECT row_to_json(preparation)::text FROM "
                "(SELECT * FROM shin_server.preparations WHERE id = " + str(job_id) + ") AS preparation"
            ),
            "retention": self.scalar(
                "SELECT COALESCE((SELECT row_to_json(retention)::text FROM "
                "(SELECT * FROM shin_server.preparation_retention WHERE preparation_id = " + str(job_id) + ") AS retention), 'null')"
            ),
            "audit": self.scalar(
                "SELECT COALESCE(jsonb_agg(event ORDER BY id)::text, '[]') FROM "
                "(SELECT * FROM shin_server.audit_events WHERE preparation_id = " + str(job_id) + ") AS event"
            ),
        }

    def _retention_row_count(self, job_id: int) -> int:
        return int(self.scalar("SELECT count(*) FROM shin_server.preparation_retention WHERE preparation_id = " + str(job_id)))

    def _retention_audit_count(self, job_id: int, action: str) -> int:
        return int(self.scalar(
            "SELECT count(*) FROM shin_server.audit_events WHERE preparation_id = " + str(job_id) +
            " AND action = " + self._sql_text(action)
        ))

    def _assert_released_inspection(self, job: dict[str, Any]) -> None:
        _, report = self._get_inspection(job["id"])
        self._assert_report(
            report,
            job,
            checks=self._checks(),
            reason_codes=["candidate-released"],
            candidate_verified=False,
        )

    def _assert_no_retention_row(self, job_id: int) -> None:
        if self._retention_row_count(job_id) != 0:
            raise IntegrationFailure("failed retention operation left a durable intent")

    def _open_existing_lock(self, job_id: int) -> int:
        path = self._assert_existing_lock(job_id)
        descriptor: int | None = None
        try:
            descriptor = os.open(path, os.O_RDWR | os.O_NOFOLLOW)
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return descriptor
        except OSError as error:
            if descriptor is not None:
                os.close(descriptor)
            raise IntegrationFailure("could not hold the existing candidate lifetime lock") from error

    @staticmethod
    def _close_lock(descriptor: int | None) -> None:
        if descriptor is None:
            return
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)

    def _insert_controlled_queued_preparation(self, label: str) -> int:
        if self.preparation_root is None:
            raise IntegrationFailure("controlled retention fixture has no private preparation root")
        route_fingerprint = self.scalar(
            "SELECT route_fingerprint FROM shin_server.preparations "
            "WHERE project_id='visible' AND route_id='visible-retrieval' ORDER BY id DESC LIMIT 1"
        )
        namespace = self.scalar(
            "SELECT artifact_namespace FROM shin_server.preparations "
            "WHERE artifact_namespace IS NOT NULL ORDER BY id DESC LIMIT 1"
        )
        if not re.fullmatch(r"[0-9a-f]{64}", route_fingerprint) or not re.fullmatch(r"[0-9a-f]{64}", namespace):
            raise IntegrationFailure("controlled retention fixture could not recover its private route binding")
        active = self.activation(PROJECT_ID, ROUTE_ID)
        snapshot = active["snapshot_id"] or ""
        artifact_key = os.urandom(32).hex()
        value = self.scalar(
            "INSERT INTO shin_server.preparations "
            "(project_id,route_id,principal_id,idempotency_key,request_hash,route_fingerprint,expected_snapshot_id,"
            "state,reserved_artifact_bytes,artifact_namespace,artifact_key,expected_route_revision) VALUES ("
            "'visible','visible-retrieval','dev-admin'," + self._sql_text("retention-controlled-" + label) + ","
            "repeat('a',64)," + self._sql_text(route_fingerprint) + "," + self._sql_text(snapshot) + ","
            "'queued',65536," + self._sql_text(namespace) + "," + self._sql_text(artifact_key) + "," +
            str(active["revision"]) + ") RETURNING id"
        )
        if not value.isdigit() or int(value) <= 0:
            raise IntegrationFailure("controlled retention fixture did not create a queued preparation")
        return int(value)

    def _set_preparation_worker_enabled(self, enabled: bool) -> None:
        if self.server is not None:
            raise IntegrationFailure("preparation worker configuration may change only while the owned server is stopped")
        config = json.loads(self.runtime_config_path.read_text(encoding="utf-8"))
        config["preparations"]["enabled"] = enabled
        self.replace_private_json(self.runtime_config_path, config)

    def _retention_plan(self, job_id: int) -> dict[str, Any]:
        report = self._retention_get(job_id)
        if report["state"] != "retained" or report["plan_id"] is None or report["can_apply"] is not True:
            raise IntegrationFailure("terminal managed candidate was not eligible for explicit retention")
        if report["reason_codes"] or report["mutation_performed"]:
            raise IntegrationFailure("read-only retention planning reported a mutation or unexpected failure")
        return report

    def _release_candidate(self, job: dict[str, Any]) -> dict[str, Any]:
        plan = self._retention_plan(job["id"])
        status, response = self._retention_post(job["id"], plan["plan_id"])
        if status != 200:
            raise IntegrationFailure("eligible retention apply did not succeed")
        report = self._assert_retention_report(response.get("result"), job["id"])
        if report["state"] != "released" or report["mutation_performed"] is not True or report["reservation_held"] is not False:
            raise IntegrationFailure("successful retention apply did not release its reservation")
        return plan

    def check_candidate_retention_clean_prepared_at_capacity(self) -> None:
        self.retention_released_baseline = int(self.scalar(
            "SELECT count(*) FROM shin_server.preparation_retention WHERE state = 'released'"
        ))
        configured_limit = json.loads(self.runtime_config_path.read_text(encoding="utf-8"))["preparations"].get("max_retained_jobs")
        if type(configured_limit) is not int or configured_limit < 1:
            raise IntegrationFailure("retention fixture has no valid configured retained-job limit")

        def retained_count() -> int:
            return int(self.scalar(
                "SELECT count(*) FROM shin_server.preparations AS preparation WHERE NOT EXISTS "
                "(SELECT 1 FROM shin_server.preparation_retention AS retention "
                "WHERE retention.preparation_id = preparation.id AND retention.state = 'released')"
            ))

        retained = retained_count()
        if retained > configured_limit:
            raise IntegrationFailure("retention fixture exceeded its configured retained-job limit before planning")
        base = self.activation(PROJECT_ID, ROUTE_ID)
        for index in range(configured_limit - retained):
            status, response = self.post_preparation(
                PROJECT_ID,
                ROUTE_ID,
                base["snapshot_id"],
                f"retention-capacity-fill-{index}",
            )
            if status != 202:
                raise IntegrationFailure("retention capacity fixture could not fill its remaining durable slot")
            self.wait_for_terminal(response["result"]["job"]["id"], "prepared")
        if retained_count() != configured_limit:
            raise IntegrationFailure("retention capacity fixture did not fill exactly the configured durable slots")
        status, response = self.post_preparation(
            PROJECT_ID,
            ROUTE_ID,
            base["snapshot_id"],
            "retention-capacity-overflow",
        )
        self._assert_error(status, response, 409, "preparation-capacity")

        retained = int(self.scalar(
            "SELECT count(*) FROM shin_server.preparations AS preparation WHERE NOT EXISTS "
            "(SELECT 1 FROM shin_server.preparation_retention AS retention "
            "WHERE retention.preparation_id = preparation.id AND retention.state = 'released')"
        ))
        if retained != configured_limit:
            raise IntegrationFailure("retention checks must begin with the configured full durable capacity")
        job = self._retention_job()
        before_public = self.get_job(job["id"])
        before = self._retention_state(job["id"])
        admin = self._retention_plan(job["id"])
        if self._retention_state(job["id"]) != before:
            raise IntegrationFailure("admin retention GET changed private state, history, files or lifetime lock")
        reader = self._retention_get(job["id"], role="reader")
        if reader["plan_id"] != admin["plan_id"] or reader["can_apply"] is not False or reader["reason_codes"]:
            raise IntegrationFailure("authorized reader did not receive a read-only retention plan")
        if self._retention_state(job["id"]) != before:
            raise IntegrationFailure("reader retention GET changed private state, history, files or lifetime lock")

        self.expect_status(404, "GET", "/api/v1/preparations/999999999/retention", headers=self.auth_headers("admin"))
        denied = self.scalar(
            "SELECT id FROM shin_server.preparations WHERE project_id = 'other' ORDER BY id ASC LIMIT 1"
        )
        if not denied.isdigit():
            raise IntegrationFailure("retention scope check has no other-project preparation")
        self.expect_status(404, "GET", f"/api/v1/preparations/{denied}/retention", headers=self.auth_headers("reader"))
        if self._retention_state(job["id"]) != before:
            raise IntegrationFailure("missing or denied retention GET touched the visible candidate")

        wrong_plan = "0" * 64 if admin["plan_id"] != "0" * 64 else "1" * 64
        status, response = self._retention_post(job["id"], wrong_plan)
        self._assert_error(status, response, 409, "retention-plan-changed")
        self._assert_no_retention_row(job["id"])
        if self._retention_state(job["id"]) != before:
            raise IntegrationFailure("wrong retained plan token wrote intent or deleted a candidate")

        status, response = self._retention_post(job["id"], admin["plan_id"])
        if status != 200:
            raise IntegrationFailure("retention apply did not free a full-capacity candidate")
        released = self._assert_retention_report(response.get("result"), job["id"])
        if released["state"] != "released" or released["mutation_performed"] is not True or released["reservation_held"] is not False:
            raise IntegrationFailure("retention apply did not report a committed release")
        if self._inspection_artifact(job["id"]).exists():
            raise IntegrationFailure("released retention candidate still has an artifact directory")
        if self.get_job(job["id"]) != before_public:
            raise IntegrationFailure("retention altered the historical public preparation job")
        self._assert_released_inspection(before_public)
        if self._retention_audit_count(job["id"], "preparation.retention-requested") != 1 or \
                self._retention_audit_count(job["id"], "preparation.retention-released") != 1:
            raise IntegrationFailure("retention did not write exactly one request and release audit event")

        replay_before = self._retention_state(job["id"])
        status, response = self._retention_post(job["id"], admin["plan_id"])
        if status != 200:
            raise IntegrationFailure("released retention plan did not replay")
        replay = self._assert_retention_report(response.get("result"), job["id"])
        if replay["mutation_performed"] is not False or replay["state"] != "released":
            raise IntegrationFailure("released retention replay reported a filesystem mutation")
        if self._retention_state(job["id"]) != replay_before:
            raise IntegrationFailure("released retention replay changed files, history or audit state")
        conflict = "0" * 64 if admin["plan_id"] != "0" * 64 else "1" * 64
        status, response = self._retention_post(job["id"], conflict)
        self._assert_error(status, response, 409, "retention-plan-conflict")
        if self._retention_state(job["id"]) != replay_before:
            raise IntegrationFailure("conflicting released plan token changed retained history")

        status, response = self.post_preparation(PROJECT_ID, ROUTE_ID, self.current_snapshot_id(), "prepare-visible")
        replayed = response.get("result", {})
        if status != 200 or replayed.get("replayed") is not True or replayed.get("job", {}).get("id") != job["id"]:
            raise IntegrationFailure("released candidate idempotency replay regenerated its historical preparation")
        if self._inspection_artifact(job["id"]).exists() or self._retention_audit_count(job["id"], "preparation.retention-released") != 1:
            raise IntegrationFailure("preparation replay regenerated an artifact or retention audit")

    def check_candidate_retention_plan_change_and_tree_safety(self) -> None:
        job = self._retention_job()
        artifact = self._inspection_artifact(job["id"])
        plan = self._retention_plan(job["id"])
        known = self._first_regular_file(artifact)
        original = known.read_bytes()
        try:
            known.write_bytes(original + b"\nretention-plan-change\n")
            status, response = self._retention_post(job["id"], plan["plan_id"])
            self._assert_error(status, response, 409, "retention-plan-changed")
            self._assert_no_retention_row(job["id"])
            if not artifact.exists():
                raise IntegrationFailure("stale retention plan deleted a modified candidate")
        finally:
            known.write_bytes(original)

        source = self._first_regular_file(self.fixture_root)
        source_original = source.read_bytes()
        route_fingerprint = self.scalar("SELECT route_fingerprint FROM shin_server.preparations WHERE id = " + str(job["id"]))
        try:
            source.write_bytes(source_original + b"\nretention-source-only-drift\n")
            source_plan = self._retention_plan(job["id"])
            if source_plan["plan_id"] != plan["plan_id"]:
                raise IntegrationFailure("source-only drift changed the managed retention plan")
            self.psql(
                "UPDATE shin_server.preparations SET route_fingerprint = repeat('a', 64) WHERE id = " + str(job["id"])
            )
            route_plan = self._retention_plan(job["id"])
            if route_plan["plan_id"] != plan["plan_id"]:
                raise IntegrationFailure("route fingerprint drift changed the namespace-bound retention plan")
        finally:
            self.psql(
                "UPDATE shin_server.preparations SET route_fingerprint = " + self._sql_text(route_fingerprint) +
                " WHERE id = " + str(job["id"])
            )
            source.write_bytes(source_original)

        def refuse_mutation(name: str, mutate: Any, restore: Any) -> None:
            baseline = self._retention_state(job["id"])
            try:
                mutate()
                report = self._retention_get(job["id"])
                if report["can_apply"] is not False or "retention-tree-unsafe" not in report["reason_codes"]:
                    raise IntegrationFailure(name + " did not produce a safe retention refusal")
                status, response = self._retention_post(job["id"], plan["plan_id"])
                self._assert_error(status, response, 409, "retention-tree-unsafe")
                self._assert_no_retention_row(job["id"])
                if not artifact.exists():
                    raise IntegrationFailure(name + " caused broad candidate deletion")
            finally:
                restore()
            if self._retention_state(job["id"]) != baseline:
                raise IntegrationFailure(name + " did not restore the owned fixture exactly")

        unknown = artifact / ".retention-unknown-entry"
        refuse_mutation(
            "unknown candidate entry",
            lambda: unknown.write_bytes(b"owned-retention-test\n"),
            lambda: unknown.unlink(missing_ok=True),
        )
        replacement = artifact.with_name("." + artifact.name + ".retention-known-original")

        def replace_with_symlink() -> None:
            known.rename(replacement)
            known.symlink_to(replacement)

        def restore_symlink() -> None:
            known.unlink(missing_ok=True)
            if replacement.exists():
                replacement.rename(known)

        refuse_mutation("known candidate symbolic link", replace_with_symlink, restore_symlink)

        def replace_with_fifo() -> None:
            known.rename(replacement)
            os.mkfifo(known, 0o600)

        def restore_fifo() -> None:
            known.unlink(missing_ok=True)
            if replacement.exists():
                replacement.rename(known)

        refuse_mutation("known candidate FIFO", replace_with_fifo, restore_fifo)

    def check_candidate_retention_busy_writer(self) -> None:
        job = self._retention_job()
        descriptor: int | None = None
        try:
            descriptor = self._open_existing_lock(job["id"])
            report = self._retention_get(job["id"])
            if report["can_apply"] is not False or "candidate-artifact-busy" not in report["reason_codes"]:
                raise IntegrationFailure("shared retention planning did not expose an active writer safely")
            status, response = self._retention_post(job["id"], "0" * 64)
            self._assert_error(status, response, 409, "candidate-artifact-busy")
        finally:
            self._close_lock(descriptor)
        self._release_candidate(job)
        released = int(self.scalar(
            "SELECT count(*) FROM shin_server.preparation_retention WHERE state = 'released'"
        ))
        if released != self.retention_released_baseline + 2:
            raise IntegrationFailure("busy-writer cleanup did not free one additional retained capacity slot")

    def check_candidate_retention_active_states(self) -> None:
        base = self.activation(PROJECT_ID, ROUTE_ID)
        candidate = self._prepare_activation_candidate(base["snapshot_id"], base["revision"], "retention-active")
        status, response = self._post_activation(PROJECT_ID, ROUTE_ID, candidate["id"], base["revision"])
        if status != 200:
            raise IntegrationFailure("retention active-candidate fixture was not explicitly activated")
        active = self.activation(PROJECT_ID, ROUTE_ID)
        report = self._retention_get(candidate["id"])
        if report["can_apply"] is not False or report["reason_codes"] != ["candidate-active"]:
            raise IntegrationFailure("active candidate retention GET was not rejected before filesystem planning")
        status, response = self._retention_post(candidate["id"], "0" * 64)
        self._assert_error(status, response, 409, "candidate-active")
        if self.activation(PROJECT_ID, ROUTE_ID) != active:
            raise IntegrationFailure("active candidate retention POST changed the managed route")
        status, response = self._post_activation(PROJECT_ID, ROUTE_ID, None, active["revision"])
        if status != 200:
            raise IntegrationFailure("retention active-candidate fixture could not be deactivated")
        self._release_candidate(candidate)

    def check_candidate_retention_cancelled_and_expired_fences(self) -> None:
        self.stop_server(expect_clean=True)
        self._set_preparation_worker_enabled(False)
        self.start_server()
        descriptor: int | None = None
        expired_id: int | None = None
        try:
            active = self.activation(PROJECT_ID, ROUTE_ID)
            status, response = self.post_preparation(
                PROJECT_ID,
                ROUTE_ID,
                active["snapshot_id"],
                "retention-worker-disabled",
            )
            self._assert_error(status, response, 503, "preparations-disabled")
            queued_id = self._insert_controlled_queued_preparation("queued-cancel")
            queued = self.get_job(queued_id)
            if queued["state"] != "queued" or self._inspection_artifact(queued_id).exists() or self._inspection_lock(queued_id).exists():
                raise IntegrationFailure("controlled queued fixture did not remain an unclaimed private candidate")
            cancelled = self.expect_status(
                200,
                "POST",
                f"/api/v1/preparations/{queued_id}/cancel",
                headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
                body=b"{}",
            )["result"]
            if cancelled["state"] != "cancelled" or cancelled["reason_code"] != "cancelled":
                raise IntegrationFailure("queued cancellation did not become a durable terminal preparation")
            if self._inspection_artifact(queued_id).exists() or self._inspection_lock(queued_id).exists():
                raise IntegrationFailure("queued cancellation created candidate filesystem state before retention")
            queued_plan = self._retention_plan(queued_id)
            status, response = self._retention_post(queued_id, queued_plan["plan_id"])
            if status != 200 or response.get("result", {}).get("state") != "released":
                raise IntegrationFailure("cancelled unclaimed preparation did not create and release its lifetime fence")
            if self._inspection_artifact(queued_id).exists() or not self._inspection_lock(queued_id).exists():
                raise IntegrationFailure("cancelled unclaimed preparation did not retain only its explicit lifetime fence")

            expired_id = self._insert_controlled_queued_preparation("expired-writer")
            worker_id = "retention-fence-worker-0001"
            self.psql(
                "UPDATE shin_server.preparations SET state='running',worker_id=" + self._sql_text(worker_id) +
                ",lease_until=clock_timestamp()+interval '30 seconds',updated_at=clock_timestamp() WHERE id=" + str(expired_id)
            )
            lifetime = self._inspection_lock(expired_id)
            descriptor = os.open(lifetime, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            fcntl.flock(descriptor, fcntl.LOCK_EX)
            self.psql(
                "UPDATE shin_server.preparations SET state='failed',lease_until=NULL,reason_code='lease-expired',"
                "updated_at=clock_timestamp() WHERE id=" + str(expired_id)
            )
            expired = self.get_job(expired_id)
            if expired["state"] != "failed" or expired["reason_code"] != "lease-expired":
                raise IntegrationFailure("controlled expired ledger fixture did not retain its terminal lease state")
            busy = self._retention_get(expired_id)
            if busy["can_apply"] is not False or busy["reason_codes"] != ["candidate-artifact-busy"]:
                raise IntegrationFailure("expired ledger was not fenced by the held private lifetime descriptor")
            status, response = self._retention_post(expired_id, "0" * 64)
            self._assert_error(status, response, 409, "candidate-artifact-busy")
        finally:
            self._close_lock(descriptor)
            try:
                if expired_id is not None:
                    expired_plan = self._retention_plan(expired_id)
                    status, response = self._retention_post(expired_id, expired_plan["plan_id"])
                    if status != 200 or response.get("result", {}).get("state") != "released":
                        raise IntegrationFailure("released lifetime descriptor did not permit explicit expired-candidate cleanup")
            finally:
                self.stop_server(expect_clean=True)
                self._set_preparation_worker_enabled(True)
                self.start_server()

    def check_candidate_retention_audit_rollback(self) -> None:
        job = self._retention_job()
        plan = self._retention_plan(job["id"])
        before = self._retention_state(job["id"])
        self.psql("REVOKE INSERT ON shin_server.audit_events FROM shin_runtime")
        try:
            status, response = self._retention_post(job["id"], plan["plan_id"])
            self._assert_error(status, response, 503, "database-unavailable")
        finally:
            self.psql("GRANT INSERT ON shin_server.audit_events TO shin_runtime")
        self._assert_no_retention_row(job["id"])
        if self._retention_state(job["id"]) != before:
            raise IntegrationFailure("audit rollback wrote retention intent or deleted candidate files")

    def check_candidate_retention_crash_recovery(self) -> None:
        job = self._retention_job()
        plan = self._retention_plan(job["id"])
        artifact = self._inspection_artifact(job["id"])
        public_before = self.get_job(job["id"])
        trigger_name = "preparation_retention_crash_hook"
        function_name = "preparation_retention_crash_hook_fn"
        self.psql(
            "CREATE OR REPLACE FUNCTION shin_server." + function_name + "() RETURNS trigger LANGUAGE plpgsql AS "
            "$$ BEGIN IF NEW.action = 'preparation.retention-released' THEN "
            "PERFORM set_config('statement_timeout', '2000', true); PERFORM pg_sleep(30); END IF; RETURN NEW; END $$"
        )
        self.psql(
            "CREATE TRIGGER " + trigger_name + " BEFORE INSERT ON shin_server.audit_events "
            "FOR EACH ROW EXECUTE FUNCTION shin_server." + function_name + "()"
        )
        outcome: list[BaseException | tuple[int, dict[str, Any]]] = []

        def apply_in_background() -> None:
            connection = http.client.HTTPConnection(HOST, PORT, timeout=8.0)
            try:
                connection.request(
                    "POST",
                    f"/api/v1/preparations/{job['id']}/retention",
                    body=json.dumps({"expected_plan_id": plan["plan_id"]}, separators=(",", ":")).encode("utf-8"),
                    headers={"Host": f"{HOST}:{PORT}", **self.auth_headers("admin"), "Content-Type": "application/json"},
                )
                response = connection.getresponse()
                payload = json.loads(response.read().decode("utf-8"))
                outcome.append((response.status, payload))
            except BaseException as error:
                outcome.append(error)
            finally:
                connection.close()

        worker = threading.Thread(target=apply_in_background)
        worker.start()
        try:
            deadline = time.monotonic() + 1.8
            while time.monotonic() < deadline and artifact.exists():
                time.sleep(0.01)
            if artifact.exists():
                raise IntegrationFailure("could not observe retention deletion before the controlled crash window")
            self.kill_owned_server()
            worker.join(timeout=10.0)
            if worker.is_alive():
                raise IntegrationFailure("controlled retention HTTP request did not finish after server crash")
        finally:
            self.psql("DROP TRIGGER IF EXISTS " + trigger_name + " ON shin_server.audit_events")
            self.psql("DROP FUNCTION IF EXISTS shin_server." + function_name + "()")
        if not outcome:
            raise IntegrationFailure("controlled retention crash request produced no outcome")
        if self.scalar("SELECT state FROM shin_server.preparation_retention WHERE preparation_id = " + str(job["id"])) != "intent":
            raise IntegrationFailure("crashed retention request did not preserve its durable intent")
        if self._retention_audit_count(job["id"], "preparation.retention-released") != 0:
            raise IntegrationFailure("crashed retention request committed a release audit")

        self.start_server()
        if self.get_job(job["id"]) != public_before:
            raise IntegrationFailure("crashed retention request changed the historical public job")
        after_restart = self._retention_get(job["id"])
        if after_restart["state"] != "intent" or after_restart["reservation_held"] is not True or artifact.exists():
            raise IntegrationFailure("server restart performed automatic retention cleanup or quota release")
        status, response = self._retention_post(job["id"], plan["plan_id"])
        if status != 200:
            raise IntegrationFailure("explicit retry did not reconcile the crashed retention intent")
        released = self._assert_retention_report(response.get("result"), job["id"])
        if released["state"] != "released" or released["mutation_performed"] is not True:
            raise IntegrationFailure("explicit retention retry did not complete the interrupted release")
        if self._retention_audit_count(job["id"], "preparation.retention-released") != 1:
            raise IntegrationFailure("interrupted retention retry wrote an incorrect release audit count")

    def check_candidate_retention_namespace_and_request_guards(self) -> None:
        job = self._retention_job()
        artifact = self._inspection_artifact(job["id"])
        lock = self._inspection_lock(job["id"])
        before = {"artifact": self._tree_fingerprint(artifact), "lock": self._tree_fingerprint(lock)}
        namespace = self.scalar("SELECT artifact_namespace FROM shin_server.preparations WHERE id = " + str(job["id"]))
        key = self.scalar("SELECT artifact_key FROM shin_server.preparations WHERE id = " + str(job["id"]))
        self.psql("DO $$ BEGIN BEGIN UPDATE shin_server.preparations SET artifact_key=NULL WHERE id=" + str(job["id"]) +
                  "; RAISE EXCEPTION 'partial binding accepted'; EXCEPTION WHEN check_violation THEN NULL; END; END $$")
        self.psql("DO $$ BEGIN BEGIN INSERT INTO shin_server.preparation_retention "
                  "(preparation_id,state,plan_id,inventory,requested_by) VALUES (" + str(job["id"]) +
                  ",'intent',repeat('f',64),'{}'::jsonb,'admin'); RAISE EXCEPTION 'missing inventory accepted'; "
                  "EXCEPTION WHEN check_violation THEN NULL; END; END $$")
        try:
            self.psql("UPDATE shin_server.preparations SET artifact_namespace = repeat('0', 64) WHERE id = " + str(job["id"]))
            report = self._retention_get(job["id"])
            if report["can_apply"] is not False or report["reason_codes"] != ["retention-artifact-unbound"]:
                raise IntegrationFailure("wrong artifact namespace did not fail closed")
            status, response = self._retention_post(job["id"], "0" * 64)
            self._assert_error(status, response, 409, "retention-artifact-unbound")

            self.psql("UPDATE shin_server.preparations SET artifact_namespace = NULL, artifact_key = NULL WHERE id = " + str(job["id"]))
            report = self._retention_get(job["id"])
            if report["can_apply"] is not False or report["reason_codes"] != ["retention-artifact-unbound"]:
                raise IntegrationFailure("unbound historical artifact row did not fail closed")
            status, response = self._retention_post(job["id"], "0" * 64)
            self._assert_error(status, response, 409, "retention-artifact-unbound")
        finally:
            self.psql(
                "UPDATE shin_server.preparations SET artifact_namespace = " + self._sql_text(namespace) +
                ", artifact_key = " + self._sql_text(key) + " WHERE id = " + str(job["id"])
            )
        after = {"artifact": self._tree_fingerprint(artifact), "lock": self._tree_fingerprint(lock)}
        if after != before or self._retention_row_count(job["id"]) != 0:
            raise IntegrationFailure("unbound retention checks touched managed files or persisted intent")

        base = f"/api/v1/preparations/{job['id']}/retention"
        self.expect_status(400, "GET", base + "?unexpected=1", headers=self.auth_headers("admin"))
        self.expect_status(
            400,
            "POST",
            base + "?unexpected=1",
            headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
            body=b'{"expected_plan_id":"' + b"0" * 64 + b'"}',
        )
        self.expect_status(
            400,
            "POST",
            base,
            headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
            body=b"{}",
        )
        self.expect_status(
            400,
            "POST",
            base,
            headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
            body=b'{"expected_plan_id":"' + b"A" * 64 + b'"}',
        )
        self.expect_status(
            400,
            "POST",
            base,
            headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
            body=b'{"expected_plan_id":"' + b"0" * 64 + b'","unexpected":true}',
        )
        status, response = self._retention_post(job["id"], "0" * 64, role="reader")
        self._assert_error(status, response, 403, "forbidden")

    def check_candidate_retention_baseline(self) -> None:
        planning: list[float] = []
        deletion: list[float] = []
        payload_bytes = 0
        for _ in range(3):
            job = self._retention_job()
            started = time.perf_counter_ns()
            plan = self._retention_plan(job["id"])
            planning.append((time.perf_counter_ns() - started) / 1_000_000)
            payload_bytes = plan["artifact_bytes"]
            started = time.perf_counter_ns()
            status, response = self._retention_post(job["id"], plan["plan_id"])
            deletion.append((time.perf_counter_ns() - started) / 1_000_000)
            if status != 200 or response.get("result", {}).get("state") != "released":
                raise IntegrationFailure("measured retention did not complete")
            if self.get_job(job["id"]) != job or self._inspection_artifact(job["id"]).exists():
                raise IntegrationFailure("measured retention changed history or retained its artifact")
        self.retention_measurement = {
            "status": "completed", "samples": 3, "payload_bytes": payload_bytes,
            "plan_http_p50_ms": self.percentile_ms(planning, 50),
            "plan_http_p95_ms": self.percentile_ms(planning, 95),
            "delete_http_p50_ms": self.percentile_ms(deletion, 50),
            "delete_http_p95_ms": self.percentile_ms(deletion, 95),
            "scope": "Three sequential small-fixture plans and durable deletions; HTTP latency, not throughput or soak.",
        }
