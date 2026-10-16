"""Isolated schema 011-to-012 preservation and privilege checks."""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any

from integration_test import IntegrationFailure


def require(condition: bool, message: str) -> None:
    if not condition:
        raise IntegrationFailure(message)


def check_publication_migration(suite: Any) -> None:
    suite.reset_schema()
    try:
        migrations = sorted((Path(__file__).resolve().parents[1] / "migrations").glob("[0-9][0-9][0-9]_*.sql"))
        old = [path for path in migrations if int(path.name[:3]) <= 11]
        require([int(path.name[:3]) for path in old] == list(range(1, 12)), "schema011 fixture migration inventory differs")
        for version, path in enumerate(old, start=1):
            suite.psql("SET shin_server.migration_source_host_id='upgrade-host';" + path.read_text(encoding="utf-8"))
            checksum = hashlib.sha256(path.read_bytes()).hexdigest()
            suite.psql(f"INSERT INTO shin_server.schema_migrations(version,checksum) VALUES ({version},'{checksum}')")
        suite.psql(
            "INSERT INTO shin_server.projects(id,display_name) VALUES ('upgrade','Upgrade');"
            "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,root,root_identity) "
            "VALUES ('upgrade','primary','upgrade-host','Local source','/fixture/source','fixture-source');"
            "INSERT INTO shin_server.preparations(project_id,route_id,principal_id,idempotency_key,request_hash,"
            "route_fingerprint,expected_snapshot_id,state,reserved_artifact_bytes) "
            "VALUES ('upgrade','source','dev-admin','old-job',repeat('a',64),repeat('b',64),'','queued',1024);"
            "INSERT INTO shin_server.audit_events(principal_id,action,project_id,preparation_id) "
            "SELECT 'dev-admin','preparation.enqueued','upgrade',id FROM shin_server.preparations"
        )
        snapshot = (
            "SELECT jsonb_build_object("
            "'projects',(SELECT jsonb_agg(to_jsonb(p)) FROM shin_server.projects p),"
            "'workspaces',(SELECT jsonb_agg(to_jsonb(w)) FROM shin_server.workspaces w),"
            "'preparations',(SELECT jsonb_agg(to_jsonb(p)-'workspace_admission'-'scope_tenant_id'-'scope_workspace_id') FROM shin_server.preparations p),"
            "'audit',(SELECT jsonb_agg(to_jsonb(a)-'publication_proposal_id') FROM shin_server.audit_events a),"
            "'identity',(SELECT to_jsonb(i) FROM shin_server.server_identity i),"
            "'history',(SELECT jsonb_agg(to_jsonb(m) ORDER BY version) FROM shin_server.schema_migrations m WHERE version<=11))::text"
        )
        before = suite.scalar(snapshot)
        suite.migrate()
        require(suite.scalar(snapshot) == before, "publication migration changed pre-existing source/job/audit/identity/history")
        require(suite.scalar("SELECT string_agg(version::text,',' ORDER BY version) FROM shin_server.schema_migrations") ==
                "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35", "publication migration history differs")
        require(suite.scalar("SELECT count(*) FROM shin_server.audit_events WHERE publication_proposal_id IS NOT NULL") == "0",
                "publication migration invented proposal audit provenance")
        for table in ("publication_proposals", "publication_resolutions", "publication_heads", "publication_withdrawals"):
            require(suite.scalar(f"SELECT count(*) FROM shin_server.{table}") == "0", "migration auto-created publication state")
        require(suite.scalar(
            "SELECT has_table_privilege('shin_runtime','shin_server.publication_proposals','INSERT')::text||':'||"
            "has_table_privilege('shin_runtime','shin_server.publication_proposals','UPDATE')::text||':'||"
            "has_table_privilege('shin_runtime','shin_server.publication_resolutions','UPDATE')::text||':'||"
            "has_table_privilege('shin_runtime','shin_server.publication_resolutions','DELETE')::text||':'||"
            "has_table_privilege('shin_runtime','shin_server.publication_heads','UPDATE')::text||':'||"
            "has_table_privilege('shin_runtime','shin_server.publication_heads','DELETE')::text"
        ) == "true:false:false:true:true:true", "runtime publication grants differ from explicit-retirement policy")
        suite.migrate()
        require(suite.scalar(snapshot) == before, "migration replay rewrote historical state")
    finally:
        suite.reset_schema()
