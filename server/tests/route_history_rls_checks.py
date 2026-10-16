"""Runtime-role route/history isolation, live authorization and statement planning."""
from preparation_rls_checks import SEED, context
from publication_test_support import require

ROUTES = """
INSERT INTO shin_server.active_routes(project_id,route_id,revision,scope_workspace_id)
 SELECT project_id,scope_workspace_id,1,scope_workspace_id FROM shin_server.preparations WHERE scope_workspace_id IS NOT NULL;
INSERT INTO shin_server.active_routes(project_id,route_id,revision) VALUES('rls-a','legacy',1);
INSERT INTO shin_server.activation_receipts(project_id,route_id,previous_revision,revision,principal_id,scope_workspace_id)
 SELECT project_id,route_id,0,1,'rls-principal-a',scope_workspace_id FROM shin_server.active_routes WHERE project_id LIKE 'rls-%';
INSERT INTO shin_server.audit_events(principal_id,action,project_id) VALUES('rls-principal-a','fixture','rls-a');
INSERT INTO shin_server.retrieval_journal_producers(instance_id,source_host_id,server_version,core_version,producer_epoch,observed_through,captured_count)
 SELECT repeat('e',64),'rls-host','test','test',allocated_through+1,1,1 FROM shin_server.retrieval_journal_lifecycle;
INSERT INTO shin_server.retrieval_journal_attempts(instance_id,sequence,record_hash,project_id,operation,selector_kind,selector_id,
 observed_at_unix_ms,status,reason_code,last_phase,elapsed_ns,validation_ns,query_capture,request_fingerprint,request,producer_epoch)
 SELECT instance_id,1,repeat('a',64),'rls-a','brief','route','primary',1,400,'invalid-request','validation',1,1,
 'captured',repeat('c',64),'{"query":"private fixture request"}'::jsonb,producer_epoch
 FROM shin_server.retrieval_journal_producers WHERE instance_id=repeat('e',64);
"""


