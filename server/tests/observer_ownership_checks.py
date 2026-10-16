"""Observer database scope, independent of HTTP filters and application replay."""
from preparation_rls_checks import SEED, context
from publication_test_support import require

STREAMS = """
INSERT INTO shin_server.observer_streams(stream_id,project_id,workspace_id,principal_id,observer_id,
 source_host_id,profile,binding_digest,enrollment_hash,scope_tenant_id)
 SELECT md5(w.project_id||w.workspace_id),w.project_id,w.workspace_id,a.principal_id,'same-observer',
 'rls-host','same-profile',repeat('a',64),repeat('b',64),w.tenant_id
 FROM shin_server.identity_workspace_owners w JOIN shin_server.identity_accounts a ON a.id=w.owner_id
 WHERE w.project_id LIKE 'rls-%';
INSERT INTO shin_server.observer_submissions
 SELECT stream_id,1,repeat(stream_id,2),repeat('c',64),repeat('d',64),'{}','{}',1,now()
 FROM shin_server.observer_streams WHERE project_id LIKE 'rls-%';
INSERT INTO shin_server.observer_tombstones(stream_id,project_id,workspace_id,principal_id,enrollment_hash,retirement,scope_tenant_id)
 SELECT md5(stream_id),project_id,workspace_id,principal_id,enrollment_hash,'{}',scope_tenant_id
 FROM shin_server.observer_streams WHERE project_id LIKE 'rls-%';
"""


def check_observer_rls(s):
    def query(sql, scope=None, before=""):
        return s.scalar("BEGIN; " + SEED + STREAMS + before + (context() if scope is None else scope) + sql + "; ROLLBACK")
    for table in ("observer_streams", "observer_submissions", "observer_tombstones"):
        count = "SELECT count(*) FROM shin_server." + table
        require(query(count) == "1", "Observer owner scope is not exact")
        require(query(count, "SET LOCAL ROLE shin_runtime; ") == "0", "unset Observer scope leaked data")
        require(query(count, context(project="rls-a-two")) == "1", "second project was not isolated")
        require(query(count, context(workspace="secondary")) == "1", "second workspace was not isolated")
        require(query(count, context("rls-principal-b", "rls-b", "rls-tenant-b", "rls-b")) == "1", "second owner lost data")
        for scope in (context(project="rls-b"), context(tenant="rls-tenant-b"), context(account="rls-b")):
            require(query(count, scope) == "0", "forged Observer coordinates crossed owner boundary")
        for change in ("UPDATE shin_server.identity_accounts SET enabled=false WHERE id='rls-a'; ",
                       "UPDATE shin_server.identity_memberships SET enabled=false WHERE account_id='rls-a'; "):
            require(query(count, before=change) == "0", "revocation left Observer history visible")
    require(query("SELECT count(*) FROM shin_server.observer_usage") == "0", "owner read global Observer quota")
    require(query("WITH changed AS (UPDATE shin_server.observer_streams SET revoked=true RETURNING 1) SELECT count(*) FROM changed") == "1",
            "unfiltered mutation crossed stream scope")
    s.psql("BEGIN; " + SEED + STREAMS + context() + "DO $check$ BEGIN "
           "UPDATE shin_server.observer_streams SET scope_tenant_id=NULL; RAISE EXCEPTION 'lost scope'; "
           "EXCEPTION WHEN insufficient_privilege THEN NULL; END $check$; ROLLBACK")
    require(s.scalar("BEGIN; " + context() + "SET LOCAL shin.observer_ops='1'; SET LOCAL shin.observer_quota='1'; COMMIT; "
                     "BEGIN; SET LOCAL ROLE shin_runtime; SELECT COALESCE(current_setting('shin.observer_ops',true),'')='' "
                     "AND COALESCE(current_setting('shin.observer_quota',true),'')=''; ROLLBACK") == "t", "Observer authority escaped transaction")
