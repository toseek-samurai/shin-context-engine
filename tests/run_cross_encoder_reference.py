#!/usr/bin/env python3
"""One offline library reranker over frozen selected candidates; no runtime promotion."""

import argparse
from collections import Counter
from copy import deepcopy
import importlib.metadata
import json
import math
import os
from pathlib import Path
import stat
import sys
import time

sys.dont_write_bytecode = True

from check_semantic_artifact_parity import CONTROL_SHA, FREEZE_SHA
from run_local_semantic_pilot import fingerprint, load_completed_run, peak_rss_bytes
from run_semantic_hybrid import lexical_batch
from shin_semantic.model import MODEL, load as load_embedding
from shin_semantic.artifact import Artifact
from shin_semantic.selection import navigate

ROOT = Path(__file__).resolve().parents[1]
POLICY_PATH = ROOT / "tests/fixtures/cross-encoder-policy.json"
NEW_PATH = ROOT / "tests/fixtures/cross-encoder-new-cases.json"


def key(row):
    return row["project"], row["path"], row["start_line"], row["end_line"], row["source_sha256"], tuple(sorted(row["record_ids"]))


def checked_candidates(rows, policy):
    if len(rows) > policy["max_candidates"] or len({key(r) for r in rows}) != len(rows):
        raise ValueError("Candidate inventory is duplicate or over budget")
    if any(count > policy["per_document"] for count in Counter(r["path"] for r in rows).values()):
        raise ValueError("Per-document candidate budget exceeded")
    if len({r["project"] for r in rows}) > 1:
        raise ValueError("Cross-project candidates")


def reorder(rows, scores, policy):
    checked_candidates(rows, policy)
    if len(scores) != len(rows) or any(not math.isfinite(score) for score in scores):
        raise ValueError("Missing or nonfinite reranker scores")
    order = sorted(range(len(rows)), key=lambda i: (-scores[i], i))
    first, extra, seen = [], [], set()
    for i in order:
        (extra if rows[i]["path"] in seen else first).append(i)
        seen.add(rows[i]["path"])
    return [deepcopy(rows[i]) for i in first + extra], first + extra


def rank(rows, expected):
    return next((i for i, row in enumerate(rows, 1) if row["path"] in expected), None)


def score_case(case, model, policy):
    rows, texts = case["candidates"], case["texts"]
    checked_candidates(rows, policy)
    if len(texts) != len(rows) or any(not 0 < len(text.encode()) <= policy["max_text_bytes"] for text in texts):
        raise ValueError("Candidate text count or size differs")
    query = case["query"]
    if not isinstance(query, str) or not 0 < len(query.encode()) <= 4096 or "\0" in query:
        raise ValueError("Invalid query")
    bypass = case["core_retrieval_mode"] in ("identifier-scoped", "owner-scoped")
    if bypass or not rows:
        return deepcopy(rows), list(range(len(rows))), None
    scores = list(model.rerank(query, texts, batch_size=policy["batch_size"]))
    selected, order = reorder(rows, scores, policy)
    return selected, order, scores


def gate(results):
    regressions, improvements, missing = [], [], []
    for row in results:
        before, after = row["before_rank"], row["after_rank"]
        if before is not None and (after is None or before <= 5 < after):
            regressions.append(row["id"])
        if row["cohort"] != "new" and (before is None or before > 5) and after is not None and after <= 5:
            improvements.append(row["id"])
        if row["cohort"] == "new" and (after is None or after > 5):
            missing.append(row["id"])
    return {"passed": not regressions and bool(improvements) and not missing,
            "regressions": regressions, "old_misses_improved": improvements, "new_first5_misses": missing}


def verify_model(directory, policy):
    total = 0
    for name, digest in policy["files"].items():
        path = directory / name
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size <= 0:
            raise ValueError("Unsafe model asset")
        total += info.st_size
        if total > policy["max_model_bytes"] or fingerprint(path) != digest:
            raise ValueError("Model asset pin or budget differs")
    config = json.loads((directory / "tokenizer_config.json").read_bytes())
    if config.get("model_max_length") != policy["max_tokens"]:
        raise ValueError("Tokenizer context differs from policy")
    if {name: importlib.metadata.version(name) for name in policy["runtime"]} != policy["runtime"]:
        raise ValueError("Use the pinned optional runtime")


