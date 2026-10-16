"""R2 stopped-fixture backup/restore proof; never a production backup utility."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import time

from integration_test import IntegrationFailure, REPOSITORY_ROOT
from retrieval_integration_test import RetrievalIntegrationSuite


SOURCE_DATABASE = "shin_server_dev"
write_json = RetrievalIntegrationSuite.replace_private_json
write_text = RetrievalIntegrationSuite.write_fixture_file


def require(condition, message):
    if not condition:
        raise IntegrationFailure(message)


def inventory(root):
    rows = []
    for path in [root, *sorted(root.rglob("*"))]:
        entry = path.lstat()
        require(stat.S_ISREG(entry.st_mode) or stat.S_ISDIR(entry.st_mode),
                "backup inventory contains a symlink or special file")
        row = {"path": path.relative_to(root).as_posix(), "mode": stat.S_IMODE(entry.st_mode)}
        if stat.S_ISREG(entry.st_mode):
            content = path.read_bytes()
            row.update(bytes=len(content), sha256=hashlib.sha256(content).hexdigest())
        rows.append(row)
    return rows


def verify_inventory(root, expected):
    require(inventory(root) == expected, "artifact inventory changed or does not match its backup")


def run_backup_restore(suite, worker, *, candidate_trial=None):
    home = worker.home
    backup = home / "backup"
    backup.mkdir(mode=0o700)
    restored = home / "restored-managed"
    source_artifacts = home / "managed"
    source_hidden = home / "source-artifacts-fenced"
    runtime_path = suite.state_root / "config/runtime.json"
    original_runtime = runtime_path.read_bytes()
    original_curl = (home / "admin.curl").read_bytes()
    source_pin = suite.read_runtime_server_id()
    destination = "shin_restore_" + secrets.token_hex(8)
    created = False
    failure = None
    checks = []
    metrics = {}
    started = time.monotonic()

    def pg(*args, input_bytes=None):
        command = [*suite.compose_prefix(), "exec", "-T", "postgres", *args]
        result = subprocess.run(command, input=input_bytes, capture_output=True, timeout=90,
                                env=dict(os.environ, SHIN_SERVER_INFRA_DIR=str(REPOSITORY_ROOT / "server/infra")))
        require(result.returncode == 0, f"isolated PostgreSQL {args[0]} failed: exit {result.returncode}")
        return result.stdout

    def fingerprint(database):
        dump = pg("pg_dump", "--format=plain", "--schema=shin_server", "--no-owner",
                  "--no-privileges", "-U", "shin_migrator", "-d", database)
        require(bool(dump), "database fingerprint dump is empty")
        # PostgreSQL 18 generates an unrelated psql restriction token on every dump.
        normalized = re.sub(rb"^\\(?:un)?restrict [^\r\n]*\r?\n", b"", dump, flags=re.MULTILINE)
        return hashlib.sha256(normalized).hexdigest()

    def cli(command, config, request=None):
        argv = [str(suite.binary), command, "--config", str(config)]
        if request is not None:
            write_json(backup / "recovery-input.json", request)
            argv += ["--input", str(backup / "recovery-input.json")]
        result = subprocess.run(argv, capture_output=True, text=True, timeout=60)
        require(result.returncode == 0, f"recovery CLI {command} failed: exit {result.returncode}")
        payload = json.loads(result.stdout)
        require(payload.get("ok") is True, f"recovery CLI {command} did not succeed")
        return payload.get("result")

    try:
        before_metadata = worker.http("GET", "/api/v1/projects/workflow-project/task-checkpoints/workflow")
        before_retrieval = worker.http("POST", "/api/v1/projects/workflow-project/routes/workflow-route/brief",
                                       {"query": "publication authorization", "require_current": True})
        suite.stop_server(expect_clean=True)
        suite.require_http_port_free()
        require(suite.scalar("SELECT current_database()") == SOURCE_DATABASE, "wrong source database")
        require(suite.scalar("SELECT count(*) FROM pg_stat_activity WHERE datname = current_database() "
                             "AND usename = 'shin_runtime' AND pid <> pg_backend_pid()") == "0",
                "a source runtime database session remains active")
        source_fingerprint = fingerprint(SOURCE_DATABASE)
        source_files = inventory(home / "source")
        source_manifest = inventory(source_artifacts)
        require(any(row["path"].endswith("manifest.json") for row in source_manifest),
                "backup fixture has no generated artifact manifest")
        dump = pg("pg_dump", "--format=custom", "--schema=shin_server", "--no-owner",
                  "--no-privileges", "-U", "shin_migrator", "-d", SOURCE_DATABASE)
        require(bool(dump), "database backup is empty")
        dump_path = backup / "database.dump"
        with dump_path.open("xb") as output:
            output.write(dump)
        metrics["dump_bytes"] = len(dump)
        metrics["dump_sha256"] = hashlib.sha256(dump).hexdigest()
        write_text(backup / "source-runtime.json", original_runtime.decode("utf-8"))
        shutil.copytree(source_artifacts, backup / "managed")
        verify_inventory(source_artifacts, source_manifest)
        verify_inventory(backup / "managed", source_manifest)
        write_json(backup / "artifact-inventory.json", source_manifest)
        metrics["artifact_files"] = sum("sha256" in row for row in source_manifest)
        metrics["artifact_bytes"] = sum(row.get("bytes", 0) for row in source_manifest)
        checks.append("stopped-nonempty-database-and-artifact-backup")

        if candidate_trial is not None:
            candidate_trial.run(suite, worker)

        tampered = backup / "managed" / next(row["path"] for row in source_manifest if "sha256" in row)
        original = tampered.read_bytes()
        tampered.write_bytes(original + b"\nchanged")
        try:
            verify_inventory(backup / "managed", source_manifest)
        except IntegrationFailure:
            pass
        else:
            raise IntegrationFailure("altered backup passed its manifest gate")
        tampered.write_bytes(original)
        verify_inventory(backup / "managed", source_manifest)
        checks.append("altered-artifact-backup-rejected")

        # createdb refuses an existing name. Only a database created by this run is dropped.
        pg("createdb", "-U", "shin_migrator", "--template=template0", destination)
        created = True
        require(pg("psql", "-X", "-Atq", "-U", "shin_migrator", "-d", destination, "-c",
                   "SELECT count(*) FROM pg_namespace WHERE nspname = 'shin_server'").strip() == b"0",
                "restore destination was not empty")
        saved_dump = dump_path.read_bytes()
        require(hashlib.sha256(saved_dump).hexdigest() == metrics["dump_sha256"], "database backup changed")
        pg("pg_restore", "--exit-on-error", "--no-owner", "--no-privileges",
           "-U", "shin_migrator", "-d", destination, input_bytes=saved_dump)
        shutil.copytree(backup / "managed", restored)
        verify_inventory(restored, source_manifest)
        require(fingerprint(destination) == source_fingerprint, "restored database differs before recovery")
        checks.append("clean-database-and-artifact-restore-match")

        configs = {}
        for role, password_file in (("runtime", "shin_runtime_password"), ("migrator", "postgres_migrator_password")):
            password = suite.read_private_token(password_file)
            require(re.fullmatch(r"[A-Za-z0-9_-]{32,128}", password) is not None,
                    "stand password is outside the developer helper's safe libpq subset")
            connection = backup / f"{role}.conninfo"
            write_text(connection, f"host=127.0.0.1 port={suite.postgres_port()} dbname={destination} user=shin_{role} "
                       f"password={password} gssencmode=disable\n")
            config = json.loads((suite.state_root / "config" / f"{role}.json").read_text())
            config["database"].update(connection_file=str(connection), expected_name=destination)
            config["preparations"]["state_dir"] = str(restored)
            configs[role] = config
            write_json(backup / f"{role}.json", config)
        cli("migrate", backup / "migrator.json")
        require(fingerprint(destination) == source_fingerprint, "migration changed restored managed content")
        recovery = backup / "recovery.json"
        write_json(recovery, {"schema_version": "shin.server-recovery-config.v1",
                             "database": configs["migrator"]["database"]})
        request = {"schema_version": "shin.server-recovery-request.v1", "operation_id": secrets.token_hex(16),
                   "expected_server_id": source_pin, "reason_code": "backup-restore", "service_fenced": True}
        planned = cli("recovery-plan", recovery, request)
        require(planned["read_only"] and not planned["restored_backup_verified"], "plan overclaimed backup proof")
        plan = planned["plan"]
        write_json(backup / "inspected-plan.json", plan)
        applied = cli("recovery-apply", recovery, {"plan": plan})
        receipt = applied["receipt"]
        replay = cli("recovery-apply", recovery, {"plan": plan})
        require(not applied["replayed"] and replay["replayed"] and replay["receipt"] == receipt,
                "recovery retry changed its result")
        require(receipt["previous_server_id"] == source_pin and receipt["server_id"] != source_pin,
                "recovery did not rotate the deployment identity")
        for field in ("restored_backup_verified", "history_complete", "local_state_verified",
                      "source_verified", "shared_publication_mutated"):
            require(receipt[field] is False, "recovery receipt overclaimed verification")
        checks.append("explicit-identity-rotation-and-exact-retry")

        # Make the old artifact directory inaccessible at its configured path.
        # A successful restored retrieval must not borrow the original files.
        source_artifacts.rename(source_hidden)
        configs["runtime"]["database"]["server_id"] = receipt["server_id"]
        write_json(runtime_path, configs["runtime"])
        suite.start_server()
        slot = "/api/v1/projects/workflow-project/task-checkpoints/workflow"
        denied = worker.http("GET", slot, expected=(409,))
        require(denied["code"] == "server-identity-mismatch", "old client pin was not fenced")
        token = suite.read_private_token("dev_admin.token")
        write_text(home / "admin.curl", f'header = "Authorization: Bearer {token}"\n'
                   f'header = "X-Shin-Server-ID: {receipt["server_id"]}"\n'
                   'header = "Content-Type: application/json"\nconnect-timeout = 2\nmax-time = 20\n')
        require(worker.http("GET", slot) == before_metadata, "restore changed the private checkpoint")
        route = "/api/v1/projects/workflow-project/routes/workflow-route/brief"
        denied = worker.http("POST", route, {"query": "publication authorization"}, expected=(409,))
        require(denied["code"] == "active-generation-route-changed", "relocated artifact namespace was silently trusted")
        checks.append("old-client-pin-and-relocated-activation-fenced")

        worker.prepare_and_activate()
        after = worker.http("POST", route, {"query": "publication authorization", "require_current": True})
        require(after["snapshot_id"] == before_retrieval["snapshot_id"] and
                after["evidence"] == before_retrieval["evidence"] and
                after["preparation_id"] != before_retrieval["preparation_id"],
                "explicit restored preparation changed unchanged-source evidence or reused old activation")
        worker.refresh_context()
        assessment = worker.http("POST", slot + "/resume", worker.example("resume"))["assessment"]
        require(assessment["index_current"] and assessment["continuation_ready"], "restored task cannot resume")
        rows = {row["receipt_id"]: row for row in assessment["receipts"]}
        require(rows["authorized-check"]["passed_current"], "restore lost a current confirmed receipt")
        require(worker.http("GET", slot) == before_metadata, "read-only resume rewrote the checkpoint")
        checks.append("explicit-reprepare-and-current-checkpoint-resume")
        require(fingerprint(SOURCE_DATABASE) == source_fingerprint, "clone recovery modified the source database")
        verify_inventory(home / "source", source_files)
        verify_inventory(source_hidden, source_manifest)
        checks.append("original-database-source-and-artifacts-preserved")
        metrics["snapshot_id"] = after["snapshot_id"]
        metrics["source_database_sha256"] = source_fingerprint
        if candidate_trial is not None:
            candidate_trial.verify_preserved(suite)
    except Exception as error:
        failure = str(error)
    finally:
        try:
            suite.stop_server(expect_clean=True)
            if candidate_trial is not None:
                try:
                    candidate_trial.cleanup(suite)
                except Exception as error:
                    failure = (failure + "; " if failure else "") + "candidate cleanup: " + str(error)
            if created:
                require(re.fullmatch(r"shin_restore_[0-9a-f]{16}", destination) is not None,
                        "refusing cleanup outside the owned restore database")
                pg("dropdb", "-U", "shin_migrator", destination)
            if source_hidden.exists():
                source_hidden.rename(source_artifacts)
            write_text(runtime_path, original_runtime.decode("utf-8"))
            write_text(home / "admin.curl", original_curl.decode("utf-8"))
            suite.start_server()
        except Exception as error:
            failure = (failure + "; " if failure else "") + "restore cleanup: " + str(error)
    return {"ok": failure is None, "checks": checks, "failure": failure, "metrics": metrics,
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "boundary": "Controlled stopped fixture; new database in the same cluster, unchanged source root "
                        "and explicit reprepare after artifact relocation. Not PITR, cross-host recovery, "
                        "credential/role backup, shared-publication rebinding or a backup authenticity proof."}
