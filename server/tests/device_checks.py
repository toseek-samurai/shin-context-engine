"""Real Store/HTTP device session lifecycle on the guarded Alice/Bob fixture."""
from concurrent.futures import ThreadPoolExecutor
import json
import subprocess

from publication_test_support import require
from publication_withdrawal_migration_checks import assert_sql_refused


def check_devices(suite, call, audit_count):
    issuer = "https://identity.example.test/realm"
    prefix = "/api/v1/identity/accounts/"
    def native(command, sid="session-A", *, who="alice", status=200, **extra):
        request = {"command": command, "issuer": issuer if who == "alice" else issuer + "-other",
                   "subject": "same-subject", "sid": sid, "input": {"display_name": "Same laptop"}, **extra}
        result = subprocess.run([str(suite.binary.parent / "shin-server-device-probe"),
                                 str(suite.state_root / "config/runtime.json")],
                                input=json.dumps(request), text=True, capture_output=True, timeout=30)
        require(result.returncode == 0, "native device probe failed")
        value = json.loads(result.stdout)
        require(value["ok"] == (status == 200) and value.get("status", 200) == status, "device admission result differs: " + value.get("code", "ok"))
        return value.get("result", value)
    def listed(who="alice", query=""):
        return call("GET", prefix + who + "/devices" + query, who=who)
    native("resolve", status=401)
    before = audit_count()
    a = native("register")
    require(not a["replayed"] and audit_count() == before + 1, "device registration audit missing")
    aid = a["device"]["id"]
    require(native("register")["replayed"] and audit_count() == before + 1, "device retry duplicated registration")
    b = native("register", who="bob")["device"]["id"]
    a2 = native("register", "session-B")["device"]["id"]
    require(len({aid, b, a2}) == 3, "device sessions conflated by label or sid across issuers")
    require(native("resolve")["principal_id"] == "dev-reader", "own device resolved incorrectly")
    require(native("resolve", who="bob")["principal_id"] == "identity-bob", "foreign issuer device resolved incorrectly")
    for extra in ({"subject": "SAME-SUBJECT"}, {"client_id": "other"}, {"expiry": -31}, {"issuer": issuer + "/"}):
        native("resolve", status=401, **extra)
    for sid in ("", "session-a", "missing"):
        native("resolve", sid, status=401)
    native("register", "", status=401)
    for data in ({"display_name": ""}, {"display_name": "x" * 121}, {"display_name": "bad\nlabel"},
                 {"display_name": "Laptop", "sid": "forged"}):
        native("register", "session-C", input=data, status=400)
    items = listed()["items"]
    require(len(items) == 2 and all("session_id" not in x and "client_id" not in x for x in items), "device metadata leaked binding")
    first = listed(query="?limit=1")
    require(first["next_cursor"] and len(first["items"]) == 1, "device first page invalid")
    require(len(listed(query="?limit=1&after=" + first["next_cursor"])["items"]) == 1, "device continuation invalid")
    for who, foreign in (("alice", "bob"), ("bob", "alice"), ("eve", "alice")):
        call("GET", prefix + foreign + "/devices", who=who, status=404)
        call("POST", prefix + foreign + "/devices/" + aid + "/revoke", {"expected_revision": 1}, who=who, status=404)
        call("POST", prefix + foreign + "/devices/revoke-all", {}, who=who, status=404)
    call("POST", prefix + "alice/devices/" + b + "/revoke", {"expected_revision": 1}, who="alice", status=404)
    for query in ("?limit=0", "?limit=101", "?after=x&after=x", "?unknown=1"):
        call("GET", prefix + "alice/devices" + query, who="alice", status=400)
    revoke = prefix + "alice/devices/" + aid + "/revoke"
    for revision in (0, -1, True, 1.0, "1", 9223372036854775807):
        call("POST", revoke, {"expected_revision": revision}, who="alice", status=400)
    call("POST", revoke, {"expected_revision": 2}, who="alice", status=409)
    before = audit_count()
    suite.psql("REVOKE INSERT ON shin_server.identity_audit FROM shin_runtime")
    try:
        call("POST", revoke, {"expected_revision": 1}, who="alice", status=503)
    finally:
        suite.psql("GRANT INSERT ON shin_server.identity_audit TO shin_runtime")
    require(audit_count() == before and not next(x for x in listed()["items"] if x["id"] == aid)["revoked"], "audit failure committed device revocation")
    def race(_):
        return call("POST", revoke, {"expected_revision": 1}, who="alice")["replayed"]
    with ThreadPoolExecutor(max_workers=4) as pool:
        outcomes = list(pool.map(race, range(8)))
    require(outcomes.count(False) == 1 and audit_count() == before + 1, "concurrent revoke duplicated mutation/audit")
    native("resolve", status=401)
    native("register", status=401, jti="new-access-token")
    native("resolve", "session-B")
    native("resolve", who="bob")
    # Re-enabling the account/binding must not resurrect its revoked sessions.
    rev = call("GET", "/api/v1/identity/account", who="alice")["revision"]
    call("POST", prefix + "alice/state", {"enabled": False, "expected_revision": rev})
    native("resolve", "session-B", status=401)
    native("register", "session-C", status=401)
    call("POST", prefix + "alice/state", {"enabled": True, "expected_revision": rev + 1})
    native("resolve", status=401)
    binding = call("GET", prefix + "alice/external")["revision"]
    call("POST", prefix + "alice/external/state", {"enabled": False, "expected_revision": binding})
    native("resolve", "session-B", status=401)
    call("POST", prefix + "alice/external/state", {"enabled": True, "expected_revision": binding + 1})
    native("resolve", status=401)
    before = audit_count()
    for i in range(31): native("register", f"capacity-{i}")
    native("register", "overflow", status=409)
    require(native("register", "session-B")["replayed"], "capacity check broke exact replay")
    suite.psql("REVOKE INSERT ON shin_server.identity_audit FROM shin_runtime")
    try:
        call("POST", prefix + "alice/devices/revoke-all", {}, who="alice", status=503)
    finally:
        suite.psql("GRANT INSERT ON shin_server.identity_audit TO shin_runtime")
    require(sum(not x["revoked"] for x in listed(query="?limit=100")["items"]) == 32 and audit_count() == before + 31,
            "failed revoke-all audit committed partial revocation")
    require(call("POST", prefix + "alice/devices/revoke-all", {}, who="alice")["revoked_count"] == 32, "revoke-all missed live sessions")
    after = audit_count()
    require(call("POST", prefix + "alice/devices/revoke-all", {}, who="alice")["revoked_count"] == 0 and audit_count() == after,
            "revoke-all retry duplicated audit")
    native("resolve", "session-B", status=401)
    native("register", "session-B", status=401)
    before = audit_count()
    suite.psql("REVOKE INSERT ON shin_server.identity_audit FROM shin_runtime")
    try: native("register", "fresh-login", status=503)
    finally: suite.psql("GRANT INSERT ON shin_server.identity_audit TO shin_runtime")
    native("resolve", "fresh-login", status=401)
    require(audit_count() == before, "failed registration audit changed history")
    with ThreadPoolExecutor(max_workers=4) as pool:
        enrolled = list(pool.map(lambda _: native("register", "fresh-login"), range(8)))
    require(sum(not x["replayed"] for x in enrolled) == 1 and len({x["device"]["id"] for x in enrolled}) == 1
            and audit_count() == before + 1, "concurrent registration duplicated device/audit")
    measurement = native("resolve", "fresh-login", samples=30)
    print("device lookup: " + json.dumps(measurement, sort_keys=True), flush=True)
    require(suite.scalar("BEGIN; SET LOCAL ROLE shin_runtime; SELECT count(*) FROM shin_server.identity_devices; COMMIT") == "0", "unset RLS exposed devices")
    require(suite.scalar("BEGIN; SET LOCAL ROLE shin_runtime; SET LOCAL shin.identity_account='bob'; "
                         "SELECT count(*) FROM shin_server.identity_devices; COMMIT") == "1", "own RLS device inventory differs")
    for mutation in ("DELETE FROM shin_server.identity_devices", "UPDATE shin_server.identity_devices SET session_id='hijacked'",
                     "UPDATE shin_server.identity_devices SET account_id='bob'", "UPDATE shin_server.identity_devices SET display_name='renamed'"):
        assert_sql_refused(suite, "SET LOCAL shin.identity_ops='1'; " + mutation, "insufficient_privilege", runtime=True)
    assert_sql_refused(suite, "SET LOCAL shin.identity_account='alice'; UPDATE shin_server.identity_devices SET revoked=false WHERE id='" + aid + "'",
                      "insufficient_privilege", runtime=True)
    assert_sql_refused(suite, "SET LOCAL shin.identity_account='alice'; INSERT INTO shin_server.identity_devices(account_id,client_id,session_id,display_name) "
                      "VALUES('bob','shin-ce','forged','Laptop')", "insufficient_privilege", runtime=True)
    assert_sql_refused(suite, "INSERT INTO shin_server.identity_devices(account_id,client_id,session_id,display_name) "
                      "VALUES('alice','shin-ce','session-A','Laptop')", "unique_violation")
    assert_sql_refused(suite, "INSERT INTO shin_server.identity_devices(account_id,client_id,session_id,display_name) "
                      "VALUES('missing','shin-ce','session-A','Laptop')", "foreign_key_violation")
    policy = suite.scalar("SELECT pg_get_expr(polqual,polrelid) FROM pg_policy WHERE polrelid='shin_server.identity_devices'::regclass AND polname='identity_devices_read'")
    suite.psql("ALTER POLICY identity_devices_read ON shin_server.identity_devices USING(true)")
    try: call("GET", "/health/ready", status=503)
    finally: suite.psql("ALTER POLICY identity_devices_read ON shin_server.identity_devices USING(" + policy + ")")
    suite.psql("GRANT UPDATE(session_id) ON shin_server.identity_devices TO shin_runtime")
    try: call("GET", "/health/ready", status=503)
    finally: suite.psql("REVOKE UPDATE(session_id) ON shin_server.identity_devices FROM shin_runtime")
    call("GET", "/health/ready")
    require(suite.scalar("SELECT count(*) FROM shin_server.identity_audit WHERE action LIKE 'device.%' "
                         "AND (tenant_id IS NOT NULL OR target_id !~ '^d[0-9a-f]{32}$')") == "0", "device audit leaked binding content")
    suite.stop_server(expect_clean=True)
    suite.start_server()
    native("resolve", status=401)
    native("register", status=401)
    native("resolve", "fresh-login")
    native("resolve", who="bob")
