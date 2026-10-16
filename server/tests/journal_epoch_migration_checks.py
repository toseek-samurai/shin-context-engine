"""Schema 015-to-016 journal epoch preservation and lifecycle initialization."""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any

from publication_test_support import require
from publication_withdrawal_migration_checks import assert_sql_refused


MIGRATION_001_015_SHA256 = (
    "677a8023ec823286cb06cac8be057dd6e670705244253b5ce0da06b9dcfbe905",
    "fdf8f1810bc7aaa9411fae530f375da1bf8941fd6829719c2234cf8d7ffd1eb1",
    "2cf8c80f698d4fe6e131703638bba8527b8fd7ee2d4a38053cc628d3644e1048",
    "13bbb30003d5484bde1f5227c28583035e4ca3a4c83c5025c9b407b6f595247f",
    "845b58261562e466937c5085c3dc222529900290eefc6999cd381be40e3833e2",
    "25b651c17e4c6e1ba5b0d16e6447d30183eedea343a230d31caea527d181a0bf",
    "0bd7907553fe05632837d8241778409ef55b5674657bf56497da371936cbf90b",
    "5ee559ebf554332c15fe10c11b20cf0d265cc57914c0f54183044a21bc7b1d95",
    "41bfb6c3403eab4115a6eba733c751cac549ce720307922ae31b1a72ed2ae4d0",
    "e32a6a05230ea93d67802ddae69083957116ba1f41bba065062db5a312a4501d",
    "306c450d2c88c6cadceff47577df17adf5062df7bdd32de7f798d7c0a687d34f",
    "8ace96ba99e5bb40e8d4054dae56b3ab50912817cc743ea63c0908360ea4b740",
    "42d2fe385797cefc213bc205a548e1f7608c8dff763e2a7640f7316efa2c4e9a",
    "4389372a5fbd8cb0740b73a9f981644e98719193a010cd683932953e64fc2811",
    "c8e6e9219da5ad5f164ba80223b0f15f17c472b94f1fca391e2092cc8df76aa3",
)


def check_journal_epoch_migration(suite: Any) -> None:
    """Integration hook: call after the schema015 retention migration check."""
    suite.reset_schema()
    try:
        paths = sorted((Path(__file__).resolve().parents[1] / "migrations").glob("[0-9][0-9][0-9]_*.sql"))
        require([int(path.name[:3]) for path in paths] == list(range(1, 36)), "current migration inventory differs")
        for version, path in enumerate(paths[:15], 1):
            require(hashlib.sha256(path.read_bytes()).hexdigest() == MIGRATION_001_015_SHA256[version - 1],
                    "schema016 rewrote migration " + str(version))
            suite.psql("SET shin_server.migration_source_host_id='upgrade-host';" + path.read_text(encoding="utf-8"))
            suite.psql(f"INSERT INTO shin_server.schema_migrations(version,checksum) VALUES({version},'{MIGRATION_001_015_SHA256[version - 1]}')")

        suite.psql(
            "INSERT INTO shin_server.projects(id,display_name) VALUES('upgrade','Upgrade');"
            "INSERT INTO shin_server.retrieval_journal_producers(instance_id,source_host_id,server_version,core_version,"
            "observed_through,captured_count,missing_count,created_at,updated_at,retired_through,retired_count,"
            "retired_missing_count,last_retention) VALUES"
            "(repeat('a',64),'upgrade-host','old-server','old-core',3,1,2,'2026-01-01T00:00:00Z','2026-01-01T00:00:00Z',0,0,0,NULL),"
            "(repeat('b',64),'upgrade-host','old-server','old-core',1,1,0,'2026-01-02T00:00:00Z','2026-01-02T00:00:00Z',1,1,0,"
            "'{\"plan\":{\"schema_version\":\"shin.retrieval-journal-retention-plan.v1\",\"plan_id\":\"legacy-plan\"},"
            "\"receipt\":{\"schema_version\":\"shin.retrieval-journal-retention-receipt.v1\",\"plan_id\":\"legacy-plan\"}}'::jsonb);"
            "INSERT INTO shin_server.retrieval_journal_attempts(instance_id,sequence,record_hash,project_id,operation,selector_kind,"
            "selector_id,observed_at_unix_ms,status,reason_code,last_phase,elapsed_ns,validation_ns,query_capture) "
            "VALUES(repeat('a',64),2,repeat('c',64),'upgrade','brief','route','test',1,400,'fixture','validation',5,5,'disabled')")
        fact_sql = "SELECT jsonb_agg(to_jsonb(j)-'producer_epoch'-'scope_principal_id'-'scope_tenant_id'-'scope_workspace_id' ORDER BY id)::text FROM shin_server.retrieval_journal_attempts j"
        producer_sql = "SELECT jsonb_agg(to_jsonb(p)-'producer_epoch' ORDER BY instance_id)::text FROM shin_server.retrieval_journal_producers p"
        legacy_receipt_sql = "SELECT last_retention::text FROM shin_server.retrieval_journal_producers WHERE instance_id=repeat('b',64)"
        before = (suite.scalar(fact_sql), suite.scalar(producer_sql), suite.scalar(legacy_receipt_sql))

        suite.migrate()
        require((suite.scalar(fact_sql), suite.scalar(producer_sql), suite.scalar(legacy_receipt_sql)) == before,
                "schema016 changed historical journal facts, hashes or producer metadata")
        require(suite.scalar("SELECT record_hash FROM shin_server.retrieval_journal_attempts") == "c" * 64,
                "schema016 rewrote the historical fact record_hash")
        require(suite.scalar(legacy_receipt_sql) == before[2], "schema016 rewrote the historical retention receipt")
        require(suite.scalar("SELECT string_agg(instance_id||':'||producer_epoch,',' ORDER BY instance_id) "
                             "FROM shin_server.retrieval_journal_producers")
                == (("a" * 64) + ":1," + ("b" * 64) + ":2"), "schema016 did not assign stable producer epochs")
        require(suite.scalar("SELECT producer_epoch FROM shin_server.retrieval_journal_attempts") == "1",
                "schema016 did not backfill fact producer_epoch")
        require(suite.scalar("SELECT allocated_through||':'||reclaimed_through||':'||reclaimed_producer_count||':'||"
                             "reclaimed_captured_count||':'||reclaimed_missing_count||':'||(last_reclamation IS NULL)::text "
                             "FROM shin_server.retrieval_journal_lifecycle") == "2:0:0:0:0:true",
                "schema016 did not establish the lifecycle singleton state")
        require(suite.scalar("SELECT has_table_privilege('shin_runtime','shin_server.retrieval_journal_producers','SELECT')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.retrieval_journal_producers','INSERT')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.retrieval_journal_producers','UPDATE')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.retrieval_journal_producers','DELETE')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.retrieval_journal_lifecycle','SELECT')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.retrieval_journal_lifecycle','INSERT')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.retrieval_journal_lifecycle','UPDATE')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.retrieval_journal_lifecycle','DELETE')::text")
                == "true:true:true:true:true:false:true:false", "runtime epoch lifecycle grants differ")
        assert_sql_refused(suite, "UPDATE shin_server.retrieval_journal_attempts SET producer_epoch=2", "foreign_key_violation")
        assert_sql_refused(suite, "UPDATE shin_server.retrieval_journal_lifecycle SET singleton=false", "check_violation")
        suite.migrate()
        require((suite.scalar(fact_sql), suite.scalar(producer_sql), suite.scalar(legacy_receipt_sql)) == before,
                "migration016 replay rewrote historical journal rows")
    finally:
        suite.reset_schema()
