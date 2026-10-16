#!/usr/bin/env python3
"""Same-question live-project observations, not an equal-corpus quality benchmark."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

from run_carry_forward_comparison import digest, save


def validate_spec(spec, output):
    if spec.get("schema_version") != "shin.project-search-comparison.v1" or spec.get("limit") != 5 or spec.get("per_document") != 2:
        raise ValueError("This cohort requires schema v1, five records and two records per document")
    names = set()
    owned_build = Path(__file__).resolve().parents[1] / "build"
    for project in spec["projects"]:
        root = Path(project["root"]).resolve(strict=True)
        if output.is_relative_to(root) and not output.is_relative_to(owned_build):
            raise ValueError("Comparison output must not write into a consumer project")
        for identifier in [project["id"], *(case["id"] for case in project["cases"])]:
            if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,159}", identifier) or identifier in names:
                raise ValueError("Comparison IDs must be unique safe path components")
            names.add(identifier)
        for case in project["cases"]:
            for path in case["expected_paths"]:
                source_record(root, path)


def execute(argv, destination):
    start = time.monotonic()
    process = subprocess.run(argv, capture_output=True, text=True, timeout=300)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.with_suffix(".stdout").write_text(process.stdout)
    destination.with_suffix(".stderr").write_text(process.stderr)
    try:
        payload = json.loads(process.stdout or process.stderr)
    except json.JSONDecodeError:
        payload = None
    result = {"exit_code": process.returncode, "wall_ms": (time.monotonic() - start) * 1000,
              "response_bytes": len(process.stdout.encode()), "result": payload}
    save(destination, result)
    return result


def source_record(root, path, start=None, end=None, content=None):
    relative = Path(path)
    source = (root / relative).resolve()
    if relative.is_absolute() or ".." in relative.parts or not source.is_relative_to(root):
        raise ValueError("Result path escapes the reviewed project")
    record = {"path": path, "valid": False}
    if not source.is_file():
        return record
    raw = source.read_bytes()
    text = raw.decode("utf-8")
    lines = text.splitlines()
    record["sha256"] = hashlib.sha256(raw).hexdigest()
    if content is None:
        record["valid"] = (type(start) is int and type(end) is int
                           and 1 <= start <= end <= len(lines))
        record["verification"] = "coordinates-only"
    else:
        # A revision label is not a hash of dirty worktree content.
        record["valid"] = bool(content) and content in text
        record["verification"] = "fragment-in-current-source"
        record["reported_start_line"] = start
    return record


def metadata_path(path):
    parts = Path(path).parts
    return ("shin-profiles" in parts or path.startswith(("profiles/", "tests/evidence/", "docs/kb/"))
            or path.startswith("tests/fixtures/") or path.endswith("-reference-cases.json"))


def observe(root, case, engine, response, before):
    payload = response["result"] or {}
    successful = response["exit_code"] == 0 and (
        payload.get("type") == "search_result" and isinstance(payload.get("results"), list)
        if engine == "jbcontext" else payload.get("ok") is True)
    records = []
    if successful:
        for row in payload.get("results" if engine == "jbcontext" else "evidence", []):
            if engine == "jbcontext":
                path = row["result"]["sourcePosition"]["relativePath"]
                record = source_record(root, path, row.get("contentStartLine"), content=row["content"])
            else:
                path = row["path"]
                record = source_record(root, path, row["start_line"], row["end_line"])
            record["evaluation_metadata"] = metadata_path(path)
            records.append(record)
    expected = case["expected_paths"]
    rank = next((index + 1 for index, record in enumerate(records)
                 if record["valid"] and not record["evaluation_metadata"] and record["path"] in expected), None)
    unchanged = all((root / path).is_file() and digest(root / path) == value
                    for path, value in before.items())
    return {"id": case["id"], "kind": case["kind"], "engine": engine,
            "successful": successful, "records": records, "expected_sources_unchanged": unchanged,
            "first_expected_record_rank": rank if unchanged else None,
            "negative_returned_records": len(records) if case["kind"] == "negative" else None,
            "query_coverage": payload.get("query_coverage"),
            "query_diagnostics": payload.get("query_diagnostics"),
            "reported_revision": payload.get("revision"),
            "wall_ms": response["wall_ms"], "response_bytes": response["response_bytes"]}


def summarize(rows):
    summary = {}
    for engine in ("stable", "native", "jbcontext"):
        summary[engine] = {}
        for kind in ("exact", "english", "russian", "negative"):
            group = [row for row in rows if row["engine"] == engine and row["kind"] == kind]
            summary[engine][kind] = {
                "attempted": len(group), "successful": sum(row["successful"] for row in group),
                "expected_path_in_top5": sum(row["first_expected_record_rank"] is not None for row in group)
                if kind != "negative" else None,
                "empty_responses": sum(not row["records"] and row["successful"] for row in group),
                "metadata_records": sum(record["evaluation_metadata"] for row in group for record in row["records"]),
                "invalid_records": sum(not record["valid"] for row in group for record in row["records"]),
            }
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("spec", "output", "stable", "native", "jbcontext"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    spec = json.loads(args.spec.read_text())
    validate_spec(spec, output)
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    save(output / "spec.json", spec)
    identities = {}
    for name in ("stable", "native", "jbcontext"):
        binary = getattr(args, name).resolve(strict=True)
        identities[name] = {"path": str(binary), "sha256": digest(binary)}
        identities[name]["version"] = subprocess.check_output([str(binary), "--version"], text=True).strip()
    save(output / "binaries.json", identities)
    rows, projects = [], []
    for project in spec["projects"]:
        root = Path(project["root"]).resolve(strict=True)
        profile_path = Path(project["profile"]).resolve(strict=True)
        profile = json.loads(profile_path.read_text())
        base = output / project["id"]
        expected = {path: digest(root / path) for case in project["cases"] for path in case["expected_paths"]}
        status = execute([identities["jbcontext"]["path"], "status", "--project-path", str(root), "--json-output"],
                         base / "reference-status.json")
        project_row = {"id": project["id"], "head": subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip(),
            "profile_sha256": digest(profile_path), "expected_source_hashes": expected,
            "reference_status_ok": status["exit_code"] == 0, "gates": {}}
        for engine in ("stable", "native", "jbcontext"):
            prefix = [identities[engine]["path"]]
            if engine != "jbcontext":
                prefix += ["--root", str(root), "--profile", profile["id"], "--profile-dir", str(profile_path.parent),
                           "--out-dir", str(base / engine / "index")]
                for command in ("build", "check", "doctor"):
                    flags = ["--strict"] if command == "doctor" else []
                    result = execute([*prefix, command, *flags], base / engine / (command + ".json"))
                    project_row["gates"][engine + "-" + command] = result["exit_code"] == 0 and bool((result["result"] or {}).get("ok"))
                if not all(project_row["gates"][engine + "-" + command] for command in ("build", "check", "doctor")):
                    raise RuntimeError("Comparison publication failed its integrity checks; see " + str(base))
            for case in project["cases"]:
                argv = ([*prefix, "search", "--project-path", str(root), "--limit", "5", "--json-output", case["query"]]
                        if engine == "jbcontext" else
                        [*prefix, "brief", case["query"], "--limit", "5", "--per-doc", "2", "--projection", "compact", "--require-current"])
                result = execute(argv, base / engine / (case["id"] + ".json"))
                row = observe(root, case, engine, result, expected)
                row["project"] = project["id"]
                rows.append(row)
                print(json.dumps({key: row[key] for key in ("project", "engine", "id", "successful", "first_expected_record_rank")}), flush=True)
        project_row["profile_unchanged"] = digest(profile_path) == project_row["profile_sha256"]
        project_row["expected_sources_unchanged"] = all(digest(root / path) == value for path, value in expected.items())
        projects.append(project_row)
    summary = summarize(rows)
    save(output / "report.json", {"spec_sha256": digest(args.spec), "projects": projects, "rows": rows, "summary": summary,
         "boundary": "Hand-authored localization observations on active working trees, not identical enrolled corpora, a frozen Git commit, task success, false-confidence rate or a speed benchmark. No query rewriting or path filter. Five records per query. Metadata hits excluded without refilling. Empty output is not proof of absence. Source hashes are checked per response and at project completion; neighboring work can still invalidate a later replay."})
    print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
