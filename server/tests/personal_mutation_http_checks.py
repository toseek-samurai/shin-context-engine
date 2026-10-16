"""HTTP adapter proof using real Core preparations and isolated existing owner fixtures."""
from concurrent.futures import ThreadPoolExecutor
import copy
import json
import secrets
import time
import threading

from publication_test_support import require


def check_personal_mutations(s, call, paths):
    original = s.runtime_config_path.read_bytes()
    baseline = s.scalar("SELECT jsonb_agg(to_jsonb(a))::text FROM shin_server.active_routes a WHERE project_id IN ('visible','other')")
    jobs = []
    routes = {who: paths[who] + "/routes/" + route for who, route in
              (("alice", "visible-retrieval"), ("bob", "other-retrieval"))}
    draft = {"schema_version": "shin.session-draft.v1", "task_request": {
        "schema_version": "shin.task-request.v1", "request_id": "http-checkpoint",
        "task": {"objective": "Preserve route policy", "must": [{"id": "policy", "query": "project identity profile fingerprint"}]},
        "budget": {"max_evidence_bytes": 8192, "max_records": 6, "per_query_records": 6, "per_document_records": 2}},
        "state": {"current_slice": "HTTP fixture", "assertions": [], "changed_paths": [], "identifiers": [], "next_actions": []},
        "receipts": [], "vcs": None, "ttl_seconds": 3600}
    def mutation(who, revision=0):
        return {"task_id": "task-" + who, "thread_id": "thread-" + who, "request_id": secrets.token_hex(16),
                "request_deadline": int(time.time()) + 120, "expected_revision": revision}
    def clean(who, job):
        path = paths[who] + "/preparations/" + str(job)
        plan = call("GET", path + "/retention", who=who)
        if plan["state"] != "released":
            require(plan["can_apply"], "own HTTP candidate is not reclaimable")
            request = {"expected_plan_id": plan["plan_id"]}
            require(call("POST", path + "/retention", request, who)["state"] == "released", "candidate cleanup failed")
            require(not call("POST", path + "/retention", request, who)["mutation_performed"], "cleanup replay mutated")
    try:
        for who in ("alice", "bob"):
            path = paths[who] + "/task-checkpoints/http-slot"
            write = {**mutation(who), "schema_version": "shin.task-checkpoint-write.v1", "require_current": True,
                     "selector": {"kind": "route", "id": "visible-retrieval" if who == "alice" else "other-retrieval"}, "draft": draft}
            saved = call("POST", path, write, who)
            require(not saved["replayed"] and call("POST", path, write, who)["replayed"], "checkpoint retry changed")
            require(call("GET", path, who=who)["owner_id"] == "scoped-" + who, "checkpoint crossed owner")
            for kind in ("publication", 7, None):
                call("POST", path, {**write, "selector": {"kind": kind, "id": "scoped-publication"}}, who, 400)
            changed = copy.deepcopy(write)
            changed["draft"]["state"]["current_slice"] = "changed"
            call("POST", path, changed, who, 409)
            for action in ("resume", "impact"):
                result = call("POST", path + "/" + action, {"schema_version": "shin.task-" + action + "-request.v1",
                              "task_id": write["task_id"], "thread_id": write["thread_id"], "require_current": True}, who)
                require(result["read_only"], "continuation mutated state")
                require(not any('"' + field + '":' in json.dumps(result) for field in
                                ("root", "out_root", "checkpoint_text", "last_request_hash", "argv")), "continuation leaked host-private data")
            call("POST", path + "/receipts", {**mutation(who, saved["revision"]), "schema_version": "shin.task-receipt-import.v1", "receipts": []}, who, 403)
            if who == "alice":
                checkpoint = json.loads(s.scalar("SELECT checkpoint_text FROM shin_server.checkpoint_slots "
                    "WHERE owner_id='scoped-alice' AND slot_id='http-slot' AND scope_workspace_id='primary'"))
                receipt_scope = {**checkpoint["binding"], "snapshot_id": checkpoint["baseline"]["snapshot_id"],
                                 "worktree_id": checkpoint["baseline"]["worktree_id"]}
                receipt = {"schema_version": "shin.execution-receipt.v1", "receipt_id": "http-proof", "kind": "test",
                           "producer": "scoped-alice", "observed_at": checkpoint["captured_at"], "ttl_seconds": 3600,
                           "result": "pass", "scope": receipt_scope, "command_fingerprint": "fnv1a64:0000000000000000"}
                with_receipt = copy.deepcopy(write)
                with_receipt.update(mutation(who, saved["revision"]))
                with_receipt["draft"]["receipts"] = [receipt]
                saved = call("POST", path, with_receipt, who)
                config = json.loads(original)
                for actor in config["principals"]:
                    if actor["id"] == "scoped-alice":
                        actor["receipt_producer"] = True
                s.stop_server(expect_clean=True)
                s.replace_private_json(s.runtime_config_path, config)
                s.start_server()
                imported = {**mutation(who, saved["revision"]), "schema_version": "shin.task-receipt-import.v1", "receipts": [receipt]}
                saved = call("POST", path + "/receipts", imported, who)
                require(not saved["replayed"] and call("POST", path + "/receipts", imported, who)["replayed"], "authenticated import retry failed")
                s.stop_server(expect_clean=True)
                s.runtime_config_path.write_bytes(original)
                s.start_server()
            other = "bob" if who == "alice" else "alice"
            call("GET", path, who=other, status=404)
            entries = call("GET", paths[who] + "/task-checkpoints?limit=1", who=who)
            require(entries["entries"][0]["slot_id"] == "http-slot", "checkpoint page lost own slot")
            for query in ("?limit=33", "?limit=1&limit=1", "?owner_id=scoped-bob"):
                call("GET", paths[who] + "/task-checkpoints" + query, who=who, status=400)
            clears = [{**mutation(who, saved["revision"]), "schema_version": "shin.task-checkpoint-clear.v1"} for _ in range(2)]
            def race(body):
                return call("POST", path + "/clear", body, who, (200, 409))
            with ThreadPoolExecutor(max_workers=2) as pool:
                results = list(pool.map(race, clears))
            require([r[0] for r in results].count(200) == 1 and
                    next(r[1] for r in results if r[0] == 409)["code"] == "checkpoint-cleared",
                    "checkpoint CAS did not select one writer: " + str([(r[0], r[1].get("code")) for r in results]))
            winner = next(i for i, r in enumerate(results) if r[0] == 200)
            require(call("POST", path + "/clear", clears[winner], who)["replayed"], "clear replay failed")
            call("POST", path + "/clear", clears[1-winner], who, 409)
            call("POST", paths[who] + "/task-checkpoints/prune", {"schema_version": "shin.checkpoint-prune-request.v1"}, who)

        s.stop_server(expect_clean=True)
        config = json.loads(original)
        config["preparations"]["enabled"] = True
        s.replace_private_json(s.runtime_config_path, config)
        s.start_server()
        bases = {who: call("GET", route + "/activation", who=who) for who, route in routes.items()}
        for who in ("alice", "bob"):
            request = {"expected_snapshot_id": bases[who]["snapshot_id"]}
            call("POST", routes[who] + "/preparations", {**request, "workspace_admission": {}}, who, 400, key="http-forged-scope")
            created = call("POST", routes[who] + "/preparations", request, who, 202, key="http-same-key")
            job = created["job"]["id"]
            jobs.append((who, job))
            require(call("POST", routes[who] + "/preparations", request, who, key="http-same-key")["replayed"], "job retry failed")
            path = paths[who] + "/preparations/" + str(job)
            other = "bob" if who == "alice" else "alice"
            for suffix in ("", "/inspection", "/preflight", "/retention"):
                call("GET", paths[other] + "/preparations/" + str(job) + suffix, who=other, status=404)
            call("POST", paths[other] + "/preparations/" + str(job) + "/cancel", {}, other, 404)
            require(call("GET", paths[who] + "/preparations?limit=1", who=who)["items"][0]["id"] == job, "job page crossed owner")
            cancelled = call("POST", path + "/cancel", {}, who)
            require(cancelled["state"] in ("prepared", "cancelled", "failed") or cancelled["cancel_requested"], "owner cancellation failed")
            call("POST", path + "/cancel", {}, who)
            deadline = time.monotonic() + 15
            while call("GET", path, who=who)["state"] not in ("prepared", "cancelled", "failed"):
                require(time.monotonic() < deadline, "cancellation did not reach a terminal state")
                threading.Event().wait(0.05)
            call("POST", path + "/retention", {"expected_plan_id": "a" * 64}, who, 409)
            clean(who, job)

        prepared = {}
        for who in ("alice", "bob"):
            result = call("POST", routes[who] + "/preparations", {"expected_snapshot_id": bases[who]["snapshot_id"]},
                          who, 202, key="http-real-build")
            job = result["job"]["id"]
            jobs.append((who, job))
            s.wait_for_seed_preparation("visible" if who == "alice" else "other", "unused", job)
            prepared[who] = job
            path = paths[who] + "/preparations/" + str(job)
            for action in ("inspection", "preflight"):
                inspected = call("GET", path + "/" + action, who=who)
                require(not any('"' + field + '":' in json.dumps(inspected) for field in
                                ("root", "out_root", "artifact_key", "artifact_namespace", "workspace_admission", "worker_id")),
                        "candidate inspection leaked internal binding")
            activation = {"preparation_id": job, "expected_revision": bases[who]["revision"]}
            require(not call("POST", routes[who] + "/activation", activation, who)["replayed"], "activation failed")
            require(call("POST", routes[who] + "/activation", activation, who)["replayed"], "activation replay failed")
            call("POST", path + "/retention", {"expected_plan_id": "a" * 64}, who, 409)
            require(call("POST", routes[who] + "/brief", {"query": "Routes"}, who)["preparation_id"] == job, "retrieval did not select personal candidate")
        s.stop_server(expect_clean=True)
        s.start_server()
        for who, job in prepared.items():
            require(call("GET", routes[who] + "/activation", who=who)["preparation_id"] == job, "restart lost activation")
        timings = {"activation": [], "preparations": []}
        for _ in range(30):
            for name, path in (("activation", routes["alice"] + "/activation"), ("preparations", paths["alice"] + "/preparations")):
                started = time.perf_counter_ns()
                call("GET", path)
                timings[name].append((time.perf_counter_ns() - started) / 1_000_000)
        s.personal_mutation_measurement = {name: {"samples": len(values), "p50_ms": sorted(values)[14], "p95_ms": sorted(values)[28]}
                                           for name, values in timings.items()}
        s.psql("UPDATE shin_server.identity_accounts SET enabled=false WHERE id='alice'")
        for path in (paths["alice"] + "/preparations", routes["alice"] + "/activation", paths["alice"] + "/task-checkpoints"):
            call("GET", path, status=404)
        call("POST", paths["alice"] + "/preparations/" + str(prepared["alice"]) + "/cancel", {}, status=404)
        s.psql("UPDATE shin_server.identity_accounts SET enabled=true WHERE id='alice'")
        for who, job in prepared.items():
            active = call("GET", routes[who] + "/activation", who=who)
            call("POST", routes[who] + "/activation", {"preparation_id": None, "expected_revision": active["revision"]}, who)
            clean(who, job)
    finally:
        s.stop_server(expect_clean=False)
        s.runtime_config_path.write_bytes(original)
        s.start_server()
        # Restore only the declared test fixture, not any live user pointer/history.
        s.psql("UPDATE shin_server.active_routes a SET revision=b.revision,preparation_id=b.preparation_id,snapshot_id=b.snapshot_id,"
               "route_fingerprint=b.route_fingerprint,scope_workspace_id=b.scope_workspace_id FROM jsonb_populate_recordset("
               "NULL::shin_server.active_routes,'" + baseline.replace("'", "''") + "'::jsonb) b "
               "WHERE a.project_id=b.project_id AND a.route_id=b.route_id")
        s.psql("DELETE FROM shin_server.checkpoint_slots WHERE slot_id='http-slot' AND owner_id IN ('scoped-alice','scoped-bob')")
        if jobs:
            ids = ",".join(str(job) for _, job in jobs)
            s.psql("BEGIN; CREATE TEMP TABLE http_released ON COMMIT DROP AS SELECT p.id FROM shin_server.preparations p "
                   "JOIN shin_server.preparation_retention r ON r.preparation_id=p.id AND r.state='released' WHERE p.id IN (" + ids + "); "
                   "DELETE FROM shin_server.audit_events WHERE preparation_id IN (SELECT id FROM http_released); "
                   "DELETE FROM shin_server.audit_events WHERE principal_id IN ('scoped-alice','scoped-bob') AND activation_route_id IS NOT NULL; "
                   "DELETE FROM shin_server.activation_receipts WHERE principal_id IN ('scoped-alice','scoped-bob'); "
                   "DELETE FROM shin_server.preparation_retention WHERE preparation_id IN (SELECT id FROM http_released); "
                   "DELETE FROM shin_server.preparations WHERE id IN (SELECT id FROM http_released); COMMIT")
