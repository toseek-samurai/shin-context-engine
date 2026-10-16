#!/usr/bin/env python3
"""Real PostgreSQL integration coverage for the isolated Shin Server development slice."""

from __future__ import annotations

import argparse
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import signal
import stat
import subprocess
import sys
import threading
import time
from typing import Any, Callable
from stand_guard import StandGuard, require_http_port_available


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SERVER_DIR = REPOSITORY_ROOT / "server"
SERVER_VERSION = (SERVER_DIR / "VERSION").read_text(encoding="utf-8").strip()
DEV_TOOL = SERVER_DIR / "tools" / "dev.py"
COMPOSE_FILE = SERVER_DIR / "infra" / "compose.yml"
BUILD_ROOT = REPOSITORY_ROOT / "build"
PROJECT_NAME = "shin-server-dev"
DATABASE_NAME = "shin_server_dev"
SCHEMA_NAME = "shin_server"
HOST = "127.0.0.1"
PORT = 19125


class IntegrationFailure(RuntimeError):
    pass


class IntegrationSuite:
    def __init__(self, binary: Path, state_root: Path, report_path: Path) -> None:
        self._stand_guard = StandGuard()
        self.binary = binary
        self.expected_server_version = SERVER_VERSION
        self.state_root = state_root
        self.report_path = report_path
        self.started_at = time.monotonic()
        self.checks: list[str] = []
        self.active_check: str | None = None
        self.failure_http_status: int | None = None
        self.failure_error_code: str | None = None
        self.failure_assertion: str | None = None
        self.server: subprocess.Popen[str] | None = None
        self.server_log: Path | None = None
        self.admin_token = ""
        self.reader_token = ""
        self.server_id = ""

    def record(self, name: str, operation: Callable[[], None]) -> None:
        self.active_check = name
        try:
            operation()
        except BaseException:
            raise
        else:
            self.checks.append(name)
            self.active_check = None

    def run_process(
        self,
        command: list[str],
        name: str,
        *,
        timeout: float = 30.0,
        environment: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        completed = subprocess.run(
            command,
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            env=environment,
        )
        if completed.returncode:
            raise IntegrationFailure(f"{name} failed with exit status {completed.returncode}")
        return completed

    def dev_command(self, *args: str) -> None:
        self.run_process(
            [sys.executable, str(DEV_TOOL), "--state-root", str(self.state_root), *args],
            f"developer helper {' '.join(args)}",
        )

    def postgres_port(self) -> int:
        result = self.run_process([sys.executable, str(DEV_TOOL), "--state-root", str(self.state_root), "port"],
                                  "developer PostgreSQL port")
        return int(result.stdout.strip())

    def compose_prefix(self) -> list[str]:
        docker = "docker"
        return [
            docker,
            "compose",
            "--project-name",
            PROJECT_NAME,
            "--project-directory",
            str(REPOSITORY_ROOT),
            "--env-file",
            str(self.state_root / "compose.env"),
            "--file",
            str(COMPOSE_FILE),
        ]

    def compose(self, *args: str, timeout: float = 30.0) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment["SHIN_SERVER_INFRA_DIR"] = str((SERVER_DIR / "infra").resolve())
        return self.run_process(
            [*self.compose_prefix(), *args],
            f"Docker Compose {' '.join(args)}",
            timeout=timeout,
            environment=environment,
        )

    def psql(self, sql: str) -> str:
        completed = self.compose(
            "exec",
            "-T",
            "postgres",
            "psql",
            "-X",
            "-v",
            "ON_ERROR_STOP=1",
            "-U",
            "shin_migrator",
            "-d",
            DATABASE_NAME,
            "-Atq",
            "-c",
            sql,
        )
        return completed.stdout.strip()

    def scalar(self, sql: str) -> str:
        value = self.psql(sql)
        if "\n" in value:
            raise IntegrationFailure("SQL assertion returned more than one row")
        return value

    def wait_for_postgres(self) -> None:
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline:
            try:
                if self.scalar("SELECT 1") == "1":
                    return
            except IntegrationFailure:
                pass
            time.sleep(0.2)
        raise IntegrationFailure("isolated PostgreSQL service did not become available")

    def wait_for_audit_sequence_advance(self, previous: int) -> None:
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            try:
                if int(self.scalar("SELECT last_value FROM shin_server.audit_events_id_seq")) > previous:
                    return
            except IntegrationFailure:
                pass
            time.sleep(0.02)
        raise IntegrationFailure("first controlled audit writer did not allocate an event id")

    def reset_schema(self) -> None:
        database = self.scalar("SELECT current_database()")
        if database != DATABASE_NAME:
            raise IntegrationFailure("refusing to reset a database other than shin_server_dev")
        self.psql("DROP SCHEMA IF EXISTS shin_server CASCADE")

    def require_http_port_free(self) -> None:
        try:
            require_http_port_available(HOST, PORT)
        except OSError as error:
            raise IntegrationFailure("HTTP port 19125 is occupied; refusing to reset the test schema") from error

    def migrate(self) -> None:
        self.run_process(
            [str(self.binary), "migrate", "--config", str(self.state_root / "config" / "migrator.json")],
            "explicit migration",
            timeout=45.0,
        )

    def read_private_token(self, name: str) -> str:
        path = self.state_root / "secrets" / name
        entry = path.lstat()
        if stat.S_ISLNK(entry.st_mode) or not stat.S_ISREG(entry.st_mode) or stat.S_IMODE(entry.st_mode) != 0o600:
            raise IntegrationFailure("development credential file is unsafe")
        value = path.read_text(encoding="utf-8").strip()
        if not value:
            raise IntegrationFailure("development credential file is empty")
        return value

    def read_runtime_server_id(self) -> str:
        path = self.state_root / "config" / "runtime.json"
        entry = path.lstat()
        if stat.S_ISLNK(entry.st_mode) or not stat.S_ISREG(entry.st_mode) or stat.S_IMODE(entry.st_mode) != 0o600:
            raise IntegrationFailure("runtime configuration is unsafe")
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor, "rb") as input_file:
            opened = os.fstat(input_file.fileno())
            if (opened.st_dev, opened.st_ino, opened.st_uid, stat.S_IMODE(opened.st_mode)) != (
                    entry.st_dev, entry.st_ino, entry.st_uid, stat.S_IMODE(entry.st_mode)):
                raise IntegrationFailure("runtime configuration changed before the safe read")
            raw = input_file.read(65537)
            after = os.fstat(input_file.fileno())
            if (after.st_size, after.st_mtime_ns, after.st_ctime_ns) != (entry.st_size, entry.st_mtime_ns, entry.st_ctime_ns):
                raise IntegrationFailure("runtime configuration changed during the safe read")
        if len(raw) > 65536:
            raise IntegrationFailure("runtime configuration exceeds its bounded size")
        try:
            value = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise IntegrationFailure("runtime configuration is not valid JSON") from error
        database = value.get("database") if isinstance(value, dict) else None
        server_id = database.get("server_id") if isinstance(database, dict) else None
        if not isinstance(server_id, str) or re.fullmatch(r"[0-9a-f]{32}", server_id) is None:
            raise IntegrationFailure("runtime configuration does not contain a valid Server identity pin")
        return server_id

    def start_server(self, *, creation_umask: int = 0) -> None:
        if self.server is not None:
            raise IntegrationFailure("server is already running")
        self.server_id = self.read_runtime_server_id()
        log_dir = self.state_root / "reports"
        entry = log_dir.lstat()
        if stat.S_ISLNK(entry.st_mode) or not stat.S_ISDIR(entry.st_mode) or stat.S_IMODE(entry.st_mode) != 0o700:
            raise IntegrationFailure("report directory is unsafe")
        self.server_log = log_dir / f"server-{secrets.token_hex(8)}.log"
        descriptor = os.open(self.server_log, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as log:
            self.server = subprocess.Popen(
                [str(self.binary), "serve", "--config", str(self.state_root / "config" / "runtime.json")],
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,
                # The standalone Server owns private creation modes, not the invoking shell.
                umask=creation_umask,
            )
        self.wait_for_response("/health/live", 200)

    def stop_server(self, *, expect_clean: bool) -> None:
        if self.server is None:
            return
        process = self.server
        self.server = None
        process.terminate()
        try:
            result = process.wait(timeout=10.0)
        except subprocess.TimeoutExpired as error:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5.0)
            raise IntegrationFailure("server did not terminate after SIGTERM") from error
        if expect_clean and result != 0:
            raise IntegrationFailure(f"server did not exit cleanly after SIGTERM: {result}")

    def auth_headers(self, role: str) -> dict[str, str]:
        token = self.admin_token if role == "admin" else self.reader_token
        headers = {"Authorization": f"Bearer {token}"}
        if self.server_id:
            headers["X-Shin-Server-ID"] = self.server_id
        return headers

    def request(
        self,
        method: str,
        target: str,
        *,
        headers: dict[str, str] | None = None,
        body: bytes | None = None,
        include_server_identity: bool = True,
    ) -> tuple[int, dict[str, Any]]:
        request_headers = {"Host": f"{HOST}:{PORT}"}
        request_headers.update(headers or {})
        if include_server_identity and self.server_id and not any(
                key.lower() == "x-shin-server-id" for key in request_headers):
            request_headers["X-Shin-Server-ID"] = self.server_id
        connection = http.client.HTTPConnection(HOST, PORT, timeout=4.0)
        try:
            connection.request(method, target, body=body, headers=request_headers)
            response = connection.getresponse()
            status = response.status
            payload = response.read()
        except OSError as error:
            raise IntegrationFailure("server did not provide an HTTP response") from error
        finally:
            connection.close()
        try:
            decoded = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise IntegrationFailure("server response is not a JSON envelope") from error
        if not isinstance(decoded, dict):
            raise IntegrationFailure("server response envelope is not an object")
        self.assert_envelope(decoded)
        return status, decoded

    def raw_status(self, method: str, target: str, *, headers: dict[str, str], include_server_identity: bool = True) -> int:
        request_headers = dict(headers)
        if include_server_identity and self.server_id and not any(
                key.lower() == "x-shin-server-id" for key in request_headers):
            request_headers["X-Shin-Server-ID"] = self.server_id
        connection = http.client.HTTPConnection(HOST, PORT, timeout=4.0)
        try:
            connection.request(method, target, headers=request_headers)
            response = connection.getresponse()
            response.read()
            return response.status
        except OSError as error:
            raise IntegrationFailure("server did not provide an HTTP response") from error
        finally:
            connection.close()

    def assert_envelope(self, envelope: dict[str, Any]) -> None:
        expected = {
            "schema_version": "shin.server-response.v1",
            "tool": "shin-server",
            "version": self.expected_server_version,
            "stability": "development",
        }
        for key, value in expected.items():
            if envelope.get(key) != value:
                raise IntegrationFailure(f"response envelope has an invalid {key}")
        if not isinstance(envelope.get("ok"), bool):
            raise IntegrationFailure("response envelope omits boolean ok")
        if envelope["ok"]:
            if "result" not in envelope or "error" in envelope:
                raise IntegrationFailure("successful response envelope is malformed")
        else:
            error = envelope.get("error")
            if "result" in envelope or not isinstance(error, dict):
                raise IntegrationFailure("failed response envelope is malformed")
            if not isinstance(error.get("code"), str) or not isinstance(error.get("message"), str):
                raise IntegrationFailure("failed response error is malformed")

    def expect_status(
        self,
        expected: int,
        method: str,
        target: str,
        *,
        headers: dict[str, str] | None = None,
        body: bytes | None = None,
    ) -> dict[str, Any]:
        status, response = self.request(method, target, headers=headers, body=body)
        if status != expected:
            self.failure_http_status = status
            error = response.get("error")
            if isinstance(error, dict) and isinstance(error.get("code"), str):
                self.failure_error_code = error["code"]
            raise IntegrationFailure(f"expected HTTP {expected}, got {status} for {method} {target}")
        return response

    def wait_for_response(self, target: str, expected_status: int, *, headers: dict[str, str] | None = None) -> dict[str, Any]:
        deadline = time.monotonic() + 20.0
        last_failure: Exception | None = None
        while time.monotonic() < deadline:
            try:
                status, response = self.request("GET", target, headers=headers)
                if status == expected_status:
                    return response
                last_failure = IntegrationFailure(f"expected HTTP {expected_status}, got {status}")
            except IntegrationFailure as error:
                last_failure = error
            time.sleep(0.2)
        raise IntegrationFailure("server did not become ready for the expected response") from last_failure

    def post_project(
        self,
        payload: dict[str, str],
        key: str,
        *,
        role: str = "admin",
        content_type: str = "application/json",
    ) -> tuple[int, dict[str, Any]]:
        headers = {
            **self.auth_headers(role),
            "Content-Type": content_type,
            "Idempotency-Key": key,
        }
        return self.request("POST", "/api/v1/projects", headers=headers, body=json.dumps(payload).encode("utf-8"))

    @staticmethod
    def project_payload(project_id: str) -> dict[str, str]:
        return {"id": project_id, "display_name": f"{project_id} project"}

    def post_workspace(
        self,
        project_id: str,
        payload: dict[str, str],
        key: str,
        *,
        role: str = "admin",
        content_type: str = "application/json",
    ) -> tuple[int, dict[str, Any]]:
        headers = {
            **self.auth_headers(role),
            "Content-Type": content_type,
            "Idempotency-Key": key,
        }
        return self.request(
            "POST",
            f"/api/v1/projects/{project_id}/workspaces",
            headers=headers,
            body=json.dumps(payload).encode("utf-8"),
        )

    @staticmethod
    def workspace_payload(workspace_id: str, root: Path) -> dict[str, str]:
        return {"id": workspace_id, "display_name": f"{workspace_id} workspace", "root": str(root)}

    def assert_project(self, project: Any, expected_id: str) -> None:
        if not isinstance(project, dict):
            raise IntegrationFailure("project response is not an object")
        expected_fields = {"id", "display_name", "created_at", "revision"}
        if set(project) != expected_fields:
            raise IntegrationFailure("project response fields do not match the contract")
        if project["id"] != expected_id:
            raise IntegrationFailure("project response has the wrong identity")
        if not isinstance(project["created_at"], str) or not project["created_at"]:
            raise IntegrationFailure("project response has no creation time")
        if project["revision"] != 1:
            raise IntegrationFailure("project response has an invalid initial revision")

    def assert_workspace(self, workspace: Any, project_id: str, workspace_id: str, root: Path) -> None:
        if not isinstance(workspace, dict):
            raise IntegrationFailure("workspace response is not an object")
        expected_fields = {
            "project_id", "id", "display_name", "source_host_id", "root", "root_identity", "source_kind", "created_at", "revision",
        }
        if set(workspace) != expected_fields:
            raise IntegrationFailure("workspace response fields do not match the contract")
        if (workspace["project_id"], workspace["id"], workspace["source_host_id"], workspace["root"]) != (
            project_id, workspace_id, "local-dev", str(root.resolve()),
        ):
            raise IntegrationFailure("workspace response has the wrong identity")
        if not isinstance(workspace["root_identity"], str) or not workspace["root_identity"]:
            raise IntegrationFailure("workspace response has no root identity")
        if not isinstance(workspace["created_at"], str) or not workspace["created_at"] or workspace["revision"] != 1:
            raise IntegrationFailure("workspace response has invalid creation metadata")

    def assert_register(self, project_id: str, key: str) -> None:
        status, response = self.post_project(self.project_payload(project_id), key)
        if status != 201 or response["result"].get("replayed") is not False:
            error = response.get("error")
            code = error.get("code") if isinstance(error, dict) else "none"
            self.failure_http_status = status
            self.failure_error_code = code if isinstance(code, str) else "none"
            raise IntegrationFailure(f"first registration did not return 201 without replay: http-{status} code-{code}")
        self.assert_project(response["result"].get("project"), project_id)

    def assert_workspace_register(self, project_id: str, workspace_id: str, root: Path, key: str) -> None:
        status, response = self.post_workspace(project_id, self.workspace_payload(workspace_id, root), key)
        if status != 201 or response["result"].get("replayed") is not False:
            raise IntegrationFailure("first workspace registration did not return 201 without replay")
        self.assert_workspace(response["result"].get("workspace"), project_id, workspace_id, root)

    def require_idempotency_storage(self) -> None:
        exists = self.scalar("SELECT to_regclass('shin_server.idempotency_requests') IS NOT NULL")
        if exists != "t":
            raise IntegrationFailure("migration did not provide the idempotency receipt table")
        workspace_exists = self.scalar("SELECT to_regclass('shin_server.workspace_requests') IS NOT NULL")
        if workspace_exists != "t":
            raise IntegrationFailure("migration did not provide the workspace idempotency receipt table")

    def count(self, table: str, where: str) -> int:
        if not re.fullmatch(r"[a-z][a-z0-9_]*", table):
            raise IntegrationFailure("unsafe table name in SQL assertion")
        return int(self.scalar(f"SELECT COUNT(*) FROM {SCHEMA_NAME}.{table} WHERE {where}"))

    def check_version(self) -> None:
        result = self.run_process([str(self.binary), "--version"], "version command")
        version = json.loads(result.stdout)
        self.assert_envelope(version)
        details = version.get("result")
        if not isinstance(details, dict) or details.get("core_version") != (REPOSITORY_ROOT / "VERSION").read_text().strip():
            raise IntegrationFailure("server did not identify its linked Core version")
        if (details.get("activation_available") is not True or
                details.get("retrieval_routing") != "managed-generation-config-v10" or
                details.get("workspace_registry_available") is not True or
                details.get("git_required") is not False):
            raise IntegrationFailure("server version response did not advertise workspace-bound Git-independent routing")

    def check_health_and_auth(self) -> None:
        self.expect_status(200, "GET", "/health/live")
        self.expect_status(401, "GET", "/health/ready")
        self.expect_status(401, "GET", "/api/v1/projects")
        self.expect_status(401, "GET", "/api/v1/projects", headers={"Authorization": "Bearer invalid"})
        ready = self.expect_status(200, "GET", "/health/ready", headers=self.auth_headers("admin")).get("result")
        if ready != {"status": "ready", "scope": "store", "server_id": self.server_id}:
            raise IntegrationFailure("readiness did not identify the configured Server database lifetime")

    def check_server_identity_header(self) -> None:
        authenticated = {"Authorization": f"Bearer {self.admin_token}"}
        unauthenticated, unauthenticated_response = self.request(
            "GET", "/api/v1/projects", headers={}, include_server_identity=False)
        if unauthenticated != 401 or unauthenticated_response.get("error", {}).get("code") != "unauthorized":
            raise IntegrationFailure("authentication did not remain first when the Server identity header was missing")
        unauthenticated, unauthenticated_response = self.request(
            "GET", "/api/v1/projects", headers={"X-Shin-Server-ID": "f" * 32})
        if unauthenticated != 401 or unauthenticated_response.get("error", {}).get("code") != "unauthorized":
            raise IntegrationFailure("authentication did not remain first when the Server identity header was wrong")

        missing, missing_response = self.request(
            "GET", "/api/v1/projects", headers=authenticated, include_server_identity=False)
        if missing != 400 or missing_response.get("error", {}).get("code") != "server-id-required":
            raise IntegrationFailure("authenticated request without a Server identity header was not rejected")
        wrong, wrong_response = self.request(
            "GET", "/api/v1/projects", headers={**authenticated, "X-Shin-Server-ID": "f" * 32})
        if wrong != 409 or wrong_response.get("error", {}).get("code") != "server-identity-mismatch":
            raise IntegrationFailure("authenticated request with a wrong Server identity header was not rejected")
        correct, _ = self.request("GET", "/api/v1/projects", headers=self.auth_headers("admin"))
        if correct != 200:
            raise IntegrationFailure("authenticated request with the configured Server identity header was rejected")
        duplicate, duplicate_response = self.request_with_duplicate_server_identity(authenticated)
        if duplicate != 400 or duplicate_response.get("error", {}).get("code") != "server-id-required":
            raise IntegrationFailure("duplicate Server identity headers were not rejected")

        ready = self.request("GET", "/health/ready", headers=authenticated, include_server_identity=False)[0]
        identity = self.request("GET", "/api/v1/observer-identity", headers=authenticated, include_server_identity=False)[0]
        if ready != 200 or identity != 200:
            raise IntegrationFailure("identity-header exceptions no longer allow authenticated health or Observer handshake requests")

    def request_with_duplicate_server_identity(self, headers: dict[str, str]) -> tuple[int, dict[str, Any]]:
        connection = http.client.HTTPConnection(HOST, PORT, timeout=4.0)
        try:
            connection.putrequest("GET", "/api/v1/projects", skip_host=True)
            connection.putheader("Host", f"{HOST}:{PORT}")
            for key, value in headers.items():
                connection.putheader(key, value)
            connection.putheader("X-Shin-Server-ID", self.server_id)
            connection.putheader("X-Shin-Server-ID", self.server_id)
            connection.endheaders()
            response = connection.getresponse()
            status = response.status
            payload = response.read()
        except OSError as error:
            raise IntegrationFailure("server did not provide an HTTP response") from error
        finally:
            connection.close()
        try:
            decoded = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise IntegrationFailure("server response is not a JSON envelope") from error
        if not isinstance(decoded, dict):
            raise IntegrationFailure("server response envelope is not an object")
        self.assert_envelope(decoded)
        return status, decoded

    def check_transport_header_guards(self) -> None:
        too_many = {"Host": f"{HOST}:{PORT}"}
        too_many.update({f"X-Header-{index}": "x" for index in range(40)})
        status = self.raw_status("GET", "/health/live", headers=too_many)
        if status != 400:
            self.failure_http_status = status
            raise IntegrationFailure("header count above 32 was not rejected with HTTP 400")

        status = self.raw_status(
            "GET",
            "/health/live",
            headers={"Host": f"{HOST}:{PORT}", "X-Header-Over-Limit": "x" * 8193},
        )
        if status != 400:
            self.failure_http_status = status
            raise IntegrationFailure("header line above 8192 bytes was not rejected with HTTP 400")

    def check_readiness_schema_validation(self) -> None:
        self.psql("DROP SCHEMA shin_server CASCADE")
        self.wait_for_response("/health/live", 200)
        self.wait_for_response("/health/ready", 503, headers=self.auth_headers("admin"))

        self.stop_server(expect_clean=True)
        self.migrate()
        self.start_server()
        self.wait_for_response("/health/ready", 200, headers=self.auth_headers("admin"))

        self.psql("ALTER TABLE shin_server.projects ADD COLUMN readiness_probe BOOLEAN")
        self.wait_for_response("/health/live", 200)
        self.wait_for_response("/health/ready", 503, headers=self.auth_headers("admin"))
        self.psql("ALTER TABLE shin_server.projects DROP COLUMN readiness_probe")
        self.wait_for_response("/health/ready", 200, headers=self.auth_headers("admin"))

    def check_readiness_runtime_security_and_checksum(self) -> None:
        self.psql("ALTER ROLE shin_runtime CREATEROLE")
        try:
            self.wait_for_response("/health/ready", 503, headers=self.auth_headers("admin"))
        finally:
            self.psql("ALTER ROLE shin_runtime NOCREATEROLE")
        self.wait_for_response("/health/ready", 200, headers=self.auth_headers("admin"))

        self.psql("GRANT CREATE ON SCHEMA shin_server TO shin_runtime")
        try:
            self.wait_for_response("/health/ready", 503, headers=self.auth_headers("admin"))
        finally:
            self.psql("REVOKE CREATE ON SCHEMA shin_server FROM shin_runtime")
        self.wait_for_response("/health/ready", 200, headers=self.auth_headers("admin"))

        checksum = self.scalar("SELECT checksum FROM shin_server.schema_migrations WHERE version = 1")
        if not re.fullmatch(r"[0-9a-f]{64}", checksum):
            raise IntegrationFailure("migration checksum is malformed")
        self.psql("UPDATE shin_server.schema_migrations SET checksum = repeat('0', 64) WHERE version = 1")
        try:
            self.wait_for_response("/health/ready", 503, headers=self.auth_headers("admin"))
        finally:
            self.psql(f"UPDATE shin_server.schema_migrations SET checksum = '{checksum}' WHERE version = 1")
        self.wait_for_response("/health/ready", 200, headers=self.auth_headers("admin"))

    def check_store_validation_drift(self) -> None:
        from store_validation_checks import check_store_validation_drift
        from checkpoint_rls_checks import check_checkpoint_rls_drift
        from preparation_rls_checks import check_preparation_rls_drift
        from route_history_rls_checks import check_route_history_drift
        try:
            check_store_validation_drift(self)
            check_checkpoint_rls_drift(self)
            check_preparation_rls_drift(self)
            check_route_history_drift(self)
        except RuntimeError as error:
            raise IntegrationFailure(str(error)) from error

    def check_registration_and_pagination(self) -> None:
        roots = self.state_root / "fixtures" / "roots"
        visible = roots / "visible"
        other = roots / "other"
        secondary = roots / "visible-secondary"
        replacement = roots / "visible-replacement"
        if (visible / ".git").exists() or (other / ".git").exists():
            raise IntegrationFailure("workspace roots must remain non-git fixtures")
        for root in (secondary, replacement):
            root.mkdir(mode=0o700, exist_ok=True)
            status = root.lstat()
            if not stat.S_ISDIR(status.st_mode) or status.st_uid != os.geteuid() or stat.S_IMODE(status.st_mode) != 0o700:
                raise IntegrationFailure("workspace fixture directory is not private and owned")

        # Project registration is intentionally logical: this identifier has no corresponding fixture directory.
        self.assert_register("logical-only", "register-logical-only")
        self.assert_register("visible", "register-visible")
        replay_status, replay = self.post_project(self.project_payload("visible"), "register-visible")
        if replay_status != 200 or replay["result"].get("replayed") is not True:
            raise IntegrationFailure("idempotent replay did not return 200 with replayed=true")
        mismatch_status, _ = self.post_project(
            {"id": "visible", "display_name": "different visible project"}, "register-visible"
        )
        if mismatch_status != 409:
            raise IntegrationFailure("idempotency hash mismatch was not rejected")

        self.assert_register("other", "register-other")
        self.assert_workspace_register("visible", "primary", visible, "register-visible-primary")
        replay_status, replay = self.post_workspace(
            "visible", self.workspace_payload("primary", visible), "register-visible-primary"
        )
        if replay_status != 200 or replay["result"].get("replayed") is not True:
            raise IntegrationFailure("workspace idempotent replay did not return 200 with replayed=true")
        workspace_mismatch, _ = self.post_workspace(
            "visible", self.workspace_payload("primary", secondary), "register-visible-primary"
        )
        if workspace_mismatch != 409:
            raise IntegrationFailure("workspace idempotency hash mismatch was not rejected")
        immutable_status, immutable = self.post_workspace(
            "visible", self.workspace_payload("primary", replacement), "register-visible-primary-replacement"
        )
        if immutable_status != 409 or immutable.get("error", {}).get("code") != "workspace-conflict":
            raise IntegrationFailure("workspace id was not immutable within its project")
        self.assert_workspace_register("visible", "secondary", secondary, "register-visible-secondary")
        registered_secondary = self.expect_status(
            200, "GET", "/api/v1/projects/visible/workspaces/secondary", headers=self.auth_headers("admin")
        )["result"]
        renamed_secondary = secondary.with_name("visible-secondary-renamed")
        if renamed_secondary.exists():
            raise IntegrationFailure("renamed workspace root fixture unexpectedly exists")
        secondary.rename(renamed_secondary)
        try:
            replay_status, replay = self.post_workspace(
                "visible", self.workspace_payload("secondary", secondary), "register-visible-secondary"
            )
            if replay_status != 200 or replay["result"].get("replayed") is not True:
                raise IntegrationFailure("workspace replay required current filesystem access")
            self.assert_workspace(replay["result"].get("workspace"), "visible", "secondary", secondary)
            if replay["result"].get("workspace") != registered_secondary:
                raise IntegrationFailure("workspace replay did not preserve the original receipt metadata")
            persisted = self.expect_status(
                200, "GET", "/api/v1/projects/visible/workspaces/secondary", headers=self.auth_headers("admin")
            )["result"]
            self.assert_workspace(persisted, "visible", "secondary", secondary)
            if persisted != registered_secondary:
                raise IntegrationFailure("workspace metadata changed after its root was renamed")
            changed_status, changed = self.post_workspace(
                "visible", self.workspace_payload("secondary", replacement), "register-visible-secondary-changed"
            )
            if changed_status != 409 or changed.get("error", {}).get("code") != "workspace-conflict":
                raise IntegrationFailure("fresh workspace request did not preserve immutable workspace identity")
        finally:
            renamed_secondary.rename(secondary)
        self.assert_workspace_register("other", "primary", other, "register-other-primary")
        conflict_status, conflict = self.post_workspace(
            "other", self.workspace_payload("same-root", visible), "register-cross-project-root"
        )
        if conflict_status != 409 or conflict.get("error", {}).get("code") != "workspace-conflict":
            raise IntegrationFailure("cross-project workspace root conflict was not rejected")

        first = self.expect_status(200, "GET", "/api/v1/projects?limit=1", headers=self.auth_headers("admin"))["result"]
        if not isinstance(first.get("items"), list) or len(first["items"]) != 1 or not isinstance(first.get("next_cursor"), str):
            raise IntegrationFailure("first project page is malformed")
        second = self.expect_status(
            200,
            "GET",
            f"/api/v1/projects?limit=1&after={first['next_cursor']}",
            headers=self.auth_headers("admin"),
        )["result"]
        if not isinstance(second.get("items"), list) or len(second["items"]) != 1:
            raise IntegrationFailure("second project page is malformed")
        if first["items"][0]["id"] == second["items"][0]["id"]:
            raise IntegrationFailure("project cursor did not advance")

        workspaces = self.expect_status(
            200, "GET", "/api/v1/projects/visible/workspaces?limit=1", headers=self.auth_headers("admin")
        )["result"]
        if not isinstance(workspaces.get("items"), list) or len(workspaces["items"]) != 1:
            raise IntegrationFailure("workspace page is malformed")
        workspace_cursor = workspaces.get("next_cursor")
        if not isinstance(workspace_cursor, str):
            raise IntegrationFailure("workspace page did not provide a cursor")
        second_workspace_page = self.expect_status(
            200,
            "GET",
            f"/api/v1/projects/visible/workspaces?limit=1&after={workspace_cursor}",
            headers=self.auth_headers("admin"),
        )["result"]
        if not isinstance(second_workspace_page.get("items"), list) or len(second_workspace_page["items"]) != 1:
            raise IntegrationFailure("second workspace page is malformed")
        if second_workspace_page["items"][0]["id"] == workspaces["items"][0]["id"]:
            raise IntegrationFailure("workspace cursor did not advance")

        reader_items = self.expect_status(200, "GET", "/api/v1/projects?limit=8", headers=self.auth_headers("reader"))["result"]
        if [item.get("id") for item in reader_items.get("items", [])] != ["visible"]:
            raise IntegrationFailure("reader project scope was not enforced")
        reader_project = self.expect_status(200, "GET", "/api/v1/projects/visible", headers=self.auth_headers("reader"))["result"]
        self.assert_project(reader_project, "visible")
        reader_workspace = self.expect_status(
            200, "GET", "/api/v1/projects/visible/workspaces/primary", headers=self.auth_headers("reader")
        )["result"]
        self.assert_workspace(reader_workspace, "visible", "primary", visible)
        self.expect_status(404, "GET", "/api/v1/projects/visible/workspaces/missing", headers=self.auth_headers("reader"))
        self.expect_status(404, "GET", "/api/v1/projects/other/workspaces/primary", headers=self.auth_headers("reader"))
        self.expect_status(404, "GET", "/api/v1/projects/other", headers=self.auth_headers("reader"))
        reader_post_status, _ = self.post_project(self.project_payload("reader-write"), "reader-write", role="reader")
        if reader_post_status != 403:
            raise IntegrationFailure("reader project registration was not forbidden")
        reader_workspace_status, _ = self.post_workspace(
            "visible", self.workspace_payload("reader-write", other), "reader-workspace-write", role="reader"
        )
        if reader_workspace_status != 403:
            raise IntegrationFailure("reader workspace registration was not forbidden")

    def check_request_guards(self) -> None:
        roots = self.state_root / "fixtures" / "roots"
        visible = roots / "visible"
        outside = self.state_root / "fixtures" / "outside"
        escape = roots / f"escape-{secrets.token_hex(8)}"
        try:
            escape.symlink_to(outside, target_is_directory=True)
        except FileExistsError as error:
            raise IntegrationFailure("test root fixture unexpectedly exists") from error
        try:
            self.assert_register("escaped", "register-escaped")
            escape_status, _ = self.post_workspace(
                "escaped", self.workspace_payload("primary", escape), "register-escaped-primary"
            )
            if escape_status != 400:
                raise IntegrationFailure("canonical root escape was not rejected")
        finally:
            escape.unlink(missing_ok=True)

        self.expect_status(
            403,
            "GET",
            "/api/v1/projects",
            headers={**self.auth_headers("admin"), "Origin": "null"},
        )
        self.expect_status(
            403,
            "GET",
            "/api/v1/projects",
            headers={**self.auth_headers("admin"), "Origin": "http://untrusted.invalid"},
        )
        self.expect_status(
            400,
            "GET",
            "/api/v1/projects",
            headers={**self.auth_headers("admin"), "Host": "untrusted.invalid"},
        )
        self.expect_status(
            200,
            "GET",
            "/api/v1/projects?limit=1",
            headers={**self.auth_headers("admin"), "Origin": f"http://{HOST}:{PORT}"},
        )
        self.expect_status(
            200,
            "GET",
            "/api/v1/projects?limit=1",
            headers={
                **self.auth_headers("admin"),
                "Host": f"localhost:{PORT}",
                "Origin": f"http://localhost:{PORT}",
            },
        )
        self.expect_status(400, "GET", "/api/v1/projects?unknown=1", headers=self.auth_headers("admin"))
        self.expect_status(400, "GET", "/api/v1/projects?limit=1&limit=1", headers=self.auth_headers("admin"))
        self.expect_status(400, "GET", "/api/v1/projects?limit=1&%6cimit=1", headers=self.auth_headers("admin"))
        self.expect_status(400, "GET", "/api/v1/projects?limit=1&", headers=self.auth_headers("admin"))
        self.expect_status(400, "GET", "/api/v1/projects?limit=0", headers=self.auth_headers("admin"))

        self.expect_status(
            400,
            "POST",
            "/api/v1/projects",
            headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
            body=json.dumps(self.project_payload("missing-key")).encode("utf-8"),
        )
        self.expect_status(
            400,
            "POST",
            "/api/v1/projects",
            headers={
                **self.auth_headers("admin"),
                "Content-Type": "application/json",
                "Idempotency-Key": "contains space",
            },
            body=json.dumps(self.project_payload("unsafe-key")).encode("utf-8"),
        )
        bad_type_status, _ = self.post_project(self.project_payload("bad-type"), "bad-content-type", content_type="text/plain")
        if bad_type_status != 415:
            raise IntegrationFailure("non-JSON content type was not rejected")
        duplicate_body = (
            "{\"id\":\"duplicate-key\",\"id\":\"other\",\"display_name\":\"Duplicate\"}"
        ).encode("utf-8")
        self.expect_status(
            400,
            "POST",
            "/api/v1/projects",
            headers={**self.auth_headers("admin"), "Content-Type": "application/json", "Idempotency-Key": "duplicate-json-key"},
            body=duplicate_body,
        )
        legacy_root_body = {**self.project_payload("legacy-root"), "root": str(visible)}
        self.expect_status(
            400,
            "POST",
            "/api/v1/projects",
            headers={**self.auth_headers("admin"), "Content-Type": "application/json", "Idempotency-Key": "legacy-root"},
            body=json.dumps(legacy_root_body).encode("utf-8"),
        )
        unknown_body = {**self.project_payload("unknown-field"), "unexpected": "value"}
        self.expect_status(
            400,
            "POST",
            "/api/v1/projects",
            headers={**self.auth_headers("admin"), "Content-Type": "application/json", "Idempotency-Key": "unknown-json-field"},
            body=json.dumps(unknown_body).encode("utf-8"),
        )
        large_body = {
            "id": "too-large",
            "display_name": "x" * (33 * 1024),
        }
        self.expect_status(
            413,
            "POST",
            "/api/v1/projects",
            headers={**self.auth_headers("admin"), "Content-Type": "application/json", "Idempotency-Key": "body-over-limit"},
            body=json.dumps(large_body).encode("utf-8"),
        )
        self.expect_status(
            400,
            "POST",
            "/api/v1/projects/visible/workspaces",
            headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
            body=json.dumps(self.workspace_payload("missing-key", visible)).encode("utf-8"),
        )
        self.expect_status(
            400,
            "GET",
            "/api/v1/projects/visible/workspaces?limit=1&after=primary&after=secondary",
            headers=self.auth_headers("admin"),
        )

    def check_audit_and_concurrency(self) -> None:
        audit = self.expect_status(200, "GET", "/api/v1/audit?limit=1", headers=self.auth_headers("admin"))["result"]
        if not isinstance(audit.get("items"), list) or len(audit["items"]) != 1:
            raise IntegrationFailure("audit pagination did not return one item")
        public_audit_fields = {
            "id",
            "principal_id",
            "action",
            "project_id",
            "workspace_id",
            "preparation_id",
            "activation_route_id",
            "activation_previous_revision",
            "observer_stream_id",
            "observer_sequence",
            "publication_proposal_id",
            "created_at",
        }
        for event in audit["items"]:
            if not isinstance(event, dict) or set(event) != public_audit_fields:
                raise IntegrationFailure("public audit event does not match the activation-aware schema")
            if event["action"] == "project.registered" and (
                    event["workspace_id"] is not None or event["preparation_id"] is not None or event["activation_route_id"] is not None or
                    event["activation_previous_revision"] is not None or event["publication_proposal_id"] is not None):
                raise IntegrationFailure("project audit event unexpectedly retained activation metadata")
            if event["action"] == "workspace.registered" and event["workspace_id"] is None:
                raise IntegrationFailure("workspace audit event did not retain its workspace identity")
        if audit.get("next_cursor") is not None and not isinstance(audit.get("next_cursor"), int):
            raise IntegrationFailure("audit next cursor is malformed")
        reader_audit = self.expect_status(200, "GET", "/api/v1/audit?limit=8", headers=self.auth_headers("reader"))["result"]
        if any(item.get("project_id") != "visible" for item in reader_audit.get("items", [])):
            raise IntegrationFailure("reader audit scope was not enforced")
        self.expect_status(400, "GET", "/api/v1/audit?after=1&after=2", headers=self.auth_headers("admin"))
        self.expect_status(400, "GET", "/api/v1/audit?unknown=1", headers=self.auth_headers("admin"))

        project_payload = self.project_payload("concurrent")
        project_barrier = threading.Barrier(3)
        project_outcomes: list[tuple[int, dict[str, Any]]] = []
        project_failures: list[BaseException] = []

        def register_project() -> None:
            try:
                project_barrier.wait(timeout=5.0)
                project_outcomes.append(self.post_project(project_payload, "register-concurrent"))
            except BaseException as error:  # Keep both worker failures for the test result.
                project_failures.append(error)

        project_workers = [threading.Thread(target=register_project), threading.Thread(target=register_project)]
        for worker in project_workers:
            worker.start()
        project_barrier.wait(timeout=5.0)
        for worker in project_workers:
            worker.join(timeout=10.0)
        if project_failures or len(project_outcomes) != 2 or sorted(status for status, _ in project_outcomes) != [200, 201]:
            raise IntegrationFailure("concurrent duplicate project registration was not idempotent")
        if any(response["result"].get("replayed") is not (status == 200) for status, response in project_outcomes):
            raise IntegrationFailure("concurrent project replay markers are inconsistent")
        if self.count("projects", "id = 'concurrent'") != 1:
            raise IntegrationFailure("concurrent project registration created more than one project row")
        if self.count("audit_events", "project_id = 'concurrent' AND workspace_id IS NULL") != 1:
            raise IntegrationFailure("concurrent project registration created more than one audit row")
        if self.count("idempotency_requests", "key = 'register-concurrent'") != 1:
            raise IntegrationFailure("concurrent project registration created more than one idempotency row")

        root = self.state_root / "fixtures" / "roots" / "concurrent"
        self.assert_register("concurrent-workspace", "register-concurrent-workspace-project")
        payload = self.workspace_payload("primary", root)
        barrier = threading.Barrier(3)
        outcomes: list[tuple[int, dict[str, Any]]] = []
        failures: list[BaseException] = []

        def register() -> None:
            try:
                barrier.wait(timeout=5.0)
                outcomes.append(self.post_workspace("concurrent-workspace", payload, "register-concurrent-workspace"))
            except BaseException as error:  # Keep both worker failures for the test result.
                failures.append(error)

        workers = [threading.Thread(target=register), threading.Thread(target=register)]
        for worker in workers:
            worker.start()
        barrier.wait(timeout=5.0)
        for worker in workers:
            worker.join(timeout=10.0)
        if failures or len(outcomes) != 2 or sorted(status for status, _ in outcomes) != [200, 201]:
            raise IntegrationFailure("concurrent duplicate workspace registration was not idempotent")
        if any(response["result"].get("replayed") is not (status == 200) for status, response in outcomes):
            raise IntegrationFailure("concurrent replay markers are inconsistent")

        if self.count("workspaces", "project_id = 'concurrent-workspace' AND id = 'primary'") != 1:
            raise IntegrationFailure("concurrent registration created more than one workspace row")
        if self.count("audit_events", "project_id = 'concurrent-workspace' AND workspace_id = 'primary'") != 1:
            raise IntegrationFailure("concurrent workspace registration created more than one audit row")

    def check_atomic_rollback(self) -> None:
        root = self.state_root / "fixtures" / "roots" / "rollback"
        self.psql("REVOKE INSERT ON TABLE shin_server.audit_events FROM shin_runtime")
        try:
            status, _ = self.post_project(self.project_payload("rollback"), "register-rollback-project")
            if status < 500:
                raise IntegrationFailure("project audit permission failure was not surfaced as a server failure")
            if self.count("projects", "id = 'rollback'") != 0:
                raise IntegrationFailure("failed project registration left a project row")
            if self.count("audit_events", "project_id = 'rollback'") != 0:
                raise IntegrationFailure("failed project registration left an audit row")
            if self.count("idempotency_requests", "key = 'register-rollback-project'") != 0:
                raise IntegrationFailure("failed project registration left an idempotency row")
        finally:
            self.psql("GRANT INSERT ON TABLE shin_server.audit_events TO shin_runtime")

        self.assert_register("rollback", "register-rollback-project")
        self.psql("REVOKE INSERT ON TABLE shin_server.audit_events FROM shin_runtime")
        try:
            status, _ = self.post_workspace("rollback", self.workspace_payload("primary", root), "register-rollback")
            if status < 500:
                raise IntegrationFailure("workspace audit permission failure was not surfaced as a server failure")
            if self.count("workspaces", "project_id = 'rollback'") != 0:
                raise IntegrationFailure("failed workspace registration left a workspace row")
            if self.count("audit_events", "project_id = 'rollback' AND workspace_id IS NOT NULL") != 0:
                raise IntegrationFailure("failed registration left an audit row")
            if self.count("workspace_requests", "project_id = 'rollback'") != 0:
                raise IntegrationFailure("failed workspace registration left an idempotency row")
        finally:
            self.psql("GRANT INSERT ON TABLE shin_server.audit_events TO shin_runtime")

        self.assert_workspace_register("rollback", "primary", root, "register-rollback")
        if self.count("workspaces", "project_id = 'rollback' AND id = 'primary'") != 1:
            raise IntegrationFailure("retry after grant did not persist the workspace")

    def check_audit_cursor_commit_order(self) -> None:
        suffix = secrets.token_hex(8)
        first_id = f"audit-first-{suffix}"
        second_id = f"audit-second-{suffix}"
        trigger_name = f"audit_cursor_trigger_{suffix}"
        function_name = f"audit_cursor_delay_{suffix}"

        baseline = int(self.scalar("SELECT last_value FROM shin_server.audit_events_id_seq"))
        self.psql(
            f"CREATE FUNCTION shin_server.{function_name}() RETURNS trigger LANGUAGE plpgsql AS $$ "
            f"BEGIN IF NEW.project_id = '{first_id}' THEN PERFORM pg_sleep(0.45); END IF; RETURN NEW; END; $$"
        )
        self.psql(
            f"CREATE TRIGGER {trigger_name} BEFORE INSERT ON shin_server.audit_events "
            f"FOR EACH ROW EXECUTE FUNCTION shin_server.{function_name}()"
        )
        first_outcome: list[tuple[int, dict[str, Any]]] = []
        second_outcome: list[tuple[int, dict[str, Any]]] = []
        failures: list[BaseException] = []

        def register(project_id: str, key: str, outcomes: list[tuple[int, dict[str, Any]]]) -> None:
            try:
                outcomes.append(self.post_project(self.project_payload(project_id), key))
            except BaseException as error:  # Worker errors remain structured test failures, not raw server output.
                failures.append(error)

        first = threading.Thread(
            target=register,
            args=(first_id, f"audit-first-{suffix}", first_outcome),
        )
        second = threading.Thread(
            target=register,
            args=(second_id, f"audit-second-{suffix}", second_outcome),
        )
        try:
            first.start()
            self.wait_for_audit_sequence_advance(baseline)
            second.start()
            second.join(timeout=10.0)
            if second.is_alive():
                raise IntegrationFailure("second controlled audit writer did not complete")
            if failures or len(second_outcome) != 1 or second_outcome[0][0] != 201:
                raise IntegrationFailure("second controlled audit writer failed")

            first_page = self.expect_status(
                200,
                "GET",
                f"/api/v1/audit?after={baseline}&limit=1",
                headers=self.auth_headers("admin"),
            )["result"]
            first_items = first_page.get("items")
            first_cursor = first_page.get("next_cursor")
            if not isinstance(first_items, list) or len(first_items) != 1 or not isinstance(first_cursor, int):
                raise IntegrationFailure("first controlled audit page is malformed")
            if first_items[0].get("project_id") != first_id:
                raise IntegrationFailure("audit cursor observed a later commit before the earlier audit id")

            first.join(timeout=10.0)
            if first.is_alive() or failures or len(first_outcome) != 1 or first_outcome[0][0] != 201:
                raise IntegrationFailure("first controlled audit writer failed")
            second_page = self.expect_status(
                200,
                "GET",
                f"/api/v1/audit?after={first_cursor}&limit=1",
                headers=self.auth_headers("admin"),
            )["result"]
            second_items = second_page.get("items")
            if not isinstance(second_items, list) or len(second_items) != 1:
                raise IntegrationFailure("second controlled audit page is malformed")
            if second_items[0].get("project_id") != second_id:
                raise IntegrationFailure("audit cursor skipped or reordered a committed event")
        finally:
            self.psql(f"DROP TRIGGER IF EXISTS {trigger_name} ON shin_server.audit_events")
            self.psql(f"DROP FUNCTION IF EXISTS shin_server.{function_name}()")

    def check_restart_and_database_recovery(self) -> None:
        self.stop_server(expect_clean=True)
        self.start_server()
        visible = self.expect_status(200, "GET", "/api/v1/projects/visible", headers=self.auth_headers("admin"))["result"]
        self.assert_project(visible, "visible")

        process = self.server
        process.kill()
        process.wait(timeout=5.0)
        self.server = None
        self.start_server()
        status, replay = self.post_project(self.project_payload("visible"), "register-visible")
        if status != 200 or replay["result"].get("replayed") is not True:
            raise IntegrationFailure("committed idempotency receipt was lost after process kill")
        if self.count("audit_events", "project_id = 'visible' AND action = 'project.registered'") != 1:
            raise IntegrationFailure("process recovery duplicated or lost committed audit")
        root = self.state_root / "fixtures" / "roots" / "visible"
        status, replay = self.post_workspace("visible", self.workspace_payload("primary", root), "register-visible-primary")
        if status != 200 or replay["result"].get("replayed") is not True:
            raise IntegrationFailure("workspace idempotency receipt was lost after process kill")

        self.compose("stop", "postgres")
        self.wait_for_response("/health/live", 200)
        self.wait_for_response("/health/ready", 503, headers=self.auth_headers("admin"))
        self.compose("start", "postgres")
        self.wait_for_response("/health/ready", 200, headers=self.auth_headers("admin"))
        self.expect_status(200, "GET", "/api/v1/projects/visible", headers=self.auth_headers("reader"))

    def write_report(self, *, ok: bool, failure: str | None = None) -> None:
        report = {
            "schema_version": "shin.server-integration-report.v1",
            "tool": "shin-server-integration-test",
            "version": SERVER_VERSION,
            "ok": ok,
            "duration_ms": int((time.monotonic() - self.started_at) * 1000),
            "checks": self.checks,
            "postgres": {"project": PROJECT_NAME, "database": DATABASE_NAME, "schema": SCHEMA_NAME},
            "benchmark": {"status": "not-run", "reason": "correctness suite does not claim performance"},
        }
        if failure is not None:
            failure_report: dict[str, Any] = {"type": failure, "stage": self.active_check}
            if self.failure_http_status is not None:
                failure_report["http_status"] = self.failure_http_status
            if self.failure_error_code is not None:
                failure_report["error_code"] = self.failure_error_code
            if self.failure_assertion is not None:
                failure_report["assertion"] = self.failure_assertion
            report["failure"] = failure_report
        descriptor = os.open(self.report_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(report, handle, sort_keys=True, separators=(",", ":"))
            handle.write("\n")

    def run(self) -> None:
        self.record("version", self.check_version)
        self.dev_command("init")
        self.dev_command("up")
        self.record("postgres-ready", self.wait_for_postgres)
        self.record("http-port-free", self.require_http_port_free)
        self.record("dedicated-database-guard", self.reset_schema)
        from workspace_migration_checks import (check_audit_history_upgrade, check_observer_history_upgrade,
                                                check_observer_workspace_upgrade, check_server_recovery_upgrade,
                                                check_workspace_migration_upgrade)
        self.record("workspace-migration-upgrade", lambda: check_workspace_migration_upgrade(self))
        self.record("observer-workspace-schema-upgrade", lambda: check_observer_workspace_upgrade(self))
        self.record("observer-history-schema-upgrade", lambda: check_observer_history_upgrade(self))
        self.record("audit-history-schema-upgrade", lambda: check_audit_history_upgrade(self))
        self.record("server-recovery-schema-upgrade", lambda: check_server_recovery_upgrade(self))
        from observer_reclamation_schema_checks import check_observer_registry_upgrade
        self.record("observer-registry-schema-upgrade", lambda: check_observer_registry_upgrade(self))
        from publication_migration_checks import check_publication_migration
        self.record("publication-schema-upgrade", lambda: check_publication_migration(self))
        from publication_withdrawal_migration_checks import check_publication_withdrawal_migration
        self.record("publication-withdrawal-schema-upgrade", lambda: check_publication_withdrawal_migration(self))
        from publication_history_checks import check_publication_history_migration
        self.record("publication-history-schema-upgrade", lambda: check_publication_history_migration(self))
        from journal_retention_migration_checks import check_journal_retention_upgrade
        self.record("journal-retention-schema-upgrade", lambda: check_journal_retention_upgrade(self))
        from journal_epoch_migration_checks import check_journal_epoch_migration
        self.record("journal-epoch-schema-upgrade", lambda: check_journal_epoch_migration(self))
        from identity_directory_migration_checks import check_identity_directory_migration
        self.record("identity-directory-schema-upgrade", lambda: check_identity_directory_migration(self))
        self.record("workspace-ownership-schema-upgrade", lambda: check_identity_directory_migration(self, baseline=20))
        self.record("preparation-workspace-schema-upgrade", lambda: check_identity_directory_migration(self, baseline=21))
        self.record("publication-workspace-history-schema-upgrade", lambda: check_identity_directory_migration(self, baseline=22))
        self.record("personal-spaces-schema-upgrade-preserves-inert-sharing", lambda: check_identity_directory_migration(self, baseline=23))
        self.record("scoped-request-keys-schema-upgrade-preserves-receipts", lambda: check_identity_directory_migration(self, baseline=24))
        self.record("workspace-request-keys-schema-upgrade-preserves-receipts", lambda: check_identity_directory_migration(self, baseline=25))
        self.record("checkpoint-workspace-slots-schema-upgrade-preserves-legacy", lambda: check_identity_directory_migration(self, baseline=26))
        self.record("preparation-relational-scope-upgrade-preserves-admission", lambda: check_identity_directory_migration(self, baseline=27))
        self.record("checkpoint-row-security-upgrade-preserves-payloads", lambda: check_identity_directory_migration(self, baseline=28))
        self.record("preparation-row-security-upgrade-preserves-jobs-and-retention", lambda: check_identity_directory_migration(self, baseline=29))
        self.record("route-history-security-upgrade-preserves-data", lambda: check_identity_directory_migration(self, baseline=30))
        self.record("journal-attribution-upgrade-preserves-unassigned-history", lambda: check_identity_directory_migration(self, baseline=31))
        self.record("observer-ownership-upgrade-preserves-unassigned-history", lambda: check_identity_directory_migration(self, baseline=32))
        self.record("external-identities-upgrade-preserves-unassigned-accounts", lambda: check_identity_directory_migration(self, baseline=33))
        self.record("device-sessions-upgrade-preserves-external-bindings", lambda: check_identity_directory_migration(self, baseline=34))
        self.record("explicit-migration", self.migrate)
        self.record("migration-schema", lambda: self._require_schema())
        self.record("idempotency-storage", self.require_idempotency_storage)
        from preparation_rls_checks import check_preparation_rls
        self.record("preparation-retention-rls-runtime-isolation", lambda: check_preparation_rls(self))
        from route_history_rls_checks import check_route_history_rls
        self.record("route-history-rls-runtime-isolation", lambda: check_route_history_rls(self))
        from journal_attribution_checks import check_journal_attribution
        self.record("journal-attribution-runtime-isolation", lambda: check_journal_attribution(self))
        from observer_ownership_checks import check_observer_rls
        self.record("observer-ownership-runtime-isolation", lambda: check_observer_rls(self))
        self.admin_token = self.read_private_token("dev_admin.token")
        self.reader_token = self.read_private_token("dev_reader.token")
        self.start_server()
        self.record("health-and-auth", self.check_health_and_auth)
        self.record("server-identity-header", self.check_server_identity_header)
        self.record("transport-header-guards", self.check_transport_header_guards)
        self.record("readiness-schema-validation", self.check_readiness_schema_validation)
        self.record("readiness-runtime-security-and-checksum", self.check_readiness_runtime_security_and_checksum)
        self.record("store-validation-drift", self.check_store_validation_drift)
        self.record("registration-pagination-scopes", self.check_registration_and_pagination)
        self.record("request-guards", self.check_request_guards)
        self.record("audit-and-concurrency", self.check_audit_and_concurrency)
        from workspace_request_key_checks import check_workspace_request_keys
        self.record("workspace-request-keys-project-isolation", lambda: check_workspace_request_keys(self))
        self.record("atomic-rollback", self.check_atomic_rollback)
        self.record("audit-cursor-commit-order", self.check_audit_cursor_commit_order)
        self.record("restart-and-database-recovery", self.check_restart_and_database_recovery)
        from identity_directory_checks import check_identity_directory
        self.record("identity-directory-isolation-rls-cas-revocation-restart", lambda: check_identity_directory(self))
        from workspace_ownership_checks import check_workspace_ownership
        self.record("personal-workspace-ownership-rls-inert-legacy-sharing", lambda: check_workspace_ownership(self))
        self.stop_server(expect_clean=True)

    def _require_schema(self) -> None:
        if self.scalar("SELECT EXISTS (SELECT 1 FROM pg_namespace WHERE nspname = 'shin_server')") != "t":
            raise IntegrationFailure("migration did not create the shin_server schema")
        versions = self.scalar(
            "SELECT COALESCE(string_agg(version::text, ',' ORDER BY version), '') "
            "FROM shin_server.schema_migrations"
        )
        if versions != "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35":
            raise IntegrationFailure("migration did not apply the expected schema versions")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="native shin-server executable")
    parser.add_argument("--state-root", default=str(BUILD_ROOT / "server-dev"), help="isolated state root below build")
    parser.add_argument("--report", help="new report path below this worktree's build directory")
    return parser.parse_args(argv)


def under_build(path: Path) -> bool:
    try:
        path.resolve(strict=False).relative_to(BUILD_ROOT.resolve(strict=False))
    except ValueError:
        return False
    return True


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    binary = Path(args.binary).resolve(strict=False)
    state_root = Path(args.state_root).resolve(strict=False)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        print("error: --binary must name an executable native shin-server", file=sys.stderr)
        return 2
    if not under_build(state_root):
        print("error: --state-root must be below this worktree's build directory", file=sys.stderr)
        return 2
    report_path = Path(args.report).resolve(strict=False) if args.report else state_root / "reports" / f"integration-{secrets.token_hex(8)}.json"
    if not under_build(report_path):
        print("error: --report must be below this worktree's build directory", file=sys.stderr)
        return 2
    if report_path.exists():
        print("error: --report already exists", file=sys.stderr)
        return 2

    suite = IntegrationSuite(binary, state_root, report_path)
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
        print(f"error: integration test failed: {type(error).__name__}", file=sys.stderr)
        return 1
    suite.write_report(ok=True)
    print("integration test passed; compact report written below the isolated build state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
