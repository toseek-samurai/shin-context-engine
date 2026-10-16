#!/usr/bin/env python3
"""One frozen, bounded input-window probe; never publishes a semantic artifact."""

import argparse
import json
from pathlib import Path
import time

from check_semantic_artifact_parity import Artifact, CONTROL_SHA, FREEZE_SHA, MODEL, ROOT, equivalent, load, navigate
from review_semantic_misses import rank
from run_local_semantic_pilot import fingerprint, load_completed_run, peak_rss_bytes
from run_semantic_hybrid import lexical_batch
from shin_semantic.artifact import encode, sha
from test_compact_query import assert_transport_lossless


def windows(text, tokenizer, policy):
    """Slice already-sanitized input with offsets; never decode or invent source."""
    tokenizer.no_truncation()
    tokenizer.no_padding()
    tokens = lambda value: tokenizer.encode(value).ids
    if len(tokens(MODEL["passage_prefix"] + text)) <= policy["max_tokens"]:
        return [], "fits"
    header, separator, body = text.partition("\nSource range: ")
    if not separator or not body:
        return [], "no-captured-body"
    prefix = MODEL["passage_prefix"] + header + separator
    header_tokens = len(tokens(prefix))
    if header_tokens > policy["max_header_tokens"]:
        return [], "header-budget"
    offsets = [(a, b) for a, b in tokenizer.encode(body, add_special_tokens=False).offsets if b > a]
    payload = policy["max_tokens"] - header_tokens - 8
    if payload <= policy["overlap_tokens"] or policy["max_windows_per_parent"] < 1:
        raise ValueError("Invalid window policy")
    result, start, covered = [], 0, 0
    while start < len(offsets) and len(result) < policy["max_windows_per_parent"]:
        end = min(start + payload, len(offsets))
        begin_char = offsets[start][0] if start else 0
        while end > start:
            end_char = len(body) if end == len(offsets) else offsets[end - 1][1]
            value = header + separator + body[begin_char:end_char]
            count = len(tokens(MODEL["passage_prefix"] + value))
            if count <= policy["max_tokens"]:
                break
            end -= 1
        if end <= start:
            raise ValueError("No model-visible body fits")
        result.append({"text": value, "body_start": begin_char, "body_end": end_char, "tokens": count})
        covered = end
        if end == len(offsets):
            break
        start = max(start + 1, end - policy["overlap_tokens"])
    return result, "complete-captured-body" if covered == len(offsets) else "window-budget"


def write(path, value):
    with path.open("xb") as stream:
        stream.write(encode(value) + b"\n")
    path.chmod(0o600)


