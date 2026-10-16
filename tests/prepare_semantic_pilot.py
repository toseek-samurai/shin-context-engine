#!/usr/bin/env python3
"""Freeze published, redacted Shin units for a local candidate-only experiment."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from run_carry_forward_comparison import digest, save
from run_project_search_comparison import execute, metadata_path, validate_spec


def capture_profile(path, directory, expected_hash):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected_hash:
        raise ValueError("Profile changed before capture")
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / path.name
    with target.open("xb") as stream:
        stream.write(data)
    return target


def published_identifiers(row):
    values = row.get("identifier_terms", [])
    if not isinstance(values, list) or any(not isinstance(value, str) or not value for value in values):
        raise ValueError("Published identifier_terms must be nonempty strings")
    return list(dict.fromkeys(values))


def owned_path(root, relative):
    path = Path(relative)
    result = (root / path).resolve(strict=True)
    if path.is_absolute() or ".." in path.parts or not result.is_relative_to(root) or not result.is_file():
        raise ValueError("Source path escapes its owner or is not a file")
    return result


def project_units(project, generated):
    root = Path(project["root"]).resolve(strict=True)
    groups, sources = {}, {}
    input_rows = excluded = 0
    for filename in ("facts.jsonl", "sections.jsonl"):
        with (generated / filename).open() as stream:
            for line in stream:
                row = json.loads(line)
                input_rows += 1
                path = row["path"]
                if metadata_path(path):
                    excluded += 1
                    continue
                if path not in sources:
                    source = owned_path(root, path)
                    raw = source.read_bytes()
                    sources[path] = {"sha256": hashlib.sha256(raw).hexdigest(),
                                     "lines": len(raw.decode("utf-8").splitlines())}
                start, end = row["start_line"], row["end_line"]
                if type(start) is not int or type(end) is not int or not 1 <= start <= end <= sources[path]["lines"]:
                    raise ValueError("Published range is outside its source")
                key = (path, start, end)
                unit = groups.setdefault(key, {"project": project["id"], "path": path,
                    "start_line": start, "end_line": end, "source_sha256": sources[path]["sha256"],
                    "record_ids": [], "parts": []})
                identity = row.get("id") or f"{row['document_id']}:{start}:{end}:{row.get('anchor', '')}"
                unit["record_ids"].append(identity)
                for field in ("title", "heading", "summary", "indexed_text"):
                    value = row.get(field)
                    if isinstance(value, str) and value and value not in unit["parts"]:
                        unit["parts"].append(value)
                if not row.get("indexed_text") and row.get("preview") and row["preview"] not in unit["parts"]:
                    unit["parts"].append(row["preview"])
    units = []
    for key in sorted(groups):
        unit = groups[key]
        unit["text"] = unit["path"] + "\n" + "\n".join(unit.pop("parts"))
        unit["record_ids"] = sorted(set(unit["record_ids"]))
        units.append(unit)
    return units, {"input_rows": input_rows, "excluded_metadata_rows": excluded,
                   "units": len(units), "sources": sources}


def sources_current(root, sources):
    changed = []
    for path, recorded in sources.items():
        try:
            current = digest(owned_path(root, path))
        except (OSError, ValueError):
            current = None
        if current != recorded["sha256"]:
            changed.append(path)
    return changed


def prepare(spec_path, policy_path, output, binary, source_exporter=None, source_chunks=False):
    spec = json.loads(spec_path.read_text())
    policy = json.loads(policy_path.read_text())
    validate_spec(spec, output)
    if source_chunks and source_exporter is None:
        raise ValueError("Chunk capture requires the native source exporter")
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    save(output / "spec.json", spec)
    save(output / "policy.json", policy)
    manifest = {"schema_version": "shin.semantic-pilot-input.v1", "projects": [],
                "projection_version": "published-summary-v1",
                "binary": {"path": str(binary), "sha256": digest(binary)},
                "spec_sha256": digest(output / "spec.json"), "policy_sha256": digest(output / "policy.json")}
    if source_exporter is not None:
        manifest["source_exporter_sha256"] = digest(source_exporter)
        manifest["source_export_projection"] = "core-bound-chunks-v1" if source_chunks else "core-bound-range-v1"
    total_units = total_bytes = 0
    with (output / "units.jsonl").open("x") as stream:
        for project in spec["projects"]:
            root = Path(project["root"]).resolve()
            profile_path = Path(project["profile"])
            profile_hash = digest(profile_path)
            profile = json.loads(profile_path.read_text())
            base = output / project["id"]
            prefix = [str(binary), "--root", str(root), "--profile", profile["id"],
                      "--profile-dir", str(profile_path.parent), "--out-dir", str(base / "index")]
            for command in ("build", "check", "doctor"):
                response = execute([*prefix, command, *(["--strict"] if command == "doctor" else [])], base / (command + ".json"))
                if response["exit_code"] or not (response["result"] or {}).get("ok"):
                    raise ValueError("Publication failed: " + project["id"] + ":" + command)
            generated = base / "index/docs/kb/generated"
            units, inventory = project_units(project, generated)
            if source_exporter is not None:
                export = base / "source-ranges.jsonl"
                command = [str(source_exporter), str(root), str(base / "index")]
                if source_chunks:
                    command.append("--chunks")
                with export.open("x") as exported:
                    subprocess.run(command, stdout=exported, check=True)
                inventory["source_projection_sha256"] = digest(export)
            if sources_current(root, inventory["sources"]) or digest(profile_path) != profile_hash:
                raise ValueError("Source/profile changed while freezing")
            response = execute([*prefix, "check"], base / "check-after-freeze.json")
            if response["exit_code"] or not (response["result"] or {}).get("ok"):
                raise ValueError("Publication became stale during freeze")
            capture_profile(profile_path, base / "profiles", profile_hash)
            for unit in units:
                serialized = json.dumps(unit, ensure_ascii=False, separators=(",", ":")) + "\n"
                total_bytes += len(serialized.encode())
                total_units += 1
                if total_bytes > policy["max_input_bytes"] or total_units > policy["max_units"]:
                    raise ValueError("Frozen input exceeds predeclared budget")
                stream.write(serialized)
            inventory.update({"id": project["id"], "root": str(root), "profile_sha256": profile_hash,
                              "publication_sha256": digest(generated / "manifest.json")})
            manifest["projects"].append(inventory)
            print(json.dumps({"project": project["id"], "rows": inventory["input_rows"], "units": inventory["units"]}), flush=True)
    if digest(binary) != manifest["binary"]["sha256"]:
        raise ValueError("Core binary changed during preparation")
    if source_exporter is not None and digest(source_exporter) != manifest["source_exporter_sha256"]:
        raise ValueError("Source exporter changed during preparation")
    manifest.update({"units": total_units, "bytes": total_bytes, "units_sha256": digest(output / "units.jsonl")})
    save(output / "input-manifest.json", manifest)
    print(json.dumps({"units": total_units, "bytes": total_bytes}), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("spec", "policy", "output", "shin"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--source-exporter", type=Path)
    parser.add_argument("--source-chunks", action="store_true")
    args = parser.parse_args()
    prepare(args.spec.resolve(), args.policy.resolve(), args.output.resolve(), args.shin.resolve(strict=True),
            args.source_exporter.resolve(strict=True) if args.source_exporter else None, args.source_chunks)