def load_reranker(directory, policy):
    verify_model(directory, policy)
    os.environ.update(HF_HUB_OFFLINE="1", HF_HUB_DISABLE_TELEMETRY="1", TOKENIZERS_PARALLELISM="false")
    from fastembed.rerank.cross_encoder import TextCrossEncoder
    from fastembed.common.model_description import ModelSource
    TextCrossEncoder.add_custom_model(policy["model"], sources=ModelSource(hf=policy["model"]),
        model_file="model.onnx", license=policy["license"], size_in_gb=0.471)
    return TextCrossEncoder(policy["model"], specific_model_path=str(directory), cache_dir=str(directory),
                            local_files_only=True, threads=policy["threads"], providers=["CPUExecutionProvider"], cuda=False)


def prepare(freeze, control, artifacts, embedding_dir, output, policy):
    if fingerprint(freeze / "freeze.json") != FREEZE_SHA or fingerprint(control / "report.json") != CONTROL_SHA:
        raise ValueError("Historical control changed")
    binding = json.loads((freeze / "freeze.json").read_bytes())
    if fingerprint(freeze / "cohorts.json") != binding["cohorts_sha256"]:
        raise ValueError("Frozen questions changed")
    _, _, manifest, source = load_completed_run(Path(binding["run"]))
    if fingerprint(source / "input-manifest.json") != binding["input_manifest_sha256"]:
        raise ValueError("Frozen input binding changed")
    cohorts = json.loads((freeze / "cohorts.json").read_bytes())
    prior = json.loads((control / "report.json").read_bytes())
    records = []
    for cohort, spec in cohorts.items():
        for project in spec["projects"]:
            for case in project["cases"]:
                old = next(r for r in prior["results"][cohort] if r["id"] == case["id"])
                if old["project"] != project["id"] or old["kind"] != case["kind"]:
                    raise ValueError("Historical case binding differs")
                records.append({**case, "cohort": cohort, "project": project["id"],
                    "core_retrieval_mode": old["core_retrieval_mode"], "candidates": old["hybrid"]["candidates"]})
    if len(records) != 60:
        raise ValueError("Incomplete historical cohort")
    new = json.loads(NEW_PATH.read_bytes())
    config = json.loads((artifacts / new["project"] / "route.json").read_bytes())
    for path, expected in new["sources"].items():
        if fingerprint(Path(config["root"]) / path) != expected:
            raise ValueError("Reviewed new-case source changed")
    artifact = Artifact(Path(config["artifact_dir"]), config["artifact_sha256"], config)
    exporter = Path(binding["exporter"])
    if fingerprint(exporter) != binding["exporter_sha256"]:
        raise ValueError("Frozen lexical exporter changed")
    native, _ = lexical_batch(exporter, {"root": config["root"], "profile": config["profile_path"]},
        Path(config["out_dir"]), [c["query"] for c in new["cases"]], artifact.manifest["binding"]["snapshot_id"])
    embedding = load_embedding(embedding_dir, MODEL)
    import numpy as np
    for case, core in zip(new["cases"], native["results"]):
        if case["query"] != core["query"]:
            raise ValueError("New question binding differs")
        artifact.admit(core)
        vector = np.asarray(next(embedding.embed([MODEL["query_prefix"] + case["query"]])), dtype=np.float32)
        selected, _ = navigate(artifact, core, artifact.matrix @ vector)
        records.append({**case, "cohort": "new", "project": new["project"],
            "core_retrieval_mode": core["retrieval_mode"], "candidates": selected["candidates"]})
    wanted = set()
    for row in records:
        checked_candidates(row["candidates"], policy)
        wanted.update(key(c) for c in row["candidates"])
    texts = {}
    with (source / "units.jsonl").open() as stream:
        for line in stream:
            unit = json.loads(line)
            if unit["project"] == new["project"] and unit["path"] in new["sources"] and unit["source_sha256"] != new["sources"][unit["path"]]:
                raise ValueError("New source review differs from frozen bytes")
            identity = key(unit)
            if identity in wanted:
                if identity in texts or not 0 < len(unit["text"].encode()) <= policy["max_text_bytes"]:
                    raise ValueError("Ambiguous or oversized frozen model text")
                texts[identity] = unit["text"]
    if set(texts) != wanted or fingerprint(source / "units.jsonl") != manifest["units_sha256"]:
        raise ValueError("Unbound candidate or changed corpus")
    for row in records:
        row["texts"] = [texts[key(c)] for c in row["candidates"]]
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    payload = {"schema_version": "shin.cross-encoder-input.v1", "policy": policy,
        "policy_sha256": fingerprint(POLICY_PATH), "new_cases_sha256": fingerprint(NEW_PATH),
        "control_sha256": CONTROL_SHA, "freeze_sha256": FREEZE_SHA,
        "input_manifest_sha256": binding["input_manifest_sha256"], "cases": records}
    save(output / "input.json", payload)
    print(json.dumps({"prepared": True, "cases": len(records), "unique_units": len(texts),
                      "input_sha256": fingerprint(output / "input.json"), "inference_performed": False}))


