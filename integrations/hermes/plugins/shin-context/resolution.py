"""Resolve one explicit, external Shin route for an adopted project."""

from __future__ import annotations

import json
import os
import shutil
from pathlib import Path
from typing import Any, Mapping

from .model import AdapterFailure, Installation, Route


MARKER_CANDIDATES = (
    "docs/machine/shin-project-adoption-v1.json",
    "docs/machine/shin-context-engine-adoption-v1.json",
    "docs/shin-context-engine-adoption.json",
    "docs/architecture/shin-context-engine-adoption.json",
)


def _portable_identifier_valid(value: str) -> bool:
    if not value or len(value) > 160:
        return False
    for index, character in enumerate(value):
        if character.isascii() and (character.islower() or character.isdigit()):
            continue
        if index > 0 and character in "-_.":
            continue
        return False
    return value[-1] not in "-_."


def existing_directory(value: Any, name: str) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise AdapterFailure(
            "INVALID_PATH",
            f"{name} must be a non-empty path.",
            remediation=f"Pass a valid absolute {name}.",
        )
    path = Path(value).expanduser()
    if not path.is_absolute():
        raise AdapterFailure(
            "RELATIVE_PATH_REJECTED",
            f"{name} must be absolute.",
            remediation=f"Resolve {name} before calling the Shin tool.",
            details={name: value},
        )
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise AdapterFailure(
            "PATH_NOT_FOUND",
            f"{name} does not exist.",
            remediation=f"Correct {name} and retry.",
            details={name: value, "reason": str(error)},
        ) from error
    if not resolved.is_dir():
        raise AdapterFailure(
            "NOT_A_DIRECTORY",
            f"{name} is not a directory.",
            remediation=f"Pass a directory as {name}.",
            details={name: str(resolved)},
        )
    return resolved


def existing_file(value: Any, name: str) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise AdapterFailure(
            "INVALID_PATH",
            f"{name} must be a non-empty path.",
            remediation=f"Pass a valid absolute {name}.",
        )
    path = Path(value).expanduser()
    if not path.is_absolute():
        raise AdapterFailure(
            "RELATIVE_PATH_REJECTED",
            f"{name} must be absolute.",
            remediation=f"Resolve {name} before calling the Shin tool.",
            details={name: value},
        )
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise AdapterFailure(
            "PATH_NOT_FOUND",
            f"{name} does not exist.",
            remediation=f"Correct {name} and retry.",
            details={name: value, "reason": str(error)},
        ) from error
    if not resolved.is_file():
        raise AdapterFailure(
            "NOT_A_FILE",
            f"{name} is not a file.",
            remediation=f"Pass a file as {name}.",
            details={name: str(resolved)},
        )
    return resolved


def resolve_installation() -> Installation:
    explicit_binary = os.environ.get("SHIN_CONTEXT_BIN")
    explicit_profiles = os.environ.get("SHIN_CONTEXT_PROFILE_DIR")
    binary = existing_file(explicit_binary, "SHIN_CONTEXT_BIN") if explicit_binary else None
    profile_dir = (
        existing_directory(explicit_profiles, "SHIN_CONTEXT_PROFILE_DIR")
        if explicit_profiles
        else None
    )
    if binary is not None and not os.access(binary, os.X_OK):
        raise AdapterFailure(
            "BINARY_NOT_EXECUTABLE",
            "SHIN_CONTEXT_BIN is not executable.",
            remediation="Install or chmod the configured Shin binary.",
            details={"binary": str(binary)},
        )

    home_candidates = []
    configured_home = os.environ.get("SHIN_CONTEXT_HOME")
    if configured_home:
        home_candidates.append(Path(configured_home).expanduser())
    home_candidates.extend(
        (
            Path.home() / "IdeaProjects" / "Shin-Context-Engine",
            Path.home() / "work" / "Shin-Context-Engine",
        )
    )

    source_home = None
    for candidate in home_candidates:
        source_binary = candidate / "build" / "shin-context"
        source_profiles = candidate / "profiles"
        installed_binary = candidate / "bin" / "shin-context"
        installed_profiles = candidate / "share" / "shin-context" / "profiles"
        if binary is None and source_binary.is_file() and os.access(source_binary, os.X_OK):
            binary = source_binary.resolve()
            source_home = candidate.resolve()
        if profile_dir is None and source_profiles.is_dir():
            profile_dir = source_profiles.resolve()
            source_home = source_home or candidate.resolve()
        if binary is None and installed_binary.is_file() and os.access(installed_binary, os.X_OK):
            binary = installed_binary.resolve()
        if profile_dir is None and installed_profiles.is_dir():
            profile_dir = installed_profiles.resolve()

    if binary is None:
        found = shutil.which("shin-context")
        if found:
            binary = Path(found).resolve()
            installed_profiles = (
                binary.parent.parent / "share" / "shin-context" / "profiles"
            )
            if profile_dir is None and installed_profiles.is_dir():
                profile_dir = installed_profiles.resolve()

    if binary is None or profile_dir is None:
        raise AdapterFailure(
            "SHIN_INSTALLATION_NOT_FOUND",
            "A Shin binary and profile registry pair could not be resolved.",
            remediation=(
                "Set SHIN_CONTEXT_BIN and SHIN_CONTEXT_PROFILE_DIR, set "
                "SHIN_CONTEXT_HOME, or install shin-context on PATH."
            ),
        )

    if source_home is None and binary.parent.name == "build":
        candidate = binary.parent.parent
        if (candidate / "profiles").is_dir():
            source_home = candidate.resolve()
    return Installation(binary=binary, profile_dir=profile_dir, source_home=source_home)


