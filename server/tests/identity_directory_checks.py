"""Native HTTP and PostgreSQL directory isolation on the existing locked test stand."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import secrets

from integration_test import IntegrationFailure

PREFIX = "/api/v1/identity"


def require(value, message):
    if not value:
        raise IntegrationFailure(message)


def check_identity_directory(suite):
    config_file = suite.state_root / "config/runtime.json"
    original = config_file.read_bytes()
    bob = secrets.token_urlsafe(40)
    eve = secrets.token_urlsafe(40)
    config = json.loads(original)
    for name, token in (("identity-bob", bob), ("identity-eve", eve)):
        config["principals"].append({"id": name, "role": "reader", "project_ids": ["other"],
                                     "token_sha256": hashlib.sha256(token.encode()).hexdigest()})
    suite.stop_server(expect_clean=True)
    config_file.write_text(json.dumps(config))
    suite.start_server()
    def headers(who):
        if who in ("admin", "alice"):
            return suite.auth_headers("admin" if who == "admin" else "reader")
        return {"Authorization": "Bearer " + (bob if who == "bob" else eve), "X-Shin-Server-ID": suite.server_id}
    def call(method, path, payload=None, who="admin", status=200):
        body = json.dumps(payload).encode() if payload is not None else None
        request_headers = headers(who)
        if body is not None:
            request_headers["Content-Type"] = "application/json"
        value = suite.expect_status(status, method, path, headers=request_headers, body=body)
        return value.get("result", value.get("error"))
    def audit_count():
        return int(suite.scalar("SELECT count(*) FROM shin_server.identity_audit"))
    accounts = [("alice", "dev-reader"), ("bob", "identity-bob")]
    try:
        suite.expect_status(401, "GET", PREFIX + "/tenants")
        call("GET", PREFIX + "/tenants", who="eve", status=404)
        call("POST", PREFIX + "/accounts", {}, who="alice", status=403)
        for principal in ("dev-admin", "unknown-principal"):
            call("POST", PREFIX + "/accounts", {"id": "bad-account", "principal_id": principal, "display_name": "Bad"}, status=400)
        for account, principal in accounts:
            body = {"id": account, "principal_id": principal, "display_name": account.title()}
            require(call("POST", PREFIX + "/accounts", body)["replayed"] is False, "account creation failed")
            count = audit_count()
            require(call("POST", PREFIX + "/accounts", body)["replayed"] is True and audit_count() == count,
                    "idempotent account creation duplicated audit")
            call("POST", PREFIX + "/accounts", {**body, "display_name": "Changed"}, status=409)
            require(call("GET", PREFIX + "/account", who=account)["id"] == account, "own account differs")
            require(call("GET", "/api/v1/projects", who=account, status=403)["code"] == "identity-data-plane-not-enabled",
                    "registered account bypassed data-plane fence")
            for method, route in (("GET", "/api/v1/telemetry/retrieval"), ("GET", "/api/v1/audit"),
                                  ("GET", "/api/v1/projects/visible/workspaces"),
                                  ("GET", "/api/v1/projects/visible/task-checkpoints"),
                                  ("GET", "/api/v1/observer-identity"), ("POST", "/api/v1/observer-streams"),
                                  ("GET", "/api/v1/preparations"), ("GET", "/api/v1/cache"),
                                  ("POST", "/api/v1/projects/visible/routes/source/brief"),
                                  ("GET", "/api/v1/projects/visible/publications/shared")):
                require(call(method, route, who=account, status=403)["code"] == "identity-data-plane-not-enabled",
                        "registered account bypassed legacy route fence")
            body = {"id": "tenant-" + account, "display_name": account.title(), "owner_id": account}
            require(call("POST", PREFIX + "/tenants", body)["replayed"] is False, "tenant creation failed")
            count = audit_count()
            require(call("POST", PREFIX + "/tenants", body)["replayed"] is True and audit_count() == count,
                    "tenant replay duplicated membership or audit")
        for who, other in (("alice", "bob"), ("bob", "alice")):
            listed = call("GET", PREFIX + "/tenants", who=who)
            require([r["id"] for r in listed["items"]] == ["tenant-" + who], "cross-tenant listing leak")
            require(call("GET", PREFIX + "/tenants/tenant-" + who, who=who)["content_access"] == "scoped-routes-only",
                    "directory asserted data-plane readiness")
            call("GET", PREFIX + "/tenants/tenant-" + other, who=who, status=404)
        from external_identity_checks import check_external_identities
        check_external_identities(suite, call, audit_count)
        from device_checks import check_devices
        check_devices(suite, call, audit_count)
        require(len(call("GET", PREFIX + "/tenants")["items"]) == 2, "maintenance directory overview differs")
        first = call("GET", PREFIX + "/tenants?limit=1")
        require(len(first["items"]) == 1 and first["next_cursor"] == "tenant-alice", "keyset first page differs")
        require(call("GET", PREFIX + "/tenants?after=tenant-alice&limit=1")["items"][0]["id"] == "tenant-bob",
                "keyset continuation differs")
        for query in ("?limit=101", "?limit=0", "?after=x&after=y", "?tenant_id=tenant-bob"):
            call("GET", PREFIX + "/tenants" + query, who="alice", status=400)
        for _ in range(12):
            for who in ("alice", "bob"):
                require([r["id"] for r in call("GET", PREFIX + "/tenants", who=who)["items"]] == ["tenant-" + who],
                        "pooled connection reused another tenant context")
        spoofed = {**headers("bob"), "X-Shin-User": "alice", "X-Shin-Tenant": "tenant-alice"}
        suite.expect_status(404, "GET", PREFIX + "/tenants/tenant-alice", headers=spoofed)

        target = PREFIX + "/tenants/tenant-alice/members"
        body = {"account_id": "bob", "role": "reader", "enabled": True, "expected_revision": 0}
        before = audit_count()
        for who in ("alice", "bob", "admin"):
            require(call("POST", target, body, who=who, status=403)["code"] == "sharing-disabled",
                    "membership sharing remained available")
        require(audit_count() == before, "denied sharing added audit")
        # Historical enabled membership, including a forged owner role, grants nothing.
        suite.psql("INSERT INTO shin_server.identity_memberships VALUES('tenant-alice','bob','owner',true,1)")
        for _ in range(4):
            require([r["id"] for r in call("GET", PREFIX + "/tenants", who="bob")["items"]] == ["tenant-bob"],
                    "historical membership exposed a foreign tenant")
            call("GET", PREFIX + "/tenants/tenant-alice", who="bob", status=404)
        alice_revision = call("GET", PREFIX + "/account", who="alice")["revision"]
        def race(_):
            return suite.request("POST", PREFIX + "/accounts/alice/state",
                headers={**headers("admin"), "Content-Type": "application/json"},
                body=json.dumps({"enabled": True, "expected_revision": alice_revision}).encode())[0]
        before = audit_count()
        with ThreadPoolExecutor(max_workers=4) as pool:
            outcomes = list(pool.map(race, range(8)))
        require(outcomes.count(200) == 1 and outcomes.count(409) == 7 and audit_count() == before + 1,
                "concurrent personal account CAS did not commit exactly once")

        require(suite.scalar("BEGIN; SET LOCAL ROLE shin_runtime; SELECT (SELECT count(*) FROM shin_server.identity_accounts) "
                             "+(SELECT count(*) FROM shin_server.identity_tenants)+(SELECT count(*) FROM shin_server.identity_memberships)"
                             "+(SELECT count(*) FROM shin_server.identity_audit); COMMIT;") == "0",
                "unset RLS context disclosed directory rows")
        suite.psql("ALTER TABLE shin_server.identity_tenants DISABLE ROW LEVEL SECURITY")
        try:
            call("GET", "/health/ready", status=503)
        finally:
            suite.psql("ALTER TABLE shin_server.identity_tenants ENABLE ROW LEVEL SECURITY")
        call("GET", "/health/ready")
        policy = suite.scalar("SELECT pg_get_expr(polqual,polrelid) FROM pg_policy "
                              "WHERE polrelid='shin_server.identity_accounts'::regclass AND polname='identity_accounts_read'")
        suite.psql("ALTER POLICY identity_accounts_read ON shin_server.identity_accounts USING (true)")
        try:
            call("GET", "/health/ready", status=503)
            call("GET", PREFIX + "/account", who="alice", status=503)
        finally:
            suite.psql("ALTER POLICY identity_accounts_read ON shin_server.identity_accounts USING (" + policy + ")")
        call("GET", "/health/ready")
        account_state = PREFIX + "/accounts/bob/state"
        before = audit_count()
        suite.psql("REVOKE INSERT ON shin_server.identity_audit FROM shin_runtime")
        try:
            call("POST", account_state, {"enabled": False, "expected_revision": 1}, status=503)
        finally:
            suite.psql("GRANT INSERT ON shin_server.identity_audit TO shin_runtime")
        require(call("GET", PREFIX + "/account", who="bob")["enabled"] is True and audit_count() == before,
                "audit failure committed account state")
        call("POST", account_state, {"enabled": False, "expected_revision": 1})
        for route in (PREFIX + "/account", PREFIX + "/tenants", "/api/v1/projects", "/health/ready"):
            require(call("GET", route, who="bob", status=401)["code"] == "account-disabled", "disabled account admitted")
        suite.stop_server(expect_clean=True)
        suite.start_server()
        call("GET", PREFIX + "/account", who="bob", status=401)
        require(call("GET", PREFIX + "/tenants", who="alice")["items"][0]["id"] == "tenant-alice",
                "directory did not survive restart")
        call("POST", account_state, {"enabled": True, "expected_revision": 2})
        require(call("GET", PREFIX + "/account", who="bob")["revision"] == 3, "account restore revision differs")
        require(call("GET", "/api/v1/projects", who="bob", status=403)["code"] == "identity-data-plane-not-enabled",
                "re-enabled account regained legacy data-plane access")
    finally:
        suite.stop_server(expect_clean=False)
        config_file.write_bytes(original)
        # These tables contain only this fixture's identities. Derived
        # legacy suites reuse dev-reader and must start with it unenrolled.
        suite.psql("DELETE FROM shin_server.workspace_publication_history; DELETE FROM shin_server.identity_ownership_audit; "
                   "DELETE FROM shin_server.identity_workspace_owners; DELETE FROM shin_server.identity_project_owners; "
                   "DELETE FROM shin_server.identity_audit; DELETE FROM shin_server.identity_memberships; "
                   "DELETE FROM shin_server.identity_tenants; DELETE FROM shin_server.identity_devices; DELETE FROM shin_server.identity_external_bindings; "
                   "DELETE FROM shin_server.identity_accounts")
        suite.start_server()
