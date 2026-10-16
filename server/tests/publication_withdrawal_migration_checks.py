"""Schema 012-to-013 preservation, typed-base constraints and runtime grants."""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any

from publication_test_support import require


# Frozen before P3E13: calculating both sides from current files would miss a rewrite.
MIGRATION_001_012_SHA256 = (
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
)


def assert_sql_refused(suite: Any, sql: str, condition: str, *, runtime: bool = False) -> None:
    require(condition in {"insufficient_privilege", "unique_violation", "check_violation", "foreign_key_violation"},
            "unknown expected SQL failure")
    # Unexpected success raises outside the caught SQLSTATE and rolls back the fixture.
    suite.psql("BEGIN; " + ("SET LOCAL ROLE shin_runtime; " if runtime else "") +
               "DO $check$ BEGIN " + sql + "; RAISE EXCEPTION 'test expected SQL refusal'; "
               "EXCEPTION WHEN " + condition + " THEN NULL; END $check$; ROLLBACK")


def check_withdrawal_constraints(suite: Any) -> None:
    suite.psql(
        "INSERT INTO shin_server.publication_withdrawals(project_id,publication_id,expected_revision,revision,"
        "previous_proposal_id,previous_preparation_id,withdrawn_by) "
        "SELECT project_id,publication_id,revision,revision+1,proposal_id,preparation_id,'dev-admin' "
        "FROM shin_server.publication_heads; DELETE FROM shin_server.publication_heads"
    )
    unique = suite.scalar(
        "SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.publication_withdrawals'::regclass "
        "AND contype='u' AND conkey=ARRAY[(SELECT attnum FROM pg_attribute "
        "WHERE attrelid='shin_server.publication_withdrawals'::regclass AND attname='previous_proposal_id')]::smallint[]"
    )
    require(unique == "1", "withdrawals are not structurally bounded by accepted proposals")
    assert_sql_refused(suite,
        "INSERT INTO shin_server.publication_withdrawals(project_id,publication_id,expected_revision,revision,"
        "previous_proposal_id,previous_preparation_id,withdrawn_by) SELECT project_id,publication_id,expected_revision,revision,"
        "previous_proposal_id,previous_preparation_id,withdrawn_by FROM shin_server.publication_withdrawals", "unique_violation")
    columns = ("project_id,publication_id,preparation_id,base_revision,base_preparation_id,base_withdrawal_id,"
               "submitted_by,idempotency_key,request_hash,source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint")

    def proposal_sql(publication: str, revision: str, preparation: str, withdrawal: str) -> str:
        return (f"INSERT INTO shin_server.publication_proposals({columns}) SELECT p.project_id,{publication},p.preparation_id,"
                f"{revision},{preparation},{withdrawal},p.submitted_by,'withdraw-upgrade-base',p.request_hash,p.source_route_id,"
                "p.workspace_id,p.source_host_id,p.profile_id,p.route_fingerprint FROM shin_server.publication_proposals p "
                "CROSS JOIN shin_server.publication_withdrawals w WHERE p.idempotency_key='upgrade-initial'")

    for revision, preparation, withdrawal in (("0", "NULL", "w.id"), ("w.revision", "p.preparation_id", "w.id"),
                                               ("w.revision", "NULL", "NULL")):
        assert_sql_refused(suite, proposal_sql("p.publication_id", revision, preparation, withdrawal), "check_violation")
    for publication, revision, withdrawal in (("'wrong-publication'", "w.revision", "w.id"),
                                               ("p.publication_id", "w.revision+1", "w.id"),
                                               ("p.publication_id", "w.revision", "w.id+100000")):
        assert_sql_refused(suite, proposal_sql(publication, revision, "NULL", withdrawal), "foreign_key_violation")
    suite.psql(proposal_sql("p.publication_id", "w.revision", "NULL", "w.id"))
    require(suite.scalar("SELECT count(*) FROM shin_server.publication_proposals p "
                         "JOIN shin_server.publication_withdrawals w ON p.base_withdrawal_id=w.id "
                         "WHERE p.base_revision=w.revision AND p.base_preparation_id IS NULL") == "1",
            "valid typed withdrawal base could not be persisted")