def _resolve_marker(root: Path, explicit: Any = None) -> tuple[Path, dict[str, Any]]:
    if explicit is not None:
        if not isinstance(explicit, str) or not explicit.strip():
            raise AdapterFailure(
                "INVALID_MARKER",
                "marker must be a non-empty repository-relative path.",
                remediation="Pass one conventional adoption marker path.",
            )
        relative = Path(explicit)
        if relative.is_absolute() or ".." in relative.parts:
            raise AdapterFailure(
                "MARKER_OUTSIDE_ROOT",
                "The adoption marker must stay inside the consumer root.",
                remediation="Pass marker as a repository-relative path without traversal.",
                details={"marker": explicit},
            )
        candidates = [root / relative]
    else:
        candidates = [root / relative for relative in MARKER_CANDIDATES]

    existing = [candidate for candidate in candidates if candidate.is_file()]
    if not existing:
        raise AdapterFailure(
            "ADOPTION_MARKER_NOT_FOUND",
            "No permanent Shin adoption marker was found.",
            remediation=(
                "Use shin_discover_project for cold start or pass the reviewed "
                "repository-relative marker explicitly."
            ),
            details={"candidates": list(MARKER_CANDIDATES)},
        )
    if len(existing) > 1:
        raise AdapterFailure(
            "AMBIGUOUS_ADOPTION_MARKER",
            "More than one conventional Shin adoption marker exists.",
            remediation="Pass the authoritative marker explicitly.",
            details={"markers": [str(path.relative_to(root)) for path in existing]},
        )

    marker = existing[0].resolve(strict=True)
    if root not in marker.parents:
        raise AdapterFailure(
            "MARKER_OUTSIDE_ROOT",
            "The resolved adoption marker escapes the consumer root.",
            remediation="Replace the marker symlink with a project-owned file.",
            details={"marker": str(marker)},
        )
    try:
        payload = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AdapterFailure(
            "INVALID_ADOPTION_MARKER",
            "The Shin adoption marker is not valid JSON.",
            remediation="Repair the project-owned adoption marker and rerun its guard.",
            details={"marker": str(marker), "reason": str(error)},
        ) from error
    if not isinstance(payload, dict):
        raise AdapterFailure(
            "INVALID_ADOPTION_MARKER",
            "The Shin adoption marker must contain a JSON object.",
            remediation="Repair the project-owned adoption marker.",
        )
    return marker, payload


def _marker_route(marker: Mapping[str, Any]) -> tuple[str, str]:
    if marker.get("schemaVersion") != "shin.project-adoption.v1":
        raise AdapterFailure(
            "UNSUPPORTED_ADOPTION_MARKER",
            "The marker is not shin.project-adoption.v1.",
            remediation="Migrate the consumer to the permanent project adoption contract.",
            details={"schemaVersion": marker.get("schemaVersion")},
        )
    if marker.get("status") != "active":
        raise AdapterFailure(
            "INACTIVE_ADOPTION_MARKER",
            "The Shin adoption marker is not active.",
            remediation="Use cold-start discovery or activate a reviewed marker.",
            details={"status": marker.get("status")},
        )
    profile_node = marker.get("profile")
    index_node = marker.get("index")
    profile = profile_node.get("id") if isinstance(profile_node, Mapping) else None
    index_key = index_node.get("key") if isinstance(index_node, Mapping) else None
    if not isinstance(profile, str) or not _portable_identifier_valid(profile):
        raise AdapterFailure(
            "INVALID_ADOPTION_ROUTE",
            "The marker does not declare a portable profile.id.",
            remediation="Repair the permanent adoption marker.",
        )
    if not isinstance(index_key, str) or not _portable_identifier_valid(index_key):
        raise AdapterFailure(
            "INVALID_ADOPTION_ROUTE",
            "The marker does not declare a portable index.key.",
            remediation="Repair the permanent adoption marker.",
        )
    return profile, index_key


