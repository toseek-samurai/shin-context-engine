#!/usr/bin/env python3
"""Freeze and evaluate the shared coarse-to-fine candidate component, never activate."""

import argparse
import json
from pathlib import Path
import sys
import time

sys.dont_write_bytecode = True

from run_cross_encoder_reference import key, rank, gate, load_reranker, verify_model, save
from run_local_semantic_pilot import fingerprint, load_completed_run, peak_rss_bytes
from check_semantic_artifact_parity import FREEZE_SHA
from shin_semantic.artifact import Artifact, sha
from shin_semantic.model import MODEL, load as load_embedding
from shin_semantic.hierarchical import make_plan, execute

ROOT = Path(__file__).resolve().parents[1]
POLICY_PATH = ROOT / "tests/fixtures/hierarchical-retrieval-policy.json"
RERANKER_PATH = ROOT / "tests/fixtures/cross-encoder-policy.json"
COMPONENT = ROOT / "integrations/semantic/hierarchical.py"
RANGE_PATH = ROOT / "tests/fixtures/semantic-answer-range-cases.json"


def prepare(original, freeze, artifacts, embedding_dir, output):
    import numpy as np

    started = time.monotonic()
    policy = json.loads(POLICY_PATH.read_bytes())
    if fingerprint(original) != policy["original_input_sha256"] or fingerprint(freeze / "freeze.json") != FREEZE_SHA:
        raise ValueError("Historical input changed")
    if fingerprint(RERANKER_PATH) != policy["reranker_policy_sha256"]:
        raise ValueError("Reranker policy changed")
    previous = json.loads(original.read_bytes())
    binding = json.loads((freeze / "freeze.json").read_bytes())
    _, _, manifest, source = load_completed_run(Path(binding["run"]))
    if fingerprint(source / "input-manifest.json") != binding["input_manifest_sha256"]:
        raise ValueError("Frozen source changed")
    model = load_embedding(embedding_dir, MODEL)
    cases, wanted, bindings = [], set(), {}
    for project in dict.fromkeys(c["project"] for c in previous["cases"]):
        config = json.loads((artifacts / project / "route.json").read_bytes())
        artifact = Artifact(Path(config["artifact_dir"]), config["artifact_sha256"], config)
        bindings[project] = {"artifact_sha256": config["artifact_sha256"],
                             "snapshot_id": artifact.manifest["binding"]["snapshot_id"]}
        for old in (c for c in previous["cases"] if c["project"] == project):
            baseline = {"mode": "preserved-control", "candidates": old["candidates"],
                "semantic_grants_lexical_coverage": False, "symbol_existence_asserted": False,
                "current_source_verified": False, "pack_completion_asserted": False}
            scoped = old["core_retrieval_mode"] in ("identifier-scoped", "owner-scoped")
            if scoped:
                plan = {"baseline": baseline, "files": [], "units": {}}
            else:
                vector = np.asarray(next(model.embed([MODEL["query_prefix"] + old["query"]])), dtype=np.float32)
                plan = make_plan(artifact, artifact.matrix @ vector, baseline)
                wanted.update(unit["text_sha256"] for unit in plan["units"].values())
            cases.append({k: old[k] for k in ("id", "kind", "cohort", "project", "query", "expected_paths")} |
                         {"scoped": scoped, "plan": plan})
        if peak_rss_bytes() > policy["max_rss_bytes"]:
            raise ValueError("Preparation memory budget exceeded")
    texts = {}
    with (source / "units.jsonl").open() as stream:
        for line in stream:
            unit = json.loads(line)
            digest = sha(unit["text"].encode())
            if digest in wanted:
                texts[digest] = unit["text"]
    if set(texts) != wanted or fingerprint(source / "units.jsonl") != manifest["units_sha256"]:
        raise ValueError("Model text does not match the frozen artifact")
    if len(cases) != 64:
        raise ValueError("Incomplete original cohort")
    output.mkdir(parents=True, mode=0o700, exist_ok=False)
    payload = {"schema_version": "shin.hierarchical-reference-input.v1", "policy": policy,
        "policy_sha256": fingerprint(POLICY_PATH), "component_sha256": fingerprint(COMPONENT),
        "range_review_sha256": fingerprint(RANGE_PATH), "bindings": bindings, "texts": texts, "cases": cases}
    if len(json.dumps(payload).encode()) > 128 * 1024 * 1024 or time.monotonic() - started > 120:
        raise ValueError("Preparation byte or time budget exceeded")
    save(output / "input.json", payload)
    print(json.dumps({"prepared": True, "cases": len(cases), "unique_passages": len(texts),
        "input_sha256": fingerprint(output / "input.json"), "component_sha256": payload["component_sha256"],
        "seconds": round(time.monotonic() - started, 3), "peak_rss_bytes": peak_rss_bytes()}))


