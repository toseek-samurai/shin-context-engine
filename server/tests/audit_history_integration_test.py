#!/usr/bin/env python3
"""Real PostgreSQL and HTTP coverage for explicit bounded audit retention."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import secrets
import sys
import time
from typing import Any

from integration_test import BUILD_ROOT, IntegrationFailure, IntegrationSuite, under_build


def compact(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


class AuditHistoryIntegrationSuite(IntegrationSuite):
    def __init__(self, binary: Path, state_root: Path, report_path: Path, load_events: int) -> None:
        super().__init__(binary, state_root, report_path)
        self.fixed_plan: dict[str, Any] | None = None
        self.first_receipt: dict[str, Any] | None = None
        self.floor = 0
        self.boundary_before = ""
        self.load_events = load_events
        self.load_metrics: dict[str, Any] | None = None

    def post_json(self, target: str, body: dict[str, Any], *, role: str = "admin") -> tuple[int, dict[str, Any]]:
        return self.request("POST", target, headers={**self.auth_headers(role), "Content-Type": "application/json"}, body=compact(body))

    def export(self, limit: int = 256, *, role: str = "admin") -> tuple[int, dict[str, Any]]:
        return self.request("GET", f"/api/v1/audit/export?limit={limit}", headers=self.auth_headers(role))

    def history(self, *, role: str = "admin") -> tuple[int, dict[str, Any]]:
        return self.request("GET", "/api/v1/audit/history", headers=self.auth_headers(role))

    def apply(self, plan: dict[str, Any], archive_saved: Any = True) -> tuple[int, dict[str, Any]]:
        return self.post_json("/api/v1/audit/retention", {"plan": plan, "archive_saved": archive_saved})

    def audit_records(self, lower: int, upper: int) -> list[dict[str, Any]]:
        value = self.scalar(
            "SELECT COALESCE(json_agg(json_build_object("
            "'id',id,'principal_id',principal_id,'action',action,'project_id',project_id,"
            "'workspace_id',workspace_id,'preparation_id',preparation_id,"
            "'activation_route_id',activation_route_id,'activation_previous_revision',activation_previous_revision,"
            "'observer_stream_id',observer_stream_id,'observer_sequence',observer_sequence,"
            "'publication_proposal_id',publication_proposal_id,"
            "'created_at',to_char(created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')) "
            "ORDER BY id),'[]')::text FROM shin_server.audit_events "
            f"WHERE id>{lower} AND id<={upper}"
        )
        return json.loads(value)

    def boundary_signature(self) -> str:
        return self.scalar(
            "SELECT jsonb_build_object("
            "'workspaces',(SELECT COALESCE(jsonb_agg(to_jsonb(w) ORDER BY project_id,id),'[]'::jsonb) FROM shin_server.workspaces w),"
            "'streams',(SELECT COALESCE(jsonb_agg(to_jsonb(s) ORDER BY stream_id),'[]'::jsonb) FROM shin_server.observer_streams s),"
            "'submissions',(SELECT COALESCE(jsonb_agg(to_jsonb(s) ORDER BY stream_id,sequence),'[]'::jsonb) FROM shin_server.observer_submissions s),"
            "'usage',(SELECT to_jsonb(u) FROM shin_server.observer_usage u),"
            "'active',(SELECT COALESCE(jsonb_agg(to_jsonb(a) ORDER BY project_id,route_id),'[]'::jsonb) FROM shin_server.active_routes a))::text"
        )

    def seed_observer_boundary(self) -> None:
        self.psql(
            "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,source_kind) VALUES ('visible','audit-observer','audit-host','Audit Observer','observer');"
            "INSERT INTO shin_server.observer_streams(stream_id,project_id,workspace_id,principal_id,observer_id,source_host_id,profile,binding_digest,enrollment_hash,accepted_sequence,observed_sequence,retained_bytes,confirmed_sequence,retired_sequence) "
            "VALUES (repeat('a',32),'visible','audit-observer','dev-reader','audit-observer','audit-host','audit-profile',repeat('b',64),repeat('c',64),1,1,32,0,0);"
            "INSERT INTO shin_server.observer_submissions(stream_id,sequence,submission_id,submission_hash,payload_hash,submission,receipt,retained_bytes) "
            "VALUES (repeat('a',32),1,repeat('d',64),repeat('e',64),repeat('f',64),'{\"observed\":1}','{\"receipt\":1}',32);"
            "UPDATE shin_server.observer_usage SET streams=1,submissions=1,retained_bytes=32"
        )

    def check_export_contract_and_privacy(self) -> None:
        for project in ("visible", "other", "audit-one", "audit-two", "audit-three", "audit-four"):
            self.assert_register(project, f"audit-register-{project}")
        self.psql("INSERT INTO shin_server.audit_events(principal_id,action,project_id) VALUES ('dev-admin','audit.utf8-\u03c0','visible')")
        self.seed_observer_boundary()
        self.boundary_before = self.boundary_signature()
        before = self.scalar("SELECT COALESCE(jsonb_agg(to_jsonb(a) ORDER BY id),'[]'::jsonb)::text FROM shin_server.audit_events a")
        status, first = self.export(10)
        if status != 200:
            raise IntegrationFailure("administrator audit export was not available")
        status, second = self.export(10)
        if status != 200 or first["result"] != second["result"]:
            raise IntegrationFailure("unchanged audit export was not deterministic")
        result = first["result"]
        if set(result) != {"schema_version", "archive", "plan", "read_only", "external_archive_verified"} or \
                result["schema_version"] != "shin.audit-export.v1" or result["read_only"] is not True or \
                result["external_archive_verified"] is not False:
            raise IntegrationFailure("audit export has an invalid public envelope")
        archive, plan = result["archive"], result["plan"]
        archive_fields = {"schema_version", "server_id", "retired_through", "through_id", "events", "previous_retention_receipt", "verification_level", "source_verified", "shared_publication_mutated"}
        plan_fields = {"schema_version", "server_id", "retired_through", "through_id", "event_count", "archive_bytes", "archive_sha256", "previous_receipt_sha256", "plan_id"}
        if set(archive) != archive_fields or set(plan) != plan_fields or archive["schema_version"] != "shin.audit-archive.v1" or \
                plan["schema_version"] != "shin.audit-retention-plan.v1" or archive["previous_retention_receipt"] is not None or \
                archive["verification_level"] != "service-observed" or archive["source_verified"] is not False or archive["shared_publication_mutated"] is not False:
            raise IntegrationFailure("audit archive or plan shape is invalid")
        expected = self.audit_records(0, archive["through_id"])
        if archive["events"] != expected or plan["event_count"] != len(expected) or archive["retired_through"] != 0:
            raise IntegrationFailure("audit archive is not the independently queried exact prefix")
        if plan["archive_bytes"] != len(compact(archive)) or plan["archive_sha256"] != hashlib.sha256(compact(archive)).hexdigest():
            raise IntegrationFailure("audit archive digest does not bind compact sorted JSON")
        if not any(event["action"] == "audit.utf8-\u03c0" for event in expected):
            raise IntegrationFailure("audit export did not preserve canonical UTF-8 action bytes")
        bare_plan = dict(plan)
        plan_id = bare_plan.pop("plan_id")
        if not isinstance(plan_id, str) or hashlib.sha256(compact(bare_plan)).hexdigest() != plan_id:
            raise IntegrationFailure("audit retention plan id is not deterministic")
        if self.scalar("SELECT COALESCE(jsonb_agg(to_jsonb(a) ORDER BY id),'[]'::jsonb)::text FROM shin_server.audit_events a") != before:
            raise IntegrationFailure("audit export mutated retained rows")
        status, history = self.history()
        history_fields = {"schema_version", "server_id", "retired_through", "retired_events", "last_retention_receipt", "read_only", "external_archive_verified"}
        if status != 200 or set(history.get("result", {})) != history_fields or history["result"]["schema_version"] != "shin.audit-history.v1" or \
                history["result"]["retired_through"] != 0 or history["result"]["retired_events"] != 0 or \
                history["result"]["last_retention_receipt"] is not None or history["result"]["read_only"] is not True or \
                history["result"]["external_archive_verified"] is not False:
            raise IntegrationFailure("audit history response exposed an invalid lifecycle state")
        for target in ("/api/v1/audit/export?limit=10", "/api/v1/audit/history"):
            status, denied = self.request("GET", target, headers=self.auth_headers("reader"))
            if status != 403 or "result" in denied or any(key in json.dumps(denied) for key in ("server_id", "retired_through", "archive")):
                raise IntegrationFailure("non-administrator audit lifecycle request disclosed metadata")
        status, denied = self.post_json("/api/v1/audit/retention", {"plan": plan, "archive_saved": True}, role="reader")
        if status != 403 or "result" in denied:
            raise IntegrationFailure("non-administrator audit retention request was not denied")
        for target in ("/api/v1/audit/export?limit=0", "/api/v1/audit/export?limit=1001", "/api/v1/audit/export?after=1"):
            if self.request("GET", target, headers=self.auth_headers("admin"))[0] != 400:
                raise IntegrationFailure("audit export did not enforce its bounded query contract")
        self.fixed_plan = plan

    def check_validation_rollback_and_fixed_prefix(self) -> None:
        if self.fixed_plan is None:
            raise IntegrationFailure("audit export fixture is unavailable")
        history_before = self.scalar("SELECT retired_through::text || ':' || retired_events::text || ':' || COALESCE(last_retention::text,'') FROM shin_server.audit_history")
        malformed = {"plan": {"schema_version": "shin.audit-retention-plan.v1"}, "archive_saved": True}
        for body, code in ((malformed, "invalid-request"), ({"plan": self.fixed_plan, "archive_saved": False}, "invalid-request"),
                           ({"plan": self.fixed_plan, "archive_saved": "true"}, "invalid-request")):
            status, response = self.post_json("/api/v1/audit/retention", body)
            if status != 400 or response.get("error", {}).get("code") != code:
                raise IntegrationFailure("malformed retention declaration did not fail closed")
        for key in ("event_count", "through_id", "retired_through", "archive_bytes"):
            fractional = json.loads(json.dumps(self.fixed_plan))
            fractional[key] = 1.5
            status, response = self.apply(fractional)
            if status != 400 or response.get("error", {}).get("code") != "invalid-request":
                raise IntegrationFailure("fractional audit retention plan field was not rejected")
        mismatched = json.loads(json.dumps(self.fixed_plan))
        mismatched["archive_sha256"] = "0" * 64
        status, response = self.apply(mismatched)
        if status != 409 or response.get("error", {}).get("code") != "audit-retention-stale":
            raise IntegrationFailure("mismatched retention plan was accepted")
        # A sequence value may be allocated then rolled back; export must retain exact event ids, not ranges.
        self.psql("BEGIN; INSERT INTO shin_server.audit_events(principal_id,action,project_id) VALUES ('dev-admin','audit.hole','visible'); ROLLBACK;")
        self.psql("INSERT INTO shin_server.audit_events(principal_id,action,project_id) VALUES ('dev-admin','audit.later','other')")
        after_later = self.scalar("SELECT count(*)::text || ':' || COALESCE(max(id),0)::text FROM shin_server.audit_events")
        self.psql("REVOKE UPDATE ON TABLE shin_server.audit_history FROM shin_runtime")
        try:
            status, _ = self.apply(self.fixed_plan)
            if status < 500:
                raise IntegrationFailure("retention did not surface denied history update")
        finally:
            self.psql("GRANT UPDATE ON TABLE shin_server.audit_history TO shin_runtime")
        if self.scalar("SELECT count(*)::text || ':' || COALESCE(max(id),0)::text FROM shin_server.audit_events") != after_later or \
                self.scalar("SELECT retired_through::text || ':' || retired_events::text || ':' || COALESCE(last_retention::text,'') FROM shin_server.audit_history") != history_before:
            raise IntegrationFailure("denied audit history update did not roll back the preceding DELETE")
        expected = self.audit_records(0, self.fixed_plan["through_id"])
        if expected != self.export(self.fixed_plan["event_count"])[1]["result"]["archive"]["events"]:
            raise IntegrationFailure("rollback or sequence hole changed the fixed exported prefix")
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            outcomes = list(workers.map(lambda _: self.apply(self.fixed_plan), range(4)))
        if any(status != 200 for status, _ in outcomes) or sum(body.get("result", {}).get("replayed") is False for _, body in outcomes) != 1:
            raise IntegrationFailure("concurrent audit retention did not commit exactly once")
        receipts = [body["result"]["receipt"] for _, body in outcomes]
        if len({compact(receipt) for receipt in receipts}) != 1:
            raise IntegrationFailure("concurrent audit retention did not return one durable receipt")
        receipt = receipts[0]
        self.first_receipt = receipt
        receipt_fields = {"schema_version", "server_id", "plan_id", "previous_retired_through", "retired_through", "deleted_events", "archive_sha256", "archive_bytes", "previous_receipt_sha256", "principal_id", "recorded_at", "archive_saved_declared", "external_archive_verified", "shared_publication_mutated"}
        if set(receipt) != receipt_fields or receipt["schema_version"] != "shin.audit-retention-receipt.v1" or \
                receipt["server_id"] != self.fixed_plan["server_id"] or receipt["plan_id"] != self.fixed_plan["plan_id"] or \
                receipt["previous_retired_through"] != 0 or receipt["retired_through"] != self.fixed_plan["through_id"] or \
                receipt["deleted_events"] != self.fixed_plan["event_count"] or receipt["archive_sha256"] != self.fixed_plan["archive_sha256"] or \
                receipt["archive_bytes"] != self.fixed_plan["archive_bytes"] or receipt["previous_receipt_sha256"] is not None or \
                receipt["principal_id"] != "dev-admin" or not isinstance(receipt["recorded_at"], str) or not receipt["recorded_at"] or \
                receipt["archive_saved_declared"] is not True or receipt["external_archive_verified"] is not False or receipt["shared_publication_mutated"] is not False:
            raise IntegrationFailure("audit retention receipt has an invalid shape")
        self.floor = receipt["retired_through"]
        if self.audit_records(0, self.floor) or self.boundary_signature() != self.boundary_before:
            raise IntegrationFailure("audit retention changed a non-audit boundary or retained fixed prefix")
        _, later = self.export(10)
        later_plan = later["result"]["plan"]
        if later_plan["event_count"] != 1 or later_plan["through_id"] - later_plan["retired_through"] <= later_plan["event_count"]:
            raise IntegrationFailure("audit export did not retain exact count across a rolled-back identity hole")
        status, response = self.request("GET", "/api/v1/audit?after=0&limit=50", headers=self.auth_headers("admin"))
        if status != 410 or response.get("error", {}).get("code") != "audit-history-retired":
            raise IntegrationFailure("retired audit cursor did not return audit-history-retired")
        status, hidden = self.request("GET", "/api/v1/audit?after=0&limit=50", headers=self.auth_headers("reader"))
        if status != 410 or "result" in hidden or any(key in json.dumps(hidden) for key in ("server_id", "retired_through", "archive")):
            raise IntegrationFailure("stale reader cursor disclosed audit lifecycle metadata")
        if self.request("GET", f"/api/v1/audit?after={self.floor}&limit=50", headers=self.auth_headers("admin"))[0] != 200:
            raise IntegrationFailure("audit cursor at the retired floor was rejected")
        self.stop_server(expect_clean=True)
        self.start_server()
        status, replay = self.apply(self.fixed_plan)
        if status != 200 or replay.get("result", {}).get("replayed") is not True or replay["result"]["receipt"] != receipt:
            raise IntegrationFailure("latest audit retention replay was not durable across restart")

    def check_stale_empty_and_reader_filter(self) -> None:
        self.psql("INSERT INTO shin_server.audit_events(principal_id,action,project_id) VALUES ('dev-admin','audit.reader-visible','visible'),('dev-admin','audit.reader-hidden','other')")
        status, reader = self.request("GET", f"/api/v1/audit?after={self.floor}&limit=50", headers=self.auth_headers("reader"))
        if status != 200 or not reader["result"]["items"] or any(event["project_id"] != "visible" for event in reader["result"]["items"]):
            raise IntegrationFailure("reader audit filtering changed after retention")
        _, older = self.export(1)
        self.psql("INSERT INTO shin_server.audit_events(principal_id,action,project_id) VALUES ('dev-admin','audit.newer','visible')")
        _, newer = self.export(3)
        newer_plan = newer["result"]["plan"]
        if self.first_receipt is None or newer_plan["previous_receipt_sha256"] != hashlib.sha256(compact(self.first_receipt)).hexdigest() or \
                newer["result"]["archive"]["previous_retention_receipt"] != self.first_receipt:
            raise IntegrationFailure("next audit retention export did not bind the previous receipt chain")
        status, newer_applied = self.apply(newer_plan)
        if status != 200 or newer_applied["result"].get("replayed") is not False or \
                newer_applied["result"]["receipt"]["previous_receipt_sha256"] != newer_plan["previous_receipt_sha256"]:
            raise IntegrationFailure("newer audit retention plan was not accepted")
        status, stale = self.apply(older["result"]["plan"])
        if status != 409 or stale.get("error", {}).get("code") != "audit-retention-stale":
            raise IntegrationFailure("prior audit plan remained applicable after newer prune")
        _, final = self.export(1000)
        if self.apply(final["result"]["plan"])[0] != 200:
            raise IntegrationFailure("final non-empty audit prefix was not pruned")
        status, empty = self.export(1000)
        if status != 200 or empty["result"]["plan"]["event_count"] != 0 or empty["result"]["archive"]["events"] != []:
            raise IntegrationFailure("empty audit export is not a valid read-only plan")
        status, denied = self.apply(empty["result"]["plan"])
        if status != 409 or denied.get("error", {}).get("code") != "audit-retention-empty":
            raise IntegrationFailure("empty audit plan did not fail with audit-retention-empty")
        # Guarded synthetic rows exercise the response budget only; this is not an ingestion benchmark.
        self.psql("INSERT INTO shin_server.audit_events(principal_id,action,project_id) "
                  "SELECT 'dev-admin',repeat('x',1200),'visible' FROM generate_series(1,1000)")
        status, oversized = self.export(1000)
        if status != 413 or oversized.get("error", {}).get("code") != "audit-export-budget":
            raise IntegrationFailure("oversized bounded audit archive was not rejected")
        for _ in range(4):
            status, bounded = self.export(256)
            if status != 200 or not bounded["result"]["archive"]["events"] or self.apply(bounded["result"]["plan"])[0] != 200:
                raise IntegrationFailure("oversized fixture did not support smaller explicit archive batches")
        if self.count("audit_events", "true") != 0:
            raise IntegrationFailure("oversized audit fixture was not retired before load")

    def check_bounded_export_prune_load(self) -> None:
        if self.load_events == 0:
            self.load_metrics = {"event_count": 0, "cycles": 0, "scope": "disabled"}
            return
        self.psql("INSERT INTO shin_server.audit_events(principal_id,action,project_id) "
                  f"SELECT 'dev-admin','audit.load','visible' FROM generate_series(1,{self.load_events})")
        exports: list[float] = []
        prunes: list[float] = []
        started = time.monotonic()
        cycles = 0
        deleted_events = 0
        for _ in range((self.load_events + 999) // 1000):
            before = time.monotonic()
            status, exported = self.export(1000)
            exports.append((time.monotonic() - before) * 1000)
            if status != 200 or exported["result"]["plan"]["event_count"] == 0:
                raise IntegrationFailure("bounded audit load export did not produce a nonempty prefix")
            before = time.monotonic()
            status, applied = self.apply(exported["result"]["plan"])
            prunes.append((time.monotonic() - before) * 1000)
            if status != 200 or applied["result"].get("replayed") is not False:
                raise IntegrationFailure("bounded audit load prune did not commit")
            deleted_events += applied["result"]["receipt"]["deleted_events"]
            cycles += 1
        if deleted_events != self.load_events or self.count("audit_events", "true") != 0 or self.boundary_signature() != self.boundary_before:
            raise IntegrationFailure("bounded audit load did not retire exactly the seeded rows or changed Observer state")
        def percentile(values: list[float], percent: int) -> float:
            ordered = sorted(values)
            return round(ordered[(len(ordered) * percent + 99) // 100 - 1], 3)
        self.load_metrics = {"event_count": self.load_events, "cycles": cycles,
            "export_p50_ms": percentile(exports, 50), "export_p95_ms": percentile(exports, 95),
            "prune_p50_ms": percentile(prunes, 50), "prune_p95_ms": percentile(prunes, 95),
            "total_ms": round((time.monotonic() - started) * 1000, 3),
            "final_retained_count": self.count("audit_events", "true"),
            "scope": "Synthetic SQL audit rows with real HTTP export/prune transactions; not ingestion throughput."}

    def run(self) -> None:
        self.dev_command("init")
        self.dev_command("up")
        self.wait_for_postgres()
        self.require_http_port_free()
        self.reset_schema()
        self.migrate()
        self.admin_token = self.read_private_token("dev_admin.token")
        self.reader_token = self.read_private_token("dev_reader.token")
        try:
            self.start_server()
            self.record("audit-export-contract-and-privacy", self.check_export_contract_and_privacy)
            self.record("audit-retention-rollback-prefix-replay", self.check_validation_rollback_and_fixed_prefix)
            self.record("audit-retention-stale-empty-reader", self.check_stale_empty_and_reader_filter)
            self.record("audit-export-prune-bounded-load", self.check_bounded_export_prune_load)
            self.stop_server(expect_clean=True)
        finally:
            self.stop_server(expect_clean=False)

    def write_report(self, *, ok: bool, failure: str | None = None) -> None:
        super().write_report(ok=ok, failure=failure)
        report = json.loads(self.report_path.read_text(encoding="utf-8"))
        report["audit_history"] = {"groups": self.checks, "load_events": self.load_events,
                                   "load_metrics": self.load_metrics,
                                   "active_routes": "empty by fixture design; no activation proof claimed"}
        self.report_path.write_bytes(compact(report) + b"\n")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--state-root", default=str(BUILD_ROOT / "server-dev"))
    parser.add_argument("--report")
    parser.add_argument("--load-events", type=int, default=4096, help="synthetic audit rows in 0..16384")
    args = parser.parse_args(argv)
    binary, state = Path(args.binary).resolve(), Path(args.state_root).resolve()
    report = Path(args.report).resolve() if args.report else state / "reports" / f"audit-history-{secrets.token_hex(8)}.json"
    if not binary.is_file() or not os.access(binary, os.X_OK) or not under_build(state) or not under_build(report) or report.exists() or \
            args.load_events < 0 or args.load_events > 16384:
        print("error: binary, new report and state root must be valid paths below this worktree build", file=sys.stderr)
        return 2
    suite = AuditHistoryIntegrationSuite(binary, state, report, args.load_events)
    try:
        suite.run()
    except Exception as error:
        suite.stop_server(expect_clean=False)
        if isinstance(error, IntegrationFailure):
            suite.failure_assertion = str(error)
        suite.write_report(ok=False, failure=type(error).__name__)
        print(f"error: audit history integration test failed: {type(error).__name__}", file=sys.stderr)
        return 1
    suite.write_report(ok=True)
    print("audit history integration test passed; compact report written below isolated build state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
