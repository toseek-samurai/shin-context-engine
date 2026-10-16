#!/usr/bin/env python3
"""Bound historical misses to candidates and actual model-visible input budgets."""

import argparse
import hashlib
import json
from pathlib import Path

from run_local_semantic_pilot import fingerprint, load_completed_run
from check_semantic_artifact_parity import CONTROL_SHA, FREEZE_SHA


def rank(rows, expected):
    return next((i for i, row in enumerate(rows, 1) if row["path"] in expected), None)


def classify(expected_count, lexical_rank, semantic_rank, selected_rank):
    if expected_count == 0:
        return "expected-source-not-enrolled"
    if lexical_rank is None and semantic_rank is None:
        return "candidate-recall-miss"
    if any(value is not None and value <= 5 for value in (lexical_rank, semantic_rank)) and (
            selected_rank is None or selected_rank > 5):
        return "first-five-lost-in-selection"
    return "candidate-present-outside-first-five"


def input_budget(unit, tokenizer, maximum):
    text = "passage: " + unit["text"]
    tokenizer.no_truncation()
    full = tokenizer.encode(text)
    tokenizer.enable_truncation(max_length=maximum)
    visible = tokenizer.encode(text)
    end = max((finish for _, finish in visible.offsets), default=0)
    return {"path": unit["path"], "start_line": unit["start_line"], "end_line": unit["end_line"],
            "source_sha256": unit["source_sha256"], "record_ids": unit["record_ids"],
            "text_sha256": hashlib.sha256(unit["text"].encode()).hexdigest(),
            "input_tokens": len(full.ids), "visible_tokens": len(visible.ids),
            "token_truncated": len(full.ids) > maximum,
            "visible_characters": end, "input_characters": len(text)}


def run(freeze, control, model_dir, output):
    from tokenizers import Tokenizer
    if fingerprint(freeze / "freeze.json") != FREEZE_SHA or fingerprint(control / "report.json") != CONTROL_SHA:
        raise ValueError("Historical control changed")
    binding = json.loads((freeze / "freeze.json").read_bytes())
    if fingerprint(freeze / "cohorts.json") != binding["cohorts_sha256"]:
        raise ValueError("Frozen questions changed")
    vector_report, _, manifest, source = load_completed_run(Path(binding["run"]))
    if fingerprint(source / "input-manifest.json") != binding["input_manifest_sha256"]:
        raise ValueError("Frozen model input changed")
    if fingerprint(model_dir / "manifest.json") != vector_report["model_manifest_sha256"]:
        raise ValueError("Model descriptor changed")
    model_manifest = json.loads((model_dir / "manifest.json").read_bytes())
    for name in ("tokenizer.json", "tokenizer_config.json"):
        if fingerprint(model_dir / name) != model_manifest["files"][name]["sha256"]:
            raise ValueError("Tokenizer bytes changed")
    tokenizer = Tokenizer.from_file(str(model_dir / "tokenizer.json"))
    tokenizer.no_padding()
    policy = json.loads((source / "policy.json").read_bytes())
    maximum = policy["max_tokens"]
    if policy["passage_prefix"] != "passage: ":
        raise ValueError("Unqualified document prefix")
    questions = json.loads((freeze / "cohorts.json").read_bytes())
    control_data = json.loads((control / "report.json").read_bytes())
    cases, wanted = [], set()
    for cohort, rows in control_data["results"].items():
        by_id = {case["id"]: case for project in questions[cohort]["projects"] for case in project["cases"]}
        for row in rows:
            case = by_id[row["id"]]
            expected = set(case["expected_paths"])
            if (rank(row["lexical_candidates"], expected) != row["lexical_rank"] or
                    rank(row["semantic_candidates"], expected) != row["semantic_rank"]):
                raise ValueError("Recorded candidate rank differs")
            selected = rank(row["hybrid"]["candidates"], expected)
            if selected != row["hybrid_rank"]:
                raise ValueError("Recorded selected rank differs")
            if case["kind"] not in ("english", "russian") or selected is not None and selected <= 5:
                continue
            wanted.update((row["project"], path) for path in expected)
            cases.append({"id": row["id"], "cohort": cohort, "project": row["project"], "query": case["query"],
                "expected_paths": sorted(expected), "lexical_rank": row["lexical_rank"],
                "semantic_rank": row["semantic_rank"], "selected_rank": selected,
                "top5": [{k: item[k] for k in ("path", "start_line", "end_line", "source_sha256")}
                         for item in row["hybrid"]["candidates"][:5]],
                "expected_units": [], "answer_sufficiency": "requires-source-review-not-a-file-rank"})
    units = {}
    with (source / "units.jsonl").open("rb") as stream:
        for line in stream:
            unit = json.loads(line)
            key = unit["project"], unit["path"]
            if key in wanted:
                units.setdefault(key, []).append(input_budget(unit, tokenizer, maximum))
    stages = {}
    for case in cases:
        case["expected_units"] = [unit for path in case["expected_paths"] for unit in units.get((case["project"], path), [])]
        case["stage"] = classify(len(case["expected_units"]), case["lexical_rank"], case["semantic_rank"], case["selected_rank"])
        stages[case["stage"]] = stages.get(case["stage"], 0) + 1
    result = {"schema_version": "shin.semantic-miss-review.v1", "scope": "frozen-source-diagnostics-not-a-ranking-fix",
              "control_sha256": CONTROL_SHA, "freeze_sha256": FREEZE_SHA,
              "input_manifest_sha256": fingerprint(source / "input-manifest.json"),
              "tokenizer_sha256": fingerprint(model_dir / "tokenizer.json"),
              "max_tokens": maximum, "misses": len(cases), "stages": stages, "cases": cases,
              "source_mutated": False, "document_vectors_computed": 0}
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    output.chmod(0o600)
    print(json.dumps({"misses": len(cases), "stages": stages, "output": str(output)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ("freeze", "control", "model-dir", "output"):
        parser.add_argument("--" + field, required=True, type=Path)
    args = parser.parse_args()
    run(args.freeze.resolve(), args.control.resolve(), args.model_dir.resolve(), args.output.resolve())
