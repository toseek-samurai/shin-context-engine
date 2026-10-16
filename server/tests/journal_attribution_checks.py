"""Scoped journal rows are read-only, live-authorized and separate from producer metadata."""
from preparation_rls_checks import SEED, context
from route_history_rls_checks import ROUTES
from publication_test_support import require

SCOPED = """
INSERT INTO shin_server.retrieval_journal_attempts(instance_id,producer_epoch,sequence,record_hash,project_id,operation,
 selector_kind,selector_id,observed_at_unix_ms,status,reason_code,last_phase,elapsed_ns,validation_ns,query_capture,
 request,request_fingerprint,scope_principal_id,scope_tenant_id,scope_workspace_id)
 SELECT p.instance_id,p.producer_epoch,row_number() OVER(ORDER BY w.project_id,w.workspace_id)+1,repeat('a',64),w.project_id,
 'brief','route','same-name',1,400,'invalid-request','validation',1,1,'captured','{"query":"same request"}',repeat('a',64),
 a.principal_id,w.tenant_id,w.workspace_id FROM shin_server.identity_workspace_owners w
 JOIN shin_server.identity_accounts a ON a.id=w.owner_id CROSS JOIN shin_server.retrieval_journal_producers p
 WHERE w.project_id LIKE 'rls-%' AND p.instance_id=repeat('e',64);
"""


def check_journal_attribution(s):
    def query(sql, scope=None, before=""):
        return s.scalar("BEGIN;" + SEED + ROUTES + SCOPED + before + (context() if scope is None else scope) + sql + ";ROLLBACK")
    count = "SELECT count(*) FROM shin_server.retrieval_journal_attempts"
    require(query(count, "SET LOCAL ROLE shin_runtime;") == "0", "unscoped journal access")
    for scope in (context(), context(workspace="secondary"), context(project="rls-a-two"),
                  context("rls-principal-b", "rls-b", "rls-tenant-b", "rls-b")):
        require(query(count, scope) == "1", "journal scope selected foreign or legacy rows")
        require(query("SELECT count(*) FROM shin_server.retrieval_journal_producers", scope) == "0", "private read exposed producer totals")
    for scope in (context(project="rls-b"), context(tenant="rls-tenant-b"), context(account="rls-b")):
        require(query(count, scope) == "0", "forged journal coordinate admitted")
    for revoke in ("UPDATE shin_server.identity_accounts SET enabled=false WHERE id='rls-a';",
                   "UPDATE shin_server.identity_memberships SET enabled=false WHERE account_id='rls-a';"):
        require(query(count, before=revoke) == "0", "revoked owner retained private history access")
    require(query("WITH d AS (DELETE FROM shin_server.retrieval_journal_attempts RETURNING 1) SELECT count(*) FROM d") == "0",
            "owner could delete immutable journal rows")
    s.psql("BEGIN;" + SEED + ROUTES + SCOPED + context() + "DO $test$ BEGIN "
           "INSERT INTO shin_server.retrieval_journal_attempts(instance_id,producer_epoch,sequence,record_hash,project_id,operation,"
           "selector_kind,selector_id,observed_at_unix_ms,status,reason_code,last_phase,elapsed_ns,validation_ns,query_capture,"
           "scope_principal_id,scope_tenant_id,scope_workspace_id) SELECT instance_id,producer_epoch,sequence+100,record_hash,project_id,"
           "operation,selector_kind,selector_id,observed_at_unix_ms,status,reason_code,last_phase,elapsed_ns,validation_ns,'disabled',"
           "scope_principal_id,scope_tenant_id,scope_workspace_id FROM shin_server.retrieval_journal_attempts; "
           "RAISE EXCEPTION 'owner inserted journal fact'; EXCEPTION WHEN insufficient_privilege THEN NULL; END $test$;ROLLBACK")