def range_review(case, result):
    review = json.loads(RANGE_PATH.read_bytes())["reviews"][0]
    if case["id"] not in review["cases"]:
        return None
    rows = [r for file in (result["collection"] or {}).get("files", []) for r in file["members"]
            if r["path"] == review["path"] and r["source_sha256"] == review["source_sha256"]]
    covered = [clause["id"] for clause in review["clauses"] if all(
        any(row["start_line"] <= start and row["end_line"] >= end for row in rows)
        for start, end in clause["spans"])]
    return {"covered": covered, "total": len(review["clauses"]),
            "ranges": [[r["start_line"], r["end_line"]] for r in rows], "supplied_bodies": False}


def run(directory, expected_sha, model_dir):
    started = time.monotonic()
    if fingerprint(directory / "input.json") != expected_sha:
        raise ValueError("Prepared input changed")
    payload = json.loads((directory / "input.json").read_bytes())
    for path, field in ((POLICY_PATH, "policy_sha256"), (COMPONENT, "component_sha256"), (RANGE_PATH, "range_review_sha256")):
        if fingerprint(path) != payload[field]:
            raise ValueError("Predeclared policy, component or range review changed")
    policy = payload["policy"]
    if fingerprint(RERANKER_PATH) != policy["reranker_policy_sha256"]:
        raise ValueError("Reranker policy changed")
    reranker_policy = json.loads(RERANKER_PATH.read_bytes())
    model = load_reranker(model_dir, reranker_policy)
    results = []
    for case in payload["cases"]:
        before = time.monotonic()
        result = execute(case["plan"], case["query"], lambda unit: payload["texts"][unit["text_sha256"]],
                         model, scoped=case["scoped"])
        rows = result["navigation"]["candidates"]
        results.append({k: case[k] for k in ("id", "kind", "cohort", "project")} | {
            "before_rank": rank(case["plan"]["baseline"]["candidates"], case["expected_paths"]),
            "after_rank": rank(rows, case["expected_paths"]),
            "file_pool_rank": rank(case["plan"]["files"], case["expected_paths"]),
            "range_review": range_review(case, result), "result": result,
            "seconds": round(time.monotonic() - before, 3)})
        print(json.dumps({"case": case["id"], "completed": len(results), "total": len(payload["cases"]),
                          "seconds": results[-1]["seconds"], "passages": result["work"]["reranked_passages"]}), flush=True)
        if time.monotonic() - started > policy["max_run_seconds"] or peak_rss_bytes() > policy["max_rss_bytes"]:
            raise ValueError("Hierarchical runtime budget exceeded")
    if fingerprint(directory / "input.json") != expected_sha:
        raise ValueError("Prepared input changed during inference")
    verify_model(model_dir, reranker_policy)
    acceptance = gate(results)
    ranges_improved = all(row["range_review"]["covered"] for row in results if row["range_review"] is not None)
    acceptance["writer_ranges_improved"] = ranges_improved
    acceptance["passed"] = acceptance["passed"] and ranges_improved
    report = {"schema_version": "shin.hierarchical-reference.v1", "completed": True,
        "input_sha256": expected_sha, "component_sha256": payload["component_sha256"],
        "policy_sha256": payload["policy_sha256"], "gate": acceptance, "results": results,
        "seconds": round(time.monotonic() - started, 3), "peak_rss_bytes": peak_rss_bytes(),
        "activated": False, "document_vectors_computed": 0,
        "scope": "separate-internal-file-pool-and-member-navigation-not-native-pack-or-answer-proof"}
    save(directory / "report.json", report)
    print(json.dumps({k: v for k, v in report.items() if k != "results"}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    prep = actions.add_parser("prepare")
    for field in ("original", "freeze", "artifacts", "embedding-dir", "output"):
        prep.add_argument("--" + field, required=True, type=Path)
    query = actions.add_parser("run")
    query.add_argument("--input", type=Path, required=True)
    query.add_argument("--input-sha256", required=True)
    query.add_argument("--model-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.action == "prepare":
        prepare(args.original.resolve(), args.freeze.resolve(), args.artifacts.resolve(), args.embedding_dir.resolve(), args.output.resolve())
    else:
        run(args.input.resolve(), args.input_sha256, args.model_dir.resolve())