def _inside(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def _resolve_out_dir(
    root: Path,
    index_key: str,
    installation: Installation,
    explicit: Any = None,
) -> Path:
    if explicit is not None:
        if not isinstance(explicit, str) or not Path(explicit).expanduser().is_absolute():
            raise AdapterFailure(
                "INVALID_OUT_DIR",
                "out_dir must be an absolute external path.",
                remediation="Pass the canonical absolute index directory.",
            )
        selected = Path(explicit).expanduser().resolve(strict=False)
    else:
        configured_base = os.environ.get("SHIN_CONTEXT_CACHE_DIR")
        if configured_base:
            base = Path(configured_base).expanduser()
            if not base.is_absolute():
                raise AdapterFailure(
                    "INVALID_CACHE_DIR",
                    "SHIN_CONTEXT_CACHE_DIR must be absolute.",
                    remediation="Configure an absolute external Shin cache directory.",
                )
            candidates = [base.resolve(strict=False) / index_key]
        else:
            xdg = os.environ.get("XDG_CACHE_HOME")
            cache_base = (
                Path(xdg).expanduser() / "shin-context"
                if xdg
                else Path.home() / ".cache" / "shin-context"
            )
            candidates = [cache_base.resolve(strict=False) / index_key]
            if installation.source_home is not None:
                candidates.append(
                    installation.source_home / "build" / "context" / index_key
                )

        published = [
            path
            for path in candidates
            if (path / "docs" / "kb" / "generated" / "manifest.json").is_file()
        ]
        if len(published) > 1:
            raise AdapterFailure(
                "AMBIGUOUS_INDEX_ROUTE",
                "Multiple published indexes exist for the adopted index key.",
                remediation="Pass the authoritative absolute out_dir explicitly.",
                details={"out_dirs": [str(path) for path in published]},
            )
        selected = published[0] if published else candidates[0]

    selected = selected.resolve(strict=False)

    if _inside(selected, root):
        raise AdapterFailure(
            "CONSUMER_INDEX_REJECTED",
            "The selected Shin index directory is inside the consumer repository.",
            remediation="Move generated context to a tool-owned external directory.",
            details={"out_dir": str(selected), "root": str(root)},
        )
    return selected


def resolve_route(args: Mapping[str, Any]) -> tuple[Installation, Route]:
    root = existing_directory(args.get("root"), "root")
    installation = resolve_installation()
    marker, marker_payload = _resolve_marker(root, args.get("marker"))
    profile, index_key = _marker_route(marker_payload)

    project_registry = root / "docs" / "machine" / "shin-profiles"
    project_profile = project_registry / f"{profile}.json"
    if project_profile.is_file():
        resolved_profile = project_profile.resolve(strict=True)
        if not _inside(resolved_profile, root):
            raise AdapterFailure(
                "PROJECT_PROFILE_OUTSIDE_ROOT",
                "The project-owned profile resolves outside the consumer repository.",
                remediation="Replace the profile symlink with a project-owned regular file.",
                details={"profile": str(resolved_profile)},
            )
        profile_dir = project_registry.resolve(strict=True)
    else:
        profile_dir = installation.profile_dir
        if not (profile_dir / f"{profile}.json").is_file():
            raise AdapterFailure(
                "PROFILE_NOT_FOUND",
                "The adopted profile is absent from the selected registry.",
                remediation=(
                    "Install the reviewed profile registry or restore the "
                    "project-owned docs/machine/shin-profiles entry."
                ),
                details={"profile": profile, "profile_dir": str(profile_dir)},
            )

    out_dir = _resolve_out_dir(root, index_key, installation, args.get("out_dir"))
    route = Route(
        root=str(root),
        marker=str(marker.relative_to(root)),
        profile=profile,
        profile_dir=str(profile_dir),
        index_key=index_key,
        out_dir=str(out_dir),
    )
    return installation, route


def route_arguments(installation: Installation, route: Route) -> list[str]:
    return [
        str(installation.binary),
        "--profile",
        route.profile,
        "--profile-dir",
        route.profile_dir,
        "--root",
        route.root,
        "--out-dir",
        route.out_dir,
    ]