def check_route_history_rls(s):
    def query(sql, scope=None, before=""):
        return s.scalar("BEGIN; " + SEED + ROUTES + before + (context() if scope is None else scope) + sql + "; ROLLBACK")

    for table in ("active_routes", "activation_receipts"):
        count = f"SELECT count(*) FROM shin_server.{table} WHERE project_id LIKE 'rls-%'"
        require(query(count, "SET LOCAL ROLE shin_runtime;") == "0", "unscoped route history leaked")
        require(query(count) == "1", "own route namespace lost")
        require(query(count, context(workspace="secondary")) == "1", "secondary workspace mixed")
        require(query(count, context(project="rls-a-two")) == "1", "second project mixed")
        require(query(count, context("rls-principal-b", "rls-b", "rls-tenant-b", "rls-b")) == "1", "second owner mixed")
        require(query(count, context(project="rls-b")) == "0", "forged route project admitted")
        require(query(count, context() + "SET LOCAL shin.content_route='legacy';") == "2", "explicit legacy source lost")
        require(query(count, context() + "SET LOCAL shin.content_route='other';") == "1", "legacy source selected by project alone")
        for revoke in ("UPDATE shin_server.identity_accounts SET enabled=false WHERE id='rls-a';",
                       "UPDATE shin_server.identity_memberships SET enabled=false WHERE account_id='rls-a';"):
            require(query(count, before=revoke) == "0", "revoked route scope remained readable")
        require(query(count, context() + "SET LOCAL shin.route_refs='1';") == "5", "reference inventory dropped a domain")

    def refused(sql, route="primary"):
        s.psql("BEGIN;" + SEED + ROUTES + context() + f"SET LOCAL shin.content_route='{route}'; DO $test$ BEGIN " + sql +
               "; RAISE EXCEPTION 'expected row-security denial'; EXCEPTION WHEN insufficient_privilege THEN NULL; END $test$; ROLLBACK")
    refused("UPDATE shin_server.active_routes SET scope_workspace_id='secondary'")
    refused("UPDATE shin_server.active_routes SET scope_workspace_id=NULL")
    refused("UPDATE shin_server.active_routes SET project_id='rls-a-two'")
    refused("INSERT INTO shin_server.active_routes(project_id,route_id,revision,scope_workspace_id) VALUES('rls-b','primary',2,'primary')")
    refused("DELETE FROM shin_server.activation_receipts")
    for table, columns, values in (
        ("active_routes", "project_id,route_id,revision,preparation_id,snapshot_id,route_fingerprint,scope_workspace_id",
         "project_id,route_id,1,id,'fnv1a64:0123456789abcdef',route_fingerprint,'primary'"),
        ("activation_receipts", "project_id,route_id,previous_revision,revision,preparation_id,snapshot_id,principal_id,scope_workspace_id",
         "project_id,route_id,0,1,id,'fnv1a64:0123456789abcdef','rls-principal-a','primary'")):
        # The source SELECT grant alone must not permit promoting a foreign-workspace candidate.
        s.psql("BEGIN;" + SEED + ROUTES + "DO $seed$ BEGIN PERFORM set_config('shin.foreign_fixture',"
               "(SELECT id::text FROM shin_server.preparations WHERE project_id='rls-a' AND scope_workspace_id='secondary'),true); END $seed$;" +
               context() + "SET LOCAL shin.content_route='route'; "
               "DO $source$ BEGIN PERFORM set_config('shin.preparation_sources',current_setting('shin.foreign_fixture'),true); END $source$; "
               f"DO $test$ BEGIN INSERT INTO shin_server.{table}({columns}) SELECT {values} FROM shin_server.preparations "
               "WHERE id::text=current_setting('shin.foreign_fixture'); RAISE EXCEPTION 'foreign candidate promoted'; "
               "EXCEPTION WHEN insufficient_privilege THEN NULL; END $test$; ROLLBACK")
    require(query("WITH u AS (UPDATE shin_server.active_routes SET revision=2 RETURNING 1) SELECT count(*) FROM u",
                  context() + "SET LOCAL shin.content_route='primary';") == "1", "own update failed or touched foreign routes")
    # Explicit first scoped transition binds a legacy pointer; later deactivation cannot erase the binding.
    require(query("WITH u AS (UPDATE shin_server.active_routes SET scope_workspace_id='primary' WHERE route_id='legacy' "
                  "RETURNING scope_workspace_id) SELECT scope_workspace_id FROM u",
                  context() + "SET LOCAL shin.content_route='legacy';") == "primary", "scoped transition failed to bind route")

    for table in ("audit_events", "audit_history", "retrieval_journal_attempts", "retrieval_journal_producers", "retrieval_journal_lifecycle"):
        # Ledger/lifecycle rows are maintenance data, not automatically user content.
        require(query(f"SELECT count(*) FROM shin_server.{table}") == "0", "ordinary owner read service history")
        require(query(f"SELECT count(*) FROM shin_server.{table}", context() + "SET LOCAL shin.identity_ops='1';") == "0",
                "directory permission became history maintenance permission")
    require(query("SELECT count(*) FROM shin_server.audit_events", context() + "SET LOCAL shin.audit_append='1';") == "0",
            "audit writer could read history")
    require(query("WITH d AS (DELETE FROM shin_server.audit_events RETURNING 1) SELECT count(*) FROM d",
                  context() + "SET LOCAL shin.audit_append='1';") == "0", "audit writer could delete history")
    s.psql("BEGIN;" + SEED + context() + "SET LOCAL shin.audit_append='1'; "
           "INSERT INTO shin_server.audit_events(principal_id,action,project_id) VALUES('rls-principal-a','fixture','rls-a');ROLLBACK")
    refused("INSERT INTO shin_server.audit_events(principal_id,action,project_id) VALUES('rls-principal-a','fixture','rls-a')")
    require(query("SELECT count(*) FROM shin_server.audit_events WHERE project_id='rls-a'",
                  context() + "SET LOCAL shin.audit_ops='1';") == "1", "maintenance audit missing")
    require(query("SELECT count(*) FROM shin_server.retrieval_journal_producers WHERE instance_id=repeat('e',64)",
                  context() + "SET LOCAL shin.journal_ops='1';") == "1", "maintenance journal missing")
    require(query("SELECT request->>'query' FROM shin_server.retrieval_journal_attempts WHERE instance_id=repeat('e',64)",
                  context() + "SET LOCAL shin.journal_ops='1';") == "private fixture request", "journal fixture missing its payload")
    require(query("WITH d AS (DELETE FROM shin_server.retrieval_journal_attempts RETURNING 1) SELECT count(*) FROM d") == "0",
            "ordinary owner could delete journal facts")
    for finish in ("COMMIT", "ROLLBACK"):
        settings = ("audit_ops", "audit_append", "audit_projects", "journal_ops", "route_refs", "content_route")
        s.psql("BEGIN; " + "".join(f"SET LOCAL shin.{k}='1';" for k in settings) + finish + "; "
               "DO $test$ BEGIN IF " + " OR ".join(f"COALESCE(current_setting('shin.{k}',true),'')<>''" for k in settings) +
               " THEN RAISE EXCEPTION 'transaction scope leaked'; END IF; END $test$;")
    # STABLE is per statement, not cached permission: revoke inside the same connection/transaction.
    s.psql("BEGIN;" + SEED + context() + "DO $test$ BEGIN IF NOT shin_server.current_workspace_allowed() THEN "
           "RAISE EXCEPTION 'initial authorization absent'; END IF; END $test$; RESET ROLE; "
           "UPDATE shin_server.identity_accounts SET enabled=false WHERE id='rls-a'; SET LOCAL ROLE shin_runtime;"
           "DO $test$ BEGIN IF shin_server.current_workspace_allowed() THEN RAISE EXCEPTION 'stale function authorization'; "
           "END IF; END $test$; ROLLBACK")


