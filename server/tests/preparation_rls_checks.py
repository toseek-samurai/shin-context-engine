"""SQL isolation on declared ledger fixtures; no artifact or lifecycle qualification."""
import json

from publication_test_support import require


SEED = """
INSERT INTO shin_server.projects(id,display_name) VALUES
 ('rls-a','Same label'),('rls-a-two','Same label'),('rls-b','Same label');
INSERT INTO shin_server.identity_accounts(id,principal_id,display_name) VALUES
 ('rls-a','rls-principal-a','A'),('rls-b','rls-principal-b','B');
INSERT INTO shin_server.identity_tenants VALUES('rls-tenant-a','A','rls-a'),('rls-tenant-b','B','rls-b');
INSERT INTO shin_server.identity_memberships VALUES
 ('rls-tenant-a','rls-a','owner',true,1),('rls-tenant-b','rls-b','owner',true,1);
INSERT INTO shin_server.identity_project_owners VALUES
 ('rls-a','rls-tenant-a'),('rls-a-two','rls-tenant-a'),('rls-b','rls-tenant-b');
INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,source_kind)
 SELECT p,w,'rls-host','Same checkout','observer' FROM
 (VALUES ('rls-a','primary'),('rls-a','secondary'),('rls-a-two','primary'),('rls-b','primary')) x(p,w);
INSERT INTO shin_server.identity_workspace_owners
 SELECT p.tenant_id,w.project_id,w.id,t.owner_id,'private' FROM shin_server.workspaces w
 JOIN shin_server.identity_project_owners p ON p.project_id=w.project_id
 JOIN shin_server.identity_tenants t ON t.id=p.tenant_id WHERE w.project_id LIKE 'rls-%';
INSERT INTO shin_server.preparations(project_id,route_id,principal_id,idempotency_key,request_hash,
 route_fingerprint,expected_snapshot_id,state,reserved_artifact_bytes,workspace_admission)
 SELECT w.project_id,'route',a.principal_id,'same-key',repeat('a',64),repeat('b',64),'','queued',1024,
 jsonb_build_object('authorization',jsonb_build_object('principal_id',a.principal_id,'ownership',
 jsonb_build_object('tenant_id',w.tenant_id,'project_id',w.project_id,'workspace_id',w.workspace_id)))
 FROM shin_server.identity_workspace_owners w JOIN shin_server.identity_accounts a ON a.id=w.owner_id
 WHERE w.project_id LIKE 'rls-%';
INSERT INTO shin_server.preparations(project_id,route_id,principal_id,idempotency_key,request_hash,
 route_fingerprint,expected_snapshot_id,state,reserved_artifact_bytes)
 VALUES ('rls-a','route','rls-maintainer','source',repeat('a',64),repeat('b',64),'','queued',1024),
 ('rls-a','other','rls-maintainer','unrelated',repeat('a',64),repeat('b',64),'','queued',1024);
INSERT INTO shin_server.preparation_retention(preparation_id,state,plan_id,inventory,requested_by)
 SELECT id,'intent',repeat('a',64),'{"entries":[]}'::jsonb,'rls-maintainer'
 FROM shin_server.preparations WHERE project_id LIKE 'rls-%';
DO $seed$ BEGIN PERFORM set_config('shin.rls_source_fixture',
 (SELECT id::text FROM shin_server.preparations WHERE project_id='rls-a' AND idempotency_key='source'),true); END $seed$;
"""


def context(principal="rls-principal-a", account="rls-a", tenant="rls-tenant-a", project="rls-a", workspace="primary"):
    settings = {"identity_principal": principal, "identity_account": account, "identity_tenant": tenant,
                "identity_ops": "0", "identity_legacy_projects": "", "content_project": project,
                "content_workspace": workspace, "preparation_legacy": "0", "preparation_sources": "", "preparation_ops": "0"}
    return "SET LOCAL ROLE shin_runtime; " + "".join(f"SET LOCAL shin.{k}='{v}'; " for k, v in settings.items())


