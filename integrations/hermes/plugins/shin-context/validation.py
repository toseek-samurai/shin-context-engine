"""Bounded argument validation for Hermes-exposed Shin operations."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Mapping

from .model import AdapterFailure


MAX_QUERY_CHARS = 4096
MAX_REQUEST_BYTES = 128 * 1024


def bounded_string(args: Mapping[str, Any], name: str) -> str:
    value = args.get(name)
    if not isinstance(value, str) or not value.strip():
        raise AdapterFailure(
            "INVALID_ARGUMENT",
            f"{name} must be a non-empty string.",
            remediation=f"Pass a bounded {name}.",
        )
    value = value.strip()
    if len(value) > MAX_QUERY_CHARS:
        raise AdapterFailure(
            "ARGUMENT_TOO_LARGE",
            f"{name} exceeds the adapter character ceiling.",
            remediation=f"Split {name} into a smaller bounded request.",
            details={"max_chars": MAX_QUERY_CHARS},
        )
    return value


def bounded_int(
    args: Mapping[str, Any], name: str, default: int, minimum: int, maximum: int
) -> int:
    value = args.get(name, default)
    if isinstance(value, bool) or not isinstance(value, int):
        raise AdapterFailure(
            "INVALID_ARGUMENT",
            f"{name} must be an integer.",
            remediation=f"Pass {name} between {minimum} and {maximum}.",
        )
    if value < minimum or value > maximum:
        raise AdapterFailure(
            "ARGUMENT_OUT_OF_RANGE",
            f"{name} is outside the supported range.",
            remediation=f"Pass {name} between {minimum} and {maximum}.",
            details={"value": value, "minimum": minimum, "maximum": maximum},
        )
    return value


def relative_expected_path(args: Mapping[str, Any]) -> str:
    value = bounded_string(args, "expected_path")
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise AdapterFailure(
            "EXPECTED_PATH_OUTSIDE_ROOT",
            "expected_path must be repository-relative without traversal.",
            remediation="Pass the canonical repository-relative evidence path.",
        )
    return path.as_posix()


def task_request(value: Any) -> str:
    if not isinstance(value, dict):
        raise AdapterFailure(
            "INVALID_TASK_REQUEST",
            "task_request must be a JSON object.",
            remediation="Pass one bounded shin.task-request.v1 object.",
        )
    if value.get("schema_version") != "shin.task-request.v1":
        raise AdapterFailure(
            "INVALID_TASK_REQUEST",
            "task_request schema_version must be shin.task-request.v1.",
            remediation="Use the accepted Agent Context Pack request contract.",
        )
    budget = value.get("budget")
    if not isinstance(budget, dict):
        raise AdapterFailure(
            "INVALID_TASK_BUDGET",
            "task_request must declare a bounded budget object.",
            remediation="Declare byte and record ceilings before requesting a Pack.",
        )
    ceilings = {
        "max_evidence_bytes": 65536,
        "max_records": 32,
        "per_query_records": 12,
        "per_document_records": 4,
    }
    for field, ceiling in ceilings.items():
        current = budget.get(field)
        if isinstance(current, bool) or not isinstance(current, int) or current < 1:
            raise AdapterFailure(
                "INVALID_TASK_BUDGET",
                f"budget.{field} must be a positive integer.",
                remediation="Correct the Agent Context Pack budget.",
            )
        if current > ceiling:
            raise AdapterFailure(
                "TASK_BUDGET_TOO_LARGE",
                f"budget.{field} exceeds the Hermes adapter ceiling.",
                remediation="Reduce the task request or split it into multiple Packs.",
                details={"field": field, "value": current, "ceiling": ceiling},
            )
    serialized = json.dumps(value, ensure_ascii=True, separators=(",", ":"))
    if len(serialized.encode("utf-8")) > MAX_REQUEST_BYTES:
        raise AdapterFailure(
            "TASK_REQUEST_TOO_LARGE",
            "The serialized task request exceeds the adapter ceiling.",
            remediation="Split the task into smaller explicit requirements.",
            details={"max_request_bytes": MAX_REQUEST_BYTES},
        )
    return serialized
