"""Upgrade checks for observer registry reclamation migration 011."""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any


MIGRATIONS = Path(__file__).resolve().parents[1] / "migrations"


def _check(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def _history(suite: Any) -> str:
    return suite.scalar(
        "SELECT COALESCE(string_agg(version::text, ',' ORDER BY version), '') "
        "FROM shin_server.schema_migrations"
    )


def _install_through_010(suite: Any) -> None:
    names = (
        "001_initial.sql",
        "002_preparations.sql",
        "003_retention.sql",
        "004_activation.sql",
        "005_activation_audit.sql",
        "006_workspaces.sql",
        "007_observer_ingestion.sql",
        "008_observer_history.sql",
        "009_audit_history.sql",
        "010_server_recovery.sql",
    )
    for version, name in enumerate(names, start=1):
        path = MIGRATIONS / name
        sql = path.read_text(encoding="utf-8")
        if version >= 6:
            sql = "SET shin_server.migration_source_host_id='upgrade-host';" + sql
        suite.psql(sql)
        checksum = hashlib.sha256(path.read_bytes()).hexdigest()
        suite.psql(
            "INSERT INTO shin_server.schema_migrations(version,checksum) "
            f"VALUES ({version},'{checksum}')"
        )


def check_observer_registry_upgrade(harness: Any) -> None:
    """Migration 011 separates identity and preserves the prior observer ledger."""
    harness.reset_schema()
    try:
        _install_through_010(harness)
        server_id = "1" * 32
        harness.psql(
            "UPDATE shin_server.observer_usage SET server_id='" + server_id + "',streams=1,submissions=1,retained_bytes=32;"
            "INSERT INTO shin_server.projects(id,display_name) VALUES ('upgrade','Upgrade');"
            "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,source_kind) "
            "VALUES ('upgrade','primary','upgrade-host','Observer','observer');"
            "INSERT INTO shin_server.observer_streams("
            "stream_id,project_id,workspace_id,principal_id,observer_id,source_host_id,profile,binding_digest,enrollment_hash,"
            "accepted_sequence,observed_sequence,retained_bytes,confirmed_sequence,retired_sequence) VALUES ("
            "repeat('a',32),'upgrade','primary','dev-reader','upgrade-observer','upgrade-host','upgrade-profile',"
            "repeat('b',64),repeat('c',64),1,1,32,1,0);"
            "INSERT INTO shin_server.observer_submissions("
            "stream_id,sequence,submission_id,submission_hash,payload_hash,submission,receipt,retained_bytes) VALUES ("
            "repeat('a',32),1,repeat('d',64),repeat('e',64),repeat('f',64),"
            "'{\"original\":1}','{\"schema_version\":\"shin.observer-ack.v1\",\"original\":1}',32);"
            "INSERT INTO shin_server.audit_events(principal_id,action,project_id,workspace_id,observer_stream_id,observer_sequence) "
            "VALUES ('dev-reader','observer.submission.accepted','upgrade','primary',repeat('a',32),1)"
        )
        before = harness.scalar(
            "SELECT jsonb_build_object("
            "'usage',(SELECT to_jsonb(u)-'server_id' FROM shin_server.observer_usage u),"
            "'streams',(SELECT jsonb_agg((to_jsonb(s)-'scope_tenant_id') ORDER BY stream_id) FROM shin_server.observer_streams s),"
            "'submissions',(SELECT jsonb_agg(to_jsonb(s) ORDER BY stream_id,sequence) FROM shin_server.observer_submissions s),"
            "'audit',(SELECT jsonb_agg((to_jsonb(a)-'publication_proposal_id') ORDER BY id) FROM shin_server.audit_events a),"
            "'recovery',(SELECT to_jsonb(r) FROM shin_server.server_recovery r))::text"
        )
        harness.migrate()
        after = harness.scalar(
            "SELECT jsonb_build_object("
            "'usage',(SELECT to_jsonb(u)-'tombstones' FROM shin_server.observer_usage u),"
            "'streams',(SELECT jsonb_agg((to_jsonb(s)-'scope_tenant_id') ORDER BY stream_id) FROM shin_server.observer_streams s),"
            "'submissions',(SELECT jsonb_agg(to_jsonb(s) ORDER BY stream_id,sequence) FROM shin_server.observer_submissions s),"
            "'audit',(SELECT jsonb_agg((to_jsonb(a)-'publication_proposal_id') ORDER BY id) FROM shin_server.audit_events a),"
            "'recovery',(SELECT to_jsonb(r) FROM shin_server.server_recovery r))::text"
        )
        _check(before == after, "observer registry upgrade rewrote prior usage, receipts, audit, or recovery metadata")
        _check(harness.scalar("SELECT count(*) FROM shin_server.audit_events WHERE publication_proposal_id IS NOT NULL") == "0",
               "publication migration invented proposal links for historical audit")
        _check(_history(harness) == "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35", "observer registry upgrade history is incomplete")
        _check(harness.scalar("SELECT server_id FROM shin_server.server_identity WHERE singleton") == server_id,
               "observer registry upgrade changed the durable Server identity")
        _check(harness.scalar("SELECT tombstones::text FROM shin_server.observer_usage WHERE singleton") == "0",
               "observer registry upgrade invented tombstone capacity usage")
        _check(harness.scalar("SELECT count(*)::text FROM shin_server.observer_tombstones") == "0",
               "observer registry upgrade invented a retired stream")
        for version in range(1, 11):
            name = (
                "001_initial.sql", "002_preparations.sql", "003_retention.sql", "004_activation.sql",
                "005_activation_audit.sql", "006_workspaces.sql", "007_observer_ingestion.sql",
                "008_observer_history.sql", "009_audit_history.sql", "010_server_recovery.sql",
            )[version - 1]
            expected = hashlib.sha256((MIGRATIONS / name).read_bytes()).hexdigest()
            actual = harness.scalar(f"SELECT checksum FROM shin_server.schema_migrations WHERE version={version}")
            _check(actual == expected, "observer registry upgrade changed an existing migration checksum")
        _check(harness.scalar(
            "SELECT has_table_privilege('shin_runtime','shin_server.server_identity','SELECT')::text || ':' || "
            "has_table_privilege('shin_runtime','shin_server.server_identity','UPDATE')::text || ':' || "
            "has_table_privilege('shin_runtime','shin_server.observer_tombstones','UPDATE')::text || ':' || "
            "has_table_privilege('shin_runtime','shin_server.observer_tombstones','DELETE')::text"
        ) == "true:false:false:false", "runtime role can rewrite the Server identity or retired stream records")
    finally:
        harness.reset_schema()
