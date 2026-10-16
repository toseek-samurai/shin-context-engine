#!/usr/bin/env python3
"""Private process, PostgreSQL and native Observer fixtures for recovery tests."""

from __future__ import annotations

import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import signal
import stat
import subprocess
import sys
import tempfile
import time
from types import ModuleType
from typing import Any, Callable
from stand_guard import StandGuard, require_http_port_available


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SERVER_DIR = REPOSITORY_ROOT / "server"
BUILD_ROOT = REPOSITORY_ROOT / "build"
DEV_TOOL = SERVER_DIR / "tools" / "dev.py"
COMPOSE_FILE = SERVER_DIR / "infra" / "compose.yml"
OBSERVER_FIXTURE = REPOSITORY_ROOT / "observer" / "tests" / "integration_test.py"
SERVER_VERSION = (SERVER_DIR / "VERSION").read_text(encoding="utf-8").strip()
PROJECT_NAME = "shin-server-dev"
DATABASE_NAME = "shin_server_dev"
SCHEMA_NAME = "shin_server"
HOST = "127.0.0.1"
PORT = 19125
IDENTITY_HEADER = "X-Shin-Server-ID"


class IntegrationFailure(RuntimeError):
    pass


def compact_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def require_hex(value: Any, length: int, message: str) -> str:
    if not isinstance(value, str) or re.fullmatch(rf"[0-9a-f]{{{length}}}", value) is None:
        raise IntegrationFailure(message)
    return value


def under_build(path: Path) -> bool:
    try:
        path.resolve(strict=False).relative_to(BUILD_ROOT.resolve(strict=False))
    except ValueError:
        return False
    return True


