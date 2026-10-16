"""Authenticated exact-record receipt checks on the isolated checkpoint stand."""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import tempfile

from publication_test_support import require


def check_origin_migration(c):
    s = c.s
    migrations = Path(__file__).resolve().parents[1] / "migrations"
    require(hashlib.sha256((migrations / "017_private_checkpoints.sql").read_bytes()).hexdigest() ==
            "d8d954888595eac80c74a1a88f7fa455c937440cd390ceb31e27d357305ef92c",
            "authenticated import rewrote migration017")
    snapshot = ("SELECT jsonb_build_object('slots',(SELECT jsonb_agg(to_jsonb(t)-'receipt_origins'-'scope_workspace_id' "
                "ORDER BY project_id,owner_id,slot_id) FROM shin_server.checkpoint_slots t),"
                "'floor',(SELECT to_jsonb(t) FROM shin_server.checkpoint_lifecycle t),"
                "'identity',(SELECT to_jsonb(t) FROM shin_server.server_identity t),"
                "'history',(SELECT jsonb_agg(to_jsonb(t) ORDER BY version) "
                "FROM shin_server.schema_migrations t WHERE version<=17))::text")
    database = "shin_checkpoint_" + secrets.token_hex(8)
    created = False

    def pg(*args, sql=None):
        result = subprocess.run([*s.compose_prefix(), "exec", "-T", "postgres", *args],
                                input=sql, capture_output=True, text=True, timeout=60,
                                env=dict(os.environ, SHIN_SERVER_INFRA_DIR=str(migrations.parent / "infra")))
        require(result.returncode == 0, f"isolated checkpoint migration {args[0]} failed")
        return result.stdout.strip()

    def isolated_sql(sql):
        return pg("psql", "-X", "-Atq", "-v", "ON_ERROR_STOP=1", "-U", "shin_migrator", "-d", database, sql=sql)

    s.stop_server(expect_clean=True)
    try:
        before_source = s.scalar(snapshot)
        saved = {}
        for table in ("projects", "checkpoint_slots", "checkpoint_lifecycle", "server_identity"):
            projection = "to_jsonb(t)-'receipt_origins'-'scope_workspace_id'" if table == "checkpoint_slots" else "to_jsonb(t)"
            saved[table] = s.sql_json(f"SELECT COALESCE(jsonb_agg({projection}),'[]'::jsonb) FROM shin_server.{table} t")
        require(bool(saved["checkpoint_slots"]), "receipt migration preservation fixture has no checkpoint")
        sequence = s.sql_json("SELECT jsonb_build_object('last_value',last_value,'is_called',is_called) "
                              "FROM shin_server.checkpoint_revisions")
        pg("createdb", "-U", "shin_migrator", "--template=template0", database)
        created = True
        # Rebuild the actual legacy schema separately; never punch a migration
        # history hole in the current stand when later migrations are added.
        for path in sorted(migrations.glob("[0-9][0-9][0-9]_*.sql"))[:17]:
            version = int(path.name[:3])
            checksum = hashlib.sha256(path.read_bytes()).hexdigest()
            isolated_sql("SET shin_server.migration_source_host_id='local-dev';" + path.read_text() +
                         f"INSERT INTO shin_server.schema_migrations(version,checksum) VALUES ({version},'{checksum}');")
        for table, rows in saved.items():
            if table in {"checkpoint_lifecycle", "server_identity"}:
                isolated_sql(f"DELETE FROM shin_server.{table}")
            isolated_sql(f"INSERT INTO shin_server.{table} SELECT * FROM jsonb_populate_recordset("
                         f"NULL::shin_server.{table}," + s._sql_text(json.dumps(rows)) + "::jsonb)")
        isolated_sql(f"SELECT setval('shin_server.checkpoint_revisions',{sequence['last_value']},"
                     + ("true" if sequence["is_called"] else "false") + ")")
        before = isolated_sql(snapshot)
        with tempfile.TemporaryDirectory(prefix="shin-checkpoint-upgrade-") as temporary:
            home = Path(temporary).resolve()
            password = s.read_private_token("postgres_migrator_password")
            require(re.fullmatch(r"[A-Za-z0-9_-]{32,128}", password) is not None, "unsafe developer credential format")
            connection = home / "migrator.conninfo"
            s.write_fixture_file(connection, f"host=127.0.0.1 port={s.postgres_port()} dbname={database} "
                                 f"user=shin_migrator password={password} gssencmode=disable\n")
            config = json.loads((s.state_root / "config/migrator.json").read_text())
            config["database"].update(expected_name=database, connection_file=str(connection),
                                      server_id=saved["server_identity"][0]["server_id"])
            config_path = home / "migrator.json"
            s.replace_private_json(config_path, config)
            for _ in range(2):
                s.run_process([str(s.binary), "migrate", "--config", str(config_path)], "isolated origin upgrade")
                require(isolated_sql(snapshot) == before, "origin upgrade changed saved payloads, replay fences or identities")
                require(isolated_sql("SELECT count(*) FROM shin_server.checkpoint_slots WHERE receipt_origins<>'[]'::jsonb") == "0",
                        "migration018 invented authenticated receipt origins")
        require(s.scalar(snapshot) == before_source, "legacy migration fixture changed the source stand")
    finally:
        if created:
            pg("dropdb", "-U", "shin_migrator", database)
        s.start_server()


