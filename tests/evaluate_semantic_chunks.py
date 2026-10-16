#!/usr/bin/env python3
"""Freeze unmeasured topics, then compare parent-only and augmented rows in one matrix."""

import argparse
from collections import Counter
from datetime import datetime, timezone
import json
from pathlib import Path
import time

from evaluate_semantic_selection import select_by_path
from prepare_semantic_pilot import sources_current
from run_carry_forward_comparison import save
from run_local_semantic_pilot import (candidate, check_budget, fingerprint, load_completed_run,
    load_input, load_model, peak_rss_bytes, summarize_quality, verify_vector_runtime)
from run_project_search_comparison import validate_spec


def validate_heldout(spec, development, manifest, units):
    identity = lambda p: (p["id"], p["root"], p["profile"])
    if sorted(map(identity, spec["projects"])) != sorted(map(identity, development["projects"])):
        raise ValueError("Held-out routes differ from the frozen development routes")
    previous = [case for p in development["projects"] for case in p["cases"]]
    old_ids = {c["id"] for c in previous}
    old_queries = {c["query"] for c in previous}
    old_paths = {(p["id"], path) for p in development["projects"] for c in p["cases"] for path in c["expected_paths"]}
    enrolled = {(u["project"], u["path"]) for u in units}
    inventories = {p["id"]: p for p in manifest["projects"]}
    expected = {}
    if len(spec["projects"]) != 5:
        raise ValueError("The held-out cohort requires five projects")
    for project in spec["projects"]:
        if Counter(c["kind"] for c in project["cases"]) != Counter({k: 1 for k in ("exact", "english", "russian", "negative")}):
            raise ValueError("Each project requires exact, English, Russian and negative controls")
        inventory = inventories[project["id"]]
        expected[project["id"]] = {}
        for case in project["cases"]:
            if case["id"] in old_ids or case["query"] in old_queries or not case["query"].strip():
                raise ValueError("Held-out queries must be new and nonempty")
            if (case["kind"] == "negative") != (not case["expected_paths"]):
                raise ValueError("Only negative controls may have no expected paths")
            for path in case["expected_paths"]:
                if (project["id"], path) in old_paths or (project["id"], path) not in enrolled:
                    raise ValueError("Held-out targets must be new topics in the frozen corpus")
                source = inventory["sources"][path]
                if fingerprint(Path(project["root"]) / path) != source["sha256"]:
                    raise ValueError("Held-out target changed since source freeze: " + path)
                expected[project["id"]][path] = source["sha256"]
    return expected


def load_selection(path):
    selection = json.loads(path.read_text())
    if (selection.get("schema_version") != "shin.semantic-selection-experiment.v1" or
            selection.get("per_document") != 2 or selection.get("candidate_limit") != 20 or
            selection.get("report_limit") != 5):
        raise ValueError("Use the predeclared cap-two/top20/first5 policy")
    return selection


def freeze(input_dir, spec_path, selection_path, output):
    manifest, development, _, units = load_input(input_dir)
    spec = json.loads(spec_path.read_text())
    validate_spec(spec, output)
    targets = validate_heldout(spec, development, manifest, units)
    selection = load_selection(selection_path)
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    save(output / "heldout.json", spec)
    save(output / "selection.json", selection)
    receipt = {"schema_version": "shin.semantic-heldout-freeze.v1",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "input_manifest_sha256": fingerprint(input_dir / "input-manifest.json"),
        "development_spec_sha256": manifest["spec_sha256"],
        "heldout_spec_sha256": fingerprint(output / "heldout.json"),
        "selection_sha256": fingerprint(output / "selection.json"), "expected_source_sha256": targets,
        "comparison": "Same frozen matrix: original parent rows versus parents plus semantic subranges; identical cosine/path cap; both cohorts reported separately.",
        "boundary": "Hand-authored unmeasured topics, not a statistically independent benchmark. No query/model/policy tuning after results."}
    save(output / "freeze.json", receipt)
    print(json.dumps(receipt, indent=2))


def verify_freeze(directory, binding):
    receipt = json.loads((directory / "freeze.json").read_text())
    if (receipt.get("schema_version") != "shin.semantic-heldout-freeze.v1" or
            fingerprint(directory / "freeze.json") != binding.get("evaluation_freeze_sha256") or
            receipt["input_manifest_sha256"] != binding["input_manifest_sha256"]):
        raise ValueError("Evaluation was not bound before this model run")
    for file, key in (("heldout.json", "heldout_spec_sha256"), ("selection.json", "selection_sha256")):
        if fingerprint(directory / file) != receipt[key]:
            raise ValueError("Predeclared evaluation changed: " + file)
    return receipt


