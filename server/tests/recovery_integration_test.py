#!/usr/bin/env python3
"""Dedicated PostgreSQL recovery contract and backup/restore regression scenarios."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import time
from typing import Any

from recovery_test_support import (
    BUILD_ROOT, DATABASE_NAME, HOST, IDENTITY_HEADER, PORT, PROJECT_NAME, SCHEMA_NAME,
    SERVER_VERSION, IntegrationFailure, RecoveryTestSupport, compact_json, require_hex, under_build,
)


class RecoveryIntegrationSuite(RecoveryTestSupport):
    def register_project_and_stream(self) -> None:
        assert self.initial_entry is not None
        status, result = self.post_json("/api/v1/projects", {"id": "visible", "display_name": "Visible recovery project"},
                                        headers={**self.auth_headers("admin"), "Idempotency-Key": "recovery-project"})
        if status != 201 or result.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("recovery project registration failed")
        enrollment = self.enrollment(self.initial_entry)
        workspace = {"id": enrollment["workspace_id"], "display_name": "Recovery observer", "source_host_id": enrollment["source_host_id"]}
        status, result = self.post_json("/api/v1/projects/visible/observer-workspaces", workspace,
                                        headers={**self.auth_headers("admin"), "Idempotency-Key": "recovery-observer-workspace"})
        if status != 201 or result.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("recovery Observer workspace registration failed")
        status, result = self.post_json("/api/v1/observer-streams", enrollment, headers=self.auth_headers("admin"))
        if status != 201 or result.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("recovery Observer enrollment failed")
        status, result = self.post_json(f"/api/v1/observer-streams/{enrollment['stream_id']}/submissions", self.initial_entry,
                                        headers=self.auth_headers("reader"))
        if status != 201 or result.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("initial native Observer packet was not accepted")
        self.initial_receipt_bytes = self.scalar(
            "SELECT receipt::text FROM shin_server.observer_submissions WHERE stream_id=" + self.sql_literal(enrollment["stream_id"]) + " AND sequence=1"
        )
        if not self.initial_receipt_bytes:
            raise IntegrationFailure("initial Observer receipt was not durably retained")
        exported = self.expect_status(200, "GET", "/api/v1/audit/export?limit=1", headers=self.auth_headers("admin"))
        self.old_audit_plan = exported["result"]["plan"]
        status, retired = self.post_json("/api/v1/audit/retention", {"plan": self.old_audit_plan, "archive_saved": True},
                                        headers=self.auth_headers("admin"))
        if status != 200 or retired.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("initial audit retention receipt was not created")

    @staticmethod
    def sql_literal(value: str) -> str:
        return "'" + value.replace("'", "''") + "'"

    def expand_registry(self) -> None:
        assert self.initial_entry is not None
        enrollment = self.enrollment(self.initial_entry)
        initial_stream = enrollment["stream_id"]
        for index in range(1, self.registry_size):
            # Reserve one slot in the initial project for the reviewed successor.
            if index % 127 == 0:
                project = f"recovery-load-{index // 127}"
                status, _ = self.post_json("/api/v1/projects", {"id": project, "display_name": project},
                    headers={**self.auth_headers("admin"), "Idempotency-Key": project})
                if status != 201:
                    raise IntegrationFailure("load project registration failed")
                status, _ = self.post_json(f"/api/v1/projects/{project}/observer-workspaces",
                    {"id": enrollment["workspace_id"], "display_name": project, "source_host_id": enrollment["source_host_id"]},
                    headers={**self.auth_headers("admin"), "Idempotency-Key": project + "-workspace"})
                if status != 201:
                    raise IntegrationFailure("load workspace registration failed")
            stream = f"{index:032x}"
            if stream == initial_stream:
                stream = f"{self.registry_size + index:032x}"
            load_enrollment = {
                **enrollment,
                "project_id": "visible" if index < 127 else f"recovery-load-{index // 127}",
                "stream_id": stream,
                "observer_id": f"recovery-load-{index:04x}",
                "workspace_binding_digest": hashlib.sha256(f"recovery-load-{index}".encode("ascii")).hexdigest(),
            }
            status, result = self.post_json("/api/v1/observer-streams", load_enrollment, headers=self.auth_headers("admin"))
            if status != 201 or result.get("result", {}).get("replayed") is not False:
                raise IntegrationFailure("bounded registry enrollment failed")
        if self.scalar("SELECT streams::text FROM shin_server.observer_usage WHERE singleton") != str(self.registry_size):
            raise IntegrationFailure("bounded registry accounting did not reach the requested size")

    def registry_runtime_config(self) -> Path:
        config = self.private_json(self.state_root / "config" / "runtime.json")
        for principal in config["principals"]:
            if principal["id"] == "dev-reader":
                principal["project_ids"] += [f"recovery-load-{index}" for index in range(1, (self.registry_size - 1) // 127 + 1)]
        return self.write_private_json("runtime-registry", config)

    def storage_signature(self) -> dict[str, str]:
        return {
            "server_id": self.scalar("SELECT server_id FROM shin_server.server_identity WHERE singleton"),
            "streams": self.scalar("SELECT count(*)::text FROM shin_server.observer_streams"),
            "active_streams": self.scalar("SELECT count(*)::text FROM shin_server.observer_streams WHERE NOT revoked"),
            "submissions": self.scalar("SELECT count(*)::text FROM shin_server.observer_submissions"),
            "usage_streams": self.scalar("SELECT streams::text FROM shin_server.observer_usage WHERE singleton"),
            "usage_submissions": self.scalar("SELECT submissions::text FROM shin_server.observer_usage WHERE singleton"),
            "usage_bytes": self.scalar("SELECT retained_bytes::text FROM shin_server.observer_usage WHERE singleton"),
            "audit_events": self.scalar("SELECT count(*)::text FROM shin_server.audit_events"),
            "audit_floor": self.scalar("SELECT to_jsonb(a)::text FROM shin_server.audit_history a WHERE singleton"),
            "recovery": self.scalar("SELECT COALESCE(last_recovery::text, 'null') FROM shin_server.server_recovery WHERE singleton"),
        }

    def managed_db_fingerprint(self) -> str:
        self.guard_database()
        completed = self.compose_database("pg_dump", "--format=plain", "--schema=shin_server", "--no-owner", "--no-privileges",
                                          "-U", "shin_migrator", "-d", DATABASE_NAME, timeout=60.0)
        if not completed.stdout:
            raise IntegrationFailure("managed database fingerprint dump was empty")
        # PostgreSQL 18 adds a generated psql restriction token to plain dumps.
        # It is not managed database state and would make a same-state comparison noisy.
        normalized = re.sub(rb"^\\(?:un)?restrict [^\r\n]*\r?\n", b"", completed.stdout, flags=re.MULTILINE)
        return hashlib.sha256(normalized).hexdigest()

    def create_backup(self) -> None:
        self.guard_database()
        path = self.state_root / "reports" / f"recovery-backup-{secrets.token_hex(8)}.dump"
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(descriptor)
        try:
            completed = self.compose_database("pg_dump", "--format=custom", "--schema=shin_server", "--no-owner", "--no-privileges",
                                              "-U", "shin_migrator", "-d", DATABASE_NAME, timeout=60.0)
            if not completed.stdout:
                raise IntegrationFailure("trusted recovery backup was empty")
            with path.open("wb") as handle:
                handle.write(completed.stdout)
        except BaseException:
            path.unlink(missing_ok=True)
            raise
        self.backup_path = path
        self.metrics["backup_bytes"] = path.stat().st_size
        self.metrics["backup_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()

    def restore_backup(self, *, record_metric: bool = True) -> None:
        if self.backup_path is None:
            raise IntegrationFailure("no trusted recovery backup is available")
        self.guard_database()
        dump = self.read_private_file(self.backup_path, 64 * 1024 * 1024)
        if hashlib.sha256(dump).hexdigest() != self.metrics.get("backup_sha256"):
            raise IntegrationFailure("trusted recovery backup changed before restore")
        started = time.monotonic()
        self.compose_database("pg_restore", "--clean", "--if-exists", "--schema=shin_server", "--no-owner", "--no-privileges",
                              "-U", "shin_migrator", "-d", DATABASE_NAME, timeout=90.0, input_bytes=dump)
        self.migrate()
        elapsed_ms = round((time.monotonic() - started) * 1000, 3)
        if record_metric:
            self.metrics["restore_ms"] = elapsed_ms
        else:
            self.metrics["cleanup_restore_ms"] = elapsed_ms

    def plan_and_validate(self) -> None:
        request = self.recovery_request()
        result = self.cli("recovery-plan", self.state_root / "config" / "recovery.json", self.write_recovery_input(request), expect_success=True)
        payload = result.get("result")
        if not isinstance(payload, dict) or set(payload) != {"plan", "read_only", "restored_backup_verified"} or \
                payload["read_only"] is not True or payload["restored_backup_verified"] is not False:
            raise IntegrationFailure("recovery plan did not preserve its read-only proof boundary")
        plan = payload["plan"]
        if not isinstance(plan, dict):
            raise IntegrationFailure("recovery plan is not an object")
        expected_id = hashlib.sha256(
            ("shin.server-recovery-identity.v1\n" + self.old_server_id + "\n" + request["operation_id"]).encode("ascii")
        ).hexdigest()[:32]
        required = {"schema_version", "request", "new_server_id", "stream_count", "active_stream_count", "observer_state_sha256",
                    "tombstone_count", "tombstone_state_sha256", "retained_submissions", "retained_bytes", "audit_state_sha256", "previous_receipt_sha256", "plan_id"}
        if set(plan) != required or plan.get("schema_version") != "shin.server-recovery-plan.v2" or plan.get("request") != request or \
                plan.get("new_server_id") != expected_id or plan.get("stream_count") != self.registry_size or \
                plan.get("active_stream_count") != self.registry_size or plan.get("tombstone_count") != 0:
            raise IntegrationFailure("recovery plan does not bind the restored observer registry")
        for field, length in (("observer_state_sha256", 64), ("tombstone_state_sha256", 64), ("audit_state_sha256", 64), ("plan_id", 64)):
            require_hex(plan.get(field), length, "recovery plan digest is invalid")
        self.initial_plan = plan
        self.metrics["plan_streams"] = self.registry_size

    def check_runtime_role_denied(self) -> None:
        config = self.private_json(self.state_root / "config" / "recovery.json")
        database = config["database"]
        database["connection_file"] = str(self.state_root / "conninfo" / "runtime.conninfo")
        denied = self.write_private_json("recovery-runtime-denied", config)
        self.cli_error("recovery-plan", denied, self.write_recovery_input(self.recovery_request()), "recovery-role-denied")

    def check_recovery_ownership_drift(self) -> None:
        """A schema owner must not recover through a table transferred to another existing role."""
        before = self.scalar(
            "SELECT current_user=pg_get_userbyid(n.nspowner) AND c.relowner=n.nspowner "
            "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
            "WHERE c.oid='shin_server.server_recovery'::regclass AND n.nspname='shin_server'"
        )
        if before != "t":
            raise IntegrationFailure("fresh recovery schema ownership does not belong to the migrator")
        self.psql("ALTER TABLE shin_server.server_recovery OWNER TO shin_runtime")
        try:
            self.cli_error("recovery-plan", self.state_root / "config" / "recovery.json",
                           self.write_recovery_input(self.recovery_request()), "recovery-role-denied")
        finally:
            # The migrator is the initial database owner in this dedicated stand;
            # CURRENT_USER restores the exact owner without creating another role.
            self.psql("ALTER TABLE shin_server.server_recovery OWNER TO CURRENT_USER")
        after = self.scalar(
            "SELECT current_user=pg_get_userbyid(n.nspowner) AND c.relowner=n.nspowner "
            "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
            "WHERE c.oid='shin_server.server_recovery'::regclass AND n.nspname='shin_server'"
        )
        if after != "t":
            raise IntegrationFailure("recovery ownership drift cleanup did not restore the schema owner")
        self.migrate()
        self.plan_and_validate()

    def check_live_fence(self) -> None:
        assert self.initial_plan is not None
        ready = self.wait_for_response("/health/ready", 200, headers=self.auth_headers("admin"))
        if ready.get("result", {}).get("server_id") != self.old_server_id:
            raise IntegrationFailure("runtime did not establish its pinned lifecycle connection before recovery fencing")
        recovery_config = self.state_root / "config" / "recovery.json"
        request_path = self.write_recovery_input(self.recovery_request())
        plan_path = self.write_recovery_input({"plan": self.initial_plan})
        self.cli_error("recovery-plan", recovery_config, request_path, "server-recovery-busy")
        self.cli_error("recovery-apply", recovery_config, plan_path, "server-recovery-busy")
        completed = subprocess.run([str(self.binary), "migrate", "--config", str(self.state_root / "config" / "migrator.json")],
                                   check=False, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45.0,
                                   env=self.process_environment)
        self.assert_no_sanitizer_output(completed.stdout.decode("utf-8", errors="replace") + completed.stderr.decode("utf-8", errors="replace"),
                                        "live migration")
        if completed.returncode == 0 or completed.stdout:
            raise IntegrationFailure("live migration was not fenced by the shared runtime lifecycle lock")
        try:
            envelope = json.loads(completed.stderr)
        except json.JSONDecodeError as error:
            raise IntegrationFailure("live migration did not return its error envelope") from error
        self.assert_envelope(envelope)
        if envelope.get("error", {}).get("code") != "server-recovery-busy":
            raise IntegrationFailure("live migration returned the wrong fence error")

    def submit_later_packet(self) -> None:
        assert self.fixture is not None and self.initial_entry is not None
        self.fixture.write_source("Later recovery fixture revision.\n")
        state = self.sample_observer(self.fixture)
        entries = state.get("outbox", {}).get("entries")
        if not isinstance(entries, list) or len(entries) != 2 or not isinstance(entries[-1], dict):
            raise IntegrationFailure("Observer did not retain a later local packet")
        later = entries[-1]
        if later.get("stream_id") != self.initial_entry.get("stream_id") or later.get("sequence") == self.initial_entry.get("sequence"):
            raise IntegrationFailure("later Observer packet did not remain in the known stream")
        status, result = self.post_json(f"/api/v1/observer-streams/{later['stream_id']}/submissions", later, headers=self.auth_headers("reader"))
        if status != 201 or result.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("later Observer packet was not accepted")

    def check_stale_plan(self) -> None:
        assert self.initial_plan is not None
        self.cli_error("recovery-apply", self.state_root / "config" / "recovery.json",
                       self.write_recovery_input({"plan": self.initial_plan}), "recovery-plan-stale")

    def check_invalid_plans(self) -> None:
        assert self.initial_plan is not None
        config = self.state_root / "config" / "recovery.json"
        self.cli_error("recovery-apply", config, self.write_recovery_input({"plan": {}}))
        forged = json.loads(json.dumps(self.initial_plan))
        forged["new_server_id"] = "f" * 32
        self.cli_error("recovery-apply", config, self.write_recovery_input({"plan": forged}), "recovery-plan-stale")

    def check_apply_rollback(self) -> None:
        assert self.initial_plan is not None
        before = self.storage_signature()
        self.psql(
            "CREATE FUNCTION shin_server.recovery_reject_update() RETURNS trigger LANGUAGE plpgsql AS "
            "$$ BEGIN RAISE EXCEPTION 'forced recovery rollback'; END $$; "
            "CREATE TRIGGER recovery_reject_update BEFORE UPDATE ON shin_server.server_recovery "
            "FOR EACH ROW EXECUTE FUNCTION shin_server.recovery_reject_update()"
        )
        try:
            self.cli_error("recovery-apply", self.state_root / "config" / "recovery.json",
                           self.write_recovery_input({"plan": self.initial_plan}), "database-unavailable")
        finally:
            self.psql("DROP TRIGGER IF EXISTS recovery_reject_update ON shin_server.server_recovery; "
                      "DROP FUNCTION IF EXISTS shin_server.recovery_reject_update()")
        if self.storage_signature() != before:
            raise IntegrationFailure("failed recovery apply did not atomically roll back identity and stream revocations")

    def apply_and_validate(self) -> dict[str, Any]:
        assert self.initial_plan is not None
        started = time.monotonic()
        result = self.cli("recovery-apply", self.state_root / "config" / "recovery.json",
                          self.write_recovery_input({"plan": self.initial_plan}), expect_success=True)
        self.metrics["apply_ms"] = round((time.monotonic() - started) * 1000, 3)
        payload = result.get("result")
        if not isinstance(payload, dict) or set(payload) != {"receipt", "replayed"} or payload["replayed"] is not False:
            raise IntegrationFailure("recovery apply did not return a first-apply receipt")
        receipt = payload["receipt"]
        if not isinstance(receipt, dict):
            raise IntegrationFailure("recovery receipt is not an object")
        expected_new_id = self.initial_plan["new_server_id"]
        required = {"schema_version", "operation_id", "previous_server_id", "server_id", "reason_code", "plan_id", "revoked_streams",
                    "retained_streams", "retained_tombstones", "previous_receipt_sha256", "recorded_at", "service_fenced_declared", "restored_backup_verified",
                    "history_complete", "local_state_verified", "source_verified", "shared_publication_mutated"}
        if set(receipt) != required or receipt.get("schema_version") != "shin.server-recovery-receipt.v2" or \
                receipt.get("operation_id") != self.recovery_request()["operation_id"] or receipt.get("previous_server_id") != self.old_server_id or \
                receipt.get("server_id") != expected_new_id or receipt.get("plan_id") != self.initial_plan["plan_id"] or \
                receipt.get("revoked_streams") != self.registry_size or receipt.get("retained_streams") != self.registry_size or receipt.get("retained_tombstones") != 0:
            raise IntegrationFailure("recovery receipt does not bind the reviewed identity transition")
        if receipt["service_fenced_declared"] is not True or any(receipt[name] is not False for name in
                ("restored_backup_verified", "history_complete", "local_state_verified", "source_verified", "shared_publication_mutated")):
            raise IntegrationFailure("recovery receipt overstates verification after a local restore rehearsal")
        after = self.storage_signature()
        if after["server_id"] != expected_new_id or after["active_streams"] != "0" or after["streams"] != str(self.registry_size) or \
                after["submissions"] != self.initial_storage["submissions"] or after["usage_streams"] != self.initial_storage["usage_streams"] or \
                after["usage_submissions"] != self.initial_storage["usage_submissions"] or after["usage_bytes"] != self.initial_storage["usage_bytes"] or \
                after["audit_events"] != self.initial_storage["audit_events"] or after["audit_floor"] != self.initial_storage["audit_floor"]:
            raise IntegrationFailure("recovery apply rewrote retained history or did not revoke every active stream")
        if self.scalar("SELECT receipt::text FROM shin_server.observer_submissions WHERE sequence=1") != self.initial_receipt_bytes:
            raise IntegrationFailure("recovery apply rewrote the retained prior Observer receipt bytes")
        self.server_id = expected_new_id
        return receipt

    def replay_apply(self, receipt: dict[str, Any]) -> None:
        assert self.initial_plan is not None
        result = self.cli("recovery-apply", self.state_root / "config" / "recovery.json",
                          self.write_recovery_input({"plan": self.initial_plan}), expect_success=True)
        replay = result.get("result")
        if not isinstance(replay, dict) or replay.get("replayed") is not True or compact_json(replay.get("receipt")) != compact_json(receipt):
            raise IntegrationFailure("exact recovery retry did not return the durable receipt unchanged")

    def reviewed_runtime_config(self) -> Path:
        current = self.private_json(self.state_root / "config" / "runtime.json")
        if self.runtime_server_id() != self.old_server_id:
            raise IntegrationFailure("old runtime configuration changed before explicit review")
        current["database"]["server_id"] = self.server_id
        current["schema_version"] = "shin.server-config.v10"
        return self.write_private_json("runtime-reviewed", current)

    def check_identity_transition_runtime(self, receipt: dict[str, Any]) -> None:
        successor_id = require_hex(receipt.get("server_id"), 32, "recovery receipt has no successor identity")
        self.server_id = self.old_server_id
        self.start_server()
        response = self.wait_for_response("/health/ready", 503, headers=self.auth_headers("admin"))
        if response.get("ok") is not False:
            raise IntegrationFailure("old runtime pin did not fail readiness before review")
        self.stop_server(expect_clean=True)

        self.server_id = successor_id
        reviewed = self.reviewed_runtime_config()
        self.server_id = self.runtime_server_id(reviewed)
        self.start_server(reviewed)
        ready = self.wait_for_response("/health/ready", 200, headers=self.auth_headers("admin"))
        if ready.get("result", {}).get("server_id") != self.server_id:
            raise IntegrationFailure("reviewed runtime configuration did not become ready under the new identity")
        wrong = self.post_json("/api/v1/projects", {"id": "identity-rejected", "display_name": "Identity rejected"}, headers={
            **self.auth_headers("admin"), IDENTITY_HEADER: self.old_server_id, "Idempotency-Key": "identity-rejected",
        })
        if wrong[0] != 409 or wrong[1].get("error", {}).get("code") != "server-identity-mismatch":
            raise IntegrationFailure("old Server identity header was not rejected before a valid request body")
        status = self.expect_status(200, "GET", "/api/v1/recovery", headers=self.auth_headers("admin"))
        if compact_json(status.get("result", {}).get("last_receipt")) != compact_json(receipt):
            raise IntegrationFailure("recovery status did not expose the latest durable receipt")
        assert self.old_audit_plan is not None
        rejected, old_plan = self.post_json("/api/v1/audit/retention", {"plan": self.old_audit_plan, "archive_saved": True},
                                           headers=self.auth_headers("admin"))
        if rejected != 409 or old_plan.get("error", {}).get("code") != "audit-retention-stale":
            raise IntegrationFailure("old audit retention receipt replay crossed the recovered identity")
        self.stop_server(expect_clean=True)

    def write_delivery_config(self, fixture: Any, server_id: str) -> Path:
        assert self.fixture_module is not None
        path = fixture.base / f"delivery-{secrets.token_hex(8)}.json"
        self.fixture_module.write_private_json(path, {
            "schema_version": "shin.observer-delivery-config.v1", "host": HOST, "port": PORT, "server_id": server_id,
            "principal_id": "dev-reader", "token_file": str(self.state_root / "secrets" / "dev_reader.token"), "request_timeout_ms": 1000,
        })
        return path

    def observer_delivery(self, fixture: Any, delivery: Path) -> dict[str, Any]:
        completed = subprocess.run([str(self.observer), "deliver", "--config", str(fixture.config), "--delivery-config", str(delivery)],
                                   check=False, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60.0,
                                   env=self.observer_environment)
        stdout = completed.stdout.decode("utf-8", errors="replace")
        stderr = completed.stderr.decode("utf-8", errors="replace")
        self.assert_no_sanitizer_output(stdout + stderr, "Observer delivery")
        if completed.returncode != 0 or stderr:
            raise IntegrationFailure("Observer delivery did not return a clean delivery envelope")
        try:
            envelope = json.loads(stdout)
        except json.JSONDecodeError as error:
            raise IntegrationFailure("Observer delivery did not return one JSON envelope") from error
        if not isinstance(envelope, dict):
            raise IntegrationFailure("Observer delivery response is malformed")
        return envelope

    @staticmethod
    def outbox_cursors(fixture: Any) -> tuple[int, int, int, int]:
        state = json.loads((fixture.state / "state.json").read_text(encoding="utf-8"))
        outbox = state.get("outbox")
        if not isinstance(outbox, dict) or not isinstance(outbox.get("entries"), list):
            raise IntegrationFailure("Observer state has no durable outbox")
        return (len(outbox["entries"]), outbox["acknowledged_sequence"], outbox["accepted_sequence"], outbox["acknowledged_observation_sequence"])

    def check_old_observer_blocked_and_new_enrollment(self) -> None:
        assert self.fixture is not None and self.initial_entry is not None and self.fixture_module is not None
        old_cursors = self.outbox_cursors(self.fixture)
        old_delivery = self.write_delivery_config(self.fixture, self.old_server_id)
        failed = self.observer_delivery(self.fixture, old_delivery)
        failed_result = failed.get("result")
        if not isinstance(failed_result, dict) or failed_result.get("delivery_failed") is not True or \
                failed_result.get("pending_drained") is not False or self.outbox_cursors(self.fixture) != old_cursors:
            raise IntegrationFailure("old Observer delivery profile did not retain local pending state after identity rejection")
        old_stream = self.enrollment(self.initial_entry)["stream_id"]
        if self.scalar("SELECT revoked::text FROM shin_server.observer_streams WHERE stream_id=" + self.sql_literal(old_stream)) != "true":
            raise IntegrationFailure("old Observer stream was implicitly reactivated")

        archived_state = (self.fixture.state / "state.json").read_bytes()
        archived_config = self.fixture.config.read_bytes()
        source_before = self.fixture_module.tree_metadata(self.fixture.root)
        index_before = self.fixture_module.tree_metadata(self.fixture.out)
        archive_root = self.fixture.base / "archive"
        archive_root.mkdir(mode=0o700)
        archive = archive_root / "unconfirmed-state.json"
        archive_receipts = []
        for command in ("outbox-export", "outbox-export", "outbox-verify"):
            completed = subprocess.run([str(self.observer), command, "--config", str(self.fixture.config),
                                        "--archive", str(archive)], capture_output=True, text=True,
                                       timeout=30, env=self.observer_environment)
            self.assert_no_sanitizer_output(completed.stdout + completed.stderr, "Observer archive")
            if completed.returncode != 0 or completed.stderr:
                raise IntegrationFailure("old Observer archive did not verify cleanly")
            archive_receipts.append(self.fixture_module.assert_observer_envelope(json.loads(completed.stdout), expected_ok=True))
        for receipt in archive_receipts:
            if (receipt["archive_sha256"] != hashlib.sha256(archived_state).hexdigest() or
                    receipt["stream_id"] != old_stream or receipt["destination"]["server_id"] != self.old_server_id or
                    receipt["pending_entries"] != old_cursors[0] or not receipt["current_state_verified"] or
                    receipt["delivery_performed"] or receipt["observer_state_mutated"] or
                    receipt["external_archive_durability_verified"]):
                raise IntegrationFailure("archive lost old destination/pending identity or overclaimed delivery")
        self.metrics["observer_archive"] = {
            "verified": True, "sha256": archive_receipts[0]["archive_sha256"],
            "pending_entries": old_cursors[0], "successor_replays_archived_packets": False,
        }

        def old_state_preserved():
            if ((self.fixture.state / "state.json").read_bytes() != archived_state or
                    archive.read_bytes() != archived_state or self.fixture.config.read_bytes() != archived_config or
                    self.fixture_module.tree_metadata(self.fixture.root) != source_before or
                    self.fixture_module.tree_metadata(self.fixture.out) != index_before):
                raise IntegrationFailure("reviewed re-enrollment changed archived state, old config, source or index")

        old_state_preserved()
        parent = Path(self._fixture_temporary.name).resolve()
        fresh = copy.copy(self.fixture)
        fresh.base = parent / "reviewed-successor"
        fresh.base.mkdir(mode=0o700)
        fresh.state = fresh.base / "state"
        fresh.state.mkdir(mode=0o700)
        fresh.config = fresh.base / "observer.json"
        config = json.loads(archived_config)
        config["state_dir"] = str(fresh.state)
        fresh.write_config(config)
        state = self.sample_observer(fresh)
        entries = state.get("outbox", {}).get("entries")
        if not isinstance(entries, list) or len(entries) != 1 or not isinstance(entries[0], dict):
            raise IntegrationFailure("new Observer enrollment fixture did not create a packet")
        entry = entries[0]
        enrollment = self.enrollment(entry)
        if enrollment["stream_id"] == old_stream:
            raise IntegrationFailure("new Observer enrollment did not allocate a fresh stream identity")
        if (enrollment["workspace_id"] != self.enrollment(self.initial_entry)["workspace_id"] or
                entry["sequence"] != 1 or state["outbox"]["acknowledged_sequence"] != 0 or
                state["outbox"]["last_receipt"] is not None or state["outbox"]["destination"] is not None):
            raise IntegrationFailure("same-workspace successor inherited old delivery continuity")
        old_state_preserved()
        status, response = self.post_json("/api/v1/observer-streams", enrollment, headers=self.auth_headers("admin"))
        if self.registry_size == 1024:
            if status != 409 or response.get("error", {}).get("code") != "observer-capacity":
                raise IntegrationFailure("recovery silently reclaimed registry capacity")
            self.metrics["successor_enrollment"] = "explicitly-blocked-by-retained-registry-capacity"
            return
        if status != 201:
            raise IntegrationFailure("explicit reviewed successor enrollment failed")
        delivered = self.observer_delivery(fresh, self.write_delivery_config(fresh, self.server_id))
        result = delivered.get("result")
        if not isinstance(result, dict) or result.get("delivery_failed") or not result.get("pending_drained"):
            raise IntegrationFailure("explicit new Observer enrollment could not deliver a fresh packet")
        if self.scalar("SELECT count(*)::text FROM shin_server.observer_submissions WHERE stream_id=" + self.sql_literal(enrollment["stream_id"])) != "1":
            raise IntegrationFailure("new Observer enrollment did not receive exactly one fresh packet")
        old_state_preserved()

    def run(self) -> None:
        self.dev_command("init")
        self.old_server_id = self.runtime_server_id()
        self.server_id = self.old_server_id
        self.admin_token = self.read_token("dev_admin.token")
        self.reader_token = self.read_token("dev_reader.token")
        self.record("postgres-up", lambda: self.dev_command("up"))
        self.record("postgres-ready", self.wait_for_postgres)
        self.record("http-port-free", self.require_http_port_free)
        self.record("dedicated-schema-guard", self.reset_schema)
        self.record("explicit-migration", self.migrate)
        self.record("native-observer-packet", self.create_observer_fixture)
        self.start_server(self.registry_runtime_config())
        self.record("initial-project-stream-and-receipt", self.register_project_and_stream)
        self.record("bounded-registry", self.expand_registry)
        self.stop_server(expect_clean=True)
        self.initial_storage = self.storage_signature()
        self.record("initial-read-only-plan", self.plan_and_validate)
        self.record("runtime-role-recovery-denied", self.check_runtime_role_denied)
        self.record("schema-and-table-ownership-drift", self.check_recovery_ownership_drift)
        initial_fingerprint = self.managed_db_fingerprint()
        self.metrics["initial_managed_db_sha256"] = initial_fingerprint
        self.record("trusted-schema-backup", self.create_backup)
        self.start_server()
        self.record("live-lifecycle-fence", self.check_live_fence)
        self.record("later-submission-after-backup", self.submit_later_packet)
        self.stop_server(expect_clean=True)
        self.record("stale-plan-denied", self.check_stale_plan)
        self.record("trusted-backup-restore", self.restore_backup)
        if self.managed_db_fingerprint() != initial_fingerprint or self.storage_signature() != self.initial_storage:
            raise IntegrationFailure("trusted backup restore did not return the exact initial managed database state")
        self.record("malformed-and-forged-plans-denied", self.check_invalid_plans)
        self.record("recovery-apply-atomic-rollback", self.check_apply_rollback)
        receipt: dict[str, Any] = {}

        def apply() -> None:
            nonlocal receipt
            receipt = self.apply_and_validate()

        self.record("recovery-apply-revocation-and-identity", apply)
        self.record("exact-recovery-retry", lambda: self.replay_apply(receipt))
        self.record("old-and-reviewed-runtime-pins", lambda: self.check_identity_transition_runtime(receipt))
        reviewed = self.reviewed_runtime_config()
        self.server_id = self.runtime_server_id(reviewed)
        self.start_server(reviewed)
        self.record("old-observer-archive-reviewed-same-workspace-enrollment", self.check_old_observer_blocked_and_new_enrollment)
        self.stop_server(expect_clean=True)
        self.record("recovery-retry-after-runtime-restart", lambda: self.replay_apply(receipt))

    def cleanup(self) -> None:
        self.stop_server(expect_clean=False)
        if self.backup_path is not None and self.backup_path.exists():
            # Return the dedicated deployment state to the old identity pin before
            # removing the private dump.  No runtime config or server-id file is edited.
            self.restore_backup(record_metric=False)
        self.cleanup_observer_fixture()
        if self.backup_path is not None:
            self.backup_path.unlink(missing_ok=True)
        for path in self.private_paths:
            path.unlink(missing_ok=True)

    def write_report(self, *, ok: bool, failure: str | None = None) -> None:
        report: dict[str, Any] = {
            "schema_version": "shin.server-recovery-integration-report.v1",
            "tool": "shin-server-recovery-integration-test",
            "version": SERVER_VERSION,
            "ok": ok,
            "duration_ms": round((time.monotonic() - self.started_at) * 1000, 3),
            "checks": self.checks,
            "registry_size": self.registry_size,
            "postgres": {"project": PROJECT_NAME, "database": DATABASE_NAME, "schema": SCHEMA_NAME},
            "metrics": self.metrics,
            "scope": "Dedicated correctness rehearsal; optional bounded registry size is not a soak or production SLO.",
        }
        if failure is not None:
            report["failure"] = {"type": failure, "stage": self.active_check}
        descriptor = os.open(self.report_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(compact_json(report) + b"\n")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="native shin-server executable")
    parser.add_argument("--observer", default=str(BUILD_ROOT / "observer-release" / "shin-observer"),
                        help="absolute shin-observer executable for the native delivery checks")
    parser.add_argument("--state-root", default=str(BUILD_ROOT / "server-dev"), help="isolated state below this worktree build directory")
    parser.add_argument("--report", help="new private report path below this worktree build directory")
    parser.add_argument("--registry-size", type=int, choices=(1, 128, 1024), default=1,
                        help="bounded active registry size; 128 and 1024 are optional latency samples")
    return parser.parse_args(argv)


def require_executable(value: str, label: str, *, absolute: bool) -> Path:
    path = Path(value)
    if absolute and not path.is_absolute():
        raise IntegrationFailure(f"--{label} must be an absolute executable path")
    path = path.resolve(strict=False)
    if not path.is_file() or not os.access(path, os.X_OK):
        raise IntegrationFailure(f"--{label} must name an executable native binary")
    return path


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        binary = require_executable(args.binary, "binary", absolute=False)
        observer = require_executable(args.observer, "observer", absolute=True)
        state_root = Path(args.state_root).resolve(strict=False)
        report = Path(args.report).resolve(strict=False) if args.report else state_root / "reports" / f"recovery-{secrets.token_hex(8)}.json"
        if not under_build(state_root) or not under_build(report) or report.exists():
            raise IntegrationFailure("state root and new report must be below this worktree build directory")
    except IntegrationFailure as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    suite = RecoveryIntegrationSuite(binary, observer, state_root, report, args.registry_size)
    failure: Exception | None = None
    try:
        suite.run()
    except Exception as error:
        failure = error
    try:
        suite.cleanup()
    except Exception as cleanup_error:
        if failure is None:
            failure = cleanup_error
    if failure is not None:
        try:
            suite.write_report(ok=False, failure=type(failure).__name__)
        except OSError:
            pass
        print(f"error: recovery integration test failed: {type(failure).__name__}", file=sys.stderr)
        return 1
    suite.write_report(ok=True)
    print("recovery integration test passed; sanitized private report written below the build state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
