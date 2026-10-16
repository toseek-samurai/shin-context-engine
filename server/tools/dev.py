#!/usr/bin/env python3
"""Create and operate the isolated local PostgreSQL state for Shin Server."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys
from typing import Any


PROJECT_NAME = "shin-server-dev"
DATABASE_NAME = "shin_server_dev"
RUNTIME_ROLE = "shin_runtime"
MIGRATOR_ROLE = "shin_migrator"
LISTEN_HOST = "127.0.0.1"
LISTEN_PORT = 19125
POSTGRES_PORT = 15432
SECRET_PATTERN = re.compile(r"[A-Za-z0-9_-]{32,128}")
SERVER_ID_PATTERN = re.compile(r"[0-9a-f]{32}\n")

SERVER_DIR = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = SERVER_DIR.parent
BUILD_ROOT = REPOSITORY_ROOT / "build"
DEFAULT_STATE_ROOT = BUILD_ROOT / "server-dev"
COMPOSE_FILE = SERVER_DIR / "infra" / "compose.yml"


class DevStateError(RuntimeError):
    pass


def postgres_port() -> int:
    value = os.environ.get("SHIN_SERVER_DEV_POSTGRES_PORT", str(POSTGRES_PORT))
    if not re.fullmatch(r"[1-9][0-9]{0,4}", value) or int(value) > 65535:
        raise DevStateError("SHIN_SERVER_DEV_POSTGRES_PORT must be an integer in 1..65535")
    return int(value)


def _is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
    except ValueError:
        return False
    return True


def _state_root(value: str | None) -> Path:
    candidate = Path(value).expanduser() if value else DEFAULT_STATE_ROOT
    if candidate.is_symlink():
        raise DevStateError("state root must not be a symbolic link")
    root = candidate.resolve(strict=False)
    build_root = BUILD_ROOT.resolve(strict=False)
    if not _is_within(root, build_root):
        raise DevStateError("state root must be below this worktree's build directory")
    return root


def _lstat(path: Path) -> os.stat_result:
    try:
        return path.lstat()
    except FileNotFoundError as error:
        raise DevStateError(f"required state entry is missing: {path.name}") from error


def _require_directory(path: Path, mode: int) -> None:
    entry = _lstat(path)
    if stat.S_ISLNK(entry.st_mode) or not stat.S_ISDIR(entry.st_mode):
        raise DevStateError(f"state entry must be a directory: {path.name}")
    if stat.S_IMODE(entry.st_mode) != mode:
        raise DevStateError(f"state directory has unsafe mode: {path.name}")


def _require_regular_private_file(path: Path) -> None:
    entry = _lstat(path)
    if stat.S_ISLNK(entry.st_mode) or not stat.S_ISREG(entry.st_mode):
        raise DevStateError(f"state entry must be a regular file: {path.name}")
    if stat.S_IMODE(entry.st_mode) != 0o600:
        raise DevStateError(f"state file has unsafe mode: {path.name}")


def _mkdir_private(path: Path) -> None:
    try:
        path.mkdir(mode=0o700)
    except FileExistsError:
        _require_directory(path, 0o700)
    else:
        os.chmod(path, 0o700)


def _write_new_private_file(path: Path, content: str) -> bool:
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        _require_regular_private_file(path)
        return False

    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
    except BaseException:
        path.unlink(missing_ok=True)
        raise
    return True


def _read_private_file(path: Path) -> str:
    _require_regular_private_file(path)
    return path.read_text(encoding="utf-8")


def _ensure_secret(path: Path) -> str:
    if not path.exists():
        _write_new_private_file(path, f"{secrets.token_urlsafe(36)}\n")
    value = _read_private_file(path).strip()
    if not SECRET_PATTERN.fullmatch(value):
        raise DevStateError(f"state secret is malformed: {path.name}")
    return value


def _ensure_server_id(path: Path) -> str:
    if not path.exists():
        _write_new_private_file(path, f"{secrets.token_hex(16)}\n")
    value = _read_private_file(path)
    if not SERVER_ID_PATTERN.fullmatch(value):
        raise DevStateError(f"state server identity is malformed: {path.name}")
    return value[:-1]


def _sha256(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _conninfo(*, role: str, password: str) -> str:
    # Generated secrets are limited to the unambiguous libpq keyword-value subset.
    # This loopback SCRAM stand has no Kerberos realm or GSS encryption service.
    return (
        f"host={LISTEN_HOST} port={postgres_port()} dbname={DATABASE_NAME} "
        f"user={role} password={password} gssencmode=disable\n"
    )


def _runtime_config(
    state_root: Path,
    connection_file: Path,
    admin_token: str,
    reader_token: str,
    server_id: str,
) -> dict[str, Any]:
    roots = state_root / "fixtures" / "roots"
    database = {
        "connection_file": str(connection_file),
        "expected_name": DATABASE_NAME,
        "runtime_role": RUNTIME_ROLE,
        "server_id": server_id,
    }
    return {
        "schema_version": "shin.server-config.v10",
        "source_host_id": "local-dev",
        "listen": {"host": LISTEN_HOST, "port": LISTEN_PORT},
        "database": database,
        "allowed_roots": [str(roots)],
        "principals": [
            {
                "id": "dev-admin",
                "role": "admin",
                "token_sha256": _sha256(admin_token),
                "project_ids": [],
            },
            {
                "id": "dev-reader",
                "role": "reader",
                "token_sha256": _sha256(reader_token),
                "project_ids": ["visible"],
            },
        ],
        "retrieval": {
            "routes": [],
            "max_payload_bytes": 512 * 1024 * 1024,
            "max_generation_bytes": 128 * 1024 * 1024,
            "max_file_bytes": 64 * 1024 * 1024,
            "max_generations": 8,
        },
        "preparations": {
            "enabled": False,
            "state_dir": str(state_root / "preparations"),
            "max_retained_jobs": 8,
            "lease_seconds": 30,
        },
        "knowledge": {
            "enabled": False,
            "principal_id": "",
            "route_ids": [],
            "interval_seconds": 60,
            "max_age_seconds": 180,
            "prepare_on_drift": False,
        },
        "journal_writer": {
            "enabled": False,
            "principal_id": "",
            "interval_ms": 1000,
            "max_retry_ms": 30000,
            "flush_on_shutdown": True,
        },
        "observability": {
            "max_retained_requests": 128,
            "max_query_bytes": 4096,
            "capture_query_projects": [],
        },
        "workers": 4,
        "queue_limit": 64,
    }


def _recovery_config(connection_file: Path, server_id: str) -> dict[str, Any]:
    return {
        "schema_version": "shin.server-recovery-config.v1",
        "database": {
            "connection_file": str(connection_file),
            "expected_name": DATABASE_NAME,
            "runtime_role": RUNTIME_ROLE,
            "server_id": server_id,
        },
    }


def _ensure_exact_private_file(path: Path, content: str) -> None:
    created = _write_new_private_file(path, content)
    if created:
        return
    if _read_private_file(path) != content:
        raise DevStateError(f"existing state file does not match this state: {path.name}")


def _replace_private_file(path: Path, content: str) -> None:
    temporary = path.with_name(f".{path.name}.{secrets.token_hex(8)}.tmp")
    temporary_created = False
    try:
        temporary_created = _write_new_private_file(temporary, content)
        if not temporary_created:
            raise DevStateError("could not allocate a private replacement state file")
        os.replace(temporary, path)
    finally:
        if temporary_created:
            temporary.unlink(missing_ok=True)


def _ensure_compose_environment(state_root: Path) -> None:
    path = state_root / "compose.env"
    legacy = f"SHIN_SERVER_DEV_STATE_DIR={state_root}\n"
    current = (
        legacy
        + f"SHIN_SERVER_INFRA_DIR={(SERVER_DIR / 'infra').resolve()}\n"
    )
    original_current = current
    if postgres_port() != POSTGRES_PORT:
        current += f"SHIN_SERVER_DEV_POSTGRES_PORT={postgres_port()}\n"
    created = _write_new_private_file(path, current)
    if created:
        return
    existing = _read_private_file(path)
    if existing == current:
        return
    if existing in (legacy, original_current):
        _replace_private_file(path, current)
        return
    raise DevStateError(
        "compose.env is not a recognized generated version; delete only compose.env, then run init again"
    )


def initialize(state_root: Path) -> None:
    if BUILD_ROOT.exists():
        entry = _lstat(BUILD_ROOT)
        if stat.S_ISLNK(entry.st_mode) or not stat.S_ISDIR(entry.st_mode):
            raise DevStateError("worktree build root is not a directory")
    else:
        BUILD_ROOT.mkdir(mode=0o700)
    _mkdir_private(state_root)
    secrets_dir = state_root / "secrets"
    config_dir = state_root / "config"
    conninfo_dir = state_root / "conninfo"
    fixtures_dir = state_root / "fixtures"
    roots_dir = fixtures_dir / "roots"
    reports_dir = state_root / "reports"
    preparations_dir = state_root / "preparations"
    for directory in (secrets_dir, config_dir, conninfo_dir, fixtures_dir, roots_dir, reports_dir, preparations_dir):
        _mkdir_private(directory)
    for fixture in ("visible", "other", "concurrent", "rollback"):
        _mkdir_private(roots_dir / fixture)
    _mkdir_private(fixtures_dir / "outside")

    migrator_password = _ensure_secret(secrets_dir / "postgres_migrator_password")
    runtime_password = _ensure_secret(secrets_dir / "shin_runtime_password")
    admin_token = _ensure_secret(secrets_dir / "dev_admin.token")
    reader_token = _ensure_secret(secrets_dir / "dev_reader.token")
    server_id = _ensure_server_id(config_dir / "server-id")

    runtime_conninfo = conninfo_dir / "runtime.conninfo"
    migrator_conninfo = conninfo_dir / "migrator.conninfo"
    _ensure_exact_private_file(
        runtime_conninfo,
        _conninfo(role=RUNTIME_ROLE, password=runtime_password),
    )
    _ensure_exact_private_file(
        migrator_conninfo,
        _conninfo(role=MIGRATOR_ROLE, password=migrator_password),
    )

    runtime_config = _runtime_config(state_root, runtime_conninfo, admin_token, reader_token, server_id)
    migrator_config = _runtime_config(state_root, migrator_conninfo, admin_token, reader_token, server_id)
    recovery_config = _recovery_config(migrator_conninfo, server_id)
    _ensure_exact_private_file(
        config_dir / "runtime.json",
        f"{json.dumps(runtime_config, sort_keys=True, separators=(',', ':'))}\n",
    )
    _ensure_exact_private_file(
        config_dir / "migrator.json",
        f"{json.dumps(migrator_config, sort_keys=True, separators=(',', ':'))}\n",
    )
    _ensure_exact_private_file(
        config_dir / "recovery.json",
        f"{json.dumps(recovery_config, sort_keys=True, separators=(',', ':'))}\n",
    )
    _ensure_compose_environment(state_root)


def _require_initialized(state_root: Path) -> None:
    _require_directory(state_root, 0o700)
    for directory in ("secrets", "config", "conninfo", "fixtures", "reports", "preparations"):
        _require_directory(state_root / directory, 0o700)
    _require_directory(state_root / "fixtures" / "roots", 0o700)
    for name in ("runtime.json", "migrator.json", "recovery.json"):
        _require_regular_private_file(state_root / "config" / name)
    _require_regular_private_file(state_root / "config" / "server-id")
    for name in ("runtime.conninfo", "migrator.conninfo"):
        _require_regular_private_file(state_root / "conninfo" / name)
    for name in ("postgres_migrator_password", "shin_runtime_password", "dev_admin.token", "dev_reader.token"):
        _require_regular_private_file(state_root / "secrets" / name)
    _require_regular_private_file(state_root / "compose.env")


def _compose_command(state_root: Path) -> list[str]:
    docker = shutil.which("docker")
    if docker is None:
        raise DevStateError("docker is required for the isolated PostgreSQL service")
    if not COMPOSE_FILE.is_file():
        raise DevStateError("server Docker Compose definition is missing")
    return [
        docker,
        "compose",
        "--project-name",
        PROJECT_NAME,
        "--project-directory",
        str(REPOSITORY_ROOT),
        "--env-file",
        str(state_root / "compose.env"),
        "--file",
        str(COMPOSE_FILE),
    ]


def _run_compose(
    state_root: Path,
    args: list[str],
    *,
    allow_failure: bool = False,
    capture_output: bool = False,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["SHIN_SERVER_DEV_POSTGRES_PORT"] = str(postgres_port())
    environment["SHIN_SERVER_INFRA_DIR"] = str((SERVER_DIR / "infra").resolve())
    completed = subprocess.run(
        [*_compose_command(state_root), *args],
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE if capture_output else None,
        stderr=subprocess.PIPE if capture_output else None,
        text=True,
        env=environment,
    )
    if completed.returncode and not allow_failure:
        raise DevStateError("Docker Compose command failed; inspect the dedicated service with status")
    return completed


def command_init(state_root: Path) -> None:
    initialize(state_root)
    print("initialized isolated Shin Server development state")


def command_up(state_root: Path) -> None:
    initialize(state_root)
    _run_compose(state_root, ["up", "--detach", "postgres"])
    print("started isolated Shin Server PostgreSQL service")


def command_down(state_root: Path) -> None:
    if not state_root.exists():
        print("isolated Shin Server development state is not initialized")
        return
    _require_initialized(state_root)
    _run_compose(state_root, ["down", "--remove-orphans"])
    print("stopped isolated Shin Server PostgreSQL service; retained its named volume")


def command_status(state_root: Path) -> None:
    if not state_root.exists():
        print("isolated Shin Server development state is not initialized")
        return
    _require_initialized(state_root)
    result = _run_compose(
        state_root,
        ["ps", "--format", "json"],
        allow_failure=True,
        capture_output=True,
    )
    if result.returncode:
        raise DevStateError("Docker Compose status command failed")
    output = result.stdout.strip()
    if not output:
        print("no isolated Shin Server containers")
        return
    try:
        decoded = json.loads(output)
    except json.JSONDecodeError as error:
        raise DevStateError("Docker Compose returned an invalid status response") from error
    entries = decoded if isinstance(decoded, list) else [decoded]
    services = [
        {
            "service": entry.get("Service"),
            "state": entry.get("State"),
            "health": entry.get("Health"),
        }
        for entry in entries
        if isinstance(entry, dict)
    ]
    print(json.dumps({"project": PROJECT_NAME, "services": services}, separators=(",", ":")))


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-root", help="private state directory below this worktree's build directory")
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("init", "up", "down", "status", "port"):
        commands.add_parser(command)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        port = postgres_port()
        state_root = _state_root(args.state_root)
        if args.command == "port":
            print(port)
        elif args.command == "init":
            command_init(state_root)
        elif args.command == "up":
            command_up(state_root)
        elif args.command == "down":
            command_down(state_root)
        else:
            command_status(state_root)
    except DevStateError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
