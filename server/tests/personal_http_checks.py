"""Personal HTTP retrieval/history over the existing native scoped fixture."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import secrets
import time

from publication_test_support import require


def check_personal_http(s):
    original = s.runtime_config_path.read_bytes()
    config = json.loads(original)
    tokens = {name: secrets.token_urlsafe(40) for name in ("alice", "bob")}
    for name, token in tokens.items():
        config["principals"].append({"id": "scoped-" + name, "role": "reader", "project_ids": [],
                                     "token_sha256": hashlib.sha256(token.encode()).hexdigest()})
    config["observability"] = {"max_retained_requests": 256, "max_query_bytes": 4096,
                                "capture_query_projects": ["visible", "other"]}
    s.replace_private_json(s.runtime_config_path, config)
    base = "/api/v1/personal/tenants/{}/projects/{}/workspaces/primary"
    paths = {"alice": base.format("scope-a", "visible"), "bob": base.format("scope-b", "other")}
    def call(method, path, body=None, who="alice", status=200, key=None):
        headers = s.auth_headers("admin") if who == "admin" else {
            "Authorization": "Bearer " + tokens[who], "X-Shin-Server-ID": s.server_id}
        if body is not None:
            headers["Content-Type"] = "application/json"
        if key is not None:
            headers["Idempotency-Key"] = key
        if isinstance(status, tuple):
            code, result = s.request(method, path, headers=headers, body=json.dumps(body).encode() if body is not None else None)
            require(code in status, "unexpected concurrent mutation HTTP status: " + str(code))
            return code, result.get("result", result.get("error"))
        result = s.expect_status(status, method, path, headers=headers,
                                 body=json.dumps(body).encode() if body is not None else None)
        return result.get("result", result.get("error"))
    def brief(who="alice", status=200):
        route = "visible-retrieval" if who == "alice" else "other-retrieval"
        return call("POST", paths[who] + "/routes/" + route + "/brief", {"query": "Routes"}, who, status)
    capture = None
    try:
        s.start_server()
        for who in tokens:
            require(brief(who)["evidence"]["ok"], "personal HTTP retrieval failed")
        for who, other in (("alice", "bob"), ("bob", "alice")):
            route = "visible-retrieval" if other == "alice" else "other-retrieval"
            call("POST", paths[other] + "/routes/" + route + "/brief", {"query": "Routes"}, who, 404)
            call("GET", paths[other] + "/journal", who=who, status=404)
            for legacy in ("/api/v1/cache", "/api/v1/preparations", "/api/v1/projects/visible/publications/shared"):
                require(call("GET", legacy, who=who, status=403)["code"] == "identity-data-plane-not-enabled", "legacy fence opened")
        with ThreadPoolExecutor(max_workers=4) as pool:
            replies = list(pool.map(brief, ["alice", "bob"] * 8))
        require(all(r["tenant_id"] == ("scope-a" if i % 2 == 0 else "scope-b") for i, r in enumerate(replies)),
                "pooled personal request reused another scope")
        samples = []
        for _ in range(30):
            started = time.perf_counter_ns()
            brief()
            samples.append((time.perf_counter_ns() - started) / 1_000_000)
        s.personal_http_measurement = {"samples": len(samples), "p50_ms": sorted(samples)[14], "p95_ms": sorted(samples)[28],
                                      "scope": "Warm scoped brief, loopback HTTP; not a production SLO or paired speedup."}
        capture = call("POST", "/api/v1/telemetry/retrieval/journal/capture", {}, "admin")
        page = call("GET", paths["alice"] + "/journal?limit=1")
        require(len(page["items"]) == 1 and page["next_after"] and page["history_complete"] is False, "private page/cursor differs")
        require("request" not in page["items"][0] and "sequence" not in page["items"][0], "private page leaked internal fields")
        event = page["items"][0]["id"]
        detail = call("GET", paths["alice"] + "/journal/" + str(event))
        require(detail["attempt"]["request"] == {"query": "Routes"}, "own opted-in request lost")
        foreign = call("GET", paths["bob"] + "/journal", who="bob")["items"][0]["id"]
        call("GET", paths["alice"] + "/journal/" + str(foreign), status=404)
        for query in ("?limit=33", "?limit=1&limit=1", "?tenant_id=scope-b"):
            call("GET", paths["alice"] + "/journal" + query, status=400)
        s.psql("UPDATE shin_server.identity_accounts SET enabled=false WHERE id='alice'")
        brief(status=404)
        call("GET", paths["alice"] + "/journal", status=404)
        s.psql("UPDATE shin_server.identity_accounts SET enabled=true WHERE id='alice'")
        s.psql("UPDATE shin_server.identity_memberships SET enabled=false WHERE tenant_id='scope-a' AND account_id='alice'")
        brief(status=404)
        s.psql("UPDATE shin_server.identity_memberships SET enabled=true WHERE tenant_id='scope-a' AND account_id='alice'")
        s.stop_server(expect_clean=True)
        s.start_server()
        require(call("GET", paths["alice"] + "/journal/" + str(event)) == detail, "private HTTP journal lost after restart")
        brief()
        from personal_mutation_http_checks import check_personal_mutations
        s.record("personal-http-checkpoints-preparation-activation-cleanup", lambda: check_personal_mutations(s, call, paths))
    finally:
        # Reclaim just this completed fixture's producer using the product lifecycle.
        if capture is not None:
            route = "/api/v1/telemetry/retrieval/journal/producers/" + capture["instance_id"] + "/retention"
            plan = call("GET", route + "?through=" + str(capture["observed_through"]), who="admin")
            call("POST", route, {"plan": plan, "discard_confirmed": True}, "admin")
            route = "/api/v1/telemetry/retrieval/journal/epochs/reclamation"
            plan = call("GET", route + "?through=" + str(capture["producer_epoch"]), who="admin")
            require(plan["producer_count"] == 1, "private fixture reclamation includes foreign producer")
            call("POST", route, {"plan": plan, "discard_future_confirmed": True}, "admin")
        s.stop_server(expect_clean=False)
        s.runtime_config_path.write_bytes(original)
