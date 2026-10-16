#!/usr/bin/env python3
"""Compare the independent query policy to a pinned historical 60-case receipt."""

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import time

from run_local_semantic_pilot import fingerprint, peak_rss_bytes
from run_semantic_hybrid import lexical_batch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("semantic_entry", ROOT / "tools/shin_semantic.py")
entry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(entry)
from shin_semantic.artifact import Artifact, encode
from shin_semantic.model import MODEL, load
from shin_semantic.selection import navigate
from test_compact_query import assert_transport_lossless

CONTROL_SHA = "034018f7c57796f37756308a7022940af393335d0099fccfa0ae79c40b2dd3f8"
FREEZE_SHA = "cad8f1ada62fad77758f2582e624d65ff4db1ef6bb873a4b57af815134df2b87"


def equivalent(left, right):
    if type(left) is float and type(right) is float:
        return abs(left - right) <= 0.000001
    if isinstance(left, dict) and isinstance(right, dict):
        return set(left) == set(right) and all(equivalent(left[k], right[k]) for k in left)
    if isinstance(left, list) and isinstance(right, list):
        return len(left) == len(right) and all(equivalent(a, b) for a, b in zip(left, right))
    if left == "hybrid-candidate-experiment":
        return right == "semantic-navigation"
    return left == right


def run(artifacts, freeze, control, model_dir, output, probe_live=False):
    import numpy as np

    if fingerprint(control / "report.json") != CONTROL_SHA or fingerprint(freeze / "freeze.json") != FREEZE_SHA:
        raise ValueError("Historical control changed")
    receipt = json.loads((freeze / "freeze.json").read_text())
    if fingerprint(freeze / "cohorts.json") != receipt["cohorts_sha256"]:
        raise ValueError("Frozen queries changed")
    cohorts = json.loads((freeze / "cohorts.json").read_text())
    expected = json.loads((control / "report.json").read_text())
    started = time.monotonic()
    model = load(model_dir, MODEL)
    results, admissions, query_vectors, live = [], {}, 0, []
    for project in cohorts["development"]["projects"]:
        config = json.loads((artifacts / project["id"] / "route.json").read_text())
        artifact = Artifact(Path(config["artifact_dir"]), config["artifact_sha256"], config)
        admissions[project["id"]] = {"sha256": config["artifact_sha256"], "units": len(artifact.units)}
        if probe_live:
            query = next(case["query"] for case in project["cases"] if case["kind"] == "english")
            before = time.monotonic()
            process = subprocess.run([sys.executable, str(ROOT / "tools/shin_semantic.py"), "--config",
                str(artifacts / project["id"] / "current-route.json"), query], capture_output=True, timeout=60,
                env={"PATH": "", "LC_ALL": "C"})
            response = json.loads(process.stdout)
            if process.stderr:
                raise ValueError("Query CLI exposed diagnostics")
            navigation = response.get("navigation") or {}
            live.append({"project": project["id"], "ok": response["ok"], "exit_code": process.returncode,
                "error": response.get("error"), "semantic_status": response.get("semantic_status"),
                "first_path": next((row["path"] for row in navigation.get("candidates", [])), None),
                "milliseconds": round((time.monotonic() - before) * 1000, 3), "output_bytes": len(process.stdout)})
        for name, cohort in cohorts.items():
            cases = next(row for row in cohort["projects"] if row["id"] == project["id"])["cases"]
            lexical = json.loads((control / f"{name}-{project['id']}-lexical.json").read_text())["results"]
            if len(cases) != len(lexical):
                raise ValueError("Recorded Core query count differs")
            for case, core in zip(cases, lexical):
                if core["query"] != case["query"]:
                    raise ValueError("Recorded Core query differs")
                artifact.admit(core)
                query = np.asarray(next(model.embed([MODEL["query_prefix"] + case["query"]])), dtype=np.float32)
                actual, excluded = navigate(artifact, core, artifact.matrix @ query)
                assert_transport_lossless(core, actual)
                prior = next(row for row in expected["results"][name] if row["id"] == case["id"])
                same = equivalent(prior["hybrid"], actual) and excluded == prior["lexical_excluded"]
                results.append({"id": case["id"], "project": project["id"], "cohort": name, "equal": same})
                query_vectors += 1
        if project["id"] == "retool":
            queries = ["apps/admin/AccountEditor2/functions.rsx", "AccountEditorPageGuard.js"]
            exporter = Path(receipt["exporter"])
            if fingerprint(exporter) != receipt["exporter_sha256"]:
                raise ValueError("Native snapshot control exporter changed")
            payload, _ = lexical_batch(exporter, {"root": config["root"], "profile": config["profile_path"]},
                                       Path(config["out_dir"]), queries, artifact.manifest["binding"]["snapshot_id"])
            intent = []
            targets = [queries[0], "apps/admin/AccountEditor2/lib/AccountEditorPageGuard.js"]
            for query, core, target in zip(queries, payload["results"], targets):
                # Exact routes bypass inference in Session.query; check actual native routing.
                intent.append({"query": query, "retrieval_mode": core["retrieval_mode"],
                               "model_required": core["retrieval_mode"] not in ("identifier-scoped", "owner-scoped"),
                               "first_path": core["evidence"][0]["path"] if core["evidence"] else None, "expected_path": target})
    passed = (len(results) == 60 and all(row["equal"] for row in results)
              and all(not row["model_required"] and row["first_path"] == row["expected_path"] for row in intent))
    report = {"schema_version": "shin.semantic-artifact-parity.v1", "ok": passed, "cases": results,
              "control_sha256": CONTROL_SHA, "freeze_sha256": FREEZE_SHA, "artifacts": admissions,
              "caller_target": intent, "query_vectors_computed": query_vectors, "document_vectors_computed": 0,
              "live_route_observations": live,
              "seconds": round(time.monotonic() - started, 3), "peak_rss_bytes": peak_rss_bytes(),
              "comparison": "Exact rows/order/provenance excluding renamed candidate channel; similarity tolerance1e-6",
              "agent_projection_roundtrips": len(results),
              "source_admission": "frozen-snapshot-control-not-live-currentness"}
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as stream:
        stream.write(encode(report) + b"\n")
    print(json.dumps({k: v for k, v in report.items() if k not in ("cases", "artifacts")}, indent=2))
    if not passed:
        raise ValueError("Candidate policy differs from its preserved control")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ("artifacts", "freeze", "control", "model-dir", "output"):
        parser.add_argument("--" + field, required=True, type=Path)
    parser.add_argument("--probe-live-routes", action="store_true")
    args = parser.parse_args()
    run(args.artifacts.resolve(), args.freeze.resolve(), args.control.resolve(), args.model_dir.resolve(), args.output.resolve(), args.probe_live_routes)
