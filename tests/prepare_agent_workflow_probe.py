#!/usr/bin/env python3
"""Prepare isolated, model-neutral fixtures for manual fresh-agent acceptance."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest.mock import patch

from run_hermes_plugin_tests import load_plugin


MARKER = "docs/machine/shin-project-adoption-v1.json"
HISTORY_MARKER = "docs/machine/shin-history-adoption-v1.json"
RULE = ".cursor/rules/publication.mdc"


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value if isinstance(value, str) else json.dumps(value, indent=2) + "\n")


def inventory(paths):
    return {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
            for root in paths for path in sorted(root.rglob("*"))
            if path.is_file() and "__pycache__" not in path.parts}


def prepare(source, tool, output):
    output.mkdir(parents=True, exist_ok=False)
    copied_codex = output / "delivery/codex/skills/shin-context"
    copied_hermes = output / "delivery/hermes/plugins/shin-context"
    shutil.copytree(source / "integrations/codex/skills/shin-context", copied_codex)
    shutil.copytree(source / "integrations/hermes/plugins/shin-context", copied_hermes,
                    ignore=shutil.ignore_patterns("__pycache__"))
    data_root = Path(tempfile.mkdtemp(prefix="shin-agent-workflow-")).resolve()
    if any((parent / ".git").exists() for parent in (data_root, *data_root.parents)):
        raise RuntimeError("The agent fixture must be outside Git ancestry")
    records = {}
    for scenario in ("current", "stale", "denied", "ambiguous", "mdc-codex", "mdc-hermes"):
        base = data_root / scenario
        root = base / "project"
        registry = root / "docs/machine/shin-profiles"
        working = base / "cache/working"
        history = base / "cache/history"
        profile_id = "workflow-probe"
        history_id = "workflow-history"
        case_tool = tool
        is_mdc = scenario.startswith("mdc-")
        if scenario == "ambiguous":
            installation = base / "installation"
            (installation / "build").mkdir(parents=True)
            (installation / "profiles").mkdir()
            case_tool = installation / "build/shin-context"
            shutil.copy2(tool, case_tool)
            shutil.copytree(source / "docs/machine", installation / "docs/machine")
            working = base / "cache-default/shin-context/working"
            alternate = installation / "build/context/working"
        marker = {
            "schemaVersion": "shin.project-adoption.v1", "status": "active",
            "document": "workflow-probe-adoption", "summary": "Isolated agent probe",
            "project": {"id": "workflow-probe", "contextEpoch": "cf6-probe-v1",
                        "rootPolicy": "cli-root"},
            "engine": {"minimumVersion": "1.27.2",
                       "generatorIdentity": "shin-generator-v20-preview-redaction"},
            "profile": {"id": profile_id, "registry": "explicit-profile-dir",
                        "kind": "permanent", "genericBootstrapAllowed": False},
            "index": {"ownership": "tool-external", "key": "working",
                      "consumerWritesAllowed": False, "requireCurrent": True},
            "qualityGates": {"profileCheckStrict": True, "check": True,
                             "doctorStrict": True, "evalStrict": True, "packEvalStrict": True},
            "agent": {"entryPath": "AGENTS.md", "contextEpochReferenceRequired": True,
                      "markerReferenceRequired": True, "profileReferenceRequired": True},
        }
        historical = copy.deepcopy(marker)
        historical["profile"]["id"] = history_id
        historical["index"]["key"] = "history"
        write(root / MARKER, marker)
        write(root / HISTORY_MARKER, historical)
        write(root / "AGENTS.md", f"""# Workflow Fixture

Context epoch: `cf6-probe-v1`.
This is a non-Git project. Current canonical rules are in `{RULE}` and
`docs/access.md`; old decisions are in `docs/history.md`.

The Shin binary is `{case_tool}`. The project-owned registry is `{registry}`.
Normal work uses profile `{profile_id}`, marker `{MARKER}` and external
publication root `{working}`. Its owner is this file, not cache recency.
For an explicitly historical task only, use profile `{history_id}`, marker
`{HISTORY_MARKER}` and external publication root `{history}`. It is never
a fallback for failed working readiness. Do not merge the two routes.

The task is read-only: do not rebuild, repair, alter profiles, relax checks,
change skills or settings, or call another agent. Request scratch files may be
written only under `{output / 'scratch'}`. Verify returned evidence in source.
""")
        if scenario == "ambiguous":
            entry = (root / "AGENTS.md").read_text()
            entry = entry.replace(
                f"Normal work uses profile `{profile_id}`, marker `{MARKER}` and external\n"
                f"publication root `{working}`. Its owner is this file, not cache recency.",
                f"Normal work uses profile `{profile_id}` and marker `{MARKER}`.\n"
                f"Two former external publication roots exist: `{working}` and `{alternate}`.\n"
                "The current canonical output binding has not been recorded by the project owner.\n"
                "Neither marker identity, index health nor cache recency declares that ownership.")
            write(root / "AGENTS.md", entry)
        elif is_mdc:
            entry = (root / "AGENTS.md").read_text().replace(
                "The task is read-only: do not rebuild, repair, alter profiles, relax checks,\n"
                "change skills or settings, or call another agent.",
                f"For the explicitly authorized enrollment task, only `{registry / (profile_id + '.json')}`\n"
                f"and the external working publication `{working}` may change.\n"
                "Preserve the marker, original rules, existing eval/Pack cases, all other\n"
                "sources, history and installed guidance. Do not call another agent.")
            write(root / "AGENTS.md", entry)
        write(root / RULE, """---
