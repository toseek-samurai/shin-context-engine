#!/usr/bin/env python3
"""End-to-end PostgreSQL and HTTP coverage for Observer ingestion sidecar APIs."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import stat
import subprocess
import sys
import tempfile
import threading
import time
from types import ModuleType
from typing import Any

from integration_test import BUILD_ROOT, PORT, IntegrationFailure, IntegrationSuite, under_build
from native_json_codec import NativeJsonCodec


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
OBSERVER_FIXTURE_TEST = REPOSITORY_ROOT / "observer" / "tests" / "integration_test.py"
PROJECT_ID = "visible"
WORKSPACE_ID = "observer-fixture"
OBSERVER_ID = "observer-test-agent"
SOURCE_HOST_ID = "observer-test-host"
PROFILE = "observer-fixture"
RECEIPT_FIELDS = {
    "schema_version", "server_id", "stream_id", "sequence", "submission_id", "payload_sha256",
    "project_id", "workspace_id", "received_at", "verification_level", "server_source_verified",
    "shared_publication_mutated",
}
OBSERVER_IDENTITY_FIELDS = {
    "schema_version", "server_id", "principal_id", "submission_schema", "acknowledgement_schema",
    "confirmation_schema", "reconciliation_schema",
    "request_identity_header",
    "verification_level", "source_verified", "shared_publication_mutated",
}


def load_observer_fixture_module() -> ModuleType:
    spec = importlib.util.spec_from_file_location("shin_observer_fixture_integration", OBSERVER_FIXTURE_TEST)
    if spec is None or spec.loader is None:
        raise IntegrationFailure("Observer integration Fixture module is unavailable")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    if not isinstance(getattr(module, "Fixture", None), type):
        raise IntegrationFailure("Observer integration Fixture is unavailable")
    return module


class ObserverIngestionIntegrationSuite(IntegrationSuite):
    def __init__(self, binary: Path, observer: Path, state_root: Path, report_path: Path) -> None:
        super().__init__(binary, state_root, report_path)
        self.observer = observer
        self.json_codec = NativeJsonCodec(binary.parent / "shin-server-json-codec-probe")
        self.fixture_module = load_observer_fixture_module()
        self.fixture_root: Path | None = None
        self.fixture: Any = None
        self.entry_one: dict[str, Any] | None = None
        self.entry_two: dict[str, Any] | None = None
        self.stream_id = ""
        self.binding_digest = ""
        self.owner_headers = self.auth_headers("reader")
        self.other_headers: dict[str, str] | None = None
        self.runtime_config_original: bytes | None = None
        self.observer_checks: list[dict[str, Any]] = []
        self.source_before_ingestion: list[tuple[str, int, int, int]] | None = None
        self.active_routes_before_ingestion: str | None = None
        self.server_identity = ""
        self.load_requests = 0
        self.load_result: dict[str, Any] | None = None
        self.native_delivery_result: dict[str, Any] | None = None

    def record_observer(self, name: str, operation: Any) -> None:
        started = time.monotonic()
        self.record(name, operation)
        self.observer_checks.append({"name": name, "elapsed_ms": round((time.monotonic() - started) * 1000, 3)})

    @staticmethod
    def compact_json(value: Any) -> bytes:
        return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")

    def payload_digest(self, value: Any) -> str:
        # Python repr and nlohmann::json can encode the same double differently.
        return hashlib.sha256(self.json_codec.encode(value)).hexdigest()

    @staticmethod
    def require_hex(value: Any, length: int, message: str) -> str:
        if not isinstance(value, str) or len(value) != length or any(character not in "0123456789abcdef" for character in value):
            raise IntegrationFailure(message)
        return value

    def replace_private_json(self, path: Path, value: dict[str, Any]) -> None:
        entry = path.lstat()
        if stat.S_ISLNK(entry.st_mode) or not stat.S_ISREG(entry.st_mode) or stat.S_IMODE(entry.st_mode) != 0o600:
            raise IntegrationFailure("runtime configuration is not a private regular file")
        temporary = path.with_name(f".{path.name}.{secrets.token_hex(8)}.tmp")
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            with os.fdopen(descriptor, "wb") as output:
                output.write(self.compact_json(value) + b"\n")
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)

    def add_non_owner_reader(self) -> None:
        path = self.state_root / "config" / "runtime.json"
        self.runtime_config_original = path.read_bytes()
        config = json.loads(self.runtime_config_original)
        token = secrets.token_urlsafe(36)
        principals = config.get("principals")
        if not isinstance(principals, list):
            raise IntegrationFailure("runtime configuration does not contain principals")
        principals.append({
            "id": "observer-other-reader",
            "role": "reader",
            "token_sha256": hashlib.sha256(token.encode("utf-8")).hexdigest(),
            "project_ids": [PROJECT_ID],
        })
        self.replace_private_json(path, config)
        self.other_headers = {"Authorization": f"Bearer {token}", "X-Shin-Server-ID": self.server_id}

    def restore_runtime_config(self) -> None:
        if self.runtime_config_original is None:
            return
        path = self.state_root / "config" / "runtime.json"
        descriptor = os.open(path, os.O_WRONLY | os.O_TRUNC)
        with os.fdopen(descriptor, "wb") as output:
            output.write(self.runtime_config_original)
            output.flush()
            os.fsync(output.fileno())
        self.runtime_config_original = None
        self.other_headers = None

    def prepare_observer_fixture(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="shin-observer-ingestion-")
        self._fixture_temporary = temporary
        root = Path(temporary.name).resolve(strict=True)
        try:
            root.relative_to(REPOSITORY_ROOT)
        except ValueError:
            pass
        else:
            raise IntegrationFailure("Observer fixture must be external to the repository")
        self.fixture_root = root
        fixture_type = self.fixture_module.Fixture
        self.fixture = fixture_type(root, "observer-ingestion")
        config = self.fixture.config_data()
        config.update({
            "project_id": PROJECT_ID,
            "workspace_id": WORKSPACE_ID,
            "observer_id": OBSERVER_ID,
            "source_host_id": SOURCE_HOST_ID,
        })
        config["outbox"] = {"enabled": True, "max_entries": 8, "max_bytes": 128 * 1024}
        self.fixture.write_config(config)

        first = self.sample_observer()
        outbox = first.get("outbox")
        if not isinstance(outbox, dict) or not isinstance(outbox.get("entries"), list) or len(outbox["entries"]) != 1:
            raise IntegrationFailure("Observer did not emit its first durable outbox submission")
        self.entry_one = self.native_submission(outbox["entries"][0])
        self.stream_id = self.require_hex(outbox.get("stream_id"), 32, "Observer emitted an invalid stream id")
        self.binding_digest = self.require_hex(first.get("binding_key"), 64, "Observer emitted an invalid binding digest")

        self.fixture.write_source("Observer ingestion fixture revision two.\n")
        second = self.sample_observer()
        entries = second.get("outbox", {}).get("entries") if isinstance(second.get("outbox"), dict) else None
        if not isinstance(entries, list) or len(entries) != 2:
            raise IntegrationFailure("Observer did not retain a second ordered outbox submission")
        self.entry_two = self.native_submission(entries[1])
        if self.entry_one["stream_id"] != self.stream_id or self.entry_two["stream_id"] != self.stream_id or \
                self.entry_one["sequence"] != 1 or self.entry_two["sequence"] != 2:
            raise IntegrationFailure("Observer outbox did not produce consecutive submissions")

    def cleanup_observer_fixture(self) -> None:
        self.json_codec.close()
        temporary = getattr(self, "_fixture_temporary", None)
        if temporary is not None:
            temporary.cleanup()
            self._fixture_temporary = None
        self.fixture_root = None
        self.fixture = None

    @property
    def observer_environment(self) -> dict[str, str]:
        environment = {"PATH": "", "LC_ALL": os.environ.get("LC_ALL", "C")}
        for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS", "ASAN_SYMBOLIZER_PATH"):
            if name in os.environ:
                environment[name] = os.environ[name]
        return environment

    def sample_observer(self) -> dict[str, Any]:
        if self.fixture is None:
            raise IntegrationFailure("Observer fixture was not prepared")
        completed = subprocess.run(
            [str(self.observer), "sample", "--config", str(self.fixture.config)],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=30.0,
            env=self.observer_environment,
        )
        if completed.returncode != 0 or completed.stderr:
            raise IntegrationFailure("Observer sample did not complete cleanly")
        try:
            response = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise IntegrationFailure("Observer sample did not return JSON") from error
        if not isinstance(response, dict) or response.get("ok") is not True or not isinstance(response.get("result"), dict):
            raise IntegrationFailure("Observer sample did not return a success envelope")
        state = response["result"].get("state", response["result"])
        if not isinstance(state, dict):
            raise IntegrationFailure("Observer sample did not return persisted state")
        return state

    def native_submission(self, value: Any) -> dict[str, Any]:
        fields = {"schema_version", "stream_id", "sequence", "submission_id", "payload_sha256", "payload"}
        if not isinstance(value, dict) or set(value) != fields:
            raise IntegrationFailure("Observer outbox submission does not match shin.observer-submission.v1")
        if value["schema_version"] != "shin.observer-submission.v1" or not isinstance(value["sequence"], int):
            raise IntegrationFailure("Observer outbox submission has invalid identity fields")
        self.require_hex(value["stream_id"], 32, "Observer outbox stream id is invalid")
        self.require_hex(value["submission_id"], 64, "Observer outbox submission id is invalid")
        self.require_hex(value["payload_sha256"], 64, "Observer outbox payload digest is invalid")
        if not isinstance(value["payload"], dict) or self.payload_digest(value["payload"]) != value["payload_sha256"]:
            raise IntegrationFailure("Observer outbox payload digest does not bind the native payload")
        return json.loads(json.dumps(value))

    def post_json(self, target: str, body: dict[str, Any], *, headers: dict[str, str]) -> tuple[int, dict[str, Any]]:
        return self.request("POST", target, headers={**headers, "Content-Type": "application/json"}, body=self.compact_json(body))

    def expect_error(self, status: int, code: str, method: str, target: str, *, headers: dict[str, str], body: bytes | None = None) -> None:
        response = self.expect_status(status, method, target, headers=headers, body=body)
        if response.get("error", {}).get("code") != code:
            raise IntegrationFailure("Observer endpoint returned the wrong public error code")

    def observer_workspace(self) -> dict[str, Any]:
        return {"id": WORKSPACE_ID, "display_name": "Observer fixture", "source_host_id": SOURCE_HOST_ID}

    def enrollment(self) -> dict[str, Any]:
        return {
            "stream_id": self.stream_id,
            "project_id": PROJECT_ID,
            "workspace_id": WORKSPACE_ID,
            "principal_id": "dev-reader",
            "observer_id": OBSERVER_ID,
            "source_host_id": SOURCE_HOST_ID,
            "profile": PROFILE,
            "workspace_binding_digest": self.binding_digest,
        }

    def post_submission(self, submission: dict[str, Any], *, headers: dict[str, str] | None = None) -> tuple[int, dict[str, Any]]:
        return self.post_json(
            f"/api/v1/observer-streams/{self.stream_id}/submissions",
            submission,
            headers=headers or self.owner_headers,
        )

    def assert_observer_workspace(self, value: Any) -> None:
        fields = {"project_id", "id", "display_name", "source_host_id", "root", "root_identity", "source_kind", "created_at", "revision"}
        if not isinstance(value, dict) or set(value) != fields:
            raise IntegrationFailure("Observer workspace response does not match the public schema")
        if (value["project_id"], value["id"], value["source_host_id"], value["root"], value["root_identity"], value["source_kind"]) != \
                (PROJECT_ID, WORKSPACE_ID, SOURCE_HOST_ID, None, None, "observer"):
            raise IntegrationFailure("Observer workspace did not retain its rootless observer binding")

    def assert_stream_metadata(self, value: Any, *, accepted_sequence: int | None = None, revoked: bool | None = None) -> dict[str, Any]:
        if not isinstance(value, dict):
            raise IntegrationFailure("Observer stream metadata is not an object")
        forbidden = {"receipt", "payload", "submission", "payload_sha256"}
        if forbidden.intersection(value):
            raise IntegrationFailure("Observer stream metadata exposed a retained submission payload or receipt")
        required = {"stream_id", "project_id", "workspace_id", "principal_id", "observer_id", "source_host_id", "profile",
                    "confirmed_sequence", "retired_sequence", "retained_submissions",
                    "workspace_binding_digest", "accepted_sequence", "observed_sequence", "retained_bytes", "revoked",
                    "shared_publication_mutated", "verification_level", "server_source_verified"}
        if set(value) != required:
            raise IntegrationFailure("Observer stream metadata omitted required public fields")
        if (value["stream_id"], value["project_id"], value["workspace_id"], value["principal_id"], value["observer_id"],
                value["source_host_id"], value["profile"], value["workspace_binding_digest"]) != (
                    self.stream_id, PROJECT_ID, WORKSPACE_ID, "dev-reader", OBSERVER_ID, SOURCE_HOST_ID, PROFILE, self.binding_digest):
            raise IntegrationFailure("Observer stream metadata crossed its enrollment boundary")
        if type(value["accepted_sequence"]) is not int or type(value["observed_sequence"]) is not int or \
                type(value["retained_bytes"]) is not int or value["retained_bytes"] < 0 or type(value["revoked"]) is not bool or \
                value["shared_publication_mutated"] is not False or value["verification_level"] != "producer-observed" or \
                value["server_source_verified"] is not False:
            raise IntegrationFailure("Observer stream metadata has invalid mutable state")
        if accepted_sequence is not None and value["accepted_sequence"] != accepted_sequence:
            raise IntegrationFailure("Observer stream metadata has the wrong accepted sequence")
        if not 0 <= value["retired_sequence"] <= value["confirmed_sequence"] <= value["accepted_sequence"] or \
                value["retained_submissions"] != value["accepted_sequence"] - value["retired_sequence"]:
            raise IntegrationFailure("Observer stream metadata has inconsistent retention cursors")
        if revoked is not None and value["revoked"] is not revoked:
            raise IntegrationFailure("Observer stream metadata has the wrong revocation state")
        return value

    def assert_receipt(self, value: Any, submission: dict[str, Any]) -> dict[str, Any]:
        if not isinstance(value, dict) or set(value) != RECEIPT_FIELDS:
            raise IntegrationFailure("Observer receipt does not match shin.observer-ack.v1")
        if value["schema_version"] != "shin.observer-ack.v1" or \
                (value["stream_id"], value["sequence"], value["submission_id"], value["payload_sha256"], value["project_id"], value["workspace_id"]) != (
                    self.stream_id, submission["sequence"], submission["submission_id"], submission["payload_sha256"], PROJECT_ID, WORKSPACE_ID) or \
                self.require_hex(value["server_id"], 32, "Observer receipt has an invalid server id") != value["server_id"] or \
                not isinstance(value["received_at"], str) or not value["received_at"] or \
                value["verification_level"] != "producer-observed" or value["server_source_verified"] is not False or \
                value["shared_publication_mutated"] is not False:
            raise IntegrationFailure("Observer receipt does not preserve the immutable acknowledgement contract")
        return value

    def assert_observer_identity(self, value: Any, principal_id: str) -> str:
        if not isinstance(value, dict) or set(value) != OBSERVER_IDENTITY_FIELDS:
            raise IntegrationFailure("Observer identity response does not match the public schema")
        server_id = self.require_hex(value["server_id"], 32, "Observer identity has an invalid server id")
        if (value["schema_version"], value["principal_id"], value["submission_schema"], value["acknowledgement_schema"],
                value["verification_level"], value["source_verified"], value["shared_publication_mutated"]) != (
                    "shin.observer-server-identity.v3", principal_id, "shin.observer-submission.v1",
                    "shin.observer-ack.v1", "service-observed", False, False):
            raise IntegrationFailure("Observer identity response crossed its service-observed boundary")
        if value["confirmation_schema"] != "shin.observer-confirmation.v1" or value["reconciliation_schema"] != "shin.observer-reconcile-result.v1" or \
                value["request_identity_header"] != "X-Shin-Server-ID":
            raise IntegrationFailure("Observer identity omitted current history protocol")
        return server_id

    def active_routes_signature(self) -> str:
        return self.scalar(
            "SELECT COALESCE(string_agg(project_id || ':' || route_id || ':' || revision::text || ':' || "
            "COALESCE(snapshot_id, ''), ',' ORDER BY project_id, route_id), '') FROM shin_server.active_routes"
        )

    def observer_storage_signature(self) -> tuple[str, int, int, int, int, int, int]:
        return (
            self.scalar("SELECT server_id FROM shin_server.server_identity WHERE singleton"),
            self.count("observer_streams", "true"),
            self.count("observer_submissions", "true"),
            self.count("audit_events", "true"),
            int(self.scalar("SELECT streams FROM shin_server.observer_usage WHERE singleton")),
            int(self.scalar("SELECT submissions FROM shin_server.observer_usage WHERE singleton")),
            int(self.scalar("SELECT retained_bytes FROM shin_server.observer_usage WHERE singleton")),
        )

    def check_observer_identity(self) -> None:
        self.expect_error(401, "unauthorized", "GET", "/api/v1/observer-identity", headers={})
        self.expect_error(400, "invalid-query", "GET", "/api/v1/observer-identity?unexpected=1", headers=self.owner_headers)
        before = self.observer_storage_signature()
        admin_id = self.assert_observer_identity(
            self.expect_status(200, "GET", "/api/v1/observer-identity", headers=self.auth_headers("admin"))["result"],
            "dev-admin",
        )
        reader_id = self.assert_observer_identity(
            self.expect_status(200, "GET", "/api/v1/observer-identity", headers=self.owner_headers)["result"],
            "dev-reader",
        )
        if admin_id != reader_id or admin_id != before[0] or self.observer_storage_signature() != before:
            raise IntegrationFailure("Observer identity handshake did not remain read-only and server-stable")
        self.server_identity = reader_id

    def source_metadata(self) -> list[tuple[str, int, int, int]]:
        if self.fixture is None:
            raise IntegrationFailure("Observer fixture was not prepared")
        return self.fixture_module.tree_metadata(self.fixture.root)

    def check_workspace_and_enrollment(self) -> None:
        status, response = self.post_json(
            f"/api/v1/projects/{PROJECT_ID}/observer-workspaces",
            self.observer_workspace(),
            headers={**self.auth_headers("admin"), "Idempotency-Key": "observer-workspace-enroll"},
        )
        if status != 201 or response.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("first Observer workspace registration did not return 201")
        self.assert_observer_workspace(response["result"].get("workspace"))
        replay_status, replay = self.post_json(
            f"/api/v1/projects/{PROJECT_ID}/observer-workspaces",
            self.observer_workspace(),
            headers={**self.auth_headers("admin"), "Idempotency-Key": "observer-workspace-enroll"},
        )
        if replay_status != 200 or replay.get("result", {}).get("replayed") is not True:
            raise IntegrationFailure("Observer workspace idempotency replay was not preserved")
        self.assert_observer_workspace(replay["result"].get("workspace"))

        self.expect_status(
            403,
            "POST",
            "/api/v1/observer-streams",
            headers={**self.owner_headers, "Content-Type": "application/json"},
            body=self.compact_json(self.enrollment()),
        )
        status, response = self.post_json("/api/v1/observer-streams", self.enrollment(), headers=self.auth_headers("admin"))
        if status != 201 or response.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("first Observer stream enrollment did not return 201")
        self.assert_stream_metadata(response["result"].get("stream"), accepted_sequence=0, revoked=False)
        if self.count("observer_streams", f"stream_id = '{self.stream_id}'") != 1:
            raise IntegrationFailure("Observer enrollment did not write exactly one stream")
        replay_status, replay = self.post_json("/api/v1/observer-streams", self.enrollment(), headers=self.auth_headers("admin"))
        if replay_status != 200 or replay.get("result", {}).get("replayed") is not True:
            raise IntegrationFailure("Observer enrollment replay was not retained")
        changed = dict(self.enrollment(), profile="different-profile")
        conflict_status, _ = self.post_json("/api/v1/observer-streams", changed, headers=self.auth_headers("admin"))
        if conflict_status != 409:
            raise IntegrationFailure("conflicting Observer enrollment was not rejected")

    def check_stream_read_access_and_rootless_boundary(self) -> None:
        before_source = self.source_metadata()
        before_routes = self.active_routes_signature()
        admin = self.expect_status(200, "GET", f"/api/v1/observer-streams/{self.stream_id}", headers=self.auth_headers("admin"))
        self.assert_stream_metadata(admin.get("result"), accepted_sequence=0, revoked=False)
        owner = self.expect_status(200, "GET", f"/api/v1/observer-streams/{self.stream_id}", headers=self.owner_headers)
        self.assert_stream_metadata(owner.get("result"), accepted_sequence=0, revoked=False)
        absent = "f" * 32
        self.expect_error(404, "observer-not-found", "GET", f"/api/v1/observer-streams/{absent}", headers=self.auth_headers("admin"))
        if self.other_headers is None:
            raise IntegrationFailure("non-owner reader is unavailable")
        self.expect_error(404, "observer-not-found", "GET", f"/api/v1/observer-streams/{self.stream_id}", headers=self.other_headers)
        assert self.entry_one is not None
        self.expect_error(404, "observer-not-found", "POST", f"/api/v1/observer-streams/{self.stream_id}/submissions",
                          headers={**self.other_headers, "Content-Type": "application/json"}, body=self.compact_json(self.entry_one))
        public_audit = self.expect_status(200, "GET", "/api/v1/audit?limit=100", headers=self.other_headers)["result"]["items"]
        if any(event["observer_stream_id"] is not None for event in public_audit):
            raise IntegrationFailure("project audit exposed another owner's Observer activity")
        owner_audit = self.expect_status(200, "GET", "/api/v1/audit?limit=100", headers=self.owner_headers)["result"]["items"]
        if not any(event["observer_stream_id"] == self.stream_id for event in owner_audit):
            raise IntegrationFailure("stream owner could not inspect its enrollment audit")
        if self.source_metadata() != before_source:
            raise IntegrationFailure("Observer ingestion mutated the external source fixture")
        if self.active_routes_signature() != before_routes:
            raise IntegrationFailure("Observer metadata changed shared active routes")

    def check_submission_validation_and_first_receipt(self) -> None:
        assert self.entry_one is not None and self.entry_two is not None
        malformed = json.loads(json.dumps(self.entry_one))
        malformed["payload"]["observation"]["details_omitted"] += 1
        self.expect_error(
            400, "invalid-submission", "POST", f"/api/v1/observer-streams/{self.stream_id}/submissions",
            headers={**self.owner_headers, "Content-Type": "application/json"}, body=self.compact_json(malformed),
        )
        mismatched = json.loads(json.dumps(self.entry_one))
        mismatched["payload"]["observer_id"] = "other-observer"
        mismatched["payload_sha256"] = self.payload_digest(mismatched["payload"])
        self.expect_error(
            403, "observer-binding-mismatch", "POST", f"/api/v1/observer-streams/{self.stream_id}/submissions",
            headers={**self.owner_headers, "Content-Type": "application/json"}, body=self.compact_json(mismatched),
        )
        self.expect_error(
            409, "observer-sequence-gap", "POST", f"/api/v1/observer-streams/{self.stream_id}/submissions",
            headers={**self.owner_headers, "Content-Type": "application/json"}, body=self.compact_json(self.entry_two),
        )
        self.expect_status(
            403, "POST", f"/api/v1/observer-streams/{self.stream_id}/submissions",
            headers={**self.auth_headers("admin"), "Content-Type": "application/json"}, body=self.compact_json(self.entry_one),
        )
        status, response = self.post_submission(self.entry_one)
        if status != 201 or response.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("first native Observer submission did not return 201")
        receipt = self.assert_receipt(response["result"].get("receipt"), self.entry_one)
        replay_status, replay = self.post_submission(self.entry_one)
        if replay_status != 200 or replay.get("result", {}).get("replayed") is not True or \
                self.assert_receipt(replay["result"].get("receipt"), self.entry_one) != receipt:
            raise IntegrationFailure("Observer submission replay changed its immutable receipt")
        changed = json.loads(json.dumps(self.entry_one))
        changed["payload"]["observation"]["duration_ms"] += 1.0
        changed["payload_sha256"] = self.payload_digest(changed["payload"])
        self.expect_error(
            409, "submission-conflict", "POST", f"/api/v1/observer-streams/{self.stream_id}/submissions",
            headers={**self.owner_headers, "Content-Type": "application/json"}, body=self.compact_json(changed),
        )
        self.assert_stream_metadata(
            self.expect_status(200, "GET", f"/api/v1/observer-streams/{self.stream_id}", headers=self.owner_headers).get("result"),
            accepted_sequence=1,
            revoked=False,
        )

    def check_parallel_duplicate_and_restart_replay(self) -> None:
        assert self.entry_two is not None
        barrier = threading.Barrier(3)
        outcomes: list[tuple[int, dict[str, Any]]] = []
        failures: list[BaseException] = []

        def submit() -> None:
            try:
                barrier.wait(timeout=5.0)
                outcomes.append(self.post_submission(self.entry_two))
            except BaseException as error:
                failures.append(error)

        workers = [threading.Thread(target=submit), threading.Thread(target=submit)]
        for worker in workers:
            worker.start()
        barrier.wait(timeout=5.0)
        for worker in workers:
            worker.join(timeout=10.0)
        if any(worker.is_alive() for worker in workers) or failures or sorted(status for status, _ in outcomes) != [200, 201]:
            raise IntegrationFailure("parallel native Observer duplicates were not idempotent")
        receipts = [self.assert_receipt(response.get("result", {}).get("receipt"), self.entry_two) for _, response in outcomes]
        if receipts[0] != receipts[1] or any(response.get("result", {}).get("replayed") is not (status == 200)
                                      for status, response in outcomes):
            raise IntegrationFailure("parallel Observer duplicates changed their receipt or replay marker")
        self.stop_server(expect_clean=True)
        self.start_server()
        identity = self.assert_observer_identity(
            self.expect_status(200, "GET", "/api/v1/observer-identity", headers=self.owner_headers)["result"],
            "dev-reader",
        )
        if identity != self.server_identity:
            raise IntegrationFailure("Observer identity changed after Server restart")
        status, replay = self.post_submission(self.entry_two)
        if status != 200 or replay.get("result", {}).get("replayed") is not True or \
                self.assert_receipt(replay["result"].get("receipt"), self.entry_two) != receipts[0]:
            raise IntegrationFailure("restart or ambiguous reply did not preserve Observer submission replay")
        metadata = self.assert_stream_metadata(
            self.expect_status(200, "GET", f"/api/v1/observer-streams/{self.stream_id}", headers=self.owner_headers).get("result"),
            accepted_sequence=2,
            revoked=False,
        )
        if metadata["observed_sequence"] != 2:
            raise IntegrationFailure("Observer stream did not retain its observed sequence")
        if self.count("observer_submissions", f"stream_id = '{self.stream_id}'") != 2 or \
                self.count("observer_streams", f"stream_id = '{self.stream_id}'") != 1 or \
                self.scalar("SELECT submissions::text FROM shin_server.observer_usage WHERE singleton") != "2":
            raise IntegrationFailure("Observer ingestion did not retain exact stream, submission, and usage writes")

    def check_audit_atomic_rollback(self) -> None:
        if self.fixture_root is None:
            raise IntegrationFailure("Observer fixture root is unavailable")
        rollback_fixture = self.fixture_module.Fixture(self.fixture_root, "rollback")
        config = rollback_fixture.config_data()
        config.update({
            "project_id": PROJECT_ID,
            "workspace_id": WORKSPACE_ID,
            "observer_id": "observer-rollback-agent",
            "source_host_id": SOURCE_HOST_ID,
        })
        config["outbox"] = {"enabled": True, "max_entries": 4, "max_bytes": 128 * 1024}
        rollback_fixture.write_config(config)
        original_fixture = self.fixture
        self.fixture = rollback_fixture
        try:
            state = self.sample_observer()
        finally:
            self.fixture = original_fixture
        entry = self.native_submission(state["outbox"]["entries"][0])
        rollback_stream = entry["stream_id"]
        enrollment = {
            "stream_id": rollback_stream, "project_id": PROJECT_ID, "workspace_id": WORKSPACE_ID,
            "principal_id": "dev-reader", "observer_id": "observer-rollback-agent", "source_host_id": SOURCE_HOST_ID,
            "profile": PROFILE, "workspace_binding_digest": state["binding_key"],
        }
        status, _ = self.post_json("/api/v1/observer-streams", enrollment, headers=self.auth_headers("admin"))
        if status != 201:
            raise IntegrationFailure("rollback Observer stream enrollment failed")
        before = {
            "audit": self.count("audit_events", "true"),
            "submissions": self.count("observer_submissions", f"stream_id = '{rollback_stream}'"),
            "head": self.scalar(f"SELECT accepted_sequence::text || ':' || observed_sequence::text || ':' || retained_bytes::text FROM shin_server.observer_streams WHERE stream_id = '{rollback_stream}'"),
            "usage": self.scalar("SELECT submissions::text || ':' || retained_bytes::text FROM shin_server.observer_usage WHERE singleton"),
        }
        self.psql("REVOKE INSERT ON TABLE shin_server.audit_events FROM shin_runtime")
        try:
            status, _ = self.post_json(
                f"/api/v1/observer-streams/{rollback_stream}/submissions", entry,
                headers={**self.owner_headers, "Content-Type": "application/json"},
            )
            if status < 500:
                raise IntegrationFailure("Observer audit grant failure was not surfaced as a server failure")
            after = {
                "audit": self.count("audit_events", "true"),
                "submissions": self.count("observer_submissions", f"stream_id = '{rollback_stream}'"),
                "head": self.scalar(f"SELECT accepted_sequence::text || ':' || observed_sequence::text || ':' || retained_bytes::text FROM shin_server.observer_streams WHERE stream_id = '{rollback_stream}'"),
                "usage": self.scalar("SELECT submissions::text || ':' || retained_bytes::text FROM shin_server.observer_usage WHERE singleton"),
            }
            if after != before:
                raise IntegrationFailure("Observer audit failure did not roll back usage, submission, and stream head atomically")
        finally:
            self.psql("GRANT INSERT ON TABLE shin_server.audit_events TO shin_runtime")
        retained = self.scalar("SELECT COALESCE(sum(retained_bytes), 0)::text FROM shin_server.observer_submissions")
        usage = self.scalar(
            "SELECT streams::text || ':' || submissions::text || ':' || retained_bytes::text "
            "FROM shin_server.observer_usage WHERE singleton"
        )
        if self.count("observer_streams", "true") != 2 or self.count("observer_submissions", "true") != 2 or \
                usage != f"2:2:{retained}":
            raise IntegrationFailure("Observer stream, submission, and usage rows diverged after controlled rollback")

    def check_revocation(self) -> None:
        body: dict[str, Any] = {}
        status, response = self.post_json(
            f"/api/v1/observer-streams/{self.stream_id}/revoke", body, headers=self.auth_headers("admin")
        )
        if status != 200 or response.get("result", {}).get("replayed") is not False:
            raise IntegrationFailure("Observer stream revocation did not return its first receipt")
        self.assert_stream_metadata(response["result"].get("stream"), accepted_sequence=2, revoked=True)
        replay_status, replay = self.post_json(
            f"/api/v1/observer-streams/{self.stream_id}/revoke", body, headers=self.auth_headers("admin")
        )
        if replay_status != 200 or replay.get("result", {}).get("replayed") is not True:
            raise IntegrationFailure("Observer stream revocation replay was not retained")
        self.expect_status(
            403, "POST", f"/api/v1/observer-streams/{self.stream_id}/revoke",
            headers={**self.owner_headers, "Content-Type": "application/json"}, body=self.compact_json(body),
        )
        assert self.entry_two is not None
        next_entry = json.loads(json.dumps(self.entry_two))
        next_entry["sequence"] = 3
        next_entry["submission_id"] = hashlib.sha256(f"{self.stream_id}:3".encode()).hexdigest()
        next_entry["payload"]["observer_sequence"] = 3
        next_entry["payload_sha256"] = self.payload_digest(next_entry["payload"])
        for submission in (self.entry_two, next_entry):
            self.expect_error(
                403, "observer-revoked", "POST", f"/api/v1/observer-streams/{self.stream_id}/submissions",
                headers={**self.owner_headers, "Content-Type": "application/json"}, body=self.compact_json(submission),
            )
        self.assert_stream_metadata(
            self.expect_status(200, "GET", f"/api/v1/observer-streams/{self.stream_id}", headers=self.auth_headers("admin")).get("result"),
            accepted_sequence=2,
            revoked=True,
        )
        if self.source_before_ingestion is None or self.active_routes_before_ingestion is None:
            raise IntegrationFailure("Observer sidecar baseline is unavailable")
        if self.source_metadata() != self.source_before_ingestion:
            raise IntegrationFailure("Observer sidecar mutated the external fixture source")
        if self.active_routes_signature() != self.active_routes_before_ingestion:
            raise IntegrationFailure("Observer sidecar changed shared active routes")

    def write_report(self, *, ok: bool, failure: str | None = None) -> None:
        super().write_report(ok=ok, failure=failure)
        report = json.loads(self.report_path.read_text(encoding="utf-8"))
        report["observer_ingestion"] = {"checks": self.observer_checks, "timing_note": "Local correctness timings, not a performance claim."}
        report["observer_load"] = self.load_result
        report["native_delivery"] = self.native_delivery_result
        report["personal_observer_measurement"] = getattr(self, "personal_observer_measurement", None)
        self.replace_private_json(self.report_path, report)

    def check_local_workspace_does_not_promote_producer_evidence(self) -> None:
        assert self.fixture_root is not None
        workspace = self.expect_status(200, "GET", "/api/v1/projects/visible/workspaces/primary",
                                       headers=self.owner_headers)["result"]
        if workspace["source_kind"] != "local" or not workspace["root"]:
            raise IntegrationFailure("local workspace fixture was not preserved")
        fixture = self.fixture_module.Fixture(self.fixture_root, "local-workspace-observer")
        config = fixture.config_data()
        config.update(project_id=PROJECT_ID, workspace_id="primary", source_host_id=workspace["source_host_id"],
                      observer_id="observer-local-agent", root=workspace["root"])
        config["outbox"]["enabled"] = True
        fixture.write_config(config)
        original_fixture = self.fixture
        self.fixture = fixture
        try:
            state = self.sample_observer()
        finally:
            self.fixture = original_fixture
        entry = self.native_submission(state["outbox"]["entries"][0])
        binding = {key: entry["payload"][key] for key in ("project_id", "workspace_id", "observer_id", "source_host_id", "profile", "workspace_binding_digest")}
        binding.update(stream_id=entry["stream_id"], principal_id="dev-reader")
        if self.post_json("/api/v1/observer-streams", binding, headers=self.auth_headers("admin"))[0] != 201:
            raise IntegrationFailure("explicit local reporter enrollment was rejected")
        status, response = self.post_json(f'/api/v1/observer-streams/{entry["stream_id"]}/submissions', entry,
                                          headers=self.owner_headers)
        receipt = response.get("result", {}).get("receipt", {})
        if status != 201 or receipt.get("server_source_verified") is not False or receipt.get("verification_level") != "producer-observed" or \
                receipt.get("shared_publication_mutated") is not False:
            raise IntegrationFailure("local source availability promoted producer-reported metadata")
        after = self.expect_status(200, "GET", "/api/v1/projects/visible/workspaces/primary", headers=self.owner_headers)["result"]
        if after != workspace or self.active_routes_signature() != self.active_routes_before_ingestion:
            raise IntegrationFailure("Observer submission modified local workspace or publication authority")

    def check_bounded_load(self) -> None:
        assert self.entry_one is not None
        def lane(number: int) -> list[float]:
            stream = secrets.token_hex(16)
            observer_id = f"observer-load-{number}"
            enrollment = dict(self.enrollment(), stream_id=stream, observer_id=observer_id)
            status, _ = self.post_json("/api/v1/observer-streams", enrollment, headers=self.auth_headers("admin"))
            if status != 201:
                raise IntegrationFailure("load stream enrollment failed")
            timings: list[float] = []
            for sequence in range(1, self.load_requests // 4 + 1):
                entry = json.loads(json.dumps(self.entry_one))
                entry.update(stream_id=stream, sequence=sequence,
                             submission_id=hashlib.sha256(f"{stream}:{sequence}".encode()).hexdigest())
                entry["payload"].update(observer_id=observer_id, observer_sequence=sequence)
                entry["payload"]["observation"]["worktree_id"] = f"fnv1a64:{sequence:016x}"
                entry["payload_sha256"] = self.payload_digest(entry["payload"])
                start = time.monotonic()
                status, result = self.post_json(f"/api/v1/observer-streams/{stream}/submissions", entry, headers=self.owner_headers)
                timings.append((time.monotonic() - start) * 1000)
                if status != 201 or result["result"]["replayed"] or result["result"]["receipt"]["sequence"] != sequence:
                    raise IntegrationFailure("load submission was not committed exactly once")
            if self.load_requests == 4096:
                replay_status, replay = self.post_json(f"/api/v1/observer-streams/{stream}/submissions", entry, headers=self.owner_headers)
                if replay_status != 200 or replay["result"]["replayed"] is not True:
                    raise IntegrationFailure("capacity rejected an already committed replay")
                full = json.loads(json.dumps(entry))
                full["sequence"] = 1025
                full["submission_id"] = hashlib.sha256(f"{stream}:1025".encode()).hexdigest()
                full["payload"]["observer_sequence"] = 1025
                full["payload_sha256"] = self.payload_digest(full["payload"])
                status, denied = self.post_json(f"/api/v1/observer-streams/{stream}/submissions", full, headers=self.owner_headers)
                if status != 409 or denied.get("error", {}).get("code") != "observer-capacity":
                    raise IntegrationFailure("per-stream receipt cap was not enforced")
            return timings
        before = int(self.scalar("SELECT submissions FROM shin_server.observer_usage WHERE singleton"))
        start = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            timings = [sample for group in workers.map(lane, range(4)) for sample in group]
        elapsed = time.monotonic() - start
        if int(self.scalar("SELECT submissions FROM shin_server.observer_usage WHERE singleton")) - before != self.load_requests:
            raise IntegrationFailure("load quota accounting lost submissions")
        ordered = sorted(timings)
        self.load_result = {"requests": len(timings), "clients": 4, "elapsed_ms": round(elapsed * 1000, 3),
                            "requests_per_second": round(len(timings) / elapsed, 2),
                            "p50_ms": round(ordered[len(ordered) // 2], 3),
                            "p95_ms": round(ordered[(len(ordered) * 95 + 99) // 100 - 1], 3),
                            "scope": "Synthetic producer metadata; includes HTTP, PostgreSQL commit, quota and audit. Not a source execution or production SLO."}

    def check_native_delivery(self) -> None:
        assert self.fixture_root is not None
        fixture = self.fixture_module.Fixture(self.fixture_root, "native-delivery")
        config = fixture.config_data()
        config.update(project_id=PROJECT_ID, workspace_id="native-delivery", observer_id="native-delivery")
        config["outbox"] = {"enabled": True, "max_entries": 128, "max_bytes": 196608}
        fixture.write_config(config)
        original = self.fixture
        self.fixture = fixture
        try:
            state = self.sample_observer()
            entry = state["outbox"]["entries"][0]
            stream = entry["stream_id"]
            self.post_json(f"/api/v1/projects/{PROJECT_ID}/observer-workspaces",
                           {"id": "native-delivery", "display_name": "Native delivery", "source_host_id": SOURCE_HOST_ID},
                           headers={**self.auth_headers("admin"), "Idempotency-Key": "native-delivery-workspace"})
            binding = {key: entry["payload"][key] for key in ("project_id", "workspace_id", "observer_id", "source_host_id", "profile", "workspace_binding_digest")}
            binding.update(stream_id=stream, principal_id="dev-reader")
            if self.post_json("/api/v1/observer-streams", binding, headers=self.auth_headers("admin"))[0] != 201:
                raise IntegrationFailure("native delivery enrollment failed")
            delivery_config = fixture.base / "delivery.json"
            self.fixture_module.write_private_json(delivery_config, {
                "schema_version": "shin.observer-delivery-config.v1", "host": "127.0.0.1", "port": PORT,
                "server_id": self.server_identity, "principal_id": "dev-reader", "request_timeout_ms": 1000,
                "token_file": str(self.state_root / "secrets" / "dev_reader.token"),
            })

            def deliver(command: str = "deliver", *, expect_success: bool = True) -> dict[str, Any]:
                child = subprocess.run([str(self.observer), command, "--config", str(fixture.config),
                                        "--delivery-config", str(delivery_config)], stdin=subprocess.DEVNULL,
                                       capture_output=True, text=True, timeout=60, env=self.observer_environment, check=False)
                if child.returncode != (0 if expect_success else 1) or (child.stderr if expect_success else child.stdout):
                    raise IntegrationFailure("native delivery did not return a clean envelope")
                value = json.loads(child.stdout if expect_success else child.stderr)
                return self.fixture_module.assert_observer_envelope(value, expected_ok=expect_success)

            state_bytes = (fixture.state / "state.json").read_bytes()
            unbound = deliver("reconcile", expect_success=False)
            if unbound["code"] != "delivery-binding-required" or (fixture.state / "state.json").read_bytes() != state_bytes:
                raise IntegrationFailure("unbound native reconciliation did not preserve its local state")
            self.stop_server(expect_clean=True)
            bound = deliver()
            if not bound["delivery_failed"] or bound["state"]["outbox"]["entries"] != [entry] or \
                    bound["state"]["outbox"]["destination"] is None:
                raise IntegrationFailure("offline explicit delivery did not persist the destination and packet")
            self.start_server()
            state_bytes = (fixture.state / "state.json").read_bytes()
            inspection = deliver("reconcile")
            if inspection["state"] != "send-next" or inspection["observer_state_mutated"] or \
                    inspection["source_verified"] or (fixture.state / "state.json").read_bytes() != state_bytes:
                raise IntegrationFailure("native reconciliation was not an unchanged read-only send-next probe")
            # A committed head with no local receipt models recovery after an uncertain response.
            status, committed = self.post_json(f"/api/v1/observer-streams/{stream}/submissions", entry, headers=self.owner_headers)
            if status != 201:
                raise IntegrationFailure("uncertain-response setup did not commit")
            inspection = deliver("reconcile")
            if inspection["state"] != "replay-head" or not inspection["resume_allowed"] or \
                    (fixture.state / "state.json").read_bytes() != state_bytes:
                raise IntegrationFailure("native reconciliation consumed the replayable local head")
            replay = deliver()
            if replay["state"]["outbox"]["last_receipt"] != committed["result"]["receipt"] or not replay["pending_drained"]:
                raise IntegrationFailure("native recovery did not apply the original committed receipt")
            self.stop_server(expect_clean=True)
            fixture.write_source("Local observation remains useful while Server is stopped.\n")
            offline = self.sample_observer()
            pending = offline["outbox"]["entries"]
            failed = deliver()
            if not failed["delivery_failed"] or failed["state"]["outbox"]["entries"] != pending:
                raise IntegrationFailure("offline native delivery lost its retained packet")
            self.start_server()
            recovered = deliver()
            if recovered["delivery_failed"] or not recovered["pending_drained"] or recovered["state"]["outbox"]["acknowledged_sequence"] != 2:
                raise IntegrationFailure("native delivery did not recover after Server restart")
            state_bytes = (fixture.state / "state.json").read_bytes()
            inspection = deliver("reconcile")
            if inspection["state"] != "caught-up" or not inspection["confirmation_required"] or \
                    (fixture.state / "state.json").read_bytes() != state_bytes:
                raise IntegrationFailure("native caught-up probe changed local state or hid pending durability confirmation")
            for index in range(128):
                fixture.write_source(f"Native bounded delivery source revision {index}.\n")
                state = self.sample_observer()
            pending_count = len(state["outbox"]["entries"])
            if pending_count != 128:
                raise IntegrationFailure("native load did not retain its requested bounded batch")
            source_before = self.source_metadata()
            start = time.monotonic()
            result = deliver()
            elapsed = time.monotonic() - start
            if result["delivery_failed"] or not result["pending_drained"] or result["attempts_completed"] != pending_count or \
                    result["state"]["outbox"]["acknowledged_sequence"] != 130:
                raise IntegrationFailure("native bounded batch did not drain exactly")
            if self.count("observer_submissions", f"stream_id='{stream}'") != 130 or \
                    self.count("audit_events", f"observer_stream_id='{stream}' AND action='observer.submission.accepted'") != 130:
                raise IntegrationFailure("native replay duplicated acceptance or audit")
            if self.source_metadata() != source_before or self.active_routes_signature() != self.active_routes_before_ingestion:
                raise IntegrationFailure("native delivery mutated source or publication")
            self.native_delivery_result = {"requests": pending_count, "elapsed_ms": round(elapsed * 1000, 3),
                "requests_per_second": round(pending_count / elapsed, 2),
                "scope": "Single native producer, identity handshake per submission, PostgreSQL commit and atomic local state commits. Includes process startup; not a production SLO."}
        finally:
            self.fixture = original

    def run(self) -> None:
        super().run()
        try:
            self.owner_headers = self.auth_headers("reader")
            self.add_non_owner_reader()
            self.prepare_observer_fixture()
            self.source_before_ingestion = self.source_metadata()
            self.psql("INSERT INTO shin_server.active_routes(project_id,route_id,revision) VALUES ('visible','observer-boundary',7)")
            self.active_routes_before_ingestion = self.active_routes_signature()
            self.start_server()
            self.record_observer("observer-server-identity-handshake", self.check_observer_identity)
            self.record_observer("observer-workspace-and-enrollment", self.check_workspace_and_enrollment)
            self.record_observer("observer-stream-read-access-and-rootless-boundary", self.check_stream_read_access_and_rootless_boundary)
            self.record_observer("observer-submission-validation-and-first-receipt", self.check_submission_validation_and_first_receipt)
            self.record_observer("observer-parallel-duplicate-and-restart-replay", self.check_parallel_duplicate_and_restart_replay)
            self.record_observer("observer-audit-usage-submission-head-atomic-rollback", self.check_audit_atomic_rollback)
            if self.load_requests:
                self.record_observer("observer-bounded-four-client-load", self.check_bounded_load)
            self.record_observer("observer-revocation", self.check_revocation)
            self.record_observer("observer-local-workspace-authority-boundary", self.check_local_workspace_does_not_promote_producer_evidence)
            self.record_observer("observer-native-delivery-offline-replay-and-bounded-batch", self.check_native_delivery)
            from personal_observer_checks import check_personal_observer
            self.record_observer("personal-observer-http-owner-lifecycle-isolation", lambda: check_personal_observer(self))
        finally:
            self.stop_server(expect_clean=False)
            self.restore_runtime_config()
            self.cleanup_observer_fixture()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="native shin-server executable")
    parser.add_argument("--observer", required=True, help="absolute newly built shin-observer executable")
    parser.add_argument("--state-root", default=str(BUILD_ROOT / "server-dev"), help="isolated state below this worktree build directory")
    parser.add_argument("--report", help="new report path below this worktree build directory")
    parser.add_argument("--load-requests", type=int, default=0, help="optional bounded multiple of four, up to 4096")
    return parser.parse_args(argv)


def require_executable(value: str, label: str, *, absolute: bool) -> Path:
    candidate = Path(value)
    if absolute and not candidate.is_absolute():
        raise IntegrationFailure(f"--{label} must be an absolute executable path")
    resolved = candidate.resolve(strict=False)
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise IntegrationFailure(f"--{label} must name an executable native binary")
    return resolved


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        binary = require_executable(args.binary, "binary", absolute=False)
        observer = require_executable(args.observer, "observer", absolute=True)
        state_root = Path(args.state_root).resolve(strict=False)
        report_path = Path(args.report).resolve(strict=False) if args.report else state_root / "reports" / f"observer-ingestion-{secrets.token_hex(8)}.json"
        if not under_build(state_root) or not under_build(report_path) or report_path.exists():
            raise IntegrationFailure("state root and report must be new paths below this worktree build directory")
    except IntegrationFailure as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    suite = ObserverIngestionIntegrationSuite(binary, observer, state_root, report_path)
    if args.load_requests < 0 or args.load_requests > 4096 or args.load_requests % 4 != 0:
        print("error: --load-requests must be a multiple of four in 0..4096", file=sys.stderr)
        return 2
    suite.load_requests = args.load_requests
    try:
        suite.run()
    except Exception as error:
        suite.stop_server(expect_clean=False)
        if isinstance(error, IntegrationFailure):
            suite.failure_assertion = str(error)
        try:
            suite.write_report(ok=False, failure=type(error).__name__)
        except OSError:
            pass
        print(f"error: observer ingestion integration test failed: {type(error).__name__}", file=sys.stderr)
        return 1
    suite.write_report(ok=True)
    print("observer ingestion integration test passed; compact report written below the isolated build state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