def evaluate(run_dir, model_dir, freeze_dir, output):
    started = time.monotonic()
    report, binding, manifest, input_dir = load_completed_run(run_dir)
    receipt = verify_freeze(freeze_dir, binding)
    selection = load_selection(freeze_dir / "selection.json")
    manifest, development, policy, units = load_input(input_dir)
    if manifest["spec_sha256"] != receipt["development_spec_sha256"]:
        raise ValueError("Development questions changed")
    if any(policy[key] != selection[key] for key in ("candidate_limit", "report_limit")):
        raise ValueError("Output budget differs from the frozen pilot")
    heldout = json.loads((freeze_dir / "heldout.json").read_text())
    verify_vector_runtime(report, policy, model_dir)
    model = load_model(model_dir, policy)
    import numpy as np
    vectors = np.load(run_dir / "vectors.npy", mmap_mode="r", allow_pickle=False)
    if vectors.shape != (len(units), policy["dimensions"]) or vectors.dtype != np.float32:
        raise ValueError("Invalid frozen vector shape/type")
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    save(output / "binding.json", {"run_report_sha256": fingerprint(run_dir / "report.json"),
        "freeze_sha256": fingerprint(freeze_dir / "freeze.json"), "runner_sha256": fingerprint(Path(__file__)),
        "comparison": receipt["comparison"]})
    results, summaries, replays = {}, {}, 0
    for name, spec in (("development", development), ("heldout", heldout)):
        rows = {"parents": [], "augmented": []}
        for project in spec["projects"]:
            indexes = np.array([i for i, u in enumerate(units) if u["project"] == project["id"]])
            paths = [units[int(i)]["path"] for i in indexes]
            matrix = vectors[indexes]
            parent_mask = [units[int(i)].get("unit_kind") != "semantic-subrange" for i in indexes]
            for case in project["cases"]:
                query = np.array(next(model.embed([policy["query_prefix"] + case["query"]])))
                if query.shape != (policy["dimensions"],) or not np.isfinite(query).all():
                    raise ValueError("Invalid query vector")
                scores = matrix @ query
                order = np.argsort(-scores, kind="stable")
                if name == "development":
                    raw = [candidate(units[int(indexes[i])], scores[i]) for i in order[:policy["candidate_limit"]]]
                    original = next(r for r in report["results"] if r["id"] == case["id"])
                    identity = lambda c: {k: v for k, v in c.items() if k != "similarity"}
                    if list(map(identity, raw)) != list(map(identity, original["candidates"])):
                        raise ValueError("Raw augmented top20 replay differs in provenance/order")
                    replays += 1
                for variant in rows:
                    eligible = order if variant == "augmented" else (i for i in order if parent_mask[i])
                    selected = select_by_path(eligible, paths, selection["candidate_limit"], selection["per_document"])
                    candidates = [candidate(units[int(indexes[i])], scores[i]) for i in selected]
                    rank = next((i + 1 for i, c in enumerate(candidates) if c["path"] in case["expected_paths"]), None)
                    rows[variant].append({"project": project["id"], "id": case["id"], "kind": case["kind"],
                        "expected_rank_at_20": rank, "expected_in_first_5": rank is not None and rank <= selection["report_limit"],
                        "candidates": candidates})
                check_budget(policy, started)
        results[name] = rows
        summaries[name] = {}
        for variant, values in rows.items():
            summary, passed = summarize_quality(values, policy)
            summaries[name][variant] = {"summary": summary, "cohort_threshold_met": passed}
    result = {"completed": True, "summaries": summaries, "results": results,
        "raw_top20_replays_verified": replays, "document_vectors_recomputed": 0, "query_vectors_computed": 40,
        "source_drift_since_freeze": {p["id"]: sources_current(Path(p["root"]), p["sources"]) for p in manifest["projects"]},
        "total_seconds": time.monotonic() - started, "peak_rss_bytes": peak_rss_bytes(),
        "boundary": receipt["boundary"] + " Dense negatives remain navigation candidates, not existence proof. Not production enablement, currentness, Pack completion or release qualification."}
    check_budget(policy, started)
    save(output / "report.json", result)
    print(json.dumps({k: v for k, v in result.items() if k not in ("results", "source_drift_since_freeze")}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    prepare = subs.add_parser("freeze")
    for name in ("input", "spec", "selection-policy", "output"):
        prepare.add_argument("--" + name, type=Path, required=True)
    score = subs.add_parser("evaluate")
    for name in ("run", "model-dir", "freeze", "output"):
        score.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    if args.command == "freeze":
        freeze(args.input.resolve(), args.spec.resolve(), args.selection_policy.resolve(), args.output.resolve())
    else:
        evaluate(args.run.resolve(), args.model_dir.resolve(), args.freeze.resolve(), args.output.resolve())
