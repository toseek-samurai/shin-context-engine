"""Owner-scoped Observer lifecycle over real HTTP, PostgreSQL and native packets."""
from concurrent.futures import ThreadPoolExecutor
import copy
import hashlib
import json
import secrets
import time

from publication_test_support import require


def check_personal_observer(s):
    projects = "('po-a','po-a2','po-b')"
    s.psql("INSERT INTO shin_server.projects(id,display_name) VALUES('po-a','Same'),('po-a2','Same'),('po-b','Same');"
           "INSERT INTO shin_server.identity_accounts(id,principal_id,display_name) VALUES"
           "('po-a','dev-reader','Same'),('po-b','observer-other-reader','Same');"
           "INSERT INTO shin_server.identity_tenants VALUES('po-a','Same','po-a'),('po-b','Same','po-b');"
           "INSERT INTO shin_server.identity_memberships VALUES('po-a','po-a','owner',true,1),('po-b','po-b','owner',true,1);"
           "INSERT INTO shin_server.identity_project_owners VALUES('po-a','po-a'),('po-a2','po-a'),('po-b','po-b');"
           "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,source_kind) "
           "SELECT p,w,'observer-test-host','Same','observer' FROM (VALUES('po-a','primary'),('po-a','secondary'),"
           "('po-a2','primary'),('po-b','primary')) x(p,w);"
           "INSERT INTO shin_server.identity_workspace_owners SELECT o.tenant_id,w.project_id,w.id,o.tenant_id,'private' "
           "FROM shin_server.workspaces w JOIN shin_server.identity_project_owners o ON o.project_id=w.project_id "
           "WHERE w.project_id IN " + projects)
    def prefix(tenant="po-a", project="po-a", workspace="primary"):
        return f"/api/v1/personal/tenants/{tenant}/projects/{project}/workspaces/{workspace}/observer-streams"
    def call(method, path, body=None, who="alice", status=200):
        headers = dict(s.owner_headers if who == "alice" else s.other_headers)
        if body is not None:
            headers["Content-Type"] = "application/json"
        result = s.expect_status(status, method, path, headers=headers,
                                 body=s.compact_json(body) if body is not None else None)
        return result.get("result", result.get("error"))
    enrollments = []
    try:
        for tenant, project, workspace, who in (("po-a", "po-a", "primary", "alice"),
                                               ("po-a", "po-a", "secondary", "alice"),
                                               ("po-a", "po-a2", "primary", "alice"),
                                               ("po-b", "po-b", "primary", "bob")):
            body = {"stream_id": secrets.token_hex(16), "observer_id": "same-observer", "source_host_id": "observer-test-host",
                    "profile": "observer-fixture", "workspace_binding_digest": "a" * 64}
            base = prefix(tenant, project, workspace)
            require(call("POST", base, body, who, 201)["replayed"] is False, "personal enrollment failed")
            require(call("POST", base, body, who)["replayed"] is True, "personal enrollment retry changed identity")
            call("POST", base, {**body, "principal_id": "someone-else"}, who, 400)
            call("POST", base, {**body, "profile": "changed"}, who, 409)
            enrollments.append((base, body, who, project, workspace))
        base, body, who, project, workspace = enrollments[0]
        stream = body["stream_id"]
        route = base + "/" + stream
        call("GET", route, who="bob", status=404)
        call("GET", prefix("po-b", "po-b") + "/" + stream, who="bob", status=404)
        call("GET", prefix(project="po-a2") + "/" + stream, status=404)
        call("GET", prefix(workspace="secondary") + "/" + stream, status=404)
        call("GET", base + "/" + s.stream_id, status=404)  # no adoption of an old NULL-scope stream
        call("GET", "/api/v1/observer-streams/" + stream, status=403)
        def packet(sequence):
            value = copy.deepcopy(s.entry_one)
            value.update(stream_id=stream, sequence=sequence,
                         submission_id=hashlib.sha256(f"{stream}:{sequence}".encode()).hexdigest())
            value["payload"].update(project_id=project, workspace_id=workspace, observer_id=body["observer_id"],
                                     workspace_binding_digest=body["workspace_binding_digest"], observer_sequence=sequence)
            value["payload_sha256"] = s.payload_digest(value["payload"])
            return value
        first = packet(1)
        receipt = call("POST", route + "/submissions", first, status=201)["receipt"]
        with ThreadPoolExecutor(max_workers=4) as pool:
            replies = list(pool.map(lambda _: call("POST", route + "/submissions", first), range(12)))
        require(all(r["replayed"] and r["receipt"] == receipt for r in replies), "personal concurrent retry duplicated observation")
        call("POST", route + "/submissions", first, who="bob", status=404)
        call("POST", route + "/confirmations", {"receipt": receipt})
        reconcile = {"schema_version": "shin.observer-reconcile-request.v1", "server_id": s.server_id,
                     "stream_id": stream, "acknowledged_sequence": 1, "accepted_sequence": 1,
                     "last_receipt": receipt, "next_submission": None}
        require(call("POST", route + "/reconciliation", reconcile)["state"] == "caught-up", "personal reconciliation changed")
        for change, undo in (("UPDATE shin_server.identity_accounts SET enabled=false WHERE id='po-a'",
                              "UPDATE shin_server.identity_accounts SET enabled=true WHERE id='po-a'"),
                             ("UPDATE shin_server.identity_memberships SET enabled=false WHERE tenant_id='po-a'",
                              "UPDATE shin_server.identity_memberships SET enabled=true WHERE tenant_id='po-a'")):
            s.psql(change)
            call("GET", route, status=404)
            call("POST", route + "/submissions", first, status=404)
            call("POST", route + "/confirmations", {"receipt": receipt}, status=404)
            s.psql(undo)
        s.stop_server(expect_clean=True)
        s.start_server()
        require(call("POST", route + "/submissions", first)["receipt"] == receipt, "restart lost exact producer receipt")
        second = packet(2)
        s.psql("REVOKE INSERT ON shin_server.audit_events FROM shin_runtime")
        try:
            call("POST", route + "/submissions", second, status=503)
        finally:
            s.psql("GRANT INSERT ON shin_server.audit_events TO shin_runtime")
        require(call("GET", route)["accepted_sequence"] == 1, "audit failure committed personal submission")
        receipt2 = call("POST", route + "/submissions", second, status=201)["receipt"]
        call("POST", route + "/confirmations", {"receipt": receipt2})
        plan = call("GET", route + "/retention")
        call("POST", route + "/retention", {"plan": plan})
        require(call("POST", route + "/retention", {"plan": plan})["replayed"], "retention retry changed")
        call("POST", route + "/submissions", first, status=410)
        samples = []
        for _ in range(30):
            started = time.perf_counter_ns()
            call("GET", route)
            samples.append((time.perf_counter_ns() - started) / 1_000_000)
        s.personal_observer_measurement = {"samples": 30, "p50_ms": sorted(samples)[14], "p95_ms": sorted(samples)[28],
                                          "scope": "Scoped metadata HTTP, warm pool; not a load test or production SLO."}
        for base, body, who, _, _ in enrollments:
            route = base + "/" + body["stream_id"]
            call("POST", route + "/revoke", {}, who)
            plan = call("GET", route + "/reclamation", who=who)
            result = call("POST", route + "/reclamation", {"plan": plan}, who)
            require(not result["replayed"], "first personal reclamation replayed")
            require(call("POST", route + "/reclamation", {"plan": plan}, who)["replayed"], "tombstone retry failed")
            call("GET", route, who=who, status=410)
            call("POST", base, body, who, 410)
            call("GET", route, who="bob" if who == "alice" else "alice", status=404)
        for table in ("observer_streams", "observer_submissions", "observer_tombstones", "observer_usage"):
            s.psql(f"ALTER TABLE shin_server.{table} DISABLE ROW LEVEL SECURITY")
            try:
                s.expect_status(503, "GET", "/health/ready", headers=s.auth_headers("admin"))
            finally:
                s.psql(f"ALTER TABLE shin_server.{table} ENABLE ROW LEVEL SECURITY")
    finally:
        # Remove only this fixture, preserving prior suite streams and global accounting.
        s.psql("BEGIN; UPDATE shin_server.observer_usage SET "
               "streams=streams-(SELECT count(*) FROM shin_server.observer_streams WHERE project_id IN " + projects + "),"
               "submissions=submissions-(SELECT count(*) FROM shin_server.observer_submissions WHERE stream_id IN "
               "(SELECT stream_id FROM shin_server.observer_streams WHERE project_id IN " + projects + ")),"
               "retained_bytes=retained_bytes-COALESCE((SELECT sum(retained_bytes) FROM shin_server.observer_streams WHERE project_id IN " + projects + "),0),"
               "tombstones=tombstones-(SELECT count(*) FROM shin_server.observer_tombstones WHERE project_id IN " + projects + ");"
               "DELETE FROM shin_server.observer_submissions WHERE stream_id IN (SELECT stream_id FROM shin_server.observer_streams WHERE project_id IN " + projects + ");"
               "DELETE FROM shin_server.observer_streams WHERE project_id IN " + projects + ";"
               "DELETE FROM shin_server.observer_tombstones WHERE project_id IN " + projects + ";"
               "DELETE FROM shin_server.audit_events WHERE project_id IN " + projects + ";"
               "DELETE FROM shin_server.identity_workspace_owners WHERE project_id IN " + projects + ";"
               "DELETE FROM shin_server.identity_project_owners WHERE project_id IN " + projects + ";"
               "DELETE FROM shin_server.identity_memberships WHERE tenant_id IN ('po-a','po-b');"
               "DELETE FROM shin_server.identity_tenants WHERE id IN ('po-a','po-b');"
               "DELETE FROM shin_server.identity_accounts WHERE id IN ('po-a','po-b');"
               "DELETE FROM shin_server.workspaces WHERE project_id IN " + projects + ";"
               "DELETE FROM shin_server.projects WHERE id IN " + projects + "; COMMIT")
