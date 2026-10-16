"""Upgrade regression coverage for local and Observer workspace schemas."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any


MIGRATIONS = Path(__file__).resolve().parents[1] / "migrations"
LEGACY_FINGERPRINT = "c" * 64
LEGACY_SNAPSHOT = "fnv1a64:0123456789abcdef"


def _check(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def _migrations(suite: Any) -> None:
    for version, name in enumerate((
        "001_initial.sql",
        "002_preparations.sql",
        "003_retention.sql",
        "004_activation.sql",
        "005_activation_audit.sql",
    ), start=1):
        path = MIGRATIONS / name
        suite.psql(path.read_text(encoding="utf-8"))
        checksum = hashlib.sha256(path.read_bytes()).hexdigest()
        suite.psql(
            "INSERT INTO shin_server.schema_migrations(version,checksum) "
            f"VALUES ({version},'{checksum}')"
        )


def _history(suite: Any) -> str:
    return suite.scalar(
        "SELECT COALESCE(string_agg(version::text, ',' ORDER BY version), '') "
        "FROM shin_server.schema_migrations"
    )


def check_observer_workspace_upgrade(suite: Any) -> None:
    suite.reset_schema()
    try:
        _migrations(suite)
        path = MIGRATIONS / "006_workspaces.sql"
        suite.psql("SET shin_server.migration_source_host_id='upgrade-host';" + path.read_text())
        checksum = hashlib.sha256(path.read_bytes()).hexdigest()
        suite.psql(f"INSERT INTO shin_server.schema_migrations(version,checksum) VALUES (6,'{checksum}')")
        suite.psql("INSERT INTO shin_server.projects(id,display_name) VALUES ('upgrade','Upgrade');"
                   "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,root,root_identity) "
                   "VALUES ('upgrade','primary','upgrade-host','Local','/unchanged/local','10:20');"
                   "INSERT INTO shin_server.workspace_requests(principal_id,key,request_hash,project_id,workspace_id,response) "
                   "SELECT 'dev-admin','upgrade-local',repeat('a',64),'upgrade','primary',to_jsonb(w) "
                   "FROM shin_server.workspaces w WHERE project_id='upgrade'")
        before = suite.scalar("SELECT response::text FROM shin_server.workspace_requests WHERE key='upgrade-local'")
        suite.migrate()
        _check(_history(suite) == "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35", "Observer upgrade history is incomplete")
        _check(suite.scalar("SELECT root || ':' || root_identity || ':' || source_kind FROM shin_server.workspaces WHERE project_id='upgrade'") ==
               "/unchanged/local:10:20:local", "Observer upgrade changed the physical workspace binding")
        after = json.loads(suite.scalar("SELECT response::text FROM shin_server.workspace_requests WHERE key='upgrade-local'"))
        _check(after.pop("source_kind") == "local" and after == json.loads(before), "Observer upgrade changed historical response fields")
        _check(suite.scalar("SELECT request_hash FROM shin_server.workspace_requests WHERE key='upgrade-local'") == "a" * 64,
               "Observer upgrade changed a consumed request identity")
        server_id = suite.scalar("SELECT server_id FROM shin_server.server_identity WHERE singleton")
        suite.migrate()
        _check(suite.scalar("SELECT server_id FROM shin_server.server_identity WHERE singleton") == server_id,
               "Repeated migration rotated durable Server identity")
    finally:
        suite.reset_schema()


def check_observer_history_upgrade(suite: Any) -> None:
    suite.reset_schema()
    try:
        _migrations(suite)
        for version, name in ((6, "006_workspaces.sql"), (7, "007_observer_ingestion.sql")):
            path = MIGRATIONS / name
            suite.psql("SET shin_server.migration_source_host_id='upgrade-host';" + path.read_text())
            checksum = hashlib.sha256(path.read_bytes()).hexdigest()
            suite.psql(f"INSERT INTO shin_server.schema_migrations(version,checksum) VALUES ({version},'{checksum}')")
        suite.psql("INSERT INTO shin_server.projects(id,display_name) VALUES ('upgrade','Upgrade');"
                   "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,source_kind) "
                   "VALUES ('upgrade','primary','upgrade-host','Observer','observer');"
                   "INSERT INTO shin_server.observer_streams(stream_id,project_id,workspace_id,principal_id,observer_id,source_host_id,profile,binding_digest,enrollment_hash,accepted_sequence,observed_sequence,retained_bytes) "
                   "VALUES (repeat('a',32),'upgrade','primary','dev-reader','upgrade-observer','upgrade-host','upgrade-profile',repeat('b',64),repeat('c',64),1,2,32);"
                   "INSERT INTO shin_server.observer_submissions(stream_id,sequence,submission_id,submission_hash,payload_hash,submission,receipt,retained_bytes) "
                   "VALUES (repeat('a',32),1,repeat('d',64),repeat('e',64),repeat('f',64),'{\"original\":1}','{\"receipt\":1}',32);"
                   "UPDATE shin_server.observer_usage SET streams=1,submissions=1,retained_bytes=32;"
                   "INSERT INTO shin_server.audit_events(principal_id,action,project_id,workspace_id,observer_stream_id,observer_sequence) "
                   "VALUES ('dev-reader','observer.submission.accepted','upgrade','primary',repeat('a',32),1)")
        before = suite.scalar("SELECT jsonb_build_object('usage',(SELECT to_jsonb(u) FROM shin_server.observer_usage u),"
                              "'receipt',(SELECT to_jsonb(s) FROM shin_server.observer_submissions s),"
                              "'audit',(SELECT (to_jsonb(a)-'publication_proposal_id') FROM shin_server.audit_events a))::text")
        suite.migrate()
        after = suite.scalar("SELECT jsonb_build_object('usage',(SELECT (to_jsonb(u)-'tombstones') || "
                             "jsonb_build_object('server_id',(SELECT server_id FROM shin_server.server_identity)) FROM shin_server.observer_usage u),"
                             "'receipt',(SELECT to_jsonb(s) FROM shin_server.observer_submissions s),"
                             "'audit',(SELECT (to_jsonb(a)-'publication_proposal_id') FROM shin_server.audit_events a))::text")
        _check(before == after, "History upgrade rewrote prior ledger, audit or identity")
        _check(_history(suite) == "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35", "History upgrade versions are incomplete")
        _check(suite.scalar("SELECT confirmed_sequence::text || ':' || retired_sequence::text || ':' || (last_retention IS NULL)::text "
                            "FROM shin_server.observer_streams") == "0:0:true", "History upgrade invented producer confirmation")
    finally:
        suite.reset_schema()


def check_audit_history_upgrade(suite: Any) -> None:
    """Migrations 009 and 010 preserve the existing audit and Observer ledger."""
    suite.reset_schema()
    try:
        _migrations(suite)
        for version, name in ((6, "006_workspaces.sql"), (7, "007_observer_ingestion.sql"),
                              (8, "008_observer_history.sql")):
            path = MIGRATIONS / name
            suite.psql("SET shin_server.migration_source_host_id='upgrade-host';" + path.read_text())
            checksum = hashlib.sha256(path.read_bytes()).hexdigest()
            suite.psql(f"INSERT INTO shin_server.schema_migrations(version,checksum) VALUES ({version},'{checksum}')")
        suite.psql(
            "INSERT INTO shin_server.projects(id,display_name) VALUES ('upgrade','Upgrade');"
            "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,source_kind) "
            "VALUES ('upgrade','primary','upgrade-host','Observer','observer');"
            "INSERT INTO shin_server.observer_streams(stream_id,project_id,workspace_id,principal_id,observer_id,source_host_id,profile,binding_digest,enrollment_hash,accepted_sequence,observed_sequence,retained_bytes,confirmed_sequence,retired_sequence) "
            "VALUES (repeat('a',32),'upgrade','primary','dev-reader','upgrade-observer','upgrade-host','upgrade-profile',repeat('b',64),repeat('c',64),1,2,32,1,0);"
            "INSERT INTO shin_server.observer_submissions(stream_id,sequence,submission_id,submission_hash,payload_hash,submission,receipt,retained_bytes) "
            "VALUES (repeat('a',32),1,repeat('d',64),repeat('e',64),repeat('f',64),'{\"original\":1}','{\"receipt\":1}',32);"
            "UPDATE shin_server.observer_usage SET streams=1,submissions=1,retained_bytes=32;"
            "INSERT INTO shin_server.audit_events(principal_id,action,project_id,workspace_id,observer_stream_id,observer_sequence) "
            "VALUES ('dev-reader','observer.submission.accepted','upgrade','primary',repeat('a',32),1)"
        )
        before = suite.scalar(
            "SELECT jsonb_build_object('audit',(SELECT jsonb_agg((to_jsonb(a)-'publication_proposal_id') ORDER BY id) FROM shin_server.audit_events a),"
            "'streams',(SELECT jsonb_agg((to_jsonb(s)-'scope_tenant_id') ORDER BY stream_id) FROM shin_server.observer_streams s),"
            "'submissions',(SELECT jsonb_agg(to_jsonb(s) ORDER BY stream_id,sequence) FROM shin_server.observer_submissions s),"
            "'usage',(SELECT to_jsonb(u) FROM shin_server.observer_usage u))::text"
        )
        suite.migrate()
        after = suite.scalar(
            "SELECT jsonb_build_object('audit',(SELECT jsonb_agg((to_jsonb(a)-'publication_proposal_id') ORDER BY id) FROM shin_server.audit_events a),"
            "'streams',(SELECT jsonb_agg((to_jsonb(s)-'scope_tenant_id') ORDER BY stream_id) FROM shin_server.observer_streams s),"
            "'submissions',(SELECT jsonb_agg(to_jsonb(s) ORDER BY stream_id,sequence) FROM shin_server.observer_submissions s),"
            "'usage',(SELECT (to_jsonb(u)-'tombstones') || jsonb_build_object('server_id',"
            "(SELECT server_id FROM shin_server.server_identity)) FROM shin_server.observer_usage u))::text"
        )
        _check(before == after, "Audit history upgrade rewrote prior audit or Observer state")
        _check(_history(suite) == "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35", "Audit history upgrade versions are incomplete")
        _check(suite.scalar("SELECT retired_through::text || ':' || retired_events::text || ':' || "
                            "(last_retention IS NULL)::text FROM shin_server.audit_history WHERE singleton") == "0:0:true",
               "Audit history upgrade invented a retention receipt or pruned audit rows")
        _check(suite.scalar("SELECT count(*)::text || ':' || "
                            "(SELECT (last_recovery IS NULL)::text FROM shin_server.server_recovery WHERE singleton)") == "1:true",
               "Server recovery upgrade did not create an empty singleton recovery record")
    finally:
        suite.reset_schema()


def check_server_recovery_upgrade(suite: Any) -> None:
    """Migration 010 adds recovery state without rotating the durable Server identity."""
    suite.reset_schema()
    try:
        _migrations(suite)
        for version, name in ((6, "006_workspaces.sql"), (7, "007_observer_ingestion.sql"),
                              (8, "008_observer_history.sql"), (9, "009_audit_history.sql")):
            path = MIGRATIONS / name
            suite.psql("SET shin_server.migration_source_host_id='upgrade-host';" + path.read_text())
            checksum = hashlib.sha256(path.read_bytes()).hexdigest()
            suite.psql(f"INSERT INTO shin_server.schema_migrations(version,checksum) VALUES ({version},'{checksum}')")
        before = suite.scalar(
            "SELECT jsonb_build_object('usage',(SELECT to_jsonb(u) FROM shin_server.observer_usage u),"
            "'audit_history',(SELECT to_jsonb(a) FROM shin_server.audit_history a))::text"
        )
        suite.migrate()
        after = suite.scalar(
            "SELECT jsonb_build_object('usage',(SELECT (to_jsonb(u)-'tombstones') || jsonb_build_object('server_id',"
            "(SELECT server_id FROM shin_server.server_identity)) FROM shin_server.observer_usage u),"
            "'audit_history',(SELECT to_jsonb(a) FROM shin_server.audit_history a))::text"
        )
        _check(before == after, "Server recovery upgrade rewrote durable Server or audit lifetime state")
        _check(_history(suite) == "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35", "Server recovery upgrade versions are incomplete")
        _check(suite.scalar("SELECT count(*)::text || ':' || "
                            "(SELECT (last_recovery IS NULL)::text FROM shin_server.server_recovery WHERE singleton)") == "1:true",
               "Server recovery upgrade did not preserve the empty recovery singleton")
    finally:
        suite.reset_schema()


def _seed_project(suite: Any, project_id: str, root: str, identity: str) -> None:
    suite.psql(
        "INSERT INTO shin_server.projects(id,display_name,root,root_identity) VALUES "
        f"('{project_id}','{project_id} project','{root}','{identity}')"
    )


def _seed_success_case(suite: Any) -> None:
    _migrations(suite)
    _seed_project(suite, "legacy-one", "/legacy/one", "100:1")
    _seed_project(suite, "legacy-two", "/legacy/two", "100:2")
    suite.psql(
        "INSERT INTO shin_server.idempotency_requests(principal_id,key,request_hash,project) VALUES "
        "('dev-admin','legacy-project-key',repeat('a',64),"
        "'{\"id\":\"legacy-one\",\"display_name\":\"legacy-one project\","
        "\"root\":\"/legacy/one\",\"root_identity\":\"100:1\"}'::jsonb)"
    )
    preparation_id = suite.scalar(
        "INSERT INTO shin_server.preparations("
        "project_id,route_id,principal_id,idempotency_key,request_hash,route_fingerprint,"
        "expected_snapshot_id,state,worker_id,artifact_snapshot_id,artifact_bytes,"
        "reserved_artifact_bytes,artifact_namespace,artifact_key,expected_route_revision) VALUES ("
        "'legacy-one','legacy-route','dev-admin','legacy-preparation-key',repeat('b',64),"
        f"'{LEGACY_FINGERPRINT}','{LEGACY_SNAPSHOT}','prepared','legacy-worker-0001',"
        f"'{LEGACY_SNAPSHOT}',1,1024,repeat('d',64),repeat('e',64),0) RETURNING id"
    )
    _check(preparation_id.isdigit(), "legacy preparation fixture did not receive an identity")
    suite.psql(
        "INSERT INTO shin_server.active_routes(project_id,route_id,revision,preparation_id,snapshot_id,route_fingerprint) VALUES "
        f"('legacy-one','legacy-route',1,{preparation_id},'{LEGACY_SNAPSHOT}','{LEGACY_FINGERPRINT}')"
    )
    suite.psql(
        "INSERT INTO shin_server.activation_receipts("
        "project_id,route_id,previous_revision,revision,preparation_id,snapshot_id,principal_id) VALUES "
        f"('legacy-one','legacy-route',0,1,{preparation_id},'{LEGACY_SNAPSHOT}','dev-admin')"
    )


def _assert_success_case(suite: Any) -> None:
    _check(_history(suite) == "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35", "workspace migration did not retain ordered history")
    _check(suite.scalar(
        "SELECT (SELECT count(*) FROM shin_server.projects)||':'||"
        "(SELECT count(*) FROM shin_server.workspaces)||':'||"
        "(SELECT count(*) FROM shin_server.preparations)||':'||"
        "(SELECT count(*) FROM shin_server.active_routes)||':'||"
        "(SELECT count(*) FROM shin_server.activation_receipts)"
    ) == "2:2:1:1:1", "workspace migration changed logical or publication row counts")
    _check(suite.scalar(
        "SELECT count(*) FROM pg_attribute WHERE attrelid='shin_server.projects'::regclass "
        "AND attname IN ('root','root_identity') AND NOT attisdropped"
    ) == "0", "logical projects retained physical root columns")
    _check(suite.scalar(
        "SELECT count(*) FROM shin_server.workspaces WHERE id='primary' AND source_host_id='local-dev' "
        "AND (project_id,root,root_identity) IN ("
        "('legacy-one','/legacy/one','100:1'),('legacy-two','/legacy/two','100:2'))"
    ) == "2", "workspace migration did not backfill every primary local binding")
    _check(suite.scalar(
        "SELECT count(*) FROM shin_server.idempotency_requests WHERE principal_id='dev-admin' "
        "AND key='legacy-project-key' AND request_hash=repeat('a',64) "
        "AND project->>'root'='/legacy/one' AND project->>'root_identity'='100:1'"
    ) == "1", "legacy project receipt was rewritten or released")
    _check(suite.scalar(
        "SELECT count(*) FROM shin_server.preparations WHERE project_id='legacy-one' "
        f"AND route_id='legacy-route' AND route_fingerprint='{LEGACY_FINGERPRINT}'"
    ) == "1", "legacy preparation fingerprint changed during migration")
    _check(suite.scalar(
        "SELECT count(*) FROM shin_server.active_routes WHERE project_id='legacy-one' "
        f"AND route_id='legacy-route' AND revision=1 AND route_fingerprint='{LEGACY_FINGERPRINT}'"
    ) == "1", "migration created or rewrote the historical active route")


def _assert_duplicate_failure(suite: Any) -> None:
    _migrations(suite)
    _seed_project(suite, "legacy-one", "/legacy/one", "100:3")
    _seed_project(suite, "legacy-two", "/legacy/two", "100:3")
    try:
        suite.migrate()
    except RuntimeError as error:
        _check(str(error) == "explicit migration failed with exit status 1", "migration failed unexpectedly")
    else:
        raise RuntimeError("duplicate legacy physical identity unexpectedly migrated")
    _check(_history(suite) == "1,2,3,4,5", "failed migration changed history")
    _check(suite.scalar("SELECT to_regclass('shin_server.workspaces') IS NULL") == "t",
           "failed migration left a partial workspace table")
    _check(suite.scalar(
        "SELECT count(*) FROM pg_attribute WHERE attrelid='shin_server.projects'::regclass "
        "AND attname IN ('root','root_identity') AND NOT attisdropped"
    ) == "2", "failed migration removed legacy project binding columns")


def check_workspace_migration_upgrade(suite: Any) -> None:
    suite.reset_schema()
    try:
        _seed_success_case(suite)
        suite.migrate()
        _assert_success_case(suite)
    finally:
        suite.reset_schema()
    try:
        _assert_duplicate_failure(suite)
    finally:
        suite.reset_schema()
