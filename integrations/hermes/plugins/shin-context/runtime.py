"""Bounded subprocess transport for the Shin CLI."""

from __future__ import annotations

import json
import subprocess
from dataclasses import asdict, dataclass
from typing import Any, Mapping, Sequence

from .model import (
    ADAPTER_VERSION,
    RESULT_SCHEMA,
    AdapterFailure,
    CommandResult,
    Installation,
    Route,
)
from .resolution import route_arguments


MAX_OUTPUT_BYTES = 2 * 1024 * 1024
CORE_TOOL = "shin-context"
CORE_ERROR_SCHEMA = "shin.error.v1"


def _reject_duplicate_keys(pairs: Sequence[tuple[str, Any]]) -> Mapping[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate JSON object key")
        value[key] = item
    return value


def _reject_nonfinite_number(value: str) -> None:
    raise ValueError(f"non-finite JSON number: {value}")


def _decode_json(document: str) -> Any:
    return json.loads(
        document,
        object_pairs_hook=_reject_duplicate_keys,
        parse_constant=_reject_nonfinite_number,
    )


def _check_stream_size(stream: str, value: str) -> None:
    if len(value.encode("utf-8")) > MAX_OUTPUT_BYTES:
        raise AdapterFailure(
            "SHIN_OUTPUT_TOO_LARGE",
            f"The Shin {stream} stream exceeded the Hermes adapter output ceiling.",
            remediation="Use compact Brief or reduce the Agent Context Pack budget.",
            details={"stream": stream, "max_output_bytes": MAX_OUTPUT_BYTES},
        )


@dataclass(frozen=True)
class CoreResultDescriptor:
    """The exact Core envelope a Hermes call site is prepared to consume."""

    command: str
    schema_version: str


DISCOVER_RESULT = CoreResultDescriptor("discover", "shin.discovery.v1")
READY_RESULT = CoreResultDescriptor("ready", "shin.agent-readiness.v1")
BRIEF_RESULT = CoreResultDescriptor("brief", "shin.context-brief-compact.v1")
PACK_RESULT = CoreResultDescriptor("pack", "shin.agent-context-pack-compact.v1")
DIAGNOSE_RESULT = CoreResultDescriptor("diagnose", "shin.diagnose.v1")
WORKTREE_DIFF_RESULT = CoreResultDescriptor("worktree-diff", "shin.worktree-diff.v1")
SUGGEST_PROFILE_RESULT = CoreResultDescriptor(
    "suggest-profile", "shin.profile-suggestion.v1"
)


def _invalid_output(message: str, *, exit_code: int) -> AdapterFailure:
    return AdapterFailure(
        "INVALID_SHIN_OUTPUT",
        message,
        remediation="Run shin-context --version directly and inspect the installation.",
        details={"exit_code": exit_code},
    )


def _validate_result(
    payload: Any, expected: CoreResultDescriptor, *, exit_code: int
) -> Mapping[str, Any]:
    if not isinstance(payload, dict):
        raise _invalid_output(
            "The Shin subprocess returned JSON that is not an object.",
            exit_code=exit_code,
        )
    if payload.get("schema_version") != expected.schema_version:
        raise _invalid_output(
            "The Shin subprocess returned an unexpected result schema.",
            exit_code=exit_code,
        )
    if payload.get("tool") != CORE_TOOL:
        raise _invalid_output(
            "The Shin subprocess returned an unexpected result identity.",
            exit_code=exit_code,
        )
    if payload.get("command") != expected.command:
        raise _invalid_output(
            "The Shin subprocess returned a result for a different command.",
            exit_code=exit_code,
        )
    if type(payload.get("ok")) is not bool:
        raise _invalid_output(
            "The Shin subprocess result is missing a boolean ok field.",
            exit_code=exit_code,
        )
    if payload["ok"] and exit_code != 0:
        raise _invalid_output(
            "The Shin subprocess reported success with a nonzero exit state.",
            exit_code=exit_code,
        )
    return payload


def _structured_error(
    stderr: str, expected: CoreResultDescriptor, *, exit_code: int
) -> Mapping[str, Any] | None:
    try:
        payload = _decode_json(stderr)
    except (json.JSONDecodeError, ValueError):
        return None
    if not isinstance(payload, dict):
        return None
    if (
        payload.get("schema_version") != CORE_ERROR_SCHEMA
        or payload.get("tool") != CORE_TOOL
        or payload.get("command") != expected.command
        or payload.get("ok") is not False
        or exit_code == 0
    ):
        return None
    error = payload.get("error")
    if not isinstance(error, dict):
        return None
    if (
        not all(
            isinstance(error.get(field), str) and error[field]
            for field in ("code", "category", "message", "remediation")
        )
        or type(error.get("exit_code")) is not int
        or error["exit_code"] != exit_code
        or type(error.get("retryable")) is not bool
    ):
        return None
    # Keep only the documented Core error fields; arbitrary child stderr is not a log channel.
    return {
        "schema_version": CORE_ERROR_SCHEMA,
        "tool": CORE_TOOL,
        "command": expected.command,
        "ok": False,
        "error": {
            field: error[field]
            for field in (
                "code",
                "category",
                "message",
                "exit_code",
                "retryable",
                "remediation",
            )
        },
    }


def run(
    command: Sequence[str], timeout_seconds: int, expected: CoreResultDescriptor
) -> CommandResult:
    try:
        completed = subprocess.run(
            list(command),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_seconds,
            check=False,
            shell=False,
        )
    except subprocess.TimeoutExpired as error:
        raise AdapterFailure(
            "SHIN_TIMEOUT",
            "The bounded Shin subprocess timed out.",
            remediation="Inspect project size and the selected route before retrying.",
            details={"timeout_seconds": timeout_seconds},
        ) from error
    except OSError as error:
        raise AdapterFailure(
            "SHIN_PROCESS_FAILED",
            "The Shin subprocess could not be started.",
            remediation="Verify the configured executable and local permissions.",
            details={"reason": str(error)},
        ) from error

    _check_stream_size("stdout", completed.stdout)
    _check_stream_size("stderr", completed.stderr)
    output = completed.stdout.strip()
    if not output:
        error_payload = _structured_error(
            completed.stderr.strip(), expected, exit_code=completed.returncode
        )
        if error_payload is None:
            raise _invalid_output(
                "The Shin subprocess did not return one valid JSON result.",
                exit_code=completed.returncode,
            )
        return CommandResult(
            ok=False,
            exit_code=completed.returncode,
            payload=error_payload,
            stderr="",
        )
    try:
        payload = _decode_json(output)
    except (json.JSONDecodeError, ValueError) as error:
        raise _invalid_output(
            "The Shin subprocess did not return one JSON result.",
            exit_code=completed.returncode,
        ) from error
    payload = _validate_result(payload, expected, exit_code=completed.returncode)
    return CommandResult(
        ok=completed.returncode == 0 and payload["ok"],
        exit_code=completed.returncode,
        payload=payload,
        stderr="",
    )


def command_envelope(
    operation: str,
    result: CommandResult,
    *,
    route: Route | None = None,
    readiness: CommandResult | None = None,
    next_action: str,
) -> Mapping[str, Any]:
    envelope: dict[str, Any] = {
        "schema_version": RESULT_SCHEMA,
        "adapter_version": ADAPTER_VERSION,
        "operation": operation,
        "ok": result.ok,
        "result": result.payload,
        "process": {
            "exit_code": result.exit_code,
            "stderr": result.stderr,
        },
        "next_action": next_action,
    }
    if route is not None:
        envelope["route"] = asdict(route)
    if readiness is not None:
        envelope["readiness"] = readiness.payload
    if not result.ok:
        core_payload = result.payload if isinstance(result.payload, dict) else {}
        core_error = core_payload.get("error")
        if core_payload.get("schema_version") == CORE_ERROR_SCHEMA and isinstance(core_error, dict):
            envelope["error"] = core_error
        else:
            envelope["error"] = {
                "code": "SHIN_COMMAND_REJECTED",
                "message": "Shin rejected the selected read-only operation.",
                "remediation": next_action,
            }
    return envelope


def ready(
    installation: Installation, route: Route, expected: CoreResultDescriptor
) -> CommandResult:
    return run(
        route_arguments(installation, route)
        + ["ready", "--marker", route.marker, "--strict"],
        120,
        expected,
    )


def ready_then(
    operation: str,
    installation: Installation,
    route: Route,
    command: Sequence[str],
    timeout_seconds: int,
    expected: CoreResultDescriptor,
) -> Mapping[str, Any]:
    readiness = ready(installation, route, READY_RESULT)
    if not readiness.ok:
        return command_envelope(
            operation,
            readiness,
            route=route,
            next_action=(
                "Use shin_worktree_diff for unpublished drift or rebuild the same "
                "external index through an explicit terminal workflow."
            ),
        )
    result = run(command, timeout_seconds, expected)
    return command_envelope(
        operation,
        result,
        route=route,
        readiness=readiness,
        next_action=(
            "Open and verify the returned repository-relative evidence paths."
            if result.ok
            else "Inspect the returned Shin diagnostic without broad scanning."
        ),
    )
