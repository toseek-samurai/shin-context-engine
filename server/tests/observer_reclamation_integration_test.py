#!/usr/bin/env python3
"""Real PostgreSQL coverage for explicit inactive Observer stream reclamation."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
from typing import Any

from integration_test import BUILD_ROOT, IntegrationFailure, under_build
from observer_history_integration_test import ObserverHistoryIntegrationSuite
from observer_ingestion_integration_test import PROFILE, PROJECT_ID, WORKSPACE_ID, require_executable


class ObserverReclamationIntegrationSuite(ObserverHistoryIntegrationSuite):
    """Adds reclamation assertions after the history fixture has established a valid stream."""

    def __init__(self, binary: Path, observer: Path, state_root: Path, report_path: Path) -> None:
        super().__init__(binary, observer, state_root, report_path)
        self.reclamation_checks: list[dict[str, Any]] = []
        self.reclaimed: tuple[str, dict[str, Any], dict[str, Any], dict[str, Any]] | None = None
        self.reclamation_private_paths: list[Path] = []
        self.recovery_applied = False
        self.reclamation_projects: set[str] = set()
        self.discarded: tuple[str, dict[str, Any]] | None = None

    def record_reclamation(self, name: str, operation: Any) -> None:
        self.record(name, operation)
        self.reclamation_checks.append({"name": name})

    def reclamation(self, stream: str, *, headers: dict[str, str] | None = None) -> tuple[int, dict[str, Any]]:
        return self.request("GET", f"/api/v1/observer-streams/{stream}/reclamation", headers=headers or self.owner_headers)

    def apply_reclamation(self, stream: str, plan: dict[str, Any], *, headers: dict[str, str] | None = None) -> tuple[int, dict[str, Any]]:
        return self.post_json(
            f"/api/v1/observer-streams/{stream}/reclamation", {"plan": plan},
            headers=headers or self.auth_headers("admin"),
        )

    def discard_plan(self, stream: str, *, eligible: bool = True) -> dict[str, Any]:
        status, response = self.request("GET", f"/api/v1/observer-streams/{stream}/discard",
                                        headers=self.auth_headers("admin"))
        plan = response.get("result", {})
        if status != 200 or plan.get("schema_version") != "shin.observer-discard-plan.v1" or \
                plan.get("eligible") is not eligible or plan.get("stream_id") != stream or \
                plan.get("server_id") != self.server_identity or \
                plan.get("disposition") != "discard-unconfirmed-history" or \
                plan.get("unconfirmed_submissions") != plan.get("accepted_sequence", 0) - plan.get("confirmed_sequence", 0):
            raise IntegrationFailure("discard inspection did not return the explicit bounded disposition plan")
        self.require_hex(plan.get("history_sha256"), 64, "discard plan must bind exact retained content")
        return plan

    def apply_discard(self, stream: str, plan: dict[str, Any], **kwargs: Any) -> tuple[int, dict[str, Any]]:
        return self.post_json(f"/api/v1/observer-streams/{stream}/discard",
                              {"plan": plan, "confirmation": "discard-unconfirmed-history"},
                              headers=kwargs.get("headers", self.auth_headers("admin")))

    def pending_discard(self, label: str) -> tuple[str, dict[str, Any], dict[str, Any]]:
        stream, observer, enrollment = self.enroll(label)
        _, receipt = self.accept_for(stream, observer)
        self.confirm_for(stream, receipt)
        self.accept_for(stream, observer, 2)
        self.revoke(stream)
        return stream, enrollment, self.discard_plan(stream)

    def assert_discard_receipt(self, response: dict[str, Any], plan: dict[str, Any]) -> dict[str, Any]:
        result = response.get("result", {})
        receipt = result.get("receipt", {})
        if receipt.get("schema_version") != "shin.observer-discard-receipt.v1" or \
                receipt.get("plan_id") != plan["plan_id"] or receipt.get("server_id") != plan["server_id"] or \
                receipt.get("stream_id") != plan["stream_id"] or receipt.get("history_sha256") != plan["history_sha256"] or \
                receipt.get("disposition") != "discard-unconfirmed-history" or \
                receipt.get("confirmed_sequence") != plan["confirmed_sequence"] or \
                receipt.get("discarded_unconfirmed_submissions") != plan["unconfirmed_submissions"] or \
                receipt.get("deleted_submissions") != plan["delete_submissions"] or \
                receipt.get("deleted_bytes") != plan["delete_bytes"] or receipt.get("identity_retained") is not True or \
                receipt.get("audit_preserved") is not True or receipt.get("source_verified") is not False or \
                receipt.get("shared_publication_mutated") is not False or \
                receipt.get("producer_durability_verified") is not False or receipt.get("archive_durability_verified") is not False:
            raise IntegrationFailure("discard receipt invented confirmation, archive durability or changed its plan binding")
        return receipt

    def check_discard_authority_and_eligibility(self) -> None:
        active, observer, _ = self.enroll("discard-active")
        self.accept_for(active, observer)
        before = self.storage_signature(active)
        plan = self.discard_plan(active, eligible=False)
        if plan.get("blocked_reason") != "active-stream":
            raise IntegrationFailure("discard plan did not preserve active stream protection")
        for method in ("GET", "POST"):
            if method == "GET":
                status, _ = self.request(method, f"/api/v1/observer-streams/{active}/discard", headers=self.owner_headers)
            else:
                status, _ = self.apply_discard(active, plan, headers=self.owner_headers)
            if status != 403:
                raise IntegrationFailure("non-administrator could inspect or apply unconfirmed discard")
        status, response = self.apply_discard(active, plan)
        if status != 409 or response.get("error", {}).get("code") != "observer-reclamation-ineligible" or \
                self.storage_signature(active) != before:
            raise IntegrationFailure("discard read/denial changed active state")
        self.revoke(active)
        plan = self.discard_plan(active)
        for body in ({"plan": plan}, {"plan": plan, "confirmation": "archive"},
                     {"plan": plan, "confirmation": "discard-unconfirmed-history", "extra": True}):
            before = self.storage_signature(active)
            status, _ = self.post_json(f"/api/v1/observer-streams/{active}/discard", body, headers=self.auth_headers("admin"))
            if status != 400 or self.storage_signature(active) != before:
                raise IntegrationFailure("discard accepted missing, incorrect or surplus intent fields")
        confirmed, _, _, _, _ = self.retire_full("discard-confirmed")
        if self.discard_plan(confirmed, eligible=False).get("blocked_reason") != "no-unconfirmed-history":
            raise IntegrationFailure("confirmed history did not retain its original reclamation route")

    def check_discard_replay_and_reenrollment(self) -> None:
        for cycle in range(2):
            stream, enrollment, plan = self.pending_discard(f"discard-cycle-{cycle}")
            before = self.storage_signature(stream)
            ordinary = self.plan(stream, eligible=False, blocked_reason="unconfirmed-history")
            self.expect_reclamation_error(stream, ordinary, "observer-reclamation-ineligible")
            self.expect_reclamation_error(stream, plan, "observer-reclamation-stale")
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
                outcomes = list(workers.map(lambda _: self.apply_discard(stream, plan), range(4)))
            if any(status != 200 for status, _ in outcomes) or \
                    sum(response.get("result", {}).get("replayed") is False for _, response in outcomes) != 1:
                raise IntegrationFailure("concurrent discard did not produce one original and three replayed results")
            receipts = [self.assert_discard_receipt(response, plan) for _, response in outcomes]
            if any(receipt != receipts[0] for receipt in receipts):
                raise IntegrationFailure("discard retries returned different durable receipts")
            after = self.storage_signature(stream)
            if after["stream"] != "missing" or after["usage_streams"] != before["usage_streams"] - 1 or \
                    after["usage_submissions"] != before["usage_submissions"] - 2 or \
                    after["usage_retained_bytes"] != before["usage_retained_bytes"] - plan["delete_bytes"] or \
                    after["usage_tombstones"] != before["usage_tombstones"] + 1 or \
                    self.count("audit_events", f"observer_stream_id='{stream}' AND action='observer.discarded'") != 1:
                raise IntegrationFailure("discard did not atomically release history/live registry and preserve its identity")
            self.stop_server(expect_clean=True)
            self.start_server()
            if self.discard_plan(stream) != plan:
                raise IntegrationFailure("saved discard plan was unavailable after restart")
            status, replay = self.apply_discard(stream, plan)
            if status != 200 or replay.get("result", {}).get("replayed") is not True or \
                    self.assert_discard_receipt(replay, plan) != receipts[0]:
                raise IntegrationFailure("lost discard acknowledgement was not replayable after restart")
            status, response = self.post_json("/api/v1/observer-streams", enrollment, headers=self.auth_headers("admin"))
            if status != 410 or response.get("error", {}).get("code") != "observer-stream-retired":
                raise IntegrationFailure("discard allowed old stream identity resurrection")
            replacement = {**enrollment, "stream_id": secrets.token_hex(16)}
            status, response = self.post_json("/api/v1/observer-streams", replacement, headers=self.auth_headers("admin"))
            if status != 201 or response.get("result", {}).get("stream", {}).get("accepted_sequence") != 0:
                raise IntegrationFailure("explicit fresh enrollment inherited discarded delivery progress")
            self.discarded = (stream, plan)

    def check_discard_content_capacity_and_rollback(self) -> None:
        stream, _, plan = self.pending_discard("discard-atomic")
        before = self.storage_signature(stream)
        self.psql(f"UPDATE shin_server.observer_submissions SET submission=submission || '{{\"fixture_drift\":true}}'::jsonb WHERE stream_id='{stream}'")
        status, response = self.apply_discard(stream, plan)
        if status != 409 or response.get("error", {}).get("code") != "observer-reclamation-stale" or \
                self.storage_signature(stream) != before:
            raise IntegrationFailure("discard ignored same-counter retained payload drift")
        self.psql(f"UPDATE shin_server.observer_submissions SET submission=submission - 'fixture_drift' WHERE stream_id='{stream}'")
        fixture = f"discard-capacity-{secrets.token_hex(8)}"
        needed = 512 - self.count("observer_tombstones", f"project_id='{PROJECT_ID}'")
        self.psql("INSERT INTO shin_server.observer_tombstones(stream_id,project_id,workspace_id,principal_id,enrollment_hash,retirement) "
                  f"SELECT md5('{fixture}:' || n::text),'{PROJECT_ID}','{WORKSPACE_ID}','dev-reader',repeat('a',64),"
                  f"jsonb_build_object('fixture','{fixture}') FROM generate_series(1,{needed}) n;"
                  f"UPDATE shin_server.observer_usage SET tombstones=tombstones+{needed} WHERE singleton")
        try:
            before = self.storage_signature(stream)
            status, response = self.apply_discard(stream, plan)
            if status != 409 or response.get("error", {}).get("code") != "observer-tombstone-capacity" or \
                    self.storage_signature(stream) != before:
                raise IntegrationFailure("discard bypassed saturated protected-identity capacity")
        finally:
            self.remove_tombstones(fixture, needed)
        self.psql("CREATE FUNCTION shin_server.fail_discard_fixture() RETURNS trigger LANGUAGE plpgsql AS $$ "
                  "BEGIN RAISE EXCEPTION 'discard fixture failure'; END $$;"
                  "CREATE TRIGGER fail_discard_fixture AFTER DELETE ON shin_server.observer_submissions "
                  "FOR EACH ROW EXECUTE FUNCTION shin_server.fail_discard_fixture()")
        try:
            before = self.storage_signature(stream)
            status, _ = self.apply_discard(stream, plan)
            if status < 500 or self.storage_signature(stream) != before:
                raise IntegrationFailure("failed discard did not roll back payload, quotas, tombstone and audit")
        finally:
            self.psql("DROP TRIGGER fail_discard_fixture ON shin_server.observer_submissions;"
                      "DROP FUNCTION shin_server.fail_discard_fixture()")
        status, response = self.apply_discard(stream, plan)
        if status != 200:
            raise IntegrationFailure("discard could not retry after rollback")
        self.assert_discard_receipt(response, plan)

    def enroll(self, label: str) -> tuple[str, str, dict[str, Any]]:
        stream = secrets.token_hex(16)
        observer = f"observer-reclamation-{label}-{secrets.token_hex(4)}"
        enrollment = self.history_enrollment(stream, observer)
        status, response = self.post_json("/api/v1/observer-streams", enrollment, headers=self.auth_headers("admin"))
        if status != 201 or response.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("reclamation fixture enrollment was not accepted")
        return stream, observer, enrollment

    def accept_for(self, stream: str, observer: str, sequence: int = 1) -> tuple[dict[str, Any], dict[str, Any]]:
        submission = self.synthetic_submission(sequence, stream=stream, observer_id=observer)
        status, response = self.post_json(f"/api/v1/observer-streams/{stream}/submissions", submission, headers=self.owner_headers)
        if status != 201 or response.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("reclamation fixture submission was not accepted")
        return submission, self.assert_history_receipt(response.get("result", {}).get("receipt"), submission)

    def confirm_for(self, stream: str, receipt: dict[str, Any]) -> None:
        status, response = self.post_json(f"/api/v1/observer-streams/{stream}/confirmations", {"receipt": receipt}, headers=self.owner_headers)
        result = response.get("result")
        if status != 200 or not isinstance(result, dict) or result.get("confirmed_sequence") != receipt["sequence"]:
            raise IntegrationFailure("reclamation fixture confirmation did not persist")

    def revoke(self, stream: str) -> None:
        status, response = self.post_json(f"/api/v1/observer-streams/{stream}/revoke", {}, headers=self.auth_headers("admin"))
        if status != 200 or response.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("reclamation fixture stream was not revoked")

    def plan(self, stream: str, *, eligible: bool | None = None, blocked_reason: str | None = None,
             project_id: str = PROJECT_ID, workspace_id: str = WORKSPACE_ID,
             headers: dict[str, str] | None = None) -> dict[str, Any]:
        status, response = self.reclamation(stream, headers=headers)
        value = response.get("result")
        fields = {
            "schema_version", "eligible", "blocked_reason", "server_id", "stream_id", "project_id", "workspace_id",
            "enrollment_hash", "accepted_sequence", "confirmed_sequence", "retired_sequence", "observed_sequence",
            "delete_submissions", "delete_bytes", "plan_id", "source_verified", "shared_publication_mutated",
        }
        if status != 200 or not isinstance(value, dict) or set(value) != fields or \
                value.get("schema_version") != "shin.observer-reclamation-plan.v1" or value.get("server_id") != self.server_identity or \
                value.get("stream_id") != stream or value.get("project_id") != project_id or value.get("workspace_id") != workspace_id or \
                value.get("source_verified") is not False or value.get("shared_publication_mutated") is not False or \
                not isinstance(value.get("eligible"), bool) or value.get("blocked_reason") not in {None, "active-stream", "unconfirmed-history"}:
            raise IntegrationFailure("reclamation inspection did not return the direct public plan")
        self.require_hex(value.get("enrollment_hash"), 64, "reclamation plan enrollment hash is invalid")
        self.require_hex(value.get("plan_id"), 64, "reclamation plan identity is invalid")
        if any(type(value[field]) is not int or value[field] < 0 for field in (
                "accepted_sequence", "confirmed_sequence", "retired_sequence", "observed_sequence", "delete_submissions", "delete_bytes")):
            raise IntegrationFailure("reclamation plan contains an invalid counter")
        if eligible is not None and value["eligible"] is not eligible:
            raise IntegrationFailure("reclamation plan changed eligibility unexpectedly")
        if blocked_reason is not None and value["blocked_reason"] != blocked_reason:
            raise IntegrationFailure("reclamation plan returned the wrong blocking reason")
        return value

    def storage_signature(self, stream: str) -> dict[str, int | str]:
        stream_state = self.scalar(
            "SELECT COALESCE((SELECT accepted_sequence::text || ':' || confirmed_sequence::text || ':' || "
            "retired_sequence::text || ':' || observed_sequence::text || ':' || retained_bytes::text "
            f"FROM shin_server.observer_streams WHERE stream_id='{stream}'),'missing')"
        )
        tombstone_count, tombstone_bytes = (int(value) for value in self.scalar(
            "SELECT count(*)::text || ':' || COALESCE(sum(octet_length(retirement::text)),0)::text "
            "FROM shin_server.observer_tombstones"
        ).split(":"))
        usage = (int(value) for value in self.scalar(
            "SELECT streams::text || ':' || submissions::text || ':' || retained_bytes::text || ':' || tombstones::text "
            "FROM shin_server.observer_usage WHERE singleton"
        ).split(":"))
        streams, submissions, retained_bytes, tombstones = usage
        return {
            "stream": stream_state,
            "stream_submissions": self.count("observer_submissions", f"stream_id='{stream}'"),
            "stream_tombstones": self.count("observer_tombstones", f"stream_id='{stream}'"),
            "stream_audit": self.count("audit_events", f"observer_stream_id='{stream}'"),
            "tombstone_count": tombstone_count,
            "tombstone_bytes": tombstone_bytes,
            "usage_streams": streams,
            "usage_submissions": submissions,
            "usage_retained_bytes": retained_bytes,
            "usage_tombstones": tombstones,
        }

    def assert_reclamation_receipt(self, response: dict[str, Any], plan: dict[str, Any], *, replayed: bool) -> dict[str, Any]:
        result = response.get("result")
        receipt = result.get("receipt") if isinstance(result, dict) else None
        result_fields = {"receipt", "replayed"}
        receipt_fields = {
            "schema_version", "server_id", "stream_id", "plan_id", "principal_id", "accepted_sequence",
            "confirmed_sequence", "deleted_submissions", "deleted_bytes", "identity_retained", "audit_preserved",
            "source_verified", "shared_publication_mutated",
        }
        if not isinstance(result, dict) or set(result) != result_fields or result.get("replayed") is not replayed or \
                not isinstance(receipt, dict) or set(receipt) != receipt_fields or \
                receipt.get("schema_version") != "shin.observer-reclamation-receipt.v1" or \
                receipt.get("server_id") != self.server_identity or receipt.get("stream_id") != plan["stream_id"] or \
                receipt.get("plan_id") != plan["plan_id"] or receipt.get("principal_id") != "dev-admin" or \
                receipt.get("accepted_sequence") != plan["accepted_sequence"] or \
                receipt.get("confirmed_sequence") != plan["confirmed_sequence"] or \
                receipt.get("deleted_submissions") != plan["delete_submissions"] or \
                receipt.get("deleted_bytes") != plan["delete_bytes"] or \
                receipt.get("identity_retained") is not True or receipt.get("audit_preserved") is not True or \
                receipt.get("source_verified") is not False or receipt.get("shared_publication_mutated") is not False or \
                any(type(receipt[field]) is not int or receipt[field] < 0 for field in
                    ("accepted_sequence", "confirmed_sequence", "deleted_submissions", "deleted_bytes")):
            raise IntegrationFailure("reclamation apply did not return a bound receipt")
        return receipt

    def expect_reclamation_error(self, stream: str, plan: dict[str, Any], code: str) -> None:
        status, response = self.apply_reclamation(stream, plan)
        if status != 409 or response.get("error", {}).get("code") != code:
            raise IntegrationFailure("reclamation apply returned the wrong public failure")

    def retire_full(self, label: str) -> tuple[str, dict[str, Any], dict[str, Any], dict[str, Any], dict[str, Any]]:
        stream, observer, enrollment = self.enroll(label)
        submission, acknowledgement = self.accept_for(stream, observer)
        self.confirm_for(stream, acknowledgement)
        self.revoke(stream)
        plan = self.plan(stream, eligible=True)
        if (plan["accepted_sequence"], plan["confirmed_sequence"], plan["retired_sequence"], plan["observed_sequence"],
                plan["delete_submissions"]) != (1, 1, 0, 1, 1):
            raise IntegrationFailure("fully confirmed revoked stream did not produce a full retirement plan")
        return stream, enrollment, submission, acknowledgement, plan

    def check_zero_active_and_unconfirmed_plans(self) -> None:
        zero, _, _ = self.enroll("zero")
        active = self.plan(zero, eligible=False, blocked_reason="active-stream")
        before = self.storage_signature(zero)
        self.expect_reclamation_error(zero, active, "observer-reclamation-ineligible")
        if self.storage_signature(zero) != before:
            raise IntegrationFailure("active stream reclamation changed durable state")
        self.revoke(zero)
        zero_plan = self.plan(zero, eligible=True)
        before = self.storage_signature(zero)
        self.expect_reclamation_error(zero, active, "observer-reclamation-stale")
        if self.storage_signature(zero) != before:
            raise IntegrationFailure("stale pre-revocation plan changed durable state")
        if (zero_plan["accepted_sequence"], zero_plan["confirmed_sequence"], zero_plan["retired_sequence"],
                zero_plan["observed_sequence"], zero_plan["delete_submissions"], zero_plan["delete_bytes"]) != (0, 0, 0, 0, 0, 0):
            raise IntegrationFailure("revoked empty stream was not eligible for zero-payload retirement")
        status, applied = self.apply_reclamation(zero, zero_plan)
        zero_after = self.storage_signature(zero)
        if status != 200 or self.assert_reclamation_receipt(applied, zero_plan, replayed=False).get("stream_id") != zero or \
                (zero_after["stream"], zero_after["stream_submissions"], zero_after["stream_tombstones"]) != ("missing", 0, 1):
            raise IntegrationFailure("zero-payload retirement did not replace the revoked stream with a tombstone")

        pending, observer, _ = self.enroll("unconfirmed")
        self.accept_for(pending, observer)
        self.revoke(pending)
        unconfirmed = self.plan(pending, eligible=False, blocked_reason="unconfirmed-history")
        before = self.storage_signature(pending)
        self.expect_reclamation_error(pending, unconfirmed, "observer-reclamation-ineligible")
        if self.storage_signature(pending) != before:
            raise IntegrationFailure("unconfirmed stream reclamation changed durable state")

    def check_full_retirement_replay_and_tombstone_boundary(self) -> None:
        stream, enrollment, submission, acknowledgement, plan = self.retire_full("full")
        before = self.storage_signature(stream)
        status, applied = self.apply_reclamation(stream, plan)
        if status != 200:
            raise IntegrationFailure("first full-stream reclamation did not return HTTP 200")
        receipt = self.assert_reclamation_receipt(applied, plan, replayed=False)
        after = self.storage_signature(stream)
        if (after["stream"], after["stream_submissions"], after["stream_tombstones"]) != ("missing", 0, 1) or \
                after["usage_streams"] != before["usage_streams"] - 1 or \
                after["usage_submissions"] != before["usage_submissions"] - plan["delete_submissions"] or \
                after["usage_retained_bytes"] != before["usage_retained_bytes"] - plan["delete_bytes"] or \
                after["usage_tombstones"] != before["usage_tombstones"] + 1:
            raise IntegrationFailure("reclamation did not remove the stream and free retained accounting")
        if after["stream_audit"] <= before["stream_audit"] or self.count("audit_events", f"observer_stream_id='{stream}' AND observer_sequence=1") == 0:
            raise IntegrationFailure("reclamation did not preserve and extend immutable audit history")
        if self.scalar("SELECT tombstones::text FROM shin_server.observer_usage WHERE singleton") != \
                self.scalar("SELECT count(*)::text FROM shin_server.observer_tombstones"):
            raise IntegrationFailure("tombstone counter does not equal durable retired stream rows")

        self.stop_server(expect_clean=True)
        self.start_server()
        replay_status, replay = self.apply_reclamation(stream, plan)
        if replay_status != 200 or self.assert_reclamation_receipt(replay, plan, replayed=True) != receipt:
            raise IntegrationFailure("exact latest reclamation retry was not durable under the same Server identity")
        tombstone_status, tombstone = self.reclamation(stream)
        if tombstone_status != 200 or tombstone.get("result") != plan:
            raise IntegrationFailure("authorized reclamation inspection did not return the saved current tombstone plan")
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            outcomes = list(workers.map(lambda _: self.apply_reclamation(stream, plan), range(4)))
        if any(status != 200 or self.assert_reclamation_receipt(response, plan, replayed=True) != receipt for status, response in outcomes):
            raise IntegrationFailure("concurrent exact reclamation retries did not preserve one durable receipt")
        for method, target, body in (
            ("GET", f"/api/v1/observer-streams/{stream}", None),
            ("POST", f"/api/v1/observer-streams/{stream}/submissions", self.compact_json(submission)),
            ("POST", f"/api/v1/observer-streams/{stream}/confirmations", self.compact_json({"receipt": acknowledgement})),
            ("POST", f"/api/v1/observer-streams/{stream}/reconciliation", self.compact_json({
                "schema_version": "shin.observer-reconcile-request.v1", "server_id": self.server_identity, "stream_id": stream,
                "acknowledged_sequence": 1, "accepted_sequence": 1, "last_receipt": acknowledgement, "next_submission": None,
            })),
        ):
            headers = {**self.owner_headers, **({"Content-Type": "application/json"} if body is not None else {})}
            self.expect_error(410, "observer-stream-retired", method, target, headers=headers, body=body)
        changed_enrollment = {**enrollment, "observer_id": enrollment["observer_id"] + "-other"}
        self.expect_error(410, "observer-stream-retired", "POST", "/api/v1/observer-streams", headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
                          body=self.compact_json(changed_enrollment))
        if self.other_headers is None:
            raise IntegrationFailure("reclamation fixture lacks the cross-owner principal")
        self.expect_error(404, "observer-not-found", "GET", f"/api/v1/observer-streams/{stream}", headers=self.other_headers)
        self.expect_error(404, "observer-not-found", "GET", f"/api/v1/observer-streams/{stream}/reclamation", headers=self.other_headers)
        self.reclaimed = (stream, enrollment, submission, plan)

    def check_concurrent_first_reclamation(self) -> None:
        stream, _, _, _, plan = self.retire_full("concurrent-first")
        before = self.storage_signature(stream)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            outcomes = list(workers.map(lambda _: self.apply_reclamation(stream, plan), range(4)))
        if any(status != 200 for status, _ in outcomes) or \
                sum(response.get("result", {}).get("replayed") is False for _, response in outcomes) != 1 or \
                sum(response.get("result", {}).get("replayed") is True for _, response in outcomes) != 3:
            raise IntegrationFailure("concurrent first reclamation did not produce one apply and three exact replays")
        receipts = [self.assert_reclamation_receipt(response, plan, replayed=response["result"]["replayed"]) for _, response in outcomes]
        if any(receipt != receipts[0] for receipt in receipts):
            raise IntegrationFailure("concurrent first reclamation did not preserve one receipt")
        after = self.storage_signature(stream)
        if after["stream_audit"] != before["stream_audit"] + 1 or \
                after["usage_streams"] != before["usage_streams"] - 1 or \
                after["usage_tombstones"] != before["usage_tombstones"] + 1:
            raise IntegrationFailure("concurrent first reclamation changed audit or counters more than once")

    def check_per_project_reclamation_capacity(self) -> None:
        fillers: list[str] = []
        while self.count("observer_streams", f"project_id='{PROJECT_ID}'") < 128:
            stream, _, _ = self.enroll("capacity")
            fillers.append(stream)
        overflow = self.history_enrollment(secrets.token_hex(16), f"observer-reclamation-overflow-{secrets.token_hex(4)}")
        status, response = self.post_json("/api/v1/observer-streams", overflow, headers=self.auth_headers("admin"))
        if status != 409 or response.get("error", {}).get("code") != "observer-capacity":
            raise IntegrationFailure("per-project Observer registry capacity was not enforced")
        reclaim = fillers[0] if fillers else self.scalar(
            "SELECT stream_id FROM shin_server.observer_streams "
            f"WHERE project_id='{PROJECT_ID}' AND revoked=false ORDER BY stream_id LIMIT 1"
        )
        self.revoke(reclaim)
        plan = self.plan(reclaim, eligible=True)
        status, applied = self.apply_reclamation(reclaim, plan)
        if status != 200:
            raise IntegrationFailure("empty stream at per-project capacity was not reclaimed")
        self.assert_reclamation_receipt(applied, plan, replayed=False)
        replacement = self.history_enrollment(secrets.token_hex(16), f"observer-reclamation-replacement-{secrets.token_hex(4)}")
        status, response = self.post_json("/api/v1/observer-streams", replacement, headers=self.auth_headers("admin"))
        if status != 201 or response.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("reclaimed per-project capacity did not admit a replacement stream")
        self.revoke(replacement["stream_id"])
        cleanup = self.plan(replacement["stream_id"], eligible=True)
        status, cleanup_response = self.apply_reclamation(replacement["stream_id"], cleanup)
        if status != 200:
            raise IntegrationFailure("per-project capacity fixture did not clean up its replacement stream")
        self.assert_reclamation_receipt(cleanup_response, cleanup, replayed=False)

    def register_fixture_project(self, project: str) -> None:
        if not project.startswith("reclamation-"):
            raise IntegrationFailure("capacity fixture must use its isolated project namespace")
        self.psql(
            f"INSERT INTO shin_server.projects(id,display_name) VALUES ('{project}','{project}') ON CONFLICT (id) DO NOTHING;"
            "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,source_kind) "
            f"VALUES ('{project}','observer','fixture-host','Fixture observer','observer') "
            "ON CONFLICT (project_id,id) DO NOTHING"
        )
        self.reclamation_projects.add(project)

    def cleanup_reclamation_projects(self) -> None:
        for project in sorted(self.reclamation_projects):
            self.psql(f"DELETE FROM shin_server.workspaces WHERE project_id='{project}';"
                      f"DELETE FROM shin_server.projects WHERE id='{project}'")
        self.reclamation_projects.clear()

    def seed_tombstones(self, project: str, count: int, fixture: str, offset: int) -> None:
        if count < 1 or count > 512:
            raise IntegrationFailure("invalid bounded tombstone fixture size")
        self.register_fixture_project(project)
        self.psql(
            "INSERT INTO shin_server.observer_tombstones("
            "stream_id,project_id,workspace_id,principal_id,enrollment_hash,retirement) "
            f"SELECT md5('{fixture}:' || series::text),'{project}','observer','dev-reader',repeat('a',64),"
            f"jsonb_build_object('fixture','{fixture}','plan',jsonb_build_object(),'receipt',jsonb_build_object()) "
            f"FROM generate_series({offset + 1},{offset + count}) AS series;"
            f"UPDATE shin_server.observer_usage SET tombstones=tombstones+{count} WHERE singleton"
        )

    def remove_tombstones(self, fixture: str, count: int) -> None:
        removed = int(self.scalar(
            f"WITH removed AS (DELETE FROM shin_server.observer_tombstones WHERE retirement->>'fixture'='{fixture}' RETURNING 1) "
            "SELECT count(*) FROM removed"
        ))
        if removed != count:
            raise IntegrationFailure("tombstone fixture cleanup did not remove its exact owned rows")
        self.psql(f"UPDATE shin_server.observer_usage SET tombstones=tombstones-{count} WHERE singleton")

    def seed_revoked_empty_stream(self, project: str) -> str:
        stream = secrets.token_hex(16)
        self.register_fixture_project(project)
        self.psql(
            "INSERT INTO shin_server.observer_streams("
            "stream_id,project_id,workspace_id,principal_id,observer_id,source_host_id,profile,binding_digest,enrollment_hash,revoked) "
            f"VALUES ('{stream}','{project}','observer','dev-reader','fixture-{stream[:8]}','fixture-host','fixture',"
            "repeat('b',64),repeat('c',64),TRUE);"
            "UPDATE shin_server.observer_usage SET streams=streams+1 WHERE singleton"
        )
        return stream

    def remove_fixture_stream(self, stream: str) -> None:
        self.psql(
            f"DELETE FROM shin_server.observer_streams WHERE stream_id='{stream}';"
            "UPDATE shin_server.observer_usage SET streams=streams-1 WHERE singleton"
        )

    def assert_tombstone_accounting(self, stream: str) -> None:
        state = self.storage_signature(stream)
        if state["usage_tombstones"] != state["tombstone_count"]:
            raise IntegrationFailure("tombstone usage counter does not equal durable tombstone rows")

    def check_tombstone_capacity_and_atomic_failure(self) -> None:
        project_fixture = f"reclamation-project-{secrets.token_hex(4)}"
        project_rows = 512
        target = self.seed_revoked_empty_stream(project_fixture)
        fixture = f"project-{secrets.token_hex(8)}"
        project_seeded = False
        try:
            self.seed_tombstones(project_fixture, project_rows, fixture, 0)
            project_seeded = True
            self.assert_tombstone_accounting(target)
            plan = self.plan(target, eligible=True, project_id=project_fixture, workspace_id="observer", headers=self.auth_headers("admin"))
            before = self.storage_signature(target)
            self.expect_reclamation_error(target, plan, "observer-tombstone-capacity")
            if self.storage_signature(target) != before:
                raise IntegrationFailure("per-project tombstone capacity rejection did not roll back state")
        finally:
            if project_seeded:
                self.remove_tombstones(fixture, project_rows)
            self.remove_fixture_stream(target)

        stream, _, _, _, plan = self.retire_full("global-capacity")
        before = self.storage_signature(stream)
        needed = 4096 - int(before["tombstone_count"])
        if needed < 1 or self.count("observer_tombstones", f"project_id='{PROJECT_ID}'") >= 512:
            raise IntegrationFailure("global tombstone fixture has no bounded target project capacity")
        global_fixture = f"global-{secrets.token_hex(8)}"
        seeded: list[int] = []
        try:
            offset = 0
            while offset < needed:
                count = min(512, needed - offset)
                project = f"reclamation-global-{len(seeded)}-{secrets.token_hex(3)}"
                self.seed_tombstones(project, count, global_fixture, offset)
                seeded.append(count)
                offset += count
            saturated = self.storage_signature(stream)
            if saturated["tombstone_count"] != 4096 or saturated["usage_tombstones"] != 4096:
                raise IntegrationFailure("global tombstone fixture did not establish counter-consistent capacity")
            self.expect_reclamation_error(stream, plan, "observer-tombstone-capacity")
            if self.storage_signature(stream) != saturated:
                raise IntegrationFailure("global tombstone capacity rejection did not roll back stream state")
        finally:
            self.remove_tombstones(global_fixture, sum(seeded))
            self.assert_tombstone_accounting(stream)

        self.psql(
            "CREATE FUNCTION shin_server.fail_reclamation_fixture() RETURNS trigger LANGUAGE plpgsql AS $$ "
            "BEGIN RAISE EXCEPTION 'reclamation fixture failure'; END $$;"
            "CREATE TRIGGER fail_reclamation_fixture AFTER DELETE ON shin_server.observer_submissions "
            "FOR EACH ROW EXECUTE FUNCTION shin_server.fail_reclamation_fixture()"
        )
        try:
            before = self.storage_signature(stream)
            status, _ = self.apply_reclamation(stream, plan)
            if status < 500 or self.storage_signature(stream) != before:
                raise IntegrationFailure("post-delete failure did not roll back payload, quota, tombstone, and audit")
        finally:
            self.psql("DROP TRIGGER IF EXISTS fail_reclamation_fixture ON shin_server.observer_submissions;"
                      "DROP FUNCTION IF EXISTS shin_server.fail_reclamation_fixture()")
        status, retried = self.apply_reclamation(stream, plan)
        if status != 200:
            raise IntegrationFailure("valid reclamation could not retry after a rolled-back failure")
        self.assert_reclamation_receipt(retried, plan, replayed=False)

    def write_recovery_input(self, value: dict[str, Any]) -> Path:
        path = self.state_root / "config" / f"reclamation-recovery-{secrets.token_hex(8)}.json"
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(self.compact_json(value) + b"\n")
        self.reclamation_private_paths.append(path)
        return path

    def recovery_cli(self, command: str, input_value: dict[str, Any], expected_error: str | None = None,
                     *, config_path: Path | None = None) -> dict[str, Any]:
        completed = subprocess.run(
            [str(self.binary), command, "--config", str(config_path or self.state_root / "config" / "recovery.json"),
             "--input", str(self.write_recovery_input(input_value))],
            check=False, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45.0,
        )
        if expected_error is not None:
            if completed.returncode != 1 or completed.stdout:
                raise IntegrationFailure("offline recovery did not reject changed protected state")
            value = json.loads(completed.stderr)
            self.assert_envelope(value)
            if value.get("error", {}).get("code") != expected_error:
                raise IntegrationFailure("offline recovery returned the wrong rejection: expected " + expected_error +
                                         ", got " + str(value.get("error", {}).get("code")))
            return value
        if completed.returncode != 0 or completed.stderr:
            raise IntegrationFailure(f"{command} failed for the owned offline recovery fixture")
        try:
            value = json.loads(completed.stdout)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise IntegrationFailure(f"{command} did not return a JSON envelope") from error
        self.assert_envelope(value)
        return value

    def check_recovery_identity_fences_old_plan(self) -> None:
        stream, _, _, _, plan = self.retire_full("recovery")
        status, applied = self.apply_reclamation(stream, plan)
        if status != 200:
            raise IntegrationFailure("recovery fence fixture did not create a retired stream tombstone")
        self.assert_reclamation_receipt(applied, plan, replayed=False)
        forged = {**plan, "plan_id": "0" * 64}
        self.expect_reclamation_error(stream, forged, "observer-reclamation-stale")
        before = self.storage_signature(stream)
        old_identity = self.server_identity
        request = {
            "schema_version": "shin.server-recovery-request.v1", "operation_id": secrets.token_hex(16),
            "expected_server_id": old_identity, "reason_code": "backup-restore", "service_fenced": True,
        }
        self.stop_server(expect_clean=True)
        planned = self.recovery_cli("recovery-plan", request).get("result")
        recovery_plan = planned.get("plan") if isinstance(planned, dict) else None
        if not isinstance(planned, dict) or planned.get("read_only") is not True or planned.get("restored_backup_verified") is not False or \
                not isinstance(recovery_plan, dict) or recovery_plan.get("schema_version") != "shin.server-recovery-plan.v2" or \
                recovery_plan.get("tombstone_count") != before["tombstone_count"]:
            raise IntegrationFailure("offline recovery plan did not bind existing tombstones")
        self.require_hex(recovery_plan.get("tombstone_state_sha256"), 64, "recovery plan tombstone hash is invalid")
        replayed_plan = self.recovery_cli("recovery-plan", request).get("result")
        if not isinstance(replayed_plan, dict) or replayed_plan.get("plan") != recovery_plan:
            raise IntegrationFailure("offline recovery plan did not preserve a deterministic tombstone-state hash")
        tombstones_before = self.scalar("SELECT jsonb_agg(to_jsonb(t) ORDER BY stream_id)::text FROM shin_server.observer_tombstones t")
        retirement_before = self.scalar(f"SELECT retirement::text FROM shin_server.observer_tombstones WHERE stream_id='{stream}'")
        try:
            self.psql(f"UPDATE shin_server.observer_tombstones SET retirement=retirement || '{{\"fixture_drift\":true}}'::jsonb WHERE stream_id='{stream}'")
            self.recovery_cli("recovery-apply", {"plan": recovery_plan}, "recovery-plan-stale")
            if self.scalar("SELECT server_id FROM shin_server.server_identity") != old_identity:
                raise IntegrationFailure("stale tombstone recovery plan rotated identity")
        finally:
            literal = retirement_before.replace("'", "''")
            self.psql(f"UPDATE shin_server.observer_tombstones SET retirement='{literal}'::jsonb WHERE stream_id='{stream}'")
        applied_recovery = self.recovery_cli("recovery-apply", {"plan": recovery_plan}).get("result")
        recovery_receipt = applied_recovery.get("receipt") if isinstance(applied_recovery, dict) else None
        if not isinstance(applied_recovery, dict) or applied_recovery.get("replayed") is not False or not isinstance(recovery_receipt, dict) or \
                recovery_receipt.get("schema_version") != "shin.server-recovery-receipt.v2" or \
                recovery_receipt.get("retained_tombstones") != before["tombstone_count"] or \
                recovery_receipt.get("server_id") != recovery_plan.get("new_server_id"):
            raise IntegrationFailure("offline recovery receipt did not preserve tombstone count and successor identity")
        self.recovery_applied = True
        if self.storage_signature(stream)["tombstone_count"] != before["tombstone_count"] or \
                self.scalar("SELECT jsonb_agg(to_jsonb(t) ORDER BY stream_id)::text FROM shin_server.observer_tombstones t") != tombstones_before:
            raise IntegrationFailure("offline recovery rewrote durable tombstone rows")
        saved_recovery = json.loads(self.scalar("SELECT last_recovery::text FROM shin_server.server_recovery WHERE singleton"))
        if saved_recovery.get("plan", {}).get("tombstone_state_sha256") != recovery_plan["tombstone_state_sha256"]:
            raise IntegrationFailure("offline recovery did not persist the reviewed tombstone-state hash")
        path = self.state_root / "config" / "runtime.json"
        config = json.loads(path.read_text(encoding="utf-8"))
        config["database"]["server_id"] = recovery_plan["new_server_id"]
        self.replace_private_json(path, config)
        self.server_identity = recovery_plan["new_server_id"]
        self.owner_headers = self.auth_headers("reader")
        self.start_server()
        self.owner_headers = self.auth_headers("reader")
        status, response = self.reclamation(stream)
        if status != 409 or response.get("error", {}).get("code") != "observer-reclamation-stale":
            raise IntegrationFailure("recovery identity did not fence old tombstone reclamation inspection")
        self.expect_reclamation_error(stream, plan, "observer-reclamation-stale")

        if self.discarded is not None:
            discarded, discard_plan = self.discarded
            status, response = self.apply_discard(discarded, discard_plan)
            if status != 409 or response.get("error", {}).get("code") != "observer-reclamation-stale":
                raise IntegrationFailure("recovery identity did not fence an old discard retry")

    def run(self) -> None:
        super().run()
        try:
            if self.entry_one is None or not self.server_identity:
                raise IntegrationFailure("history fixture did not provide an Observer submission and Server identity")
            self.prepare_observer_fixture()
            if self.entry_one.get("payload", {}).get("profile") != PROFILE:
                raise IntegrationFailure("reclamation fixture did not produce the current observer profile payload")
            fixture_source_before = self.source_metadata()
            source_before = self.scalar(
                "SELECT COALESCE(string_agg(project_id || ':' || id || ':' || source_kind, ',' ORDER BY project_id,id), '') "
                "FROM shin_server.workspaces"
            )
            publications_before = self.active_routes_signature()
            self.owner_headers = self.auth_headers("reader")
            self.add_non_owner_reader()
            self.start_server()
            self.record_reclamation("observer-reclamation-eligibility", self.check_zero_active_and_unconfirmed_plans)
            self.record_reclamation("observer-discard-authority-and-eligibility", self.check_discard_authority_and_eligibility)
            self.record_reclamation("observer-discard-replay-and-reenrollment", self.check_discard_replay_and_reenrollment)
            self.record_reclamation("observer-discard-content-capacity-and-rollback", self.check_discard_content_capacity_and_rollback)
            self.record_reclamation("observer-reclamation-full-retirement-and-replay", self.check_full_retirement_replay_and_tombstone_boundary)
            self.record_reclamation("observer-reclamation-concurrent-first-apply", self.check_concurrent_first_reclamation)
            self.record_reclamation("observer-reclamation-per-project-capacity", self.check_per_project_reclamation_capacity)
            self.record_reclamation("observer-reclamation-atomic-capacity-and-trigger", self.check_tombstone_capacity_and_atomic_failure)
            self.cleanup_reclamation_projects()
            self.record_reclamation("observer-reclamation-recovery-identity-fence", self.check_recovery_identity_fences_old_plan)
            from observer_identity_retirement_checks import check_observer_identity_retirement
            check_observer_identity_retirement(self)
            self.cleanup_reclamation_projects()
            if source_before != self.scalar(
                "SELECT COALESCE(string_agg(project_id || ':' || id || ':' || source_kind, ',' ORDER BY project_id,id), '') "
                "FROM shin_server.workspaces"
            ) or publications_before != self.active_routes_signature() or self.source_metadata() != fixture_source_before:
                raise IntegrationFailure("reclamation fixture changed source bindings or shared publication state")
        finally:
            self.stop_server(expect_clean=False)
            self.cleanup_reclamation_projects()
            self.restore_runtime_config()
            self.cleanup_observer_fixture()
            for path in self.reclamation_private_paths:
                path.unlink(missing_ok=True)
            if self.recovery_applied:
                self.reset_schema()

    def write_report(self, *, ok: bool, failure: str | None = None) -> None:
        super().write_report(ok=ok, failure=failure)
        report = json.loads(self.report_path.read_text(encoding="utf-8"))
        report["observer_reclamation"] = {
            "checks": self.reclamation_checks,
            "scope": "Real PostgreSQL stream retirement, tombstone capacity, and replay correctness; not a performance claim.",
        }
        self.replace_private_json(self.report_path, report)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="native shin-server executable")
    parser.add_argument("--observer", required=True, help="absolute native shin-observer executable")
    parser.add_argument("--state-root", default=str(BUILD_ROOT / "server-dev"), help="isolated state below this worktree build directory")
    parser.add_argument("--report", help="new report path below this worktree build directory")
    parser.add_argument("--load-requests", type=int, default=1100, help="sequential inherited history load in 0..4096")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        binary = require_executable(args.binary, "binary", absolute=False)
        observer = require_executable(args.observer, "observer", absolute=True)
        state_root = Path(args.state_root).resolve(strict=False)
        report_path = (Path(args.report).resolve(strict=False) if args.report else
                       state_root / "reports" / f"observer-reclamation-{secrets.token_hex(8)}.json")
        if not under_build(state_root) or not under_build(report_path) or report_path.exists():
            raise IntegrationFailure("state root and report must be new paths below this worktree build directory")
        if args.load_requests < 0 or args.load_requests > 4096:
            raise IntegrationFailure("--load-requests must be in 0..4096")
    except IntegrationFailure as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    suite = ObserverReclamationIntegrationSuite(binary, observer, state_root, report_path)
    suite.load_requests = args.load_requests
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
        print(f"error: observer reclamation integration test failed: {type(error).__name__}", file=sys.stderr)
        return 1
    suite.write_report(ok=True)
    print("observer reclamation integration test passed; compact report written below the isolated build state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
