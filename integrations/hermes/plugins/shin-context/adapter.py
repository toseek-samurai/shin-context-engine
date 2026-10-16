"""Hermes tool handlers for read-only Shin project context."""

from __future__ import annotations

import os
import tempfile
from pathlib import Path
from typing import Any, Mapping

from .model import dispatch
from .resolution import (
    existing_directory,
    existing_file,
    resolve_installation,
    resolve_route,
    route_arguments,
)
from .runtime import (
    BRIEF_RESULT,
    DIAGNOSE_RESULT,
    DISCOVER_RESULT,
    PACK_RESULT,
    READY_RESULT,
    SUGGEST_PROFILE_RESULT,
    WORKTREE_DIFF_RESULT,
    command_envelope,
    ready,
    ready_then,
    run,
)
from .validation import bounded_int, bounded_string, relative_expected_path, task_request


def _discover_action(args: Mapping[str, Any]) -> Mapping[str, Any]:
    root = existing_directory(args.get("root"), "root")
    installation = resolve_installation()
    result = run(
        [str(installation.binary), "--root", str(root), "discover"],
        60,
        DISCOVER_RESULT,
    )
    return command_envelope(
        "discover",
        result,
        next_action=(
            "Follow bootstrap.primary_profile without mutating the consumer repository."
            if result.ok
            else "Inspect the result and stop if the repository is not indexable."
        ),
    )


def discover_project(args: Mapping[str, Any], **_: Any) -> str:
    return dispatch("discover", args, _discover_action)


def _ready_action(args: Mapping[str, Any]) -> Mapping[str, Any]:
    installation, route = resolve_route(args)
    result = ready(installation, route, READY_RESULT)
    return command_envelope(
        "ready",
        result,
        route=route,
        next_action=(
            "Use shin_brief_context or shin_pack_context."
            if result.ok
            else "Use shin_worktree_diff or rebuild the exact selected index."
        ),
    )


def ready_context(args: Mapping[str, Any], **_: Any) -> str:
    return dispatch("ready", args, _ready_action)


def _brief_action(args: Mapping[str, Any]) -> Mapping[str, Any]:
    query = bounded_string(args, "query")
    limit = bounded_int(args, "limit", 10, 1, 20)
    per_doc = bounded_int(args, "per_doc", 2, 1, 4)
    installation, route = resolve_route(args)
    command = route_arguments(installation, route) + [
        "brief",
        query,
        "--limit",
        str(limit),
        "--per-doc",
        str(per_doc),
        "--projection",
        "compact",
        "--require-current",
    ]
    return ready_then("brief", installation, route, command, 60, BRIEF_RESULT)


def brief_context(args: Mapping[str, Any], **_: Any) -> str:
    return dispatch("brief", args, _brief_action)


def _pack_action(args: Mapping[str, Any]) -> Mapping[str, Any]:
    request = task_request(args.get("task_request"))
    jobs = bounded_int(args, "jobs", min(os.cpu_count() or 1, 4), 1, 16)
    installation, route = resolve_route(args)
    with tempfile.TemporaryDirectory(prefix="shin-hermes-pack-") as directory:
        request_path = Path(directory) / "task-request.json"
        request_path.write_text(request, encoding="utf-8")
        command = route_arguments(installation, route) + [
            "pack",
            "--input",
            str(request_path),
            "--jobs",
            str(jobs),
            "--projection",
            "compact",
            "--require-current",
            "--strict",
        ]
        return ready_then("pack", installation, route, command, 120, PACK_RESULT)


def pack_context(args: Mapping[str, Any], **_: Any) -> str:
    return dispatch("pack", args, _pack_action)


def _diagnose_action(args: Mapping[str, Any]) -> Mapping[str, Any]:
    query = bounded_string(args, "query")
    expected_path = relative_expected_path(args)
    limit = bounded_int(args, "limit", 10, 1, 20)
    per_doc = bounded_int(args, "per_doc", 2, 1, 4)
    installation, route = resolve_route(args)
    command = route_arguments(installation, route) + [
        "diagnose",
        query,
        "--expected-path",
        expected_path,
        "--limit",
        str(limit),
        "--per-doc",
        str(per_doc),
        "--require-current",
    ]
    return ready_then("diagnose", installation, route, command, 60, DIAGNOSE_RESULT)


def diagnose_context(args: Mapping[str, Any], **_: Any) -> str:
    return dispatch("diagnose", args, _diagnose_action)


def _worktree_diff_action(args: Mapping[str, Any]) -> Mapping[str, Any]:
    limit = bounded_int(args, "limit", 50, 1, 200)
    installation, route = resolve_route(args)
    result = run(
        route_arguments(installation, route)
        + ["worktree-diff", "--limit", str(limit)],
        120,
        WORKTREE_DIFF_RESULT,
    )
    return command_envelope(
        "worktree-diff",
        result,
        route=route,
        next_action=(
            "Open changed profile-owned paths directly and rebuild only when their "
            "current bytes must become retrievable."
        ),
    )


def worktree_diff(args: Mapping[str, Any], **_: Any) -> str:
    return dispatch("worktree-diff", args, _worktree_diff_action)


def _suggest_profile_action(args: Mapping[str, Any]) -> Mapping[str, Any]:
    installation = resolve_installation()
    config = existing_file(args.get("routing_config"), "routing_config")
    query = bounded_string(args, "query")
    limit = bounded_int(args, "limit", 5, 1, 20)
    command = [
        str(installation.binary),
        "suggest-profile",
        "--config",
        str(config),
        query,
        "--limit",
        str(limit),
    ]
    if args.get("from_root") is not None:
        from_root = existing_directory(args.get("from_root"), "from_root")
        command.extend(["--from-root", str(from_root)])
    result = run(command, 30, SUGGEST_PROFILE_RESULT)
    return command_envelope(
        "suggest-profile",
        result,
        next_action=(
            "Use a unique recommendation only after reading that project's marker; "
            "do not merge indexes automatically."
        ),
    )


def suggest_profile(args: Mapping[str, Any], **_: Any) -> str:
    return dispatch("suggest-profile", args, _suggest_profile_action)
