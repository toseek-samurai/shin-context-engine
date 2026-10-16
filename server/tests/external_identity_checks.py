"""Explicit external identity bindings on the directory's existing Alice/Bob fixture."""
from concurrent.futures import ThreadPoolExecutor
import json

from publication_test_support import require
from publication_withdrawal_migration_checks import assert_sql_refused


def check_external_identities(suite, call, audit_count):
    prefix = "/api/v1/identity/accounts"
    issuer = "https://identity.example.test/realm"
    key = {"issuer": issuer, "subject": "same-subject"}
    alice, bob = prefix + "/alice/external", prefix + "/bob/external"
    probe = suite.binary.parent / "shin-server-external-identity-probe"

    def resolve(subject, expected, source=issuer):
        result = suite.run_process([str(probe), str(suite.state_root / "config/runtime.json"), source, subject, expected],
                                   "external identity native admission", timeout=30)
        value = json.loads(result.stdout)
        require(value["ok"] is True, "external identity probe failed")
        if "lookup_ms_p95" in value:
            print("external lookup: " + json.dumps(value, sort_keys=True), flush=True)
        return value

    require([a["id"] for a in call("GET", prefix)["items"]] == ["alice", "bob"], "account inventory leaked unrelated data")
    first = call("GET", prefix + "?limit=1")
    require(first["next_cursor"] == "alice" and len(first["items"]) == 1, "account pagination first page failed")
    require(call("GET", prefix + "?after=alice&limit=1")["items"][0]["id"] == "bob", "account continuation failed")
    for who in ("alice", "bob", "eve"):
        call("GET", prefix, who=who, status=403)
        call("POST", alice, key, who=who, status=403)
    for query in ("?limit=0", "?limit=101", "?after=x&after=x", "?unknown=1"):
        call("GET", prefix + query, status=400)
    call("POST", prefix + "/missing/external", key, status=404)
    for malformed in ({**key, "issuer": "http://identity.example.test"}, {**key, "subject": ""},
                      {**key, "subject": "x" * 256}, {**key, "issuer": "https://user@host"},
                      {**key, "account_id": "bob"}):
        call("POST", alice, malformed, status=400)
    resolve(key["subject"], "denied")
    before = audit_count()
    require(call("POST", alice, key)["replayed"] is False, "binding not created")
    require(call("POST", alice, key)["replayed"] is True and audit_count() == before + 1, "binding retry duplicated audit")
    call("POST", bob, key, status=409)
    call("POST", alice, {**key, "subject": "changed"}, status=409)
    # Same subject under another issuer is a different identity, never an email merge.
    bob_key = {**key, "issuer": issuer + "-other"}
    call("POST", bob, bob_key)
    for who, own, foreign in (("alice", alice, bob), ("bob", bob, alice)):
        require(call("GET", own, who=who)["account_id"] == who, "own external identity missing")
        call("GET", foreign, who=who, status=404)
    resolve(key["subject"], "dev-reader")
    resolve(key["subject"], "identity-bob", source=bob_key["issuer"])
    resolve("SAME-SUBJECT", "denied")
    resolve(key["subject"], "denied", source=issuer + "/")
    # One winner for a revision; failed contenders append no audit.
    def race(_):
        return suite.request("POST", alice + "/state", headers={**suite.auth_headers("admin"), "Content-Type": "application/json"},
            body=json.dumps({"enabled": False, "expected_revision": 1}).encode())[0]
    before = audit_count()
    with ThreadPoolExecutor(max_workers=4) as pool:
        outcomes = list(pool.map(race, range(8)))
    require(outcomes.count(200) == 1 and outcomes.count(409) == 7 and audit_count() == before + 1,
            "binding CAS did not commit exactly once")
    replay = call("POST", alice, key)
    require(replay["replayed"] and replay["binding"]["enabled"] is False and replay["binding"]["revision"] == 2,
            "creation retry re-enabled revoked identity")
    resolve(key["subject"], "denied")
    call("POST", alice + "/state", {"enabled": True, "expected_revision": 2}, who="alice", status=403)
    for revision in (0, -1, 2.0, True, 9223372036854775807, "2"):
        call("POST", alice + "/state", {"enabled": True, "expected_revision": revision}, status=400)
    before = audit_count()
    suite.psql("REVOKE INSERT ON shin_server.identity_audit FROM shin_runtime")
    try:
        call("POST", alice + "/state", {"enabled": True, "expected_revision": 2}, status=503)
    finally:
        suite.psql("GRANT INSERT ON shin_server.identity_audit TO shin_runtime")
    require(call("GET", alice)["enabled"] is False and audit_count() == before, "failed audit committed binding state")
    call("POST", alice + "/state", {"enabled": True, "expected_revision": 2})
    resolve(key["subject"], "dev-reader")
    call("POST", prefix + "/alice/state", {"enabled": False, "expected_revision": 1})
    resolve(key["subject"], "denied")
    call("POST", prefix + "/alice/state", {"enabled": True, "expected_revision": 2})
    resolve(key["subject"], "dev-reader")
    require(suite.scalar("BEGIN; SET LOCAL ROLE shin_runtime; SELECT count(*) FROM shin_server.identity_external_bindings; COMMIT") == "0",
            "unset RLS exposed bindings")
    require(suite.scalar("BEGIN; SET LOCAL ROLE shin_runtime; SET LOCAL shin.identity_account='alice'; "
                         "SELECT count(*) FROM shin_server.identity_external_bindings; COMMIT") == "1", "RLS own mapping failed")
    for mutation in ("DELETE FROM shin_server.identity_external_bindings", "UPDATE shin_server.identity_external_bindings SET subject='hijacked'"):
        assert_sql_refused(suite, "SET LOCAL shin.identity_ops='1'; " + mutation, "insufficient_privilege", runtime=True)
    assert_sql_refused(suite, "INSERT INTO shin_server.identity_external_bindings(account_id,issuer,subject) "
                      "SELECT 'missing',issuer,subject FROM shin_server.identity_external_bindings WHERE account_id='alice'", "unique_violation")
    assert_sql_refused(suite, "INSERT INTO shin_server.identity_external_bindings(account_id,issuer,subject) "
                      "VALUES('missing','https://unique.example.test','new')", "foreign_key_violation")
    assert_sql_refused(suite, "SET LOCAL shin.identity_account='alice'; "
                      "INSERT INTO shin_server.identity_external_bindings(account_id,issuer,subject) "
                      "VALUES('alice','https://unique.example.test','new')", "insufficient_privilege", runtime=True)
    policy = suite.scalar("SELECT pg_get_expr(polqual,polrelid) FROM pg_policy "
                          "WHERE polrelid='shin_server.identity_external_bindings'::regclass AND polname='identity_external_read'")
    suite.psql("ALTER POLICY identity_external_read ON shin_server.identity_external_bindings USING(true)")
    try:
        call("GET", "/health/ready", status=503)
    finally:
        suite.psql("ALTER POLICY identity_external_read ON shin_server.identity_external_bindings USING(" + policy + ")")
    suite.psql("GRANT DELETE ON shin_server.identity_external_bindings TO shin_runtime")
    try:
        call("GET", "/health/ready", status=503)
    finally:
        suite.psql("REVOKE DELETE ON shin_server.identity_external_bindings FROM shin_runtime")
    call("GET", "/health/ready")
    suite.stop_server(expect_clean=True)
    suite.start_server()
    require(call("GET", alice, who="alice")["revision"] == 3, "binding did not survive restart")
    resolve(key["subject"], "dev-reader")
    # No lookup grants maintenance rights or lifts the legacy data-plane fence.
    call("GET", "/api/v1/projects", who="alice", status=403)
