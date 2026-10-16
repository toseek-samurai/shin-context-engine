"""Shared result and error types for the Hermes adapter."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Mapping


ADAPTER_VERSION = "1.0.1"
RESULT_SCHEMA = "shin.hermes-result.v1"


class AdapterFailure(RuntimeError):
    def __init__(
        self,
        code: str,
        message: str,
        *,
        remediation: str,
        details: Mapping[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.remediation = remediation
        self.details = dict(details or {})


@dataclass(frozen=True)
class Installation:
    binary: Path
    profile_dir: Path
    source_home: Path | None


@dataclass(frozen=True)
class Route:
    root: str
    marker: str
    profile: str
    profile_dir: str
    index_key: str
    out_dir: str


@dataclass(frozen=True)
class CommandResult:
    ok: bool
    exit_code: int
    payload: Any
    stderr: str


def encode(value: Mapping[str, Any]) -> str:
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"))


def error_envelope(operation: str, failure: AdapterFailure) -> str:
    return encode(
        {
            "schema_version": RESULT_SCHEMA,
            "adapter_version": ADAPTER_VERSION,
            "operation": operation,
            "ok": False,
            "error": {
                "code": failure.code,
                "message": failure.message,
                "remediation": failure.remediation,
                "details": failure.details,
            },
        }
    )


def dispatch(
    operation: str,
    args: Mapping[str, Any] | None,
    action: Callable[[Mapping[str, Any]], Mapping[str, Any]],
) -> str:
    try:
        if not isinstance(args, Mapping):
            raise AdapterFailure(
                "INVALID_ARGUMENTS",
                "Hermes supplied a non-object tool argument.",
                remediation="Call the tool with its declared JSON object schema.",
            )
        return encode(action(args))
    except AdapterFailure as failure:
        return error_envelope(operation, failure)
    except Exception as error:  # Hermes handlers must never raise.
        return error_envelope(
            operation,
            AdapterFailure(
                "ADAPTER_INTERNAL_ERROR",
                f"Unexpected Shin adapter failure: {type(error).__name__}",
                remediation="Inspect the Hermes log and rerun the same read-only operation.",
            ),
        )