def check_authenticated_import(c):
    s = c.s
    original = json.loads(s.runtime_config_path.read_text(encoding="utf-8"))
    slot, task, thread = c._slot("authenticated"), "authenticated-task", "authenticated-thread"
    receipt = dict(c._native_receipt(task, thread), receipt_id="exact", ttl_seconds=3600)
    sibling = dict(receipt, receipt_id="same-producer")
    expired = dict(receipt, receipt_id="expired", ttl_seconds=1)
    wrong_scope = dict(receipt, receipt_id="wrong-scope", scope=dict(receipt["scope"], task_id="other-task"))
    draft = c._draft(receipts=[receipt, sibling, expired, wrong_scope])
    saved = c._save(slot, c._save_payload(task=task, thread=thread, expected_revision=0,
                                        request_id=c._request_id(200), draft=draft))["metadata"]
    base = {"schema_version": "shin.task-receipt-import.v1", "task_id": task, "thread_id": thread,
            "expected_revision": saved["revision"], "request_id": c._request_id(201),
            "request_deadline": c._deadline(), "receipts": [receipt, expired]}

    def submit(body, role="admin"):
        return c._request("POST", c._path(slot, "/receipts"), body, role=role)

    def permission(enabled):
        config = json.loads(json.dumps(original))
        for principal in config["principals"]:
            if principal["id"] in {"dev-admin", "dev-reader"}:
                principal["receipt_producer"] = enabled
        s.stop_server(expect_clean=True)
        s.replace_private_json(s.runtime_config_path, config)
        s.start_server()

    def assessed(impact=False):
        status, response = c._resume(slot, task, thread, impact=impact, require_current=False)
        require(status == 200, "authenticated checkpoint evaluation failed")
        return c._assessment(response["result"], impact=impact)["assessment"]["receipts"]

    c._error(*submit(base), 403, "receipt-import-forbidden")
    try:
        permission(True)
        require(not any(row["producer_trusted"] for row in assessed()),
                "producer permission alone trusted saved claims")
        reader_body = dict(base, receipts=[dict(receipt, producer="dev-reader")])
        c._error(*submit(reader_body, role="reader"), 404, "checkpoint-not-found")
        c._error(*submit(dict(base, task_id="other-task")), 404, "checkpoint-not-found")
        for body, status, code in (
            (dict(base, receipts=[]), 400, "invalid-request"),
            (dict(base, receipts=[receipt, receipt]), 400, "invalid-request"),
            (dict(base, receipts=[dict(receipt, producer="dev-reader")]), 400, "invalid-request"),
            (dict(base, receipts=[dict(receipt, result="fail")]), 409, "receipt-import-mismatch"),
            (dict(base, receipts=[wrong_scope]), 409, "receipt-import-mismatch"),
            (dict(base, receipt_origins=["forged"]), 400, "invalid-request"),
            (dict(base, request_deadline=c._deadline(-1)), 409, "checkpoint-request-expired"),
        ):
            c._error(*submit(body), status, code)
        status, response = submit(base)
        require(status == 200 and response["result"]["replayed"] is False, "authenticated import failed")
        imported = c._metadata(response["result"])
        require(imported["revision"] > saved["revision"], "receipt import omitted its CAS revision")
        for field in ("checkpoint_id", "captured_at", "ttl_seconds", "provenance"):
            require(imported[field] == saved[field], "receipt import recaptured native checkpoint identity")
        status, replay = submit(base)
        require(status == 200 and replay["result"]["replayed"] is True and
                replay["result"]["revision"] == imported["revision"], "receipt retry was not idempotent")
        c._error(*submit(dict(base, receipts=[sibling])), 409, "checkpoint-request-conflict")
        observed_roots = (s.fixture_root, s.out_dir, s._inspection_artifact(saved["provenance"]["preparation_id"]))
        before_tree = tuple(s.tree_digest(root) for root in observed_roots)
        before_sql = c._sql_image()
        for impact in (False, True):
            rows = assessed(impact)
            require(rows[0]["producer_trusted"] and rows[0]["passed_current"] and
                    rows[0]["trust_source"] == "server-authenticated-receipt-origin",
                    "exact current authenticated receipt was not usable")
            require(not rows[1]["producer_trusted"], "trust leaked to another receipt from the same producer")
            require(rows[2]["producer_trusted"] and rows[2]["age"] == "expired" and not rows[2]["usable"],
                    "authenticated origin bypassed receipt expiry")
            require(not rows[0]["command_independently_verified"] and not rows[0]["continuous_execution_attestation"],
                    "origin was misrepresented as independent execution attestation")
        require(c._sql_image() == before_sql and tuple(s.tree_digest(root) for root in observed_roots) == before_tree,
                "authenticated receipt assessment mutated source, index or checkpoint storage")
        race = [dict(base, request_id=c._request_id(202 + i), expected_revision=imported["revision"],
                     receipts=[sibling]) for i in range(2)]
        with ThreadPoolExecutor(max_workers=2) as pool:
            outcomes = list(pool.map(submit, race))
        require(sorted(status for status, _ in outcomes) == [200, 409], "competing imports did not enforce CAS")
        imported = next(response["result"] for status, response in outcomes if status == 200)
        permission(True)
        require(assessed()[1]["producer_trusted"], "receipt origins did not persist across restart")
        permission(False)
        require(not any(row["producer_trusted"] for row in assessed()), "permission revocation left active trust")
        c._error(*submit(base), 403, "receipt-import-forbidden")
        permission(True)
        source = s.fixture_root / "docs/policy.md"
        text = source.read_text(encoding="utf-8")
        try:
            s.write_fixture_file(source, text + "\nAuthenticated receipt input drift.\n")
            for impact in (False, True):
                row = assessed(impact)[0]
                require(row["producer_trusted"] and not row["usable"], "origin bypassed source drift")
        finally:
            s.write_fixture_file(source, text)

        where = "project_id='visible' AND owner_id='dev-admin' AND slot_id='" + slot + "'"
        origins = s.scalar("SELECT receipt_origins::text FROM shin_server.checkpoint_slots WHERE " + where)
        try:
            s.psql("UPDATE shin_server.checkpoint_slots SET receipt_origins=jsonb_build_array(repeat('0',64)) WHERE " + where)
            c._error(*c._request("GET", c._path(slot)), 500, "database-corrupt")
        finally:
            s.psql("UPDATE shin_server.checkpoint_slots SET receipt_origins='" + origins + "'::jsonb WHERE " + where)
        updated = c._save(slot, c._save_payload(task=task, thread=thread, expected_revision=imported["revision"],
                                              request_id=c._request_id(204), draft=draft))["metadata"]
        require(not any(row["producer_trusted"] for row in assessed()), "ordinary save retained receipt origins")
        c._error(*submit(base), 409, "checkpoint-revision-conflict")
        current = dict(base, request_id=c._request_id(205), expected_revision=updated["revision"])
        status, response = submit(current)
        require(status == 200, "receipt reconfirmation failed")
        clear = {"schema_version": "shin.task-checkpoint-clear.v1", "task_id": task, "thread_id": thread,
                 "expected_revision": response["result"]["revision"], "request_id": c._request_id(206),
                 "request_deadline": c._deadline()}
        status, response = c._request("POST", c._path(slot, "/clear"), clear)
        require(status == 200, "authenticated checkpoint clear failed")
        c.records[("admin", slot)] = response["result"]
        c._error(*submit(current), 409, "checkpoint-cleared")
        require(s.scalar("SELECT receipt_origins::text FROM shin_server.checkpoint_slots WHERE " + where) == "[]",
                "clear retained receipt origins")
    finally:
        s.stop_server(expect_clean=True)
        s.replace_private_json(s.runtime_config_path, original)
        s.start_server()