description: Current publication policy
globs: ["src/**/*.ts"]
alwaysApply: false
---
# Publication Permission

An approved publication permission expires after 18 hours. Revalidation is
required after expiry. The client metadata does not exempt this rule from Shin
enrollment and cannot grant access or permission to publish.
""")
        write(root / "docs/access.md", """# Workspace Access

The selected workspace is a UI filter, not an authorization grant. The server
checks the user's permission on every privileged operation. Hiding a control
in the UI is not a security boundary. This document defines required behavior;
it does not prove that any implementation or deployment has been tested.
""")
        write(root / "docs/history.md", """# Superseded Publication Policy

Status: superseded, historical only.
The earlier publication permission lasted 12 hours. The current policy owner
is .cursor/rules/publication.mdc. This historical duration is not current policy.
""")
        request = {
            "schema_version": "shin.task-request.v1", "request_id": "workflow-questions",
            "task": {"objective": "Verify independent publication and access rules",
                     "scope": [], "must": [
                         {"id": "publication", "query": "publication permission expires hours"},
                         {"id": "access", "query": "workspace authorization server permission"}],
                     "should": []},
            "budget": {"max_evidence_bytes": 12000, "max_records": 8,
                       "per_query_records": 4, "per_document_records": 2},
        }
        profile = {
            "schemaVersion": "shin.profile.v1", "id": profile_id,
            "sourceLabel": "Agent workflow fixture", "rootFiles": ["AGENTS.md", MARKER, RULE, "docs/access.md"],
            "sourceExtensions": [".md", ".mdc", ".json"],
            "documentIncludePrefixes": ["AGENTS.md", MARKER, RULE, "docs/access.md"],
            "agentStartPaths": ["AGENTS.md", MARKER], "defaultTopic": "policy",
            "topicRules": [{"id": "policy", "description": "Project policy", "keywords": ["policy"]}],
            "evalCases": [{"id": "publication", "query": "publication permission expires hours",
                           "mustPaths": [RULE], "mustTopPaths": [RULE], "mustTopN": 3,
                           "limit": 8, "perDocLimit": 2}],
            "packEvalCases": [{"id": "workflow-pack", "request": request,
                               "mustPaths": [RULE, "docs/access.md"], "expectedComplete": True}],
        }
        if scenario == "denied":
            profile["evalCases"][0]["mustPaths"].append("docs/required-but-absent.md")
        elif is_mdc:
            profile["rootFiles"].remove(RULE)
            profile["documentIncludePrefixes"].remove(RULE)
            profile["evalCases"] = [{"id": "access", "query": "workspace authorization server permission",
                                      "mustPaths": ["docs/access.md"], "mustTopPaths": ["docs/access.md"],
                                      "mustTopN": 3, "limit": 8, "perDocLimit": 2}]
            profile["packEvalCases"][0]["request"]["task"]["must"] = [request["task"]["must"][1]]
            profile["packEvalCases"][0]["mustPaths"] = ["docs/access.md"]
        write(registry / f"{profile_id}.json", profile)
        write(base / "profile-before.json", profile)
        historical_profile = copy.deepcopy(profile)
        historical_profile["id"] = history_id
        historical_profile["rootFiles"] = ["AGENTS.md", HISTORY_MARKER, "docs/history.md"]
        historical_profile["documentIncludePrefixes"] = historical_profile["rootFiles"]
        historical_profile["agentStartPaths"] = ["AGENTS.md", HISTORY_MARKER]
        historical_profile["evalCases"] = [{"id": "history", "query": "superseded publication policy",
                                              "mustPaths": ["docs/history.md"]}]
        historical_profile["packEvalCases"] = [{
            "id": "history-pack", "request": {
                **request, "request_id": "history-probe", "task": {
                    "objective": "Read historical policy", "scope": [],
                    "must": [{"id": "history", "query": "superseded publication policy"}], "should": []}},
            "mustPaths": ["docs/history.md"], "expectedComplete": True}]
        write(registry / f"{history_id}.json", historical_profile)
        if scenario == "stale":
            write(root / RULE, (root / RULE).read_text().replace("18 hours", "12 hours"))
        checks = {}
        for selected, marker_path, publication in ((profile_id, MARKER, working),
                                                    (history_id, HISTORY_MARKER, history)):
            prefix = [str(case_tool), "--profile", selected, "--profile-dir", str(registry),
                      "--root", str(root), "--out-dir", str(publication)]
            build = subprocess.run([*prefix, "build"], capture_output=True, text=True, timeout=30)
            write(base / f"{selected}-build.json", json.loads(build.stdout))
            if build.returncode:
                raise RuntimeError(build.stdout + build.stderr)
            if scenario == "stale" and selected == profile_id:
                write(root / RULE, (root / RULE).read_text().replace("12 hours", "18 hours"))
            ready = subprocess.run([*prefix, "ready", "--marker", marker_path, "--strict"],
                                   capture_output=True, text=True, timeout=30)
            payload = json.loads(ready.stdout)
            write(base / f"{selected}-ready.json", payload)
            expected = selected == history_id or scenario not in ("stale", "denied")
            if payload.get("ready") is not expected:
                raise RuntimeError(f"Unexpected fixture readiness: {payload}")
            checks[selected] = {"returncode": ready.returncode, "ready": payload["ready"]}
        if scenario == "ambiguous":
            shutil.copytree(working, alternate)
            plugin = load_plugin(source)
            with patch.dict("os.environ", {"SHIN_CONTEXT_BIN": str(case_tool),
                                            "SHIN_CONTEXT_PROFILE_DIR": str(registry),
                                            "SHIN_CONTEXT_CACHE_DIR": "",
                                            "XDG_CACHE_HOME": str(base / "cache-default")}, clear=False):
                result = json.loads(plugin.adapter.ready_context({"root": str(root)}))
            write(base / "ambiguous-ready.json", result)
            if result.get("ok") or result.get("error", {}).get("code") != "AMBIGUOUS_INDEX_ROUTE":
                raise RuntimeError(f"Expected adapter ambiguity: {result}")
        documents = [json.loads(line)["path"] for line in
                     (working / "docs/kb/generated/documents.jsonl").read_text().splitlines()]
        if is_mdc and RULE in documents:
            raise RuntimeError("The missing-enrollment control already contains the MDC rule")
        records[scenario] = {"root": str(root), "registry": str(registry),
                             "working": str(working), "history": str(history), "controls": checks,
                             "tool": str(case_tool), "permission": "enroll-mdc" if is_mdc else "read-only",
                             "case_dir": str(base), "outside_git_ancestry": True}
        if scenario == "ambiguous":
            records[scenario].update(alternate=str(alternate), xdg_cache=str(base / "cache-default"))
    (output / "scratch").mkdir()
    write(output / "cases.json", records)
    protected = [Path(case["case_dir"]) / folder for case in records.values()
                 for folder in ("project", "cache", "cache-default", "installation")]
    protected.append(output / "delivery")
    write(output / "before.json", inventory(protected))
    print(json.dumps({"output": str(output), "cases": records}, indent=2))


def audit(output):
    before = json.loads((output / "before.json").read_text())
    cases = json.loads((output / "cases.json").read_text())
    protected = [Path(case.get("case_dir", output / scenario)) / folder for scenario, case in cases.items()
                 for folder in ("project", "cache", "cache-default", "installation")]
    protected.append(output / "delivery")
    after = inventory(protected)
    changed = sorted(path for path in before.keys() | after.keys() if before.get(path) != after.get(path))
    allowed = set()
    enrollment = {}
    for scenario, case in cases.items():
        if case.get("permission") != "enroll-mdc":
            continue
        profile_path = Path(case["registry"]) / "workflow-probe.json"
        if str(profile_path) not in changed:
            enrollment[scenario] = "not-modified"
            continue
        original = json.loads((Path(case.get("case_dir", output / scenario)) / "profile-before.json").read_text())
        updated = json.loads(profile_path.read_text())
        valid = True
        for field in ("rootFiles", "documentIncludePrefixes"):
            current = updated.get(field)
            previous = original[field]
            valid &= (isinstance(current, list) and len(current) == len(previous) + 1
                      and set(current) == set(previous) | {RULE})
            updated[field] = previous
        valid &= updated == original
        if valid:
            allowed.add(str(profile_path))
            generated = Path(case["working"]) / "docs/kb/generated"
            allowed.update(path for path in changed if path in before and Path(path).parent == generated)
            allowed.add(str(Path(case["working"]) / "docs/kb/.shin-context-freshness.json"))
        enrollment[scenario] = "approved-fields-only" if valid else "unexpected-profile-change"
    forbidden = sorted(set(changed) - allowed)
    result = {"ok": not forbidden, "changed_paths": changed, "unexpected_changes": forbidden,
              "enrollment": enrollment,
              "boundary": "Fixture bytes and explicitly authorized enrollment fields only; not model compliance or post-change readiness."}
    write(output / "readonly-audit.json", result)
    print(json.dumps(result, indent=2))
    return int(bool(forbidden))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("prepare", "audit"))
    parser.add_argument("output", type=Path)
    parser.add_argument("--tool", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if args.operation == "prepare":
        if args.tool is None:
            parser.error("--tool is required for prepare")
        prepare(Path(__file__).resolve().parents[1], args.tool.resolve(strict=True), output)
    else:
        raise SystemExit(audit(output))
