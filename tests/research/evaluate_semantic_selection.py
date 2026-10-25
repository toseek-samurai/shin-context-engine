#!/usr/bin/env python3
"""Replay completed semantic matrices with one predeclared per-path selection cap."""

import argparse
import importlib.metadata
import json
from pathlib import Path
import time

from compare_semantic_projections import compare, load_run
from run_carry_forward_comparison import save
from run_local_semantic_pilot import candidate, check_budget, fingerprint, load_model, peak_rss_bytes, summarize_quality


def select_by_path(order, paths, limit, per_document):
    if type(limit) is not int or type(per_document) is not int or min(limit, per_document) < 1:
        raise ValueError("Selection limits must be positive integers")
    counts, selected = {}, []
    for index in order:
        path = paths[index]
        if counts.get(path, 0) >= per_document:
            continue
        selected.append(index)
        counts[path] = counts.get(path, 0) + 1
        if len(selected) == limit:
            break
    return selected


def evaluate(control, body, model_dir, selection_path, output):
    started = time.monotonic()
    # No output/admission before all original corpus/model/policy bindings agree.
    comparison = compare(control, body)
    selection = json.loads(selection_path.read_text())
    if (selection.get("schema_version") != "shin.semantic-selection-experiment.v1" or
            selection.get("per_document") != 2 or selection.get("candidate_limit") != 20 or
            selection.get("report_limit") != 5):
        raise ValueError("Only the predeclared cap-two/top20/first5 experiment is supported")
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    save(output / "selection-policy.json", selection)
    save(output / "binding.json", {"control_report_sha256": comparison["control_report_sha256"],
         "body_report_sha256": comparison["candidate_report_sha256"],
         "selection_policy_sha256": fingerprint(selection_path), "runner_sha256": fingerprint(Path(__file__))})
    runs = [load_run(path) for path in (control, body)]
    policy = runs[0][1]["policy"]
    if policy["candidate_limit"] != selection["candidate_limit"] or policy["report_limit"] != selection["report_limit"]:
        raise ValueError("Output budget differs from the frozen pilot")
    versions = {name: importlib.metadata.version(name) for name in ("fastembed", "onnxruntime", "tokenizers", "numpy")}
    for report, _, _, _ in runs:
        if report["runtime_versions"] != versions or fingerprint(model_dir / "manifest.json") != report["model_manifest_sha256"]:
            raise ValueError("Runtime or model differs from the frozen run")
    model = load_model(model_dir, policy)
    import numpy as np
    units = [[json.loads(line) for line in (run[3] / "units.jsonl").open()] for run in runs]
    matrices = [np.load(path / "vectors.npy", mmap_mode="r", allow_pickle=False) for path in (control, body)]
    for matrix in matrices:
        if matrix.shape != (runs[0][2]["units"], policy["dimensions"]) or matrix.dtype != np.float32:
            raise ValueError("Invalid frozen vector shape/type")
    spec = json.loads((runs[0][3] / "spec.json").read_text())
    results = {"summary": [], "body": []}
    for project in spec["projects"]:
        indexes = np.array([i for i, unit in enumerate(units[0]) if unit["project"] == project["id"]])
        paths = [units[0][int(i)]["path"] for i in indexes]
        for case in project["cases"]:
            query = np.array(next(model.embed([policy["query_prefix"] + case["query"]])))
            if query.shape != (policy["dimensions"],) or not np.isfinite(query).all():
                raise ValueError("Invalid query embedding")
            for name, matrix, source_units, run in zip(results, matrices, units, runs, strict=True):
                scores = matrix[indexes] @ query
                order = np.argsort(-scores, kind="stable")
                original = next(row for row in run[0]["results"] if row["id"] == case["id"])
                raw = [candidate(source_units[int(indexes[i])], scores[i]) for i in order[:policy["candidate_limit"]]]
                if [{k: v for k, v in row.items() if k != "similarity"} for row in raw] != [
                        {k: v for k, v in row.items() if k != "similarity"} for row in original["candidates"]]:
                    raise ValueError("Raw top20 replay differs from its recorded provenance/order")
                selected = select_by_path(order, paths, selection["candidate_limit"], selection["per_document"])
                candidates = [candidate(source_units[int(indexes[i])], scores[i]) for i in selected]
                rank = next((i + 1 for i, row in enumerate(candidates) if row["path"] in case["expected_paths"]), None)
                results[name].append({"project": project["id"], "id": case["id"], "kind": case["kind"],
                    "raw_rank_at_20": original["expected_rank_at_20"], "expected_rank_at_20": rank,
                    "expected_in_first_5": rank is not None and rank <= selection["report_limit"],
                    "candidates": candidates})
                check_budget(policy, started)
    summaries = {}
    for name, rows in results.items():
        summary, passed = summarize_quality(rows, policy)
        summaries[name] = {"summary": summary, "selection_cohort_threshold_met": passed}
    report = {"completed": True, "raw_top20_replays_verified": 40, "document_vectors_recomputed": 0,
              "query_vectors_computed": 20, "selection": selection, "variants": summaries, "results": results,
              "total_seconds": time.monotonic() - started, "peak_rss_bytes": peak_rss_bytes(),
              "boundary": selection["scope"] + " Original raw embedding gates remain failed."}
    check_budget(policy, started)
    save(output / "report.json", report)
    print(json.dumps({key: report[key] for key in ("completed", "variants", "total_seconds", "peak_rss_bytes")}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("control", "body", "model-dir", "selection-policy", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    evaluate(args.control.resolve(), args.body.resolve(), args.model_dir.resolve(),
             args.selection_policy.resolve(), args.output.resolve())
