#!/usr/bin/env python3
"""Real PostgreSQL coverage for confirmed Observer receipt history retention."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import secrets
import sys
import time
from typing import Any

from integration_test import BUILD_ROOT, IntegrationFailure, IntegrationSuite, under_build
from observer_ingestion_integration_test import (
    PROFILE,
    PROJECT_ID,
    SOURCE_HOST_ID,
    WORKSPACE_ID,
    ObserverIngestionIntegrationSuite,
    require_executable,
)


class ObserverHistoryIntegrationSuite(ObserverIngestionIntegrationSuite):
    """Adds history assertions after the bounded ingestion fixture has stopped."""

    def __init__(self, binary: Path, observer: Path, state_root: Path, report_path: Path) -> None:
        super().__init__(binary, observer, state_root, report_path)
        self.history_stream = ""
        self.history_observer = ""
        self.history_receipts: dict[int, dict[str, Any]] = {}
        self.history_checks: list[dict[str, Any]] = []
        self.load_requests = 1100
        self.source_before_history = ""
        self.publications_before_history = ""
        self.history_load_metrics: dict[str, Any] | None = None
        self.first_retention_plan: dict[str, Any] | None = None

    def record_history(self, name: str, operation: Any) -> None:
        self.record(name, operation)
        self.history_checks.append({"name": name})

    def history_enrollment(self, stream: str, observer_id: str) -> dict[str, Any]:
        return {
            "stream_id": stream,
            "project_id": PROJECT_ID,
            "workspace_id": WORKSPACE_ID,
            "principal_id": "dev-reader",
            "observer_id": observer_id,
            "source_host_id": SOURCE_HOST_ID,
            "profile": PROFILE,
            "workspace_binding_digest": self.binding_digest,
        }

    def synthetic_submission(self, sequence: int, *, stream: str | None = None,
                             observer_id: str | None = None, variant: str = "") -> dict[str, Any]:
        if self.entry_one is None:
            raise IntegrationFailure("ingestion fixture did not retain a native submission")
        stream = stream or self.history_stream
        observer_id = observer_id or self.history_observer
        entry = json.loads(json.dumps(self.entry_one))
        entry["stream_id"] = stream
        entry["sequence"] = sequence
        entry["submission_id"] = hashlib.sha256(f"{stream}:{sequence}".encode("utf-8")).hexdigest()
        payload = entry["payload"]
        payload["observer_id"] = observer_id
        payload["observer_sequence"] = sequence
        payload["observation"]["worktree_id"] = f"fnv1a64:{sequence:016x}"
        if variant:
            payload["observation"]["duration_ms"] = float(payload["observation"]["duration_ms"]) + 1.0
        entry["payload_sha256"] = self.payload_digest(payload)
        return entry

    def post_history(self, target: str, body: dict[str, Any], *, headers: dict[str, str] | None = None) -> tuple[int, dict[str, Any]]:
        return self.post_json(target, body, headers=headers or self.owner_headers)

    def post_submission_for(self, stream: str, submission: dict[str, Any]) -> tuple[int, dict[str, Any]]:
        return self.post_history(f"/api/v1/observer-streams/{stream}/submissions", submission)

    def expect_history_error(self, expected_status: int, expected_code: str, target: str, body: dict[str, Any], *,
                             headers: dict[str, str]) -> None:
        status, envelope = self.post_history(target, body, headers=headers)
        if status != expected_status or envelope.get("error", {}).get("code") != expected_code:
            raise IntegrationFailure(f"history endpoint expected HTTP {expected_status}/{expected_code}")

    def assert_history_receipt(self, receipt: Any, submission: dict[str, Any]) -> dict[str, Any]:
        fields = {
            "schema_version", "server_id", "stream_id", "sequence", "submission_id", "payload_sha256",
            "project_id", "workspace_id", "received_at", "verification_level", "server_source_verified",
            "shared_publication_mutated",
        }
        if not isinstance(receipt, dict) or set(receipt) != fields:
            raise IntegrationFailure("history receipt has an invalid public shape")
        if (receipt["schema_version"], receipt["server_id"], receipt["stream_id"], receipt["sequence"],
                receipt["submission_id"], receipt["payload_sha256"], receipt["project_id"], receipt["workspace_id"],
                receipt["verification_level"], receipt["server_source_verified"], receipt["shared_publication_mutated"]) != (
                    "shin.observer-ack.v1", self.server_identity, submission["stream_id"], submission["sequence"],
                    submission["submission_id"], submission["payload_sha256"], PROJECT_ID, WORKSPACE_ID,
                    "producer-observed", False, False):
            raise IntegrationFailure("history receipt does not bind its accepted native submission")
        if not isinstance(receipt["received_at"], str) or not receipt["received_at"]:
            raise IntegrationFailure("history receipt does not contain a received timestamp")
        return receipt

    def accept(self, sequence: int) -> tuple[dict[str, Any], dict[str, Any]]:
        submission = self.synthetic_submission(sequence)
        status, envelope = self.post_submission_for(self.history_stream, submission)
        if status != 201 or envelope.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("synthetic history submission was not accepted exactly once")
        receipt = self.assert_history_receipt(envelope["result"].get("receipt"), submission)
        self.history_receipts[sequence] = receipt
        return submission, receipt

    def confirmation(self, receipt: dict[str, Any], *, headers: dict[str, str] | None = None) -> tuple[int, dict[str, Any]]:
        return self.post_history(
            f"/api/v1/observer-streams/{self.history_stream}/confirmations", {"receipt": receipt}, headers=headers
        )

    def confirm(self, sequence: int) -> None:
        status, envelope = self.confirmation(self.history_receipts[sequence])
        result = envelope.get("result")
        expected = {
            "schema_version": "shin.observer-confirmation.v1",
            "server_id": self.server_identity,
            "stream_id": self.history_stream,
            "confirmed_sequence": sequence,
            "producer_durability_verified": False,
            "shared_publication_mutated": False,
        }
        fields = {
            "schema_version", "server_id", "stream_id", "confirmed_sequence", "retired_sequence",
            "producer_durability_verified", "shared_publication_mutated",
        }
        if status != 200 or not isinstance(result, dict) or set(result) != fields or \
                any(result.get(key) != value for key, value in expected.items()):
            raise IntegrationFailure("owner confirmation did not return its full receipt boundary")

    def stream_signature(self, stream: str | None = None) -> str:
        stream = stream or self.history_stream
        return self.scalar(
            "SELECT accepted_sequence::text || ':' || confirmed_sequence::text || ':' || retired_sequence::text || ':' || "
            "observed_sequence::text || ':' || retained_bytes::text || ':' || COALESCE(last_retention::text,'') "
            f"FROM shin_server.observer_streams WHERE stream_id='{stream}'"
        )

    def usage_signature(self) -> tuple[int, int]:
        return tuple(int(value) for value in self.scalar(
            "SELECT submissions::text || ':' || retained_bytes::text FROM shin_server.observer_usage WHERE singleton"
        ).split(":"))

    def assert_global_usage(self) -> None:
        rows, retained_bytes = (int(value) for value in self.scalar(
            "SELECT count(*)::text || ':' || COALESCE(sum(retained_bytes),0)::text FROM shin_server.observer_submissions"
        ).split(":"))
        if self.usage_signature() != (rows, retained_bytes):
            raise IntegrationFailure("global Observer usage does not equal retained submission rows")

    def reconciliation_request(self, acknowledged: int, accepted: int, *, receipt: dict[str, Any] | None = None,
                               next_submission: dict[str, Any] | None = None, server_id: str | None = None) -> dict[str, Any]:
        return {
            "schema_version": "shin.observer-reconcile-request.v1",
            "server_id": server_id or self.server_identity,
            "stream_id": self.history_stream,
            "acknowledged_sequence": acknowledged,
            "accepted_sequence": accepted,
            "last_receipt": receipt if acknowledged else None,
            "next_submission": next_submission if accepted != acknowledged else None,
        }

    def assert_reconciliation(self, expected_state: str, request: dict[str, Any]) -> dict[str, Any]:
        before = (self.stream_signature(), self.usage_signature(), self.count("audit_events", "true"))
        status, envelope = self.post_history(
            f"/api/v1/observer-streams/{self.history_stream}/reconciliation", request
        )
        if status != 200 or (self.stream_signature(), self.usage_signature(), self.count("audit_events", "true")) != before:
            raise IntegrationFailure(f"reconciliation {expected_state} was not a read-only success (HTTP {status})")
        result = envelope.get("result")
        expected_fields = {
            "schema_version", "server_id", "stream_id", "state", "local_acknowledged_sequence",
            "local_accepted_sequence", "server_accepted_sequence", "confirmed_sequence", "retired_sequence",
            "retained_submissions", "retained_bytes", "confirmation_required", "next_action", "resume_allowed",
            "observer_state_mutated", "shared_publication_mutated", "source_verified", "local_state_verified",
            "verification_level",
        }
        if not isinstance(result, dict) or set(result) != expected_fields or result["state"] != expected_state or \
                result["observer_state_mutated"] is not False or result["shared_publication_mutated"] is not False or \
                result["source_verified"] is not False or result["local_state_verified"] is not False or \
                result["verification_level"] != "service-observed" or result["server_id"] != self.server_identity or \
                result["stream_id"] != self.history_stream or \
                result["local_acknowledged_sequence"] != request["acknowledged_sequence"] or \
                result["local_accepted_sequence"] != request["accepted_sequence"]:
            raise IntegrationFailure("reconciliation response did not retain the declared state boundary")
        resumable = expected_state in {"caught-up", "send-next", "replay-head"}
        expected_action = "none" if expected_state == "caught-up" else expected_state if resumable else "operator-review"
        if result["resume_allowed"] is not resumable or result["next_action"] != expected_action or \
                result["confirmation_required"] != (resumable and request["acknowledged_sequence"] > result["confirmed_sequence"]):
            raise IntegrationFailure("reconciliation changed the explicit action or confirmation boundary")
        return result

    def retention_plan(self) -> dict[str, Any]:
        status, envelope = self.request("GET", f"/api/v1/observer-streams/{self.history_stream}/retention", headers=self.owner_headers)
        plan = envelope.get("result")
        fields = {
            "schema_version", "server_id", "stream_id", "accepted_sequence", "confirmed_sequence", "retired_sequence",
            "through_sequence", "delete_submissions", "delete_bytes", "source_verified", "shared_publication_mutated", "plan_id",
        }
        if status != 200 or not isinstance(plan, dict) or set(plan) != fields or \
                plan["schema_version"] != "shin.observer-retention-plan.v1" or plan["server_id"] != self.server_identity or \
                plan["stream_id"] != self.history_stream or plan["source_verified"] is not False or \
                plan["shared_publication_mutated"] is not False:
            raise IntegrationFailure("retention inspection did not return a bounded deterministic plan")
        return plan

    def apply_plan(self, plan: dict[str, Any], *, headers: dict[str, str] | None = None) -> tuple[int, dict[str, Any]]:
        return self.post_history(
            f"/api/v1/observer-streams/{self.history_stream}/retention", {"plan": plan},
            headers=headers or self.auth_headers("admin"),
        )

    def check_history_schema_and_access(self) -> None:
        if self.scalar("SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.audit_events'::regclass "
                       "AND contype='f' AND confrelid='shin_server.observer_streams'::regclass") != "0":
            raise IntegrationFailure("registry migration still ties immutable audit lifetime to a reclaimable stream row")
        workspace = self.expect_status(
            200, "GET", f"/api/v1/projects/{PROJECT_ID}/workspaces/{WORKSPACE_ID}", headers=self.owner_headers
        ).get("result")
        if not isinstance(workspace, dict) or workspace.get("source_kind") != "observer" or \
                workspace.get("root") is not None or workspace.get("root_identity") is not None:
            raise IntegrationFailure("history harness did not preserve the rootless Observer workspace boundary")
        if self.other_headers is None:
            raise IntegrationFailure("history harness could not create a same-project non-owner reader")
        owner = self.expect_status(200, "GET", f"/api/v1/observer-streams/{self.history_stream}", headers=self.owner_headers).get("result")
        if not isinstance(owner, dict) or owner.get("project_id") != PROJECT_ID or owner.get("principal_id") != "dev-reader":
            raise IntegrationFailure("current project owner could not read its enrolled history stream")
        self.expect_error(404, "observer-not-found", "GET", f"/api/v1/observer-streams/{self.history_stream}", headers=self.other_headers)
        self.expect_error(404, "observer-not-found", "GET", f"/api/v1/observer-streams/{self.history_stream}/retention",
                          headers=self.other_headers)
        self.expect_history_error(403, "forbidden", f"/api/v1/observer-streams/{self.history_stream}/retention", {"plan": {}},
                                  headers=self.other_headers)
        self.expect_history_error(404, "observer-not-found", f"/api/v1/observer-streams/{self.history_stream}/reconciliation",
                                  self.reconciliation_request(0, 0), headers=self.other_headers)

    def check_reconciliation_states(self) -> None:
        self.assert_reconciliation("caught-up", self.reconciliation_request(0, 0))
        self.assert_reconciliation("server-identity-mismatch", self.reconciliation_request(0, 0, server_id="0" * 32))
        first, receipt_one = self.accept(1)
        second = self.synthetic_submission(2)
        self.assert_reconciliation("send-next", self.reconciliation_request(1, 2, receipt=receipt_one, next_submission=second))
        self.assert_reconciliation("receipt-conflict", self.reconciliation_request(
            1, 1, receipt={**receipt_one, "received_at": "2000-01-01T00:00:00.000000Z"}
        ))
        conflicting = self.synthetic_submission(2, observer_id="observer-history-other")
        self.assert_reconciliation("submission-conflict", self.reconciliation_request(
            1, 2, receipt=receipt_one, next_submission=conflicting
        ))
        self.assert_reconciliation("server-behind", self.reconciliation_request(
            2, 2, receipt={**receipt_one, "sequence": 2, "submission_id": hashlib.sha256(
                f"{self.history_stream}:2".encode("utf-8")).hexdigest()}
        ))
        _, receipt_two = self.accept(2)
        self.assert_reconciliation("replay-head", self.reconciliation_request(1, 2, receipt=receipt_one, next_submission=second))
        self.assert_reconciliation("server-ahead", self.reconciliation_request(1, 1, receipt=receipt_one))
        tampered = {**receipt_two, "payload_sha256": "0" * 64}
        self.expect_history_error(409, "observer-confirmation-conflict",
                                  f"/api/v1/observer-streams/{self.history_stream}/confirmations", {"receipt": tampered},
                                  headers=self.owner_headers)
        self.expect_history_error(403, "observer-confirmation-forbidden",
                                  f"/api/v1/observer-streams/{self.history_stream}/confirmations", {"receipt": receipt_two},
                                  headers=self.auth_headers("admin"))
        self.confirm(1)
        empty = self.retention_plan()
        if empty["delete_submissions"] != 0:
            raise IntegrationFailure("confirmed anchor was incorrectly eligible for retirement")
        self.expect_history_error(403, "forbidden", f"/api/v1/observer-streams/{self.history_stream}/retention", {"plan": empty},
                                  headers=self.owner_headers)
        self.expect_history_error(409, "observer-retention-empty", f"/api/v1/observer-streams/{self.history_stream}/retention", {"plan": empty},
                                  headers=self.auth_headers("admin"))

    def check_retention_and_replay(self) -> None:
        self.confirm(2)
        stale = self.retention_plan()
        self.accept(3)
        self.expect_history_error(409, "observer-retention-stale", f"/api/v1/observer-streams/{self.history_stream}/retention", {"plan": stale},
                                  headers=self.auth_headers("admin"))
        plan = self.retention_plan()
        self.first_retention_plan = plan
        before_usage = self.usage_signature()
        before_audit = self.count("audit_events", f"observer_stream_id = '{self.history_stream}' AND action = 'observer.history.pruned'")
        status, first = self.apply_plan(plan)
        receipt = first.get("result", {}).get("receipt")
        if status != 200 or first.get("result", {}).get("replayed") is not False or not isinstance(receipt, dict) or \
                receipt.get("deleted_submissions") != plan["delete_submissions"] or receipt.get("retired_sequence") != 1:
            raise IntegrationFailure("retention did not retire the confirmed prefix exactly once")
        replay_status, replay = self.apply_plan(plan)
        if replay_status != 200 or replay.get("result", {}).get("replayed") is not True or replay.get("result", {}).get("receipt") != receipt:
            raise IntegrationFailure("exact latest retention retry did not return its stable receipt")
        if self.count("audit_events", f"observer_stream_id = '{self.history_stream}' AND action = 'observer.history.pruned'") != before_audit + 1:
            raise IntegrationFailure("retention replay wrote a duplicate immutable audit event")
        after_usage = self.usage_signature()
        if after_usage[0] != before_usage[0] - plan["delete_submissions"] or \
                after_usage[1] != before_usage[1] - plan["delete_bytes"]:
            raise IntegrationFailure("retention did not free the exact retained global quota")
        self.assert_global_usage()
        if self.count("observer_submissions", f"stream_id = '{self.history_stream}' AND sequence = 1") != 0 or \
                self.count("observer_submissions", f"stream_id = '{self.history_stream}' AND sequence IN (2,3)") != 2:
            raise IntegrationFailure("retention did not preserve the confirmed anchor and unconfirmed head")
        if self.count("audit_events", f"observer_stream_id = '{self.history_stream}' AND observer_sequence = 1") == 0:
            raise IntegrationFailure("payload retirement deleted immutable historical audit metadata")
        before_retired = self.stream_signature()
        for retired in (self.synthetic_submission(1), self.synthetic_submission(1, variant="retired")):
            self.expect_history_error(410, "observer-history-retired", f"/api/v1/observer-streams/{self.history_stream}/submissions", retired,
                                      headers=self.owner_headers)
        if self.stream_signature() != before_retired or \
                self.count("observer_submissions", f"stream_id = '{self.history_stream}' AND sequence = 1") != 0:
            raise IntegrationFailure("retired modified submission was accepted or changed retained history")
        replay_status, replay = self.post_submission_for(self.history_stream, self.synthetic_submission(2))
        if replay_status != 200 or replay.get("result", {}).get("replayed") is not True or \
                replay.get("result", {}).get("receipt") != self.history_receipts[2]:
            raise IntegrationFailure("retained exact replay was not preserved after historical retirement")
        self.assert_reconciliation("history-retired", self.reconciliation_request(1, 1, receipt=self.history_receipts[1]))

    def check_retention_audit_rollback(self) -> None:
        self.confirm(3)
        plan = self.retention_plan()
        before = (self.stream_signature(), self.usage_signature(), self.count("observer_submissions", f"stream_id = '{self.history_stream}'"),
                  self.count("audit_events", f"observer_stream_id = '{self.history_stream}'"))
        self.psql("REVOKE INSERT ON TABLE shin_server.audit_events FROM shin_runtime")
        try:
            status, _ = self.apply_plan(plan)
            after = (self.stream_signature(), self.usage_signature(), self.count("observer_submissions", f"stream_id = '{self.history_stream}'"),
                     self.count("audit_events", f"observer_stream_id = '{self.history_stream}'"))
            if status < 500 or after != before:
                raise IntegrationFailure("retention audit failure did not roll back rows, quota, cursor floor, and receipt")
        finally:
            self.psql("GRANT INSERT ON TABLE shin_server.audit_events TO shin_runtime")
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            outcomes = list(workers.map(lambda _: self.apply_plan(plan), range(4)))
        if any(status != 200 for status, _ in outcomes) or \
                sum(envelope.get("result", {}).get("replayed") is False for _, envelope in outcomes) != 1 or \
                len({json.dumps(envelope["result"]["receipt"], sort_keys=True) for _, envelope in outcomes}) != 1:
            raise IntegrationFailure("concurrent retention retries did not produce one durable outcome")
        self.assert_global_usage()

    def check_history_gap_and_revocation(self) -> None:
        if self.first_retention_plan is None:
            raise IntegrationFailure("first retention fixture plan is missing")
        self.expect_history_error(409, "observer-retention-stale", f"/api/v1/observer-streams/{self.history_stream}/retention",
                                  {"plan": self.first_retention_plan}, headers=self.auth_headers("admin"))
        gap_stream = secrets.token_hex(16)
        gap_observer = f"observer-gap-{secrets.token_hex(4)}"
        status, _ = self.post_history("/api/v1/observer-streams", self.history_enrollment(gap_stream, gap_observer),
                                      headers=self.auth_headers("admin"))
        if status != 201:
            raise IntegrationFailure("history-gap stream enrollment failed")
        entry = self.synthetic_submission(1, stream=gap_stream, observer_id=gap_observer)
        status, accepted = self.post_submission_for(gap_stream, entry)
        if status != 201:
            raise IntegrationFailure("history-gap stream submission failed")
        gap_receipt = accepted["result"]["receipt"]
        self.psql(f"DELETE FROM shin_server.observer_submissions WHERE stream_id = '{gap_stream}' AND sequence = 1")
        self.psql("UPDATE shin_server.observer_usage SET submissions=submissions-1, retained_bytes=retained_bytes-("
                  "SELECT retained_bytes FROM shin_server.observer_streams WHERE stream_id='" + gap_stream + "') WHERE singleton")
        request = {
            "schema_version": "shin.observer-reconcile-request.v1", "server_id": self.server_identity, "stream_id": gap_stream,
            "acknowledged_sequence": 1, "accepted_sequence": 1, "last_receipt": gap_receipt, "next_submission": None,
        }
        before = self.stream_signature(gap_stream)
        status, envelope = self.post_history(f"/api/v1/observer-streams/{gap_stream}/reconciliation", request)
        if status != 200 or envelope.get("result", {}).get("state") != "history-gap" or self.stream_signature(gap_stream) != before:
            raise IntegrationFailure("missing retained evidence was not surfaced as a read-only history gap")
        self.assert_global_usage()
        status, _ = self.post_history(f"/api/v1/observer-streams/{self.history_stream}/revoke", {}, headers=self.auth_headers("admin"))
        if status != 200:
            raise IntegrationFailure("history stream revocation failed")
        self.assert_reconciliation("revoked", self.reconciliation_request(3, 3, receipt=self.history_receipts[3]))

    def check_retention_load(self) -> None:
        if self.load_requests == 0:
            return
        stream = secrets.token_hex(16)
        observer_id = f"observer-history-load-{secrets.token_hex(4)}"
        if self.post_history("/api/v1/observer-streams", self.history_enrollment(stream, observer_id),
                             headers=self.auth_headers("admin"))[0] != 201:
            raise IntegrationFailure("history load stream enrollment failed")
        timings: list[float] = []
        started = time.monotonic()
        cycles = 0
        for sequence in range(1, self.load_requests + 1):
            submission = self.synthetic_submission(sequence, stream=stream, observer_id=observer_id)
            before = time.monotonic()
            status, envelope = self.post_submission_for(stream, submission)
            timings.append((time.monotonic() - before) * 1000)
            if status != 201:
                raise IntegrationFailure("history load submission did not commit")
            receipt = self.assert_history_receipt(envelope["result"]["receipt"], submission)
            if sequence > 1 and (sequence - 1) % 1023 == 0:
                status, _ = self.post_history(f"/api/v1/observer-streams/{stream}/confirmations", {"receipt": receipt})
                if status != 200:
                    raise IntegrationFailure("history load confirmation failed at retained capacity")
                status, planned = self.request("GET", f"/api/v1/observer-streams/{stream}/retention", headers=self.owner_headers)
                if status != 200 or planned.get("result", {}).get("delete_submissions") != 1023:
                    raise IntegrationFailure("history load did not plan the confirmed prefix below the anchor")
                status, applied = self.post_history(f"/api/v1/observer-streams/{stream}/retention", {"plan": planned["result"]},
                                                    headers=self.auth_headers("admin"))
                if status != 200 or applied.get("result", {}).get("receipt", {}).get("retired_sequence") != sequence - 1:
                    raise IntegrationFailure("history load did not prune before continuing beyond capacity")
                cycles += 1
        self.assert_global_usage()
        elapsed = time.monotonic() - started
        ordered = sorted(timings)
        self.history_load_metrics = {"requests": len(timings), "clients": 1, "prune_cycles": cycles,
            "elapsed_ms": round(elapsed * 1000, 3), "requests_per_second": round(len(timings) / elapsed, 2),
            "append_p50_ms": round(ordered[len(ordered) // 2], 3),
            "append_p95_ms": round(ordered[(len(ordered) * 95 + 99) // 100 - 1], 3),
            "final_retained_rows": self.count("observer_submissions", f"stream_id='{stream}'"),
            "scope": "One synthetic producer with real PostgreSQL and explicit periodic confirmation/pruning; not native end-to-end or production capacity."}

    def run(self) -> None:
        # The inherited fixture already exercises its own optional parallel load;
        # history capacity is deliberately driven below by one sequential client.
        history_load_requests = self.load_requests
        self.load_requests = 0
        super().run()
        self.load_requests = history_load_requests
        try:
            if self.entry_one is None or not self.server_identity:
                raise IntegrationFailure("ingestion suite did not retain native fixture identity")
            self.owner_headers = self.auth_headers("reader")
            self.add_non_owner_reader()
            self.history_stream = secrets.token_hex(16)
            self.history_observer = f"observer-history-{secrets.token_hex(4)}"
            self.source_before_history = self.scalar(
                "SELECT COALESCE(string_agg(project_id || ':' || id || ':' || source_kind, ',' ORDER BY project_id,id), '') "
                "FROM shin_server.workspaces"
            )
            self.publications_before_history = self.active_routes_signature()
            self.start_server()
            status, _ = self.post_history("/api/v1/observer-streams", self.history_enrollment(self.history_stream, self.history_observer),
                                          headers=self.auth_headers("admin"))
            if status != 201:
                raise IntegrationFailure("synthetic history stream enrollment failed")
            self.record_history("observer-history-schema-and-owner-privacy", self.check_history_schema_and_access)
            self.record_history("observer-history-reconciliation-and-confirmation", self.check_reconciliation_states)
            self.record_history("observer-history-retention-and-replay", self.check_retention_and_replay)
            self.record_history("observer-history-retention-audit-rollback", self.check_retention_audit_rollback)
            self.record_history("observer-history-gap-and-revocation", self.check_history_gap_and_revocation)
            self.record_history("observer-history-retained-capacity", self.check_retention_load)
            if self.source_before_history != self.scalar(
                "SELECT COALESCE(string_agg(project_id || ':' || id || ':' || source_kind, ',' ORDER BY project_id,id), '') "
                "FROM shin_server.workspaces"
            ) or self.publications_before_history != self.active_routes_signature():
                raise IntegrationFailure("Observer history changed source bindings or shared publication state")
        finally:
            self.stop_server(expect_clean=False)
            self.restore_runtime_config()

    def write_report(self, *, ok: bool, failure: str | None = None) -> None:
        IntegrationSuite.write_report(self, ok=ok, failure=failure)
        report = json.loads(self.report_path.read_text(encoding="utf-8"))
        report["personal_observer_measurement"] = getattr(self, "personal_observer_measurement", None)
        report["observer_history"] = {
            "checks": self.history_checks,
            "load_requests": self.load_requests,
            "scope": "Real PostgreSQL retention and reconciliation correctness; not a performance claim.",
            "measurement": self.history_load_metrics,
        }
        self.replace_private_json(self.report_path, report)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="native shin-server executable")
    parser.add_argument("--observer", required=True, help="absolute native shin-observer executable")
    parser.add_argument("--state-root", default=str(BUILD_ROOT / "server-dev"), help="isolated state below this worktree build directory")
    parser.add_argument("--report", help="new report path below this worktree build directory")
    parser.add_argument("--load-requests", type=int, default=1100, help="optional sequential submissions in 0..4096")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        binary = require_executable(args.binary, "binary", absolute=False)
        observer = require_executable(args.observer, "observer", absolute=True)
        state_root = Path(args.state_root).resolve(strict=False)
        report_path = (Path(args.report).resolve(strict=False) if args.report else
                       state_root / "reports" / f"observer-history-{secrets.token_hex(8)}.json")
        if not under_build(state_root) or not under_build(report_path) or report_path.exists():
            raise IntegrationFailure("state root and report must be new paths below this worktree build directory")
        if args.load_requests < 0 or args.load_requests > 4096:
            raise IntegrationFailure("--load-requests must be in 0..4096")
    except IntegrationFailure as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    suite = ObserverHistoryIntegrationSuite(binary, observer, state_root, report_path)
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
        print(f"error: observer history integration test failed: {type(error).__name__}", file=sys.stderr)
        return 1
    suite.write_report(ok=True)
    print("observer history integration test passed; compact report written below the isolated build state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