SOURCE = "DO $scope$ BEGIN PERFORM set_config('shin.preparation_sources',current_setting('shin.rls_source_fixture'),true); END $scope$; "


def check_preparation_rls(s):
    def query(sql, scope=None, before=""):
        return s.scalar("BEGIN; " + SEED + before + (context() if scope is None else scope) + sql + "; ROLLBACK")

    for table in ("preparations", "preparation_retention"):
        count = "SELECT count(*) FROM shin_server." + table
        require(query(count, "SET LOCAL ROLE shin_runtime; ") == "0", "unscoped runtime saw preparation data")
        require(query(count) == "1", "own scope did not select exactly one ledger row")
        require(query(count, context(project="rls-a-two")) == "1", "same-owner second project lost its domain")
        require(query(count, context(workspace="secondary")) == "1", "same-owner second workspace lost its domain")
        require(query(count, context("rls-principal-b", "rls-b", "rls-tenant-b", "rls-b")) == "1", "second owner lost its domain")
        for scope in (context(project="rls-b"), context(tenant="rls-tenant-b"), context(account="rls-b")):
            require(query(count, scope) == "0", "forged coordinate crossed owner boundary")
        for revoke in ("UPDATE shin_server.identity_accounts SET enabled=false WHERE id='rls-a'; ",
                       "UPDATE shin_server.identity_memberships SET enabled=false WHERE account_id='rls-a'; "):
            require(query(count, before=revoke) == "0", "revoked authority remained readable")
            require(query(count, context() + SOURCE, before=revoke) == "0", "source grant survived revoked ownership")
        require(query(count, context() + SOURCE) == "2", "source grant exposed unrelated jobs or lost its parent retention")
        assignment = "route_fingerprint=route_fingerprint" if table == "preparations" else "inventory=inventory"
        require(query(f"WITH changed AS (UPDATE shin_server.{table} SET {assignment} RETURNING 1) SELECT count(*) FROM changed",
                      context() + SOURCE) == "1", "read-only source grant permitted a mutation")
        for finish in ("COMMIT", "ROLLBACK"):
            # Seed is rolled back; reuse the physical connection with no authority in the next transaction.
            sql = "BEGIN; " + SEED + context() + "SET LOCAL shin.preparation_ops='1'; ROLLBACK; "
            sql += "BEGIN; " + context() + "SET LOCAL shin.preparation_ops='1'; SET LOCAL shin.preparation_sources='123'; " + finish + "; "
            sql += "BEGIN; SET LOCAL ROLE shin_runtime; SELECT (" + count + ")=0 AND "
            sql += " AND ".join(f"COALESCE(current_setting('shin.{key}',true),'')=''" for key in
                                 ("preparation_ops", "preparation_sources", "content_project", "content_workspace", "identity_principal"))
            sql += "; ROLLBACK"
            require(s.scalar(sql) == "t", "worker, source or owner authority leaked across transactions")

    require(query("WITH locked AS (SELECT id FROM shin_server.preparations FOR UPDATE) SELECT count(*) FROM locked",
                  context() + SOURCE) == "1", "source SELECT grant became a write lock grant")
    require(query("WITH changed AS (UPDATE shin_server.preparations SET principal_id='rls-principal-a' "
                  "WHERE id::text=current_setting('shin.rls_source_fixture') RETURNING 1) SELECT count(*) FROM changed",
                  context() + SOURCE) == "0", "read-only legacy source could be adopted by UPDATE")

    def refused(sql):
        s.psql("BEGIN; " + SEED + context() + SOURCE + "DO $check$ BEGIN " + sql +
               "; RAISE EXCEPTION 'expected RLS refusal'; EXCEPTION WHEN insufficient_privilege THEN NULL; END $check$; ROLLBACK")
    # Durable ledgers already forbid DELETE at the table grant layer; do not weaken it for RLS tests.
    refused("DELETE FROM shin_server.preparations")
    refused("DELETE FROM shin_server.preparation_retention")
    refused("INSERT INTO shin_server.preparations(project_id,route_id,principal_id,idempotency_key,request_hash,"
            "route_fingerprint,expected_snapshot_id,state,reserved_artifact_bytes,workspace_admission) VALUES "
            "('rls-b','route','rls-principal-b','foreign-write',repeat('a',64),repeat('b',64),'','queued',1024,"
            "'{\"authorization\":{\"principal_id\":\"rls-principal-b\",\"ownership\":{\"tenant_id\":\"rls-tenant-b\","
            "\"project_id\":\"rls-b\",\"workspace_id\":\"primary\"}}}')")
    refused("UPDATE shin_server.preparations SET principal_id='rls-principal-b'")
    refused("UPDATE shin_server.preparations SET project_id='rls-a-two'")
    refused("UPDATE shin_server.preparations SET workspace_admission=NULL")
    refused("UPDATE shin_server.preparations SET workspace_admission=jsonb_set(workspace_admission,"
            "'{authorization,ownership,workspace_id}','\"secondary\"')")
    refused("UPDATE shin_server.preparation_retention SET preparation_id=current_setting('shin.rls_source_fixture')::bigint")
    refused("INSERT INTO shin_server.preparation_retention(preparation_id,state,plan_id,inventory,requested_by) "
            "VALUES(current_setting('shin.rls_source_fixture')::bigint,'intent',repeat('a',64),'{\"entries\":[]}','rls-principal-a')")
    legacy = context() + "SET LOCAL shin.preparation_legacy='1'; SET LOCAL shin.identity_legacy_projects='rls-a'; "
    # Explicit legacy reads may see NULL jobs, never scoped rows from their project grant alone.
    legacy += "SET LOCAL shin.content_project=''; SET LOCAL shin.content_workspace=''; "
    require(query("SELECT count(*) FROM shin_server.preparations", legacy) == "2", "legacy grants exposed scoped jobs")
    require(query("SELECT count(*) FROM shin_server.preparations WHERE project_id LIKE 'rls-%'",
                  context() + "SET LOCAL shin.preparation_ops='1'; ") == "6", "maintenance quota view omitted another domain")
    require(query("SELECT count(*) FROM shin_server.identity_accounts",
                  context(account="", tenant="", project="", workspace="") + "SET LOCAL shin.preparation_ops='1'; ") == "1",
            "preparation worker authority widened directory access")