def load_observer_fixture_module() -> ModuleType:
    spec = importlib.util.spec_from_file_location("shin_recovery_observer_fixture", OBSERVER_FIXTURE)
    if spec is None or spec.loader is None:
        raise IntegrationFailure("Observer fixture helpers are unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RecoveryTestSupport:
    def __init__(self, binary: Path, observer: Path, state_root: Path, report_path: Path, registry_size: int) -> None:
        self._stand_guard = StandGuard()
        self.binary = binary
        self.observer = observer
        self.state_root = state_root
        self.report_path = report_path
        self.registry_size = registry_size
        self.started_at = time.monotonic()
        self.server: subprocess.Popen[str] | None = None
        self.server_log: Path | None = None
        self.active_check: str | None = None
        self.checks: list[dict[str, Any]] = []
        self.metrics: dict[str, float | int | str] = {}
        self.admin_token = ""
        self.reader_token = ""
        self.old_server_id = ""
        self.server_id = ""
        self.initial_plan: dict[str, Any] | None = None
        self.initial_receipt_bytes = ""
        self.initial_storage: dict[str, str] = {}
        self.backup_path: Path | None = None
        self.private_paths: list[Path] = []
        self._fixture_temporary: tempfile.TemporaryDirectory[str] | None = None
        self.fixture_module: ModuleType | None = None
        self.fixture: Any = None
        self.initial_entry: dict[str, Any] | None = None
        self.old_audit_plan: dict[str, Any] | None = None

    def record(self, name: str, operation: Callable[[], None]) -> None:
        self.active_check = name
        started = time.monotonic()
        try:
            operation()
        except BaseException as error:
            self.checks.append({"name": name, "ok": False, "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
                                "failure": type(error).__name__, "assertion": str(error)})
            raise
        else:
            self.checks.append({"name": name, "ok": True, "elapsed_ms": round((time.monotonic() - started) * 1000, 3)})
        finally:
            self.active_check = None

    @property
    def process_environment(self) -> dict[str, str]:
        environment = os.environ.copy()
        environment["SHIN_SERVER_INFRA_DIR"] = str((SERVER_DIR / "infra").resolve())
        return environment

    @property
    def observer_environment(self) -> dict[str, str]:
        environment = {"PATH": "", "LC_ALL": os.environ.get("LC_ALL", "C")}
        for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS", "ASAN_SYMBOLIZER_PATH"):
            if name in os.environ:
                environment[name] = os.environ[name]
        return environment

    @staticmethod
    def assert_no_sanitizer_output(output: str, context: str) -> None:
        lowered = output.lower()
        if "addresssanitizer" in lowered or "undefinedbehaviorsanitizer" in lowered or "leaksanitizer" in lowered:
            raise IntegrationFailure(f"{context} emitted a sanitizer diagnostic")

    def run_process(
        self,
        command: list[str],
        name: str,
        *,
        timeout: float = 45.0,
        input_bytes: bytes | None = None,
        environment: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[bytes]:
        arguments: dict[str, Any] = {
            "check": False,
            "stdout": subprocess.PIPE,
            "stderr": subprocess.PIPE,
            "timeout": timeout,
            "env": environment or self.process_environment,
        }
        if input_bytes is None:
            arguments["stdin"] = subprocess.DEVNULL
        else:
            arguments["input"] = input_bytes
        completed = subprocess.run(command, **arguments)
        self.assert_no_sanitizer_output(completed.stderr.decode("utf-8", errors="replace"), name)
        if completed.returncode:
            raise IntegrationFailure(f"{name} failed with exit status {completed.returncode}")
        return completed

    def compose_prefix(self) -> list[str]:
        return [
            "docker", "compose", "--project-name", PROJECT_NAME, "--project-directory", str(REPOSITORY_ROOT),
            "--env-file", str(self.state_root / "compose.env"), "--file", str(COMPOSE_FILE),
        ]

    def compose(self, *arguments: str, timeout: float = 45.0, input_bytes: bytes | None = None) -> subprocess.CompletedProcess[bytes]:
        return self.run_process([*self.compose_prefix(), *arguments], f"Docker Compose {arguments[0]}", timeout=timeout,
                                input_bytes=input_bytes)

    def compose_database(self, *arguments: str, timeout: float = 45.0, input_bytes: bytes | None = None) -> subprocess.CompletedProcess[bytes]:
        return self.compose("exec", "-T", "postgres", *arguments, timeout=timeout, input_bytes=input_bytes)

    def psql(self, statement: str) -> str:
        completed = self.compose_database(
            "psql", "-X", "-v", "ON_ERROR_STOP=1", "-U", "shin_migrator", "-d", DATABASE_NAME, "-Atq", "-c", statement,
        )
        return completed.stdout.decode("utf-8").strip()

    def scalar(self, statement: str) -> str:
        value = self.psql(statement)
        if "\n" in value:
            raise IntegrationFailure("SQL assertion returned more than one row")
        return value

    def dev_command(self, *arguments: str) -> None:
        self.run_process([sys.executable, str(DEV_TOOL), "--state-root", str(self.state_root), *arguments], "developer helper")

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

    def require_http_port_free(self) -> None:
        try:
            require_http_port_available(HOST, PORT)
        except OSError as error:
            raise IntegrationFailure("HTTP port 19125 is occupied; refusing to reset the dedicated schema") from error

    def guard_database(self) -> None:
        if self.scalar("SELECT current_database()") != DATABASE_NAME:
            raise IntegrationFailure("refusing to operate outside shin_server_dev")
        if self.scalar("SELECT current_setting('server_version_num') <> ''") != "t":
            raise IntegrationFailure("PostgreSQL identity guard failed")

    def reset_schema(self) -> None:
        self.guard_database()
        self.psql("DROP SCHEMA IF EXISTS shin_server CASCADE")

    def migrate(self) -> None:
        self.run_process([str(self.binary), "migrate", "--config", str(self.state_root / "config" / "migrator.json")],
                         "explicit migration", timeout=60.0)
        versions = self.scalar("SELECT string_agg(version::text, ',' ORDER BY version) FROM shin_server.schema_migrations")
        if versions != "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35":
            raise IntegrationFailure("migration did not install the recovery schema")

    def read_private_file(self, path: Path, maximum: int = 65536) -> bytes:
        entry = path.lstat()
        if stat.S_ISLNK(entry.st_mode) or not stat.S_ISREG(entry.st_mode) or stat.S_IMODE(entry.st_mode) != 0o600:
            raise IntegrationFailure("private test state has an unsafe file")
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor, "rb") as handle:
            value = handle.read(maximum + 1)
        if len(value) > maximum:
            raise IntegrationFailure("private test state exceeds its bounded size")
        return value

    def private_json(self, path: Path) -> dict[str, Any]:
        try:
            value = json.loads(self.read_private_file(path).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise IntegrationFailure("private test state is not valid JSON") from error
        if not isinstance(value, dict):
            raise IntegrationFailure("private test state is not a JSON object")
        return value

    def write_private_json(self, name: str, value: dict[str, Any]) -> Path:
        path = self.state_root / "config" / f"{name}-{secrets.token_hex(8)}.json"
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(compact_json(value) + b"\n")
        self.private_paths.append(path)
        return path

    def read_token(self, name: str) -> str:
        value = self.read_private_file(self.state_root / "secrets" / name, 512).decode("ascii").strip()
        if not value:
            raise IntegrationFailure("development credential is empty")
        return value

    def runtime_server_id(self, config_path: Path | None = None) -> str:
        config = self.private_json(config_path or self.state_root / "config" / "runtime.json")
        database = config.get("database")
        if not isinstance(database, dict):
            raise IntegrationFailure("runtime configuration has no database binding")
        return require_hex(database.get("server_id"), 32, "runtime configuration has no Server identity pin")

    def start_server(self, config_path: Path | None = None) -> None:
        if self.server is not None:
            raise IntegrationFailure("server is already running")
        log_directory = self.state_root / "reports"
        entry = log_directory.lstat()
        if stat.S_ISLNK(entry.st_mode) or not stat.S_ISDIR(entry.st_mode) or stat.S_IMODE(entry.st_mode) != 0o700:
            raise IntegrationFailure("report directory is unsafe")
        self.server_log = log_directory / f"recovery-server-{secrets.token_hex(8)}.log"
        descriptor = os.open(self.server_log, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as log:
            self.server = subprocess.Popen(
                [str(self.binary), "serve", "--config", str(config_path or self.state_root / "config" / "runtime.json")],
                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, text=True, start_new_session=True,
                env=self.process_environment,
            )
        self.wait_for_response("/health/live", 200, include_server_identity=False)

    def stop_server(self, *, expect_clean: bool) -> None:
        if self.server is None:
            return
        process = self.server
        self.server = None
        process.terminate()
        try:
            exit_status = process.wait(timeout=10.0)
        except subprocess.TimeoutExpired as error:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5.0)
            raise IntegrationFailure("server did not terminate after SIGTERM") from error
        log = self.server_log.read_text(encoding="utf-8", errors="replace") if self.server_log is not None else ""
        self.assert_no_sanitizer_output(log, "server")
        if expect_clean and exit_status != 0:
            raise IntegrationFailure("server did not exit cleanly")

    def auth_headers(self, role: str) -> dict[str, str]:
        return {"Authorization": f"Bearer {self.admin_token if role == 'admin' else self.reader_token}"}

    def request(
        self,
        method: str,
        target: str,
        *,
        headers: dict[str, str] | None = None,
        body: bytes | None = None,
        include_server_identity: bool = True,
    ) -> tuple[int, dict[str, Any]]:
        request_headers = {"Host": f"{HOST}:{PORT}", **(headers or {})}
        if include_server_identity and self.server_id and not any(key.lower() == IDENTITY_HEADER.lower() for key in request_headers):
            request_headers[IDENTITY_HEADER] = self.server_id
        connection = http.client.HTTPConnection(HOST, PORT, timeout=5.0)
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
            value = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise IntegrationFailure("server response was not a JSON envelope") from error
        self.assert_envelope(value)
        return status, value

    @staticmethod
    def assert_envelope(value: Any) -> None:
        if not isinstance(value, dict) or value.get("schema_version") != "shin.server-response.v1" or \
                value.get("tool") != "shin-server" or value.get("version") != SERVER_VERSION or \
                value.get("stability") != "development" or not isinstance(value.get("ok"), bool):
            raise IntegrationFailure("server response envelope is malformed")
        if value["ok"]:
            if not isinstance(value.get("result"), dict) or "error" in value:
                raise IntegrationFailure("server success envelope is malformed")
        else:
            error = value.get("error")
            if not isinstance(error, dict) or not isinstance(error.get("code"), str) or "result" in value:
                raise IntegrationFailure("server failure envelope is malformed")

    def expect_status(self, expected: int, method: str, target: str, **kwargs: Any) -> dict[str, Any]:
        status, value = self.request(method, target, **kwargs)
        if status != expected:
            raise IntegrationFailure(f"expected HTTP {expected}, got {status}")
        return value

    def wait_for_response(self, target: str, expected: int, *, include_server_identity: bool = True,
                          headers: dict[str, str] | None = None) -> dict[str, Any]:
        deadline = time.monotonic() + 20.0
        last: Exception | None = None
        while time.monotonic() < deadline:
            try:
                status, response = self.request("GET", target, headers=headers, include_server_identity=include_server_identity)
                if status == expected:
                    return response
                last = IntegrationFailure(f"expected HTTP {expected}, got {status}")
            except IntegrationFailure as error:
                last = error
            time.sleep(0.2)
        raise IntegrationFailure("server did not become ready for the expected response") from last

    def post_json(self, target: str, body: dict[str, Any], *, headers: dict[str, str]) -> tuple[int, dict[str, Any]]:
        return self.request("POST", target, headers={**headers, "Content-Type": "application/json"}, body=compact_json(body))

    def cli(self, command: str, config: Path, input_path: Path, *, expect_success: bool) -> dict[str, Any]:
        completed = subprocess.run(
            [str(self.binary), command, "--config", str(config), "--input", str(input_path)],
            check=False, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45.0,
            env=self.process_environment,
        )
        stdout = completed.stdout.decode("utf-8", errors="replace")
        stderr = completed.stderr.decode("utf-8", errors="replace")
        self.assert_no_sanitizer_output(stdout + stderr, f"{command} CLI")
        if expect_success != (completed.returncode == 0):
            raise IntegrationFailure(f"{command} did not return the expected exit status")
        if expect_success and stderr:
            raise IntegrationFailure(f"{command} wrote stderr on success")
        if not expect_success and stdout:
            raise IntegrationFailure(f"{command} wrote stdout on failure")
        try:
            value = json.loads(stdout if expect_success else stderr)
        except json.JSONDecodeError as error:
            raise IntegrationFailure(f"{command} did not return one JSON envelope") from error
        self.assert_envelope(value)
        return value

    def cli_error(self, command: str, config: Path, input_path: Path, code: str | None = None) -> None:
        result = self.cli(command, config, input_path, expect_success=False)
        if result.get("ok") is not False:
            raise IntegrationFailure(f"{command} unexpectedly succeeded")
        if code is not None and result.get("error", {}).get("code") != code:
            raise IntegrationFailure(f"{command} returned the wrong public error code")

    def recovery_request(self) -> dict[str, Any]:
        return {
            "schema_version": "shin.server-recovery-request.v1",
            "operation_id": "13" * 16,
            "expected_server_id": self.old_server_id,
            "reason_code": "backup-restore",
            "service_fenced": True,
        }

    def write_recovery_input(self, value: dict[str, Any]) -> Path:
        path = self.write_private_json("recovery-input", value)
        return path

    def create_observer_fixture(self) -> None:
        self.fixture_module = load_observer_fixture_module()
        self._fixture_temporary = tempfile.TemporaryDirectory(prefix="shin-recovery-observer-")
        root = Path(self._fixture_temporary.name).resolve()
        self.fixture = self.fixture_module.Fixture(root, "recovering-observer")
        config = self.fixture.config_data()
        config.update(project_id="visible", workspace_id="recovery-observer", observer_id="recovery-observer")
        config["outbox"] = {"enabled": True, "max_entries": 8, "max_bytes": 65536}
        self.fixture.write_config(config)
        state = self.sample_observer(self.fixture)
        entries = state.get("outbox", {}).get("entries")
        if not isinstance(entries, list) or len(entries) != 1 or not isinstance(entries[0], dict):
            raise IntegrationFailure("Observer did not create one initial local packet")
        self.initial_entry = json.loads(json.dumps(entries[0]))

    def cleanup_observer_fixture(self) -> None:
        if self._fixture_temporary is not None:
            self._fixture_temporary.cleanup()
        self._fixture_temporary = None
        self.fixture = None
        self.fixture_module = None

    def sample_observer(self, fixture: Any) -> dict[str, Any]:
        completed = subprocess.run([str(self.observer), "sample", "--config", str(fixture.config)], check=False,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   timeout=45.0, env=self.observer_environment)
        stdout = completed.stdout.decode("utf-8", errors="replace")
        stderr = completed.stderr.decode("utf-8", errors="replace")
        self.assert_no_sanitizer_output(stdout + stderr, "Observer sample")
        if completed.returncode or stderr:
            raise IntegrationFailure("Observer sample did not complete cleanly")
        try:
            envelope = json.loads(stdout)
        except (KeyError, TypeError, json.JSONDecodeError) as error:
            raise IntegrationFailure("Observer sample did not return a success envelope") from error
        if self.fixture_module is None:
            raise IntegrationFailure("Observer fixture envelope validator is unavailable")
        try:
            result = self.fixture_module.assert_observer_envelope(envelope, expected_ok=True)
        except self.fixture_module.IntegrationFailure as error:
            raise IntegrationFailure("Observer sample did not return the current complete success envelope") from error
        state = result.get("state", result) if isinstance(result, dict) else None
        if not isinstance(state, dict):
            raise IntegrationFailure("Observer sample did not return persisted state")
        return state

    @staticmethod
    def enrollment(entry: dict[str, Any]) -> dict[str, Any]:
        payload = entry.get("payload")
        if not isinstance(payload, dict):
            raise IntegrationFailure("Observer packet has no native payload")
        required = {"project_id", "workspace_id", "observer_id", "source_host_id", "profile", "workspace_binding_digest"}
        if not required.issubset(payload):
            raise IntegrationFailure("Observer packet cannot be explicitly enrolled")
        return {
            "stream_id": require_hex(entry.get("stream_id"), 32, "Observer packet has an invalid stream identity"),
            "project_id": payload["project_id"], "workspace_id": payload["workspace_id"], "principal_id": "dev-reader",
            "observer_id": payload["observer_id"], "source_host_id": payload["source_host_id"], "profile": payload["profile"],
            "workspace_binding_digest": payload["workspace_binding_digest"],
        }