def save(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    path.chmod(0o600)


def run(directory, expected_sha, model_dir):
    started = time.monotonic()
    if fingerprint(directory / "input.json") != expected_sha:
        raise ValueError("Prepared input changed")
    payload = json.loads((directory / "input.json").read_bytes())
    policy = payload["policy"]
    if fingerprint(POLICY_PATH) != payload["policy_sha256"] or json.loads(POLICY_PATH.read_bytes()) != policy:
        raise ValueError("Predeclared policy changed")
    if fingerprint(NEW_PATH) != payload["new_cases_sha256"] or len(payload["cases"]) != 64:
        raise ValueError("New cases changed or input is incomplete")
    model = load_reranker(model_dir, policy)
    results = []
    for case in payload["cases"]:
        rows = case["candidates"]
        bypass = case["core_retrieval_mode"] in ("identifier-scoped", "owner-scoped")
        before = time.monotonic()
        selected, order, scores = score_case(case, model, policy)
        results.append({k: case[k] for k in ("id", "kind", "cohort", "project")} | {
            "before_rank": rank(rows, case["expected_paths"]), "after_rank": rank(selected, case["expected_paths"]),
            "bypass": bypass, "scores_in_original_order": scores, "source_order": order,
            "candidates": selected, "seconds": round(time.monotonic() - before, 4)})
        if time.monotonic() - started > policy["max_run_seconds"] or peak_rss_bytes() > policy["max_peak_rss_bytes"]:
            raise ValueError("Cross-encoder resource budget exceeded")
    if fingerprint(directory / "input.json") != expected_sha:
        raise ValueError("Input changed during inference")
    verify_model(model_dir, policy)
    report = {"schema_version": "shin.cross-encoder-reference.v1", "completed": True,
        "input_sha256": expected_sha, "policy_sha256": payload["policy_sha256"], "policy": policy,
        "gate": gate(results), "results": results, "seconds": round(time.monotonic() - started, 3),
        "peak_rss_bytes": peak_rss_bytes(), "activated": False, "document_vectors_computed": 0,
        "scope": "frozen-local-candidate-ranking-not-currentness-or-answer-completeness"}
    save(directory / "report.json", report)
    print(json.dumps({k: v for k, v in report.items() if k not in ("results", "policy")}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    prepare_parser = actions.add_parser("prepare")
    for field in ("freeze", "control", "artifacts", "embedding-dir", "output"):
        prepare_parser.add_argument("--" + field, required=True, type=Path)
    run_parser = actions.add_parser("run")
    run_parser.add_argument("--input", required=True, type=Path)
    run_parser.add_argument("--input-sha256", required=True)
    run_parser.add_argument("--model-dir", required=True, type=Path)
    args = parser.parse_args()
    if args.action == "prepare":
        prepare(args.freeze.resolve(), args.control.resolve(), args.artifacts.resolve(), args.embedding_dir.resolve(),
                args.output.resolve(), json.loads(POLICY_PATH.read_bytes()))
    else:
        run(args.input.resolve(), args.input_sha256, args.model_dir.resolve())