def check_preparation_rls_drift(s):
    def health(status):
        s.wait_for_response("/health/ready", status, headers=s.auth_headers("admin"))
    for table, policy_name in (("preparations", "preparations_owner"), ("preparation_retention", "preparation_retention_owner")):
        for disable, restore in (("DISABLE ROW LEVEL SECURITY", "ENABLE ROW LEVEL SECURITY"),
                                 ("NO FORCE ROW LEVEL SECURITY", "FORCE ROW LEVEL SECURITY")):
            s.psql(f"ALTER TABLE shin_server.{table} {disable}")
            try:
                health(503)
            finally:
                s.psql(f"ALTER TABLE shin_server.{table} {restore}")
            health(200)
        policy = json.loads(s.scalar("SELECT to_json(pg_get_expr(polqual,polrelid))::text FROM pg_policy WHERE "
                                     f"polrelid='shin_server.{table}'::regclass AND polname='{policy_name}'"))
        s.psql(f"ALTER POLICY {policy_name} ON shin_server.{table} USING(true)")
        try:
            health(503)
        finally:
            s.psql(f"ALTER POLICY {policy_name} ON shin_server.{table} USING({policy})")
        health(200)
        s.psql(f"CREATE POLICY rls_test_bypass ON shin_server.{table} USING(true)")
        try:
            health(503)
        finally:
            s.psql(f"DROP POLICY rls_test_bypass ON shin_server.{table}")
        health(200)