def check_publication_withdrawal_migration(suite: Any) -> None:
    suite.reset_schema()
    try:
        migrations = sorted((Path(__file__).resolve().parents[1] / "migrations").glob("[0-9][0-9][0-9]_*.sql"))
        require([int(path.name[:3]) for path in migrations] == list(range(1, 36)), "current migration inventory differs")
        require(hashlib.sha256(migrations[13].read_bytes()).hexdigest() ==
                "4389372a5fbd8cb0740b73a9f981644e98719193a010cd683932953e64fc2811",
                "retention migration rewrote the published schema014 migration")
        require(hashlib.sha256(migrations[12].read_bytes()).hexdigest() ==
                "42d2fe385797cefc213bc205a548e1f7608c8dff763e2a7640f7316efa2c4e9a",
                "journal migration rewrote the published schema013 migration")
        for version, path in enumerate(migrations[:12], start=1):
            checksum = hashlib.sha256(path.read_bytes()).hexdigest()
            require(checksum == MIGRATION_001_012_SHA256[version - 1], "P3E13 rewrote migration " + str(version))
            suite.psql("SET shin_server.migration_source_host_id='upgrade-host';" + path.read_text(encoding="utf-8"))
            suite.psql(f"INSERT INTO shin_server.schema_migrations(version,checksum) VALUES ({version},'{checksum}')")
        suite.psql(
            "INSERT INTO shin_server.projects(id,display_name) VALUES ('upgrade','Upgrade');"
            "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,root,root_identity) "
            "VALUES ('upgrade','primary','upgrade-host','Neutral fixture','/fixture/source','fixture-source');"
            "INSERT INTO shin_server.preparations(project_id,route_id,principal_id,idempotency_key,request_hash,route_fingerprint,"
            "expected_snapshot_id,state,worker_id,artifact_snapshot_id,artifact_bytes,reserved_artifact_bytes,artifact_namespace,artifact_key) "
            "VALUES ('upgrade','source','dev-admin','upgrade-job',repeat('a',64),repeat('b',64),'','prepared',"
            "'upgrade-worker-0001','fnv1a64:0123456789abcdef',512,1024,repeat('c',64),repeat('d',64));"
            "INSERT INTO shin_server.publication_proposals(project_id,publication_id,preparation_id,base_revision,base_preparation_id,"
            "submitted_by,idempotency_key,request_hash,source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint) "
            "SELECT 'upgrade','shared',id,0,NULL,'dev-admin','upgrade-initial',repeat('e',64),'source','primary',"
            "'upgrade-host','generic-docs',route_fingerprint FROM shin_server.preparations;"
            "INSERT INTO shin_server.publication_resolutions(proposal_id,project_id,publication_id,decision,expected_revision,revision,"
            "preparation_id,resolved_by) SELECT id,project_id,publication_id,'accept',0,1,preparation_id,'dev-admin' "
            "FROM shin_server.publication_proposals;"
            "INSERT INTO shin_server.publication_proposals(project_id,publication_id,preparation_id,base_revision,base_preparation_id,"
            "submitted_by,idempotency_key,request_hash,source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint) "
            "SELECT project_id,publication_id,preparation_id,1,preparation_id,submitted_by,'upgrade-'||decision,request_hash,"
            "source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint FROM shin_server.publication_proposals "
            "CROSS JOIN (VALUES ('accept'),('reject'),('request-reprepare')) decisions(decision);"
            "INSERT INTO shin_server.publication_resolutions(proposal_id,project_id,publication_id,decision,expected_revision,revision,"
            "preparation_id,resolved_by) SELECT id,project_id,publication_id,substr(idempotency_key,9),1,"
            "CASE WHEN idempotency_key='upgrade-accept' THEN 2 ELSE 1 END,preparation_id,'dev-admin' "
            "FROM shin_server.publication_proposals WHERE idempotency_key<>'upgrade-initial';"
            "INSERT INTO shin_server.publication_heads(project_id,publication_id,revision,preparation_id,snapshot_id,route_fingerprint,proposal_id) "
            "SELECT r.project_id,r.publication_id,r.revision,r.preparation_id,p.artifact_snapshot_id,p.route_fingerprint,r.proposal_id "
            "FROM shin_server.publication_resolutions r JOIN shin_server.preparations p ON p.id=r.preparation_id "
            "WHERE r.decision='accept' AND r.revision=2;"
            "INSERT INTO shin_server.audit_events(principal_id,action,project_id,preparation_id,publication_proposal_id) "
            "SELECT 'dev-admin','publication.accepted',project_id,preparation_id,proposal_id "
            "FROM shin_server.publication_resolutions WHERE decision='accept'"
        )
        snapshot = (
            "SELECT jsonb_build_object("
            "'projects',(SELECT jsonb_agg(to_jsonb(t) ORDER BY id) FROM shin_server.projects t),"
            "'workspaces',(SELECT jsonb_agg(to_jsonb(t) ORDER BY id) FROM shin_server.workspaces t),"
            "'preparations',(SELECT jsonb_agg(to_jsonb(t)-'workspace_admission'-'scope_tenant_id'-'scope_workspace_id' ORDER BY id) FROM shin_server.preparations t),"
            "'proposals',(SELECT jsonb_agg(to_jsonb(t)-ARRAY['base_withdrawal_id','history_epoch','base_source_route_id',"
            "'base_workspace_id','base_source_host_id','base_profile_id','base_route_fingerprint','history_workspace_id'] ORDER BY id) FROM shin_server.publication_proposals t),"
            "'resolutions',(SELECT jsonb_agg(to_jsonb(t) ORDER BY proposal_id) FROM shin_server.publication_resolutions t),"
            "'heads',(SELECT jsonb_agg(to_jsonb(t) ORDER BY project_id,publication_id) FROM shin_server.publication_heads t),"
            "'audit',(SELECT jsonb_agg(to_jsonb(t) ORDER BY id) FROM shin_server.audit_events t),"
            "'identity',(SELECT to_jsonb(t) FROM shin_server.server_identity t),"
            "'history',(SELECT jsonb_agg(to_jsonb(t) ORDER BY version) FROM shin_server.schema_migrations t WHERE version<=12))::text"
        )
        before = suite.scalar(snapshot)
        # Exercise the original schema013 FK contract before schema019 captures
        # base identity independently so its parent history can explicitly retire.
        suite.psql(migrations[12].read_text(encoding="utf-8"))
        checksum = hashlib.sha256(migrations[12].read_bytes()).hexdigest()
        suite.psql(f"INSERT INTO shin_server.schema_migrations(version,checksum) VALUES (13,'{checksum}')")
        require(suite.scalar(snapshot) == before, "migration013 changed existing publication state")
        check_withdrawal_constraints(suite)
        before = suite.scalar(snapshot)
        withdrawals = suite.scalar("SELECT jsonb_agg(to_jsonb(w) ORDER BY id)::text FROM shin_server.publication_withdrawals w")
        suite.migrate()
        require(suite.scalar(snapshot) == before, "migration013 changed old active head, receipts, provenance or checksums")
        require(suite.scalar("SELECT string_agg(version::text,',' ORDER BY version) FROM shin_server.schema_migrations") ==
                "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35", "withdrawal migration history differs")
        require(suite.scalar("SELECT checksum FROM shin_server.schema_migrations WHERE version=13") ==
                hashlib.sha256(migrations[12].read_bytes()).hexdigest(), "migration013 checksum differs")
        require(suite.scalar("SELECT jsonb_agg(to_jsonb(w) ORDER BY id)::text FROM shin_server.publication_withdrawals w") == withdrawals,
                "current upgrade changed existing withdrawals")
        require(suite.scalar("SELECT count(*) FROM shin_server.publication_proposals WHERE base_withdrawal_id IS NOT NULL") == "1",
                "current upgrade lost typed withdrawal bases")
        require(suite.scalar(
            "SELECT has_table_privilege('shin_runtime','shin_server.publication_withdrawals','SELECT')::text||':'||"
            "has_table_privilege('shin_runtime','shin_server.publication_withdrawals','INSERT')::text||':'||"
            "has_table_privilege('shin_runtime','shin_server.publication_withdrawals','UPDATE')::text||':'||"
            "has_table_privilege('shin_runtime','shin_server.publication_withdrawals','DELETE')::text||':'||"
            "has_table_privilege('shin_runtime','shin_server.publication_heads','DELETE')::text"
        ) == "true:true:false:true:true", "withdrawal ledger runtime grants differ")
        assert_sql_refused(suite, "UPDATE shin_server.publication_withdrawals SET withdrawn_by='dev-reader'",
                           "insufficient_privilege", runtime=True)
        suite.migrate()
        require(suite.scalar(snapshot) == before, "migration013 replay rewrote old state")
    finally:
        suite.reset_schema()