def run(args):
    import numpy as np
    from tokenizers import Tokenizer

    started = time.monotonic()
    policy_path = ROOT / "tests/fixtures/semantic-token-window-policy.json"
    policy = json.loads(policy_path.read_bytes())
    if fingerprint(args.freeze / "freeze.json") != FREEZE_SHA or fingerprint(args.control / "report.json") != CONTROL_SHA:
        raise ValueError("Historical control changed")
    binding = json.loads((args.freeze / "freeze.json").read_bytes())
    if fingerprint(args.freeze / "cohorts.json") != binding["cohorts_sha256"]:
        raise ValueError("Frozen questions changed")
    vector_report, _, manifest, source = load_completed_run(Path(binding["run"]))
    if vector_report["model_manifest_sha256"] != MODEL["manifest_sha256"]:
        raise ValueError("Control model differs")
    if fingerprint(source / "input-manifest.json") != binding["input_manifest_sha256"]:
        raise ValueError("Frozen model input changed")
    if fingerprint(args.model_dir / "manifest.json") != MODEL["manifest_sha256"]:
        raise ValueError("Model changed")
    model_manifest = json.loads((args.model_dir / "manifest.json").read_bytes())
    if fingerprint(args.model_dir / "tokenizer.json") != model_manifest["files"]["tokenizer.json"]["sha256"]:
        raise ValueError("Tokenizer changed")
    tokenizer = Tokenizer.from_file(str(args.model_dir / "tokenizer.json"))
    config = json.loads((args.artifacts / "shin/route.json").read_bytes())
    artifact = Artifact(Path(config["artifact_dir"]), config["artifact_sha256"], config)
    projected, statuses, total_bytes = [], {}, 0
    with (source / "units.jsonl").open("rb") as stream:
        for line in stream:
            unit = json.loads(line)
            if unit["project"] != "shin" or Path(unit["path"]).suffix not in policy["extensions"]:
                continue
            index = artifact.lookup[(unit["path"], unit["start_line"], unit["end_line"])]
            captured = artifact.units[index]
            if (captured["text_sha256"] != sha(unit["text"].encode()) or
                    any(captured[key] != unit[key] for key in ("source_sha256", "record_ids"))):
                raise ValueError("Model input does not match artifact")
            slices, status = windows(unit["text"], tokenizer, policy)
            statuses[status] = statuses.get(status, 0) + 1
            for item in slices:
                projected.append({"parent": index, **item})
                total_bytes += len(item["text"].encode())
    if not projected or len(projected) > policy["max_extra_vectors"] or total_bytes > policy["max_extra_bytes"]:
        raise ValueError("Window corpus budget")
    original_policy = json.loads((source / "policy.json").read_bytes())
    if (manifest["units"] + len(projected) > original_policy["max_units"] or
            manifest["bytes"] + total_bytes > original_policy["max_input_bytes"]):
        raise ValueError("Original corpus budget")
    cohorts = json.loads((args.freeze / "cohorts.json").read_bytes())
    prior = json.loads((args.control / "report.json").read_bytes())
    args.output.mkdir(parents=True, exist_ok=False)
    args.output.chmod(0o700)
    exporter = Path(binding["exporter"])
    if fingerprint(exporter) != binding["exporter_sha256"]:
        raise ValueError("Frozen lexical exporter changed")
    queries = [case["query"] for case in policy["new_cases"]]
    new_sources = {path: fingerprint(Path(config["root"]) / path)
                   for case in policy["new_cases"] for path in case["expected_paths"]}
    if any(artifact.sources.get(path) != digest for path, digest in new_sources.items()):
        raise ValueError("New question source differs from frozen source")
    new_lexical, _ = lexical_batch(exporter, {"root": config["root"], "profile": config["profile_path"]},
        Path(config["out_dir"]), queries, artifact.manifest["binding"]["snapshot_id"])
    write(args.output / "new-lexical.json", new_lexical)
    frozen = {"schema_version": "shin.semantic-token-window-freeze.v1", "policy": policy,
        "policy_sha256": fingerprint(policy_path), "runner_sha256": fingerprint(Path(__file__)),
        "freeze_sha256": FREEZE_SHA, "control_sha256": CONTROL_SHA,
        "model_manifest_sha256": MODEL["manifest_sha256"], "input_manifest_sha256": binding["input_manifest_sha256"],
        "vector_report_sha256": fingerprint(Path(binding["run"]) / "report.json"),
        "new_lexical_sha256": fingerprint(args.output / "new-lexical.json"), "new_question_sources": new_sources,
        "statuses": statuses, "extra_vectors": len(projected), "extra_input_bytes": total_bytes,
        "windows": [{**{k: v for k, v in item.items() if k != "text"}, "text_sha256": sha(item["text"].encode()),
                     "bound_parent": artifact.units[item["parent"]]} for item in projected]}
    write(args.output / "freeze.json", frozen)

    def budget():
        if time.monotonic() - started > policy["max_run_seconds"] or peak_rss_bytes() > original_policy["max_peak_rss_bytes"]:
            raise ValueError("Experiment resource budget")

    budget()
    model = load(args.model_dir, MODEL)
    vectors = []
    for start in range(0, len(projected), original_policy["batch_size"]):
        budget()
        batch = projected[start:start + original_policy["batch_size"]]
        vectors.extend(model.embed([MODEL["passage_prefix"] + item["text"] for item in batch], batch_size=original_policy["batch_size"]))
    matrix = np.asarray(vectors, dtype=np.float32)
    if (matrix.shape != (len(projected), MODEL["dimensions"]) or not np.isfinite(matrix).all()
            or not np.all(np.abs(np.linalg.norm(matrix, axis=1) - 1) < 0.001)):
        raise ValueError("Invalid window vectors")
    parents = np.asarray([item["parent"] for item in projected])
    results = []
    for project in cohorts["development"]["projects"]:
        project_id = project["id"]
        config = json.loads((args.artifacts / project_id / "route.json").read_bytes())
        artifact = Artifact(Path(config["artifact_dir"]), config["artifact_sha256"], config)
        groups = [(name, next(p for p in cohort["projects"] if p["id"] == project_id)["cases"],
                   json.loads((args.control / f"{name}-{project_id}-lexical.json").read_bytes())["results"])
                  for name, cohort in cohorts.items()]
        if project_id == "shin":
            groups.append(("new-topic", policy["new_cases"], new_lexical["results"]))
        for name, cases, lexical in groups:
            if len(cases) != len(lexical):
                raise ValueError("Question count changed")
            for case, core in zip(cases, lexical):
                budget()
                if core["query"] != case["query"]:
                    raise ValueError("Core question changed")
                artifact.admit(core)
                query = np.asarray(next(model.embed([MODEL["query_prefix"] + case["query"]])), dtype=np.float32)
                scores = artifact.matrix @ query
                parent, excluded = navigate(artifact, core, scores)
                if name != "new-topic":
                    old = next(row for row in prior["results"][name] if row["id"] == case["id"])
                    if not equivalent(old["hybrid"], parent) or excluded != old["lexical_excluded"]:
                        raise ValueError("Parent replay differs from historical control")
                if project_id == "shin":
                    np.maximum.at(scores, parents, matrix @ query)
                candidate, _ = navigate(artifact, core, scores)
                assert_transport_lossless(core, candidate)
                before, after = rank(parent["candidates"], set(case["expected_paths"])), rank(candidate["candidates"], set(case["expected_paths"]))
                regression = (before is not None and (after is None or after > before))
                if case["kind"] == "exact":
                    regression = regression or not equivalent(parent, candidate)
                if case["kind"] == "negative":
                    regression = regression or any(candidate[key] is not False for key in (
                        "semantic_grants_lexical_coverage", "symbol_existence_asserted", "current_source_verified", "pack_completion_asserted"))
                results.append({"id": case["id"], "cohort": name, "project": project_id, "kind": case["kind"],
                    "parent_rank": before, "window_rank": after, "regression": regression,
                    "improved_miss": case["kind"] in ("english", "russian") and (before is None or before > 5) and after is not None and after <= 5,
                    "parent": parent if project_id == "shin" else None,
                    "window": candidate if project_id == "shin" else None})
    budget()
    passed = (len(results) == 64 and not any(row["regression"] for row in results)
              and any(row["improved_miss"] and row["cohort"] != "new-topic" for row in results))
    report = {"schema_version": "shin.semantic-token-window-result.v1", "accepted_for_further_study": passed,
        "activated": False, "source_mutated": False, "freeze_sha256": fingerprint(args.output / "freeze.json"),
        "parent_parity_cases": sum(row["cohort"] != "new-topic" for row in results),
        "document_vectors_computed": len(projected), "query_vectors_computed": len(results),
        "regressions": sum(row["regression"] for row in results),
        "improved_existing_misses": sum(row["improved_miss"] and row["cohort"] != "new-topic" for row in results),
        "seconds": round(time.monotonic() - started, 3), "peak_rss_bytes": peak_rss_bytes(), "results": results}
    write(args.output / "report.json", report)
    print(json.dumps({k: v for k, v in report.items() if k != "results"}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ("artifacts", "freeze", "control", "model-dir", "output"):
        parser.add_argument("--" + field, required=True, type=Path)
    run(parser.parse_args())
