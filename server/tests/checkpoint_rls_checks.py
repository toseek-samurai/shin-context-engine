"""Actual runtime-role SQL, deliberately omitting application WHERE predicates."""
import json

from publication_test_support import require
from publication_withdrawal_migration_checks import assert_sql_refused


def context(principal="scoped-alice", account="alice", tenant="scope-a", project="visible", workspace="primary", legacy=False):
    # All arguments are fixed fixture identifiers, never request text.
    settings = {"identity_principal": principal, "identity_account": account, "identity_tenant": tenant,
                "identity_ops": "0", "identity_legacy_projects": "", "checkpoint_project": project,
                "checkpoint_workspace": workspace, "checkpoint_legacy": "1" if legacy else "0"}
    return "SET LOCAL ROLE shin_runtime; " + "".join(f"SET LOCAL shin.{key}='{value}'; " for key, value in settings.items())


def check_checkpoint_rls(s):
    count = "SELECT count(*) FROM shin_server.checkpoint_slots"
    own = "project_id='visible' AND owner_id='scoped-alice' AND scope_workspace_id='primary'"
    expected = s.scalar(count + " WHERE " + own)
    total = s.scalar(count)
    require(0 < int(expected) < int(total), "RLS fixture lacks private and foreign checkpoints")
    require(s.scalar("BEGIN; SET LOCAL ROLE shin_runtime; " + count + "; ROLLBACK") == "0", "unset scope disclosed payload rows")
    require(s.scalar("BEGIN; " + context() + count + "; ROLLBACK") == expected, "unfiltered read crossed scope")
    for query in ("UPDATE shin_server.checkpoint_slots SET updated_at=updated_at RETURNING 1",
                  "DELETE FROM shin_server.checkpoint_slots RETURNING 1"):
        require(s.scalar("BEGIN; " + context() + "WITH touched AS (" + query + ") SELECT count(*) FROM touched; ROLLBACK") == expected,
                "unfiltered mutation crossed scope")
    bob = context("scoped-bob", "bob", "scope-b", "other")
    require(s.scalar("BEGIN; " + bob + count + "; ROLLBACK") == s.scalar(count + " WHERE owner_id='scoped-bob' AND scope_workspace_id='primary'"),
            "second owner scope differs")
    for settings in (context(project="other"), context(tenant="scope-b"), context(account="bob"),
                     context(workspace="checkpoint-other")):
        target_count = "0" if "checkpoint-other" not in settings else s.scalar(count + " WHERE owner_id='scoped-alice' AND scope_workspace_id='checkpoint-other'")
        require(s.scalar("BEGIN; " + settings + count + "; ROLLBACK") == target_count, "project/tenant/workspace binding crossed scope")
    for change in ("owner_id='scoped-bob'", "project_id='other',provenance=jsonb_set(provenance,'{project_id}','\"other\"')",
                   "scope_workspace_id='checkpoint-other',provenance=jsonb_set(provenance,'{workspace_id}','\"checkpoint-other\"')",
                   "scope_workspace_id=NULL"):
        assert_sql_refused(s, context() + "UPDATE shin_server.checkpoint_slots SET " + change,
                           "insufficient_privilege")
    clone = ("INSERT INTO shin_server.checkpoint_slots SELECT (jsonb_populate_record(NULL::shin_server.checkpoint_slots,"
             "to_jsonb(s)||jsonb_build_object('owner_id','scoped-bob','revision',nextval('shin_server.checkpoint_revisions')))).* "
             "FROM shin_server.checkpoint_slots s LIMIT 1")
    assert_sql_refused(s, context() + clone, "insufficient_privilege")
    for revoke in ("UPDATE shin_server.identity_accounts SET enabled=false WHERE id='alice'; ",
                   "UPDATE shin_server.identity_memberships SET enabled=false WHERE account_id='alice'; "):
        require(s.scalar("BEGIN; " + revoke + context() + count + "; ROLLBACK") == "0", "SQL policy reused revoked authority")
    require(s.scalar("BEGIN; " + context(legacy=True, workspace="") + count + "; ROLLBACK") ==
            s.scalar(count + " WHERE owner_id='scoped-alice' AND project_id='visible' AND scope_workspace_id IS NULL"),
            "legacy namespace crossed scoped rows")
    for finish in ("COMMIT", "ROLLBACK"):
        # Same physical SQL connection, including previously privileged maintenance context.
        require(s.scalar("BEGIN; " + context() + "SET LOCAL shin.identity_ops='1'; " + finish +
                         "; BEGIN; SET LOCAL ROLE shin_runtime; " + count + "; ROLLBACK") == "0",
                "transaction-local authority leaked to a reused connection")
    require(s.scalar(count) == total, "negative RLS checks changed durable payloads")


def check_checkpoint_rls_drift(s):
    def health(status):
        s.wait_for_response("/health/ready", status, headers=s.auth_headers("admin"))
    for disable, restore in (("DISABLE ROW LEVEL SECURITY", "ENABLE ROW LEVEL SECURITY"),
                             ("NO FORCE ROW LEVEL SECURITY", "FORCE ROW LEVEL SECURITY")):
        s.psql("ALTER TABLE shin_server.checkpoint_slots " + disable)
        try:
            health(503)
        finally:
            s.psql("ALTER TABLE shin_server.checkpoint_slots " + restore)
        health(200)
    policy = json.loads(s.scalar("SELECT to_json(pg_get_expr(polqual,polrelid))::text FROM pg_policy WHERE "
                                "polrelid='shin_server.checkpoint_slots'::regclass AND polname='checkpoint_slots_scope'"))
    s.psql("ALTER POLICY checkpoint_slots_scope ON shin_server.checkpoint_slots USING (true)")
    try:
        health(503)
    finally:
        s.psql("ALTER POLICY checkpoint_slots_scope ON shin_server.checkpoint_slots USING (" + policy + ")")
    health(200)
    s.psql("CREATE POLICY checkpoint_test_bypass ON shin_server.checkpoint_slots USING(true)")
    try:
        health(503)
    finally:
        s.psql("DROP POLICY checkpoint_test_bypass ON shin_server.checkpoint_slots")
    health(200)