def check_route_history_drift(s):
    def health(status):
        s.wait_for_response("/health/ready", status, headers=s.auth_headers("admin"))
    for table in ("active_routes", "activation_receipts", "audit_events", "audit_history",
                  "retrieval_journal_attempts", "retrieval_journal_producers", "retrieval_journal_lifecycle"):
        for off, on in (("DISABLE ROW LEVEL SECURITY", "ENABLE ROW LEVEL SECURITY"),
                        ("NO FORCE ROW LEVEL SECURITY", "FORCE ROW LEVEL SECURITY")):
            s.psql(f"ALTER TABLE shin_server.{table} {off}")
            try:
                health(503)
            finally:
                s.psql(f"ALTER TABLE shin_server.{table} {on}")
            health(200)
        s.psql(f"CREATE POLICY unexpected_access ON shin_server.{table} USING(true)")
        try:
            health(503)
        finally:
            s.psql(f"DROP POLICY unexpected_access ON shin_server.{table}")
        health(200)
    s.psql("GRANT EXECUTE ON FUNCTION shin_server.current_workspace_allowed() TO PUBLIC")
    try:
        health(503)
    finally:
        s.psql("REVOKE EXECUTE ON FUNCTION shin_server.current_workspace_allowed() FROM PUBLIC")
    health(200)
    for changed, restored in (("SECURITY DEFINER", "SECURITY INVOKER"), ("IMMUTABLE", "STABLE"),
                              ("SET search_path=public", "SET search_path=pg_catalog")):
        s.psql(f"ALTER FUNCTION shin_server.current_workspace_allowed() {changed}")
        try:
            health(503)
        finally:
            s.psql(f"ALTER FUNCTION shin_server.current_workspace_allowed() {restored}")
        health(200)
