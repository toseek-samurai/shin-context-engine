"""Offline namespace retirement through the existing deployment identity fence."""

import concurrent.futures
import json
import secrets
import subprocess

from integration_test import IntegrationFailure, PORT


def check(condition, message):
    if not condition:
        raise IntegrationFailure(message)


def signature(harness):
    tables = ("observer_streams", "observer_submissions", "observer_tombstones", "observer_usage",
              "audit_events", "audit_history", "server_recovery", "server_identity")
    return {table: harness.scalar(
        "SELECT encode(sha256(convert_to(COALESCE(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text),"
        f"'[]'::jsonb)::text,'UTF8')),'hex') FROM shin_server.{table} t"
    ) for table in tables}


def check_observer_identity_retirement(h):
    config = json.loads((h.state_root / "config/recovery.json").read_text())
    config["database"]["server_id"] = h.server_identity
    recovery_config = h.write_recovery_input(config)
    request = {"schema_version": "shin.observer-retirement-request.v1", "operation_id": secrets.token_hex(16),
               "expected_server_id": h.server_identity, "reason_code": "retired-identity-capacity", "service_fenced": True}

    def cli(command, value, error=None, selected_config=None):
        return h.recovery_cli(command, value, error, config_path=selected_config or recovery_config).get("result")

    def plan():
        result = cli("observer-retirement-plan", request)
        value = result.get("plan", {})
        check(result.get("read_only") is True and value.get("schema_version") == "shin.observer-retirement-plan.v1"
              and value.get("request") == request, "retirement plan did not bind the explicit offline request")
        return value

    def apply(value, error=None):
        return cli("observer-retirement-apply", {"plan": value, "discard_confirmed": True}, error)

    def restore_pin(new_id):
        runtime_path = h.state_root / "config/runtime.json"
        runtime = json.loads(runtime_path.read_text())
        runtime["database"]["server_id"] = new_id
        h.replace_private_json(runtime_path, runtime)
        h.server_identity = new_id
        h.start_server()
        h.owner_headers = h.auth_headers("reader")
        ready = h.wait_for_response("/health/ready", 200, headers=h.auth_headers("admin"))
        check(ready.get("result", {}).get("server_id") == new_id,
              "restarted Server did not establish its pinned runtime lifecycle lease")

    h.record_reclamation("observer-identity-retirement-live-fence", lambda:
                         cli("observer-retirement-plan", request, "server-recovery-busy"))
    # The preceding registry-capacity fixture intentionally leaves 127 rows.
    # Retire one of its empty, now-revoked fillers through the existing API.
    filler = h.scalar("SELECT stream_id FROM shin_server.observer_streams WHERE revoked AND accepted_sequence=0 "
                      "AND observer_id LIKE 'observer-reclamation-capacity-%' ORDER BY stream_id LIMIT 1")
    check(bool(filler), "prior capacity fixture did not leave an owned empty filler")
    filler_plan = h.plan(filler, eligible=True)
    status, reclaimed = h.apply_reclamation(filler, filler_plan)
    check(status == 200, "could not release the fixture registry slot through confirmed reclamation")
    h.assert_reclamation_receipt(reclaimed, filler_plan, replayed=False)
    retired, enrollment, old_packet, _, old_plan = h.retire_full("namespace-old")
    check(h.apply_reclamation(retired, old_plan)[0] == 200, "namespace fixture did not retire its old stream")
    active, active_observer, _ = h.enroll("namespace-active")
    h.accept_for(active, active_observer)
    h.stop_server(expect_clean=True)

    def read_only_and_guards():
        before = signature(h)
        reviewed = plan()
        check(plan() == reviewed and signature(h) == before, "retirement inspection changed durable state")
        denied = json.loads(recovery_config.read_text())
        denied["database"]["connection_file"] = str(h.state_root / "conninfo/runtime.conninfo")
        cli("observer-retirement-plan", request, "recovery-role-denied", h.write_recovery_input(denied))
        h.psql("ALTER TABLE shin_server.observer_tombstones OWNER TO shin_runtime")
        try:
            cli("observer-retirement-plan", request, "recovery-role-denied")
        finally:
            h.psql("ALTER TABLE shin_server.observer_tombstones OWNER TO CURRENT_USER")
            h.migrate()
        cli("observer-retirement-plan", {**request, "service_fenced": False}, "invalid-recovery-request")
        cli("observer-retirement-apply", {"plan": reviewed}, "invalid-request")
        cli("observer-retirement-apply", {"plan": reviewed, "discard_confirmed": False}, "invalid-recovery-request")
        cli("recovery-apply", {"plan": reviewed}, "invalid-recovery-request")
        apply({**reviewed, "new_server_id": "0" * 32}, "recovery-plan-stale")
        check(signature(h) == before, "retirement denial mutated the registry or deployment identity")

    h.record_reclamation("observer-identity-retirement-read-only-and-authority", read_only_and_guards)

    def drift_and_rollback():
        reviewed = plan()
        original = h.scalar(f"SELECT retirement::text FROM shin_server.observer_tombstones WHERE stream_id='{retired}'")
        try:
            h.psql(f"UPDATE shin_server.observer_tombstones SET retirement=retirement || '{{\"drift\":true}}'::jsonb WHERE stream_id='{retired}'")
            before = signature(h)
            apply(reviewed, "recovery-plan-stale")
            check(signature(h) == before, "stale retirement mutated its changed input")
        finally:
            literal = original.replace("'", "''")
            h.psql(f"UPDATE shin_server.observer_tombstones SET retirement='{literal}'::jsonb WHERE stream_id='{retired}'")
        h.psql("CREATE FUNCTION shin_server.fail_namespace_fixture() RETURNS trigger LANGUAGE plpgsql AS $$ "
               "BEGIN RAISE EXCEPTION 'namespace fixture failure'; END $$;"
               "CREATE TRIGGER fail_namespace_fixture BEFORE UPDATE ON shin_server.server_recovery "
               "FOR EACH ROW EXECUTE FUNCTION shin_server.fail_namespace_fixture()")
        try:
            before = signature(h)
            apply(reviewed, "database-unavailable")
            check(signature(h) == before, "failed namespace retirement did not roll back every mutation")
        finally:
            h.psql("DROP TRIGGER fail_namespace_fixture ON shin_server.server_recovery;"
                   "DROP FUNCTION shin_server.fail_namespace_fixture()")

    h.record_reclamation("observer-identity-retirement-content-drift-and-rollback", drift_and_rollback)

    # Bind an actual private pending queue while offline, then refuse any repin.
    fixture = h.fixture
    delivery_path = fixture.base / "namespace-delivery.json"
    delivery = {"schema_version": "shin.observer-delivery-config.v1", "host": "127.0.0.1", "port": PORT,
                "server_id": h.server_identity, "principal_id": "dev-reader", "request_timeout_ms": 300,
                "token_file": str(h.state_root / "secrets/dev_reader.token")}
    h.fixture_module.write_private_json(delivery_path, delivery)

    def native(command="deliver", success=True):
        child = subprocess.run([str(h.observer), command, "--config", str(fixture.config),
                                "--delivery-config", str(delivery_path)], capture_output=True, text=True,
                               timeout=30, env=h.observer_environment)
        check(child.returncode == (0 if success else 1) and not (child.stderr if success else child.stdout),
              "native retirement probe did not return its expected envelope")
        return h.fixture_module.assert_observer_envelope(json.loads(child.stdout if success else child.stderr), expected_ok=success)

    h.active_check = "observer-identity-retirement-native-pending-binding"
    bound = native()
    check(bound["delivery_failed"] and bound["state"]["outbox"]["entries"], "offline queue was not retained and bound")
    old_outbox = bound["state"]["outbox"]
    old_pin = h.server_identity
    old_headers = h.auth_headers("reader")
    old_admin = h.auth_headers("admin")
    h.record_reclamation("observer-identity-retirement-native-pending-binding", lambda: None)
    latest = None
    try:
        for cycle, target in enumerate((4096, 512)):
            h.active_check = f"observer-identity-retirement-capacity-cycle-{cycle + 1}"
            fixture_id = f"namespace-cycle-{cycle}-{secrets.token_hex(6)}"
            count_before = h.count("observer_tombstones", "TRUE")
            offset = 0
            while count_before + offset < target:
                size = min(512, target - count_before - offset)
                h.seed_tombstones(f"reclamation-namespace-{cycle}-{offset}", size, fixture_id, offset)
                offset += size
            check(h.count("observer_tombstones", "TRUE") == target, "namespace fixture did not reach its exact capacity")
            reviewed = plan()
            check(reviewed["tombstone_count"] == target, "namespace plan did not cover the full bounded set")
            before = signature(h)
            input_path = h.write_recovery_input({"plan": reviewed, "discard_confirmed": True})

            def compete(_):
                child = subprocess.run([str(h.binary), "observer-retirement-apply", "--config", str(recovery_config),
                                        "--input", str(input_path)], capture_output=True, text=True, timeout=45)
                value = json.loads(child.stdout if child.returncode == 0 else child.stderr)
                h.assert_envelope(value)
                check(child.returncode == 0 or value.get("error", {}).get("code") == "server-recovery-busy",
                      "concurrent retirement failed outside the cooperative lifecycle fence")
                return value

            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as workers:
                outcomes = list(workers.map(compete, range(2)))
            successes = [item["result"] for item in outcomes if item.get("ok")]
            check(sum(item["replayed"] is False for item in successes) == 1,
                  "concurrent namespace retirement did not commit exactly once")
            latest = apply(reviewed)
            receipt = latest["receipt"]
            check(latest["replayed"] is True and receipt.get("schema_version") == "shin.observer-retirement-receipt.v1"
                  and receipt.get("retired_tombstones") == target and receipt.get("retained_tombstones") == 0
                  and receipt.get("previous_server_id") == request["expected_server_id"]
                  and receipt.get("identity_namespace_retired") is True and receipt.get("audit_preserved") is True,
                  "namespace retirement lost its exact latest receipt or made a false identity claim")
            h.recovery_applied = True
            check(h.count("observer_tombstones", "TRUE") == 0 and h.scalar("SELECT tombstones FROM shin_server.observer_usage WHERE singleton") == "0",
                  "namespace retirement did not release identity capacity")
            after = signature(h)
            for table in ("observer_submissions", "audit_events", "audit_history"):
                check(after[table] == before[table], f"namespace retirement changed protected {table}")
            check(h.count("observer_streams", "NOT revoked") == 0, "namespace transition left an old stream active")
            h.record_reclamation(f"observer-identity-retirement-capacity-cycle-{cycle + 1}", lambda: None)
            h.active_check = f"observer-identity-retirement-restart-fence-{cycle + 1}"
            restore_pin(receipt["server_id"])
            h.recovery_cli("observer-retirement-apply", {"plan": reviewed, "discard_confirmed": True},
                           "server-recovery-busy", config_path=recovery_config)
            h.stop_server(expect_clean=True)
            check(apply(reviewed)["receipt"] == receipt, "retirement retry changed after a Server restart")
            h.record_reclamation(f"observer-identity-retirement-restart-fence-{cycle + 1}", lambda: None)
            latest_retry_config = recovery_config
            config["database"]["server_id"] = receipt["server_id"]
            recovery_config = h.write_recovery_input(config)
            request = {**request, "expected_server_id": receipt["server_id"], "operation_id": secrets.token_hex(16)}

        restore_pin(latest["receipt"]["server_id"])

        def old_requests_and_new_namespace():
            for headers, path, value in ((old_headers, f"/api/v1/observer-streams/{retired}/submissions", old_packet),
                                         (old_admin, "/api/v1/observer-streams", enrollment)):
                status, response = h.post_json(path, value, headers=headers)
                check(status == 409 and response.get("error", {}).get("code") == "server-identity-mismatch",
                      "old deployment request escaped the namespace fence")
            # Deliberately reuse the bare ID to prove the deployment pin is the fence.
            status, response = h.post_json("/api/v1/observer-streams", enrollment, headers=h.auth_headers("admin"))
            if status != 201:
                h.failure_http_status = status
                h.failure_error_code = response.get("error", {}).get("code")
                raise IntegrationFailure(f"new namespace enrollment returned {status}/{h.failure_error_code}")
            check(response["result"]["stream"]["accepted_sequence"] == 0,
                  "new administrative namespace enrollment inherited old progress")
            status, response = h.post_json(f"/api/v1/observer-streams/{retired}/submissions", old_packet, headers=old_headers)
            check(status == 409 and response.get("error", {}).get("code") == "server-identity-mismatch",
                  "old packet matched a reused bare stream ID")
            current = h.synthetic_submission(1, stream=retired, observer_id=enrollment["observer_id"], variant="new-namespace")
            status, response = h.post_json(f"/api/v1/observer-streams/{retired}/submissions", current, headers=h.owner_headers)
            check(status == 201 and response["result"]["receipt"]["server_id"] != old_pin,
                  "new namespace did not admit its own separately authorized packet")
            state_before = (fixture.state / "state.json").read_bytes()
            denied = native("reconcile", success=False)
            check(denied["code"] == "identity-mismatch" and (fixture.state / "state.json").read_bytes() == state_before,
                  "old native queue silently adopted the successor identity")
            h.fixture_module.write_private_json(delivery_path, {**delivery, "server_id": h.server_identity})
            denied = native(success=False)
            check(denied["code"] == "delivery-binding-mismatch" and (fixture.state / "state.json").read_bytes() == state_before,
                  "rewriting delivery configuration relabeled retained native packets")
            check(json.loads(state_before)["outbox"] == old_outbox, "namespace maintenance changed pending local history")

        h.record_reclamation("observer-identity-retirement-old-new-request-and-native-binding", old_requests_and_new_namespace)
        h.stop_server(expect_clean=True)
        empty = plan()
        before = signature(h)
        apply(empty, "observer-retirement-empty")
        check(signature(h) == before, "empty namespace retirement rotated identity")
        h.record_reclamation("observer-identity-retirement-empty-is-nonmutating", lambda: None)
        normal_request = {**request, "schema_version": "shin.server-recovery-request.v1", "reason_code": "clone"}
        normal_plan = cli("recovery-plan", normal_request)["plan"]
        check(normal_plan["schema_version"] == "shin.server-recovery-plan.v2", "ordinary recovery lost its original schema")
        normal = cli("recovery-apply", {"plan": normal_plan})
        check(normal["receipt"]["schema_version"] == "shin.server-recovery-receipt.v2"
              and normal["receipt"]["retained_tombstones"] == 0,
              "ordinary recovery misread the prior namespace maintenance receipt")
        cli("observer-retirement-apply", {"plan": reviewed, "discard_confirmed": True},
            "recovery-identity-mismatch", latest_retry_config)
        h.record_reclamation("observer-identity-retirement-followed-by-ordinary-recovery", lambda: None)
    finally:
        h.stop_server(expect_clean=False)
        # Delete only test-seeded metadata if a failed cycle did not retire it.
        removed = int(h.scalar("WITH removed AS (DELETE FROM shin_server.observer_tombstones "
                               "WHERE retirement->>'fixture' LIKE 'namespace-cycle-%' RETURNING 1) SELECT count(*) FROM removed"))
        if removed:
            h.psql(f"UPDATE shin_server.observer_usage SET tombstones=tombstones-{removed} WHERE singleton")
