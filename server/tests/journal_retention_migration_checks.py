"""Preserve populated schema014 facts and initialize only the new retirement state."""

import hashlib
from pathlib import Path
from typing import Any

from publication_test_support import require
from publication_withdrawal_migration_checks import assert_sql_refused


def check_journal_retention_upgrade(suite: Any) -> None:
    suite.reset_schema()
    try:
        paths = sorted((Path(__file__).resolve().parents[1] / "migrations").glob("[0-9][0-9][0-9]_*.sql"))
        for version, path in enumerate(paths[:14], 1):
            suite.psql("SET shin_server.migration_source_host_id='upgrade-host';" + path.read_text())
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            suite.psql(f"INSERT INTO shin_server.schema_migrations(version,checksum) VALUES({version},'{digest}')")
        suite.psql("INSERT INTO shin_server.projects(id,display_name) VALUES('upgrade','Upgrade');"
            "INSERT INTO shin_server.retrieval_journal_producers(instance_id,source_host_id,server_version,core_version,"
            "observed_through,captured_count,missing_count) VALUES(repeat('a',64),'upgrade-host','old-server','old-core',3,1,2);"
            "INSERT INTO shin_server.retrieval_journal_attempts(instance_id,sequence,record_hash,project_id,operation,selector_kind,"
            "selector_id,observed_at_unix_ms,status,reason_code,last_phase,elapsed_ns,validation_ns,query_capture) "
            "VALUES(repeat('a',64),2,repeat('b',64),'upgrade','search','route','test',1,400,'fixture','validation',5,5,'disabled')")
        fact_sql = "SELECT (to_jsonb(j)-'producer_epoch'-'scope_principal_id'-'scope_tenant_id'-'scope_workspace_id')::text FROM shin_server.retrieval_journal_attempts j"
        producer_sql = "SELECT (to_jsonb(p)-'retired_through'-'retired_count'-'retired_missing_count'-'last_retention'-'producer_epoch')::text FROM shin_server.retrieval_journal_producers p"
        before = (suite.scalar(fact_sql), suite.scalar(producer_sql))
        suite.migrate()
        require((suite.scalar(fact_sql), suite.scalar(producer_sql)) == before, "schema015 changed historical journal facts or provenance")
        require(suite.scalar("SELECT count(*) FROM shin_server.retrieval_journal_attempts WHERE scope_principal_id IS NOT NULL "
                             "OR scope_tenant_id IS NOT NULL OR scope_workspace_id IS NOT NULL") == "0", "migration attributed schema014 history")
        require(suite.scalar("SELECT retired_through||':'||retired_count||':'||retired_missing_count||':'||"
                             "(last_retention IS NULL)::text FROM shin_server.retrieval_journal_producers") == "0:0:0:true",
                "schema015 fabricated retirement history")
        suite.migrate()
        require((suite.scalar(fact_sql), suite.scalar(producer_sql)) == before, "migration retry changed journal history")
        for assignment in ("retired_through=4,retired_missing_count=4", "retired_through=2,retired_count=2",
                           "retired_through=1", "retired_missing_count=-1", "last_retention='[]'::jsonb",
                           "last_retention=jsonb_build_object('x',repeat('x',65536))"):
            assert_sql_refused(suite, "UPDATE shin_server.retrieval_journal_producers SET " + assignment, "check_violation")
        require(suite.scalar("SELECT has_table_privilege('shin_runtime','shin_server.retrieval_journal_attempts','DELETE')") == "t",
                "runtime retention grant is missing")
    finally:
        suite.reset_schema()
