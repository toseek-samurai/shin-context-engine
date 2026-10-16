"""Explicit ownership metadata; no tenant data-plane readiness claim."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import secrets

from identity_directory_checks import PREFIX, require


def check_workspace_ownership(suite):
    config_file = suite.state_root / "config/runtime.json"
    original = config_file.read_bytes()
    config = json.loads(original)
    tokens = {who: secrets.token_urlsafe(40) for who in ("bob", "legacy", "unrelated")}
    for who, token in tokens.items():
        config["principals"].append({"id": "ownership-" + who, "role": "reader",
            "project_ids": ["owned-a" if who == "legacy" else "other"],
            "token_sha256": hashlib.sha256(token.encode()).hexdigest()})
    suite.stop_server(expect_clean=True)
    config_file.write_text(json.dumps(config))
    suite.start_server()

    def headers(who):
        if who in ("admin", "alice"):
            return suite.auth_headers("admin" if who == "admin" else "reader")
        return {"Authorization": "Bearer " + tokens[who], "X-Shin-Server-ID": suite.server_id}

    def call(method, path, payload=None, who="admin", status=200):
        body = json.dumps(payload).encode() if payload is not None else None
        value = suite.expect_status(status, method, path,
            headers={**headers(who), **({"Content-Type": "application/json"} if body is not None else {})}, body=body)
        return value.get("result", value.get("error"))

    def audit_count():
        return int(suite.scalar("SELECT count(*) FROM shin_server.identity_ownership_audit"))

    a = PREFIX + "/tenants/owner-a/projects"
    b = PREFIX + "/tenants/owner-b/projects"
    workspaces = a + "/owned-a/workspaces"
    try:
        suite.psql("INSERT INTO shin_server.projects(id,display_name) VALUES('owned-a','Same project'),('owned-b','Same project'),('owned-c','Same project');"
            "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,root,root_identity) "
            "SELECT p,w,'local-dev',w,'/fixture/'||p||'/'||w,p||'-'||w "
            "FROM (VALUES('owned-a'),('owned-b')) AS projects(p), "
            "(VALUES('alice-private'),('bob-private'),('shared'),('audit-rollback')) AS workspaces(w)")
        for account, principal in (("alice", "dev-reader"), ("bob", "ownership-bob")):
            call("POST", PREFIX + "/accounts", {"id": account, "principal_id": principal, "display_name": account})
        for tenant, account in (("owner-a", "alice"), ("owner-b", "bob")):
            call("POST", PREFIX + "/tenants", {"id": tenant, "display_name": tenant, "owner_id": account})
        membership = {"account_id": "bob", "role": "maintainer", "enabled": True, "expected_revision": 0}
        call("POST", PREFIX + "/tenants/owner-a/members", membership, who="alice", status=403)
        suite.psql("INSERT INTO shin_server.identity_memberships VALUES('owner-a','bob','maintainer',true,1)")
        call("GET", "/api/v1/projects/owned-a", who="legacy")
        call("POST", a, {}, who="alice", status=403)
        call("POST", workspaces, {}, who="alice", status=403)
        call("POST", a, {"project_id": "missing"}, status=404)
        project = {"project_id": "owned-a"}

        def concurrent_bind(_):
            return call("POST", a, project)["replayed"]

        with ThreadPoolExecutor(max_workers=4) as pool:
            outcomes = list(pool.map(concurrent_bind, range(4)))
        require(outcomes.count(False) == 1 and outcomes.count(True) == 3 and audit_count() == 1,
                "concurrent ownership binding committed more than once")
        call("POST", b, project, status=409)
        call("POST", b, {"project_id": "owned-b"})
        require(call("GET", a + "/owned-a", who="alice") == {
            "tenant_id": "owner-a", "project_id": "owned-a", "content_access": "scoped-routes-only"},
            "project metadata leaked fields or asserted content readiness")
        for route in (b + "/owned-b", b + "/owned-a", a + "/owned-b", a + "/missing"):
            call("GET", route, who="alice", status=404)
        for route in ("/api/v1/projects", "/api/v1/projects/owned-a/workspaces", "/api/v1/cache",
                      "/api/v1/projects/owned-a/publications/shared", "/api/v1/telemetry/retrieval"):
            require(call("GET", route, who="legacy", status=403)["code"] == "identity-data-plane-not-enabled",
                    "unenrolled legacy principal bypassed bound project fence")
        call("GET", "/api/v1/projects/other", who="unrelated")
        call("GET", "/health/ready", who="legacy")
        call("GET", a + "/owned-a", who="legacy", status=404)
        for name, owner in (("alice-private", "alice"), ("bob-private", "bob")):
            target = workspaces if owner == "alice" else b + "/owned-b/workspaces"
            tenant, project_id = ("owner-a", "owned-a") if owner == "alice" else ("owner-b", "owned-b")
            payload = {"workspace_id": name, "owner_id": owner}
            bound = call("POST", target, payload)
            require(bound["replayed"] is False and bound["workspace"] == {
                "tenant_id": tenant, "project_id": project_id, "workspace_id": name,
                "owner_id": owner, "visibility": "private", "content_access": "scoped-routes-only"},
                "default private metadata contract differs")
            before = audit_count()
            require(call("POST", target, payload)["replayed"] is True and audit_count() == before,
                    "workspace replay duplicated audit")
            call("POST", target, {**payload, "visibility": "shared"}, status=403)
            call("POST", target, {**payload, "owner_id": "bob" if owner == "alice" else "alice"}, status=403)
        shared = {"workspace_id": "shared", "owner_id": "alice", "visibility": "shared"}
        call("POST", workspaces, shared, status=403)
        suite.psql("INSERT INTO shin_server.identity_workspace_owners VALUES('owner-a','owned-a','shared','alice','shared')")
        # Hidden/unbound rows sort between visible rows: filtering must precede LIMIT.
        call("POST", a, {"project_id": "owned-c"})
        suite.psql("INSERT INTO shin_server.identity_workspace_owners VALUES('owner-a','owned-a','bob-private','bob','shared'); "
                   "UPDATE shin_server.workspaces SET display_name='Same checkout' WHERE project_id='owned-a'")
        audit_before_inventory = audit_count()
        project_page = call("GET", a + "?limit=1", who="alice")
        require(project_page == {"items": [{"tenant_id": "owner-a", "project_id": "owned-a", "display_name": "Same project",
                                           "content_access": "scoped-routes-only"}], "next_cursor": "owned-a"}, "project inventory scope/projection differs")
        require(call("GET", a + "?limit=1&after=owned-a", who="alice") == {
            "items": [{"tenant_id": "owner-a", "project_id": "owned-c", "display_name": "Same project", "content_access": "scoped-routes-only"}],
            "next_cursor": None}, "foreign project affected owner pagination")
        require(call("GET", a + "?after=owned-c", who="alice") == {"items": [], "next_cursor": None}, "project inventory end differs")
        require(call("GET", a + "/owned-c/workspaces", who="alice") == {"items": [], "next_cursor": None}, "empty owned workspace inventory differs")
        workspace_page = call("GET", workspaces + "?limit=1", who="alice")
        require(workspace_page == {"items": [{"tenant_id": "owner-a", "project_id": "owned-a", "workspace_id": "alice-private",
            "owner_id": "alice", "visibility": "private", "display_name": "Same checkout", "content_access": "scoped-routes-only"}],
            "next_cursor": "alice-private"}, "workspace inventory exposed host coordinates or hidden row")
        require(call("GET", workspaces + "?limit=1&after=alice-private", who="alice") == {
            "items": [{"tenant_id": "owner-a", "project_id": "owned-a", "workspace_id": "shared", "owner_id": "alice",
                       "visibility": "shared", "display_name": "Same checkout", "content_access": "scoped-routes-only"}],
            "next_cursor": None}, "inconsistent historical owner changed visible pagination")
        require(len(call("GET", workspaces)["items"]) == 3, "explicit maintenance inventory lost historical metadata")
        require(call("GET", workspaces + "?after=shared", who="alice") == {"items": [], "next_cursor": None}, "workspace end differs")
        for route in (a, workspaces):
            require(call("GET", route, who="bob", status=404)["code"] == "identity-not-found", "foreign inventory not hidden")
            call("GET", route, who="legacy", status=404)
            for query in ("limit=0", "limit=101", "limit=abc", "limit=1&limit=2", "after=bad%2Fcursor", "owner_id=alice"):
                call("GET", route + "?" + query, who="alice", status=400)
        for route in (b, b + "/owned-b/workspaces", a + "/missing/workspaces", a + "/owned-b/workspaces"):
            call("GET", route, who="alice", status=404)
        suite.expect_status(404, "GET", workspaces, headers={**headers("bob"), "X-Shin-User": "alice", "X-Shin-Tenant": "owner-a"})
        require(audit_count() == audit_before_inventory, "read-only inventories wrote audit")
        call("POST", workspaces, {**shared, "visibility": "public"}, status=400)
        call("POST", workspaces, {**shared, "tenant_id": "owner-b"}, status=400)
        call("POST", workspaces, {"workspace_id": "missing", "owner_id": "alice"}, status=404)
        call("POST", b + "/owned-b/workspaces", {"workspace_id": "alice-private", "owner_id": "alice"}, status=403)
        for _ in range(8):
            for who, other in (("alice", "bob"), ("bob", "alice")):
                own_root = workspaces if who == "alice" else b + "/owned-b/workspaces"
                other_root = workspaces if other == "alice" else b + "/owned-b/workspaces"
                own = call("GET", own_root + "/" + who + "-private", who=who)
                require(own["owner_id"] == who, "pooled ownership context leaked")
                own_inventory = call("GET", own_root + "?limit=1", who=who)
                require(own_inventory["items"][0]["owner_id"] == who, "pooled inventory scope leaked")
                call("GET", other_root, who=who, status=404)
                hidden = call("GET", other_root + "/" + other + "-private", who=who, status=404)
                absent = call("GET", other_root + "/missing", who=who, status=404)
                require(hidden == absent, "private workspace existence disclosed")
                call("GET", workspaces + "/shared", who=who, status=200 if who == "alice" else 404)
        def parallel_inventory(who):
            route = workspaces if who == "alice" else b + "/owned-b/workspaces"
            page = call("GET", route + "?limit=1", who=who)
            require(page["items"][0]["owner_id"] == who, "concurrent inventory reused another connection scope")

        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(parallel_inventory, ["alice", "bob"] * 4))
        suite.expect_status(404, "GET", workspaces + "/alice-private", headers={
            **headers("bob"), "X-Shin-User": "alice", "X-Shin-Tenant": "owner-a"})
        call("GET", workspaces + "/shared?owner_id=alice", who="bob", status=400)
        require(suite.scalar("BEGIN; SET LOCAL ROLE shin_runtime; SELECT "
            "(SELECT count(*) FROM shin_server.identity_project_owners)+"
            "(SELECT count(*) FROM shin_server.identity_workspace_owners)+"
            "(SELECT count(*) FROM shin_server.identity_ownership_audit); COMMIT") == "0", "unset RLS context leaked ownership")
        # The composite FK rejects a real account that belongs only to another tenant.
        require(suite.scalar("DO $$ BEGIN BEGIN INSERT INTO shin_server.identity_workspace_owners "
            "VALUES('owner-b','owned-b','alice-private','alice','private'); "
            "RAISE EXCEPTION 'cross-tenant owner accepted'; EXCEPTION WHEN foreign_key_violation THEN NULL; END; END $$; "
            "SELECT count(*) FROM shin_server.identity_workspace_owners WHERE project_id='owned-b'") == "1", "owner membership FK missing")
        for table in ("identity_project_owners", "identity_workspace_owners", "identity_ownership_audit"):
            suite.psql(f"ALTER TABLE shin_server.{table} DISABLE ROW LEVEL SECURITY")
            try:
                call("GET", "/health/ready", status=503)
            finally:
                suite.psql(f"ALTER TABLE shin_server.{table} ENABLE ROW LEVEL SECURITY")
        policy = json.loads(suite.scalar("SELECT to_json(pg_get_expr(polqual,polrelid))::text FROM pg_policy "
            "WHERE polrelid='shin_server.identity_workspace_owners'::regclass AND polname='identity_workspace_owners_read'"))
        suite.psql("ALTER POLICY identity_workspace_owners_read ON shin_server.identity_workspace_owners USING(true)")
        try:
            call("GET", "/health/ready", status=503)
            call("GET", workspaces + "/alice-private", who="bob", status=503)
        finally:
            suite.psql("ALTER POLICY identity_workspace_owners_read ON shin_server.identity_workspace_owners USING(" + policy + ")")
        constraint = suite.scalar("SELECT conname FROM pg_constraint WHERE conrelid='shin_server.identity_workspace_owners'::regclass "
            "AND confrelid='shin_server.identity_memberships'::regclass")
        definition = suite.scalar("SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid="
            "'shin_server.identity_workspace_owners'::regclass AND confrelid='shin_server.identity_memberships'::regclass")
        suite.psql(f'ALTER TABLE shin_server.identity_workspace_owners DROP CONSTRAINT "{constraint}"')
        try:
            call("GET", "/health/ready", status=503)
        finally:
            suite.psql(f'ALTER TABLE shin_server.identity_workspace_owners ADD CONSTRAINT "{constraint}" {definition}')
        before = audit_count()
        suite.psql("ALTER TABLE shin_server.identity_workspace_owners ALTER COLUMN visibility SET DEFAULT 'shared'")
        try:
            call("GET", "/health/ready", status=503)
        finally:
            suite.psql("ALTER TABLE shin_server.identity_workspace_owners ALTER COLUMN visibility SET DEFAULT 'private'")
        suite.psql("REVOKE INSERT ON shin_server.identity_ownership_audit FROM shin_runtime")
        try:
            call("POST", workspaces, {"workspace_id": "audit-rollback", "owner_id": "alice"}, status=503)
        finally:
            suite.psql("GRANT INSERT ON shin_server.identity_ownership_audit TO shin_runtime")
        require(audit_count() == before and suite.scalar("SELECT count(*) FROM shin_server.identity_workspace_owners "
            "WHERE workspace_id='audit-rollback'") == "0", "audit failure committed workspace ownership")
        call("POST", PREFIX + "/tenants/owner-a/members", {**membership, "enabled": False, "expected_revision": 1}, who="alice", status=403)
        for name in ("bob-private", "shared"):
            call("GET", workspaces + "/" + name, who="bob", status=404)
        call("POST", workspaces, {"workspace_id": "audit-rollback", "owner_id": "bob"}, status=403)
        call("POST", PREFIX + "/tenants/owner-a/members", {**membership, "expected_revision": 2}, who="alice", status=403)
        call("GET", b + "/owned-b/workspaces/bob-private", who="bob")
        call("POST", PREFIX + "/accounts/bob/state", {"enabled": False, "expected_revision": 1})
        call("GET", workspaces + "/bob-private", who="bob", status=401)
        call("GET", b, who="bob", status=401)
        call("GET", b + "/owned-b/workspaces", who="bob", status=401)
        suite.stop_server(expect_clean=True)
        suite.start_server()
        call("GET", workspaces + "/bob-private", who="bob", status=401)
        call("GET", workspaces + "/alice-private", who="alice")
        require(call("GET", a + "?limit=1", who="alice") == project_page, "project inventory changed after restart")
        require(call("GET", workspaces + "?limit=1", who="alice") == workspace_page, "workspace inventory changed after restart")
        call("GET", b, who="bob", status=401)
        call("GET", "/api/v1/projects", who="legacy", status=403)
        call("GET", "/api/v1/projects", who="alice", status=403)
        call("GET", "/health/ready")
    finally:
        suite.stop_server(expect_clean=False)
        config_file.write_bytes(original)
        suite.psql("DELETE FROM shin_server.workspace_publication_history; DELETE FROM shin_server.identity_ownership_audit; "
            "DELETE FROM shin_server.identity_workspace_owners; DELETE FROM shin_server.identity_project_owners; "
            "DELETE FROM shin_server.identity_audit; DELETE FROM shin_server.identity_memberships; "
            "DELETE FROM shin_server.identity_tenants; DELETE FROM shin_server.identity_accounts; "
            "DELETE FROM shin_server.workspaces WHERE project_id IN ('owned-a','owned-b');"
            "DELETE FROM shin_server.projects WHERE id IN ('owned-a','owned-b','owned-c')")
        suite.start_server()
