#!/usr/bin/env python3
"""Offline local candidate discovery; never changes Core ranking or Pack policy."""

import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import resource
import sys
import time

from prepare_semantic_pilot import sources_current
from run_carry_forward_comparison import save
from run_project_search_comparison import validate_spec


MODEL_FILES = ("model.onnx", "tokenizer.json", "tokenizer_config.json", "config.json", "special_tokens_map.json")


def fingerprint(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def seal_model(directory, policy):
    if fingerprint(directory / "model.onnx") != policy["model_sha256"]:
        raise ValueError("Model weights differ from the pinned upstream digest")
    files = {name: {"sha256": fingerprint(directory / name), "bytes": (directory / name).stat().st_size}
             for name in MODEL_FILES}
    if sum(item["bytes"] for item in files.values()) > policy["max_model_bytes"]:
        raise ValueError("Model assets exceed the frozen size budget")
    if json.loads((directory / "tokenizer_config.json").read_text())["model_max_length"] != policy["max_tokens"]:
        raise ValueError("Tokenizer limit differs from the frozen policy")
    with (directory / "manifest.json").open("x") as stream:
        json.dump({"model": policy["model"], "revision": policy["revision"], "files": files}, stream, indent=2)


def verify_model(directory, policy):
    if not (directory / "manifest.json").is_file():
        raise FileNotFoundError("Local model is unavailable; explicit provisioning is required")
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest["model"] != policy["model"] or manifest["revision"] != policy["revision"]:
        raise ValueError("Model identity differs from the frozen policy")
    if set(manifest["files"]) != set(MODEL_FILES):
        raise ValueError("Model manifest must contain exactly the reviewed assets")
    for name, item in manifest["files"].items():
        if (directory / name).is_symlink() or fingerprint(directory / name) != item["sha256"]:
            raise ValueError("Local model asset changed: " + name)
    if manifest["files"]["model.onnx"]["sha256"] != policy["model_sha256"]:
        raise ValueError("Model weights are not the pinned upstream bytes")
    return manifest


def load_model(directory, policy):
    verify_model(directory, policy)
    # Inference has no provisioning path, even when a cache or credential exists.
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["HF_HUB_DISABLE_TELEMETRY"] = "1"
    os.environ["TOKENIZERS_PARALLELISM"] = "false"
    from fastembed import TextEmbedding
    from fastembed.common.model_description import ModelSource, PoolingType
    if importlib.metadata.version("fastembed") != "0.8.1":
        raise ValueError("Use the isolated pinned pilot environment")
    TextEmbedding.add_custom_model(policy["model"], pooling=PoolingType.MEAN, normalization=True,
        sources=ModelSource(hf=policy["model"]), dim=policy["dimensions"], model_file="model.onnx",
        license=policy["license"])
    return TextEmbedding(model_name=policy["model"], specific_model_path=str(directory),
        cache_dir=str(directory), local_files_only=True, threads=policy["threads"],
        providers=["CPUExecutionProvider"], cuda=False)


def peak_rss_bytes():
    value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return value if sys.platform == "darwin" else value * 1024


def check_budget(policy, started):
    if time.monotonic() - started > policy["max_run_seconds"]:
        raise RuntimeError("Predeclared wall-time budget exceeded")
    if peak_rss_bytes() > policy["max_peak_rss_bytes"]:
        raise RuntimeError("Predeclared peak-RSS budget exceeded")


def load_input(directory):
    manifest = json.loads((directory / "input-manifest.json").read_text())
    for name, key in (("spec.json", "spec_sha256"), ("policy.json", "policy_sha256"), ("units.jsonl", "units_sha256")):
        if fingerprint(directory / name) != manifest[key]:
            raise ValueError("Frozen input changed: " + name)
    spec = json.loads((directory / "spec.json").read_text())
    policy = json.loads((directory / "policy.json").read_text())
    with (directory / "units.jsonl").open() as stream:
        units = [json.loads(line) for line in stream]
    if not units or len(units) != manifest["units"] or len(units) > policy["max_units"]:
        raise ValueError("Frozen unit count is empty, inconsistent or exceeds policy")
    if (directory / "units.jsonl").stat().st_size > policy["max_input_bytes"]:
        raise ValueError("Frozen input byte budget exceeded")
    return manifest, spec, policy, units


def candidate(unit, score):
    result = {key: unit[key] for key in ("project", "path", "start_line", "end_line", "source_sha256", "record_ids")} | {
        "similarity": float(score), "channel": "local-semantic-pilot",
        "status": "navigation-candidate-not-an-answer", "current_source_verified": False,
    }
    if unit.get("unit_kind") == "semantic-subrange":
        for key in ("unit_kind", "span_hash", "parent_ranges", "truncated", "redacted"):
            result[key] = unit[key]
    return result


def summarize_quality(rows, policy):
    summary = {kind: {"queries": sum(row["kind"] == kind for row in rows),
                     "expected_at_20": sum(row["kind"] == kind and row["expected_rank_at_20"] is not None for row in rows),
                     "expected_in_first_5": sum(row["kind"] == kind and row["expected_in_first_5"] for row in rows)}
               for kind in ("exact", "english", "russian", "negative")}
    passed = all(summary[kind]["queries"] == policy["quality"]["cohort_size_per_language"] and
                 summary[kind]["expected_at_20"] >= policy["quality"][kind + "_min_expected_paths_at_20"]
                 for kind in ("english", "russian"))
    return summary, passed


def same_vector_input(previous, current):
    if {key: value for key, value in previous.items() if key != "text"} != {
            key: value for key, value in current.items() if key != "text"}:
        raise ValueError("Reuse requires identical source units and record bindings")
    return previous["text"] == current["text"]


def reuse_compatible_policy(previous, current):
    if {key: value for key, value in previous.items() if key != "projection"} != {
            key: value for key, value in current.items() if key != "projection"}:
        raise ValueError("Reuse requires identical model, preprocessing, budgets and acceptance")


def load_completed_run(directory):
    report = json.loads((directory / "report.json").read_text())
    binding = json.loads((directory / "run-binding.json").read_text())
    source = Path(binding["input"])
    manifest = json.loads((source / "input-manifest.json").read_text())
    if report.get("completed") is not True or fingerprint(directory / "vectors.npy") != report["vectors_sha256"]:
        raise ValueError("Comparison requires complete, bound vector runs")
    if fingerprint(source / "input-manifest.json") != binding["input_manifest_sha256"]:
        raise ValueError("Run input binding changed")
    for name, key in (("policy.json", "policy_sha256"), ("spec.json", "spec_sha256"), ("units.jsonl", "units_sha256")):
        if fingerprint(source / name) != manifest[key]:
            raise ValueError("Frozen input changed")
    if json.loads((source / "policy.json").read_text()) != binding["policy"]:
        raise ValueError("Run policy differs from bound input")
    if report["units"] != manifest["units"] or not 0 < manifest["units"] <= binding["policy"]["max_units"]:
        raise ValueError("Run unit count differs or exceeds budget")
    if (source / "units.jsonl").stat().st_size > binding["policy"]["max_input_bytes"]:
        raise ValueError("Run input byte budget exceeded")
    return report, binding, manifest, source


def verify_vector_runtime(report, policy, model_dir):
    versions = {name: importlib.metadata.version(name) for name in ("fastembed", "onnxruntime", "tokenizers", "numpy")}
    options = {"enable_cpu_mem_arena": True, "providers": ["CPUExecutionProvider"],
               "threads": policy["threads"], "batch_size": policy["batch_size"], "input_loading": "streamed-jsonl"}
    if report["runtime_versions"] != versions or report["runtime_options"] != options:
        raise ValueError("Reuse runtime or preprocessing implementation differs")
    if fingerprint(model_dir / "manifest.json") != report["model_manifest_sha256"]:
        raise ValueError("Reuse model assets differ")


def project_identities(manifest):
    result = {}
    for project in manifest["projects"]:
        if project["id"] in result:
            raise ValueError("Duplicate project identity")
        result[project["id"]] = (project["root"], project["profile_sha256"])
    return result


def vector_input_key(unit):
    return hashlib.sha256(json.dumps(unit, sort_keys=True, separators=(",", ":"),
                                    ensure_ascii=True, allow_nan=False).encode()).digest()


def augmented_reuse_mapping(previous_input, previous_manifest, manifest, units):
    if project_identities(previous_manifest) != project_identities(manifest):
        raise ValueError("Augmented reuse requires identical project roots and profile ownership")
    return bound_reuse_mapping(previous_input, previous_manifest, manifest, units)


def profile_rebound_mapping(previous_input, previous_manifest, manifest, units, admission):
    old, new = project_identities(previous_manifest), project_identities(manifest)
    if {p: v[0] for p, v in old.items()} != {p: v[0] for p, v in new.items()}:
        raise ValueError("Profile transition cannot change project roots or membership")
    changes = [{"project": p, "root": old[p][0], "from": old[p][1], "to": new[p][1]}
               for p in sorted(old) if old[p][1] != new[p][1]]
    if (admission.get("schema_version") != "shin.semantic-profile-transition.v1" or not changes or
            admission.get("changes") != changes or
            admission.get("donor_manifest_digest") != vector_input_key(previous_manifest).hex() or
            admission.get("destination_manifest_digest") != vector_input_key(manifest).hex()):
        raise ValueError("Profile transition requires an exact reviewed manifest pair and profile changes")
    return bound_reuse_mapping(previous_input, previous_manifest, manifest, units)


def bound_reuse_mapping(previous_input, previous_manifest, manifest, units):
    sources = {p["id"]: p["sources"] for p in previous_manifest["projects"]}
    current_sources = {p["id"]: p["sources"] for p in manifest["projects"]}

    def bound_key(unit, inventory):
        source = inventory.get(unit["project"], {}).get(unit["path"])
        if source is None or unit["source_sha256"] != source["sha256"]:
            raise ValueError("Unit source binding differs from its frozen inventory")
        return vector_input_key(unit)

    old, hashed = {}, hashlib.sha256()
    with (previous_input / "units.jsonl").open("rb") as stream:
        for index, line in enumerate(stream):
            hashed.update(line)
            key = bound_key(json.loads(line), sources)
            if key in old or index >= previous_manifest["units"]:
                raise ValueError("Duplicate or excess reuse units")
            old[key] = index
    if len(old) != previous_manifest["units"] or hashed.hexdigest() != previous_manifest["units_sha256"]:
        raise ValueError("Reuse units changed or count differs")
    seen, mapping = set(), []
    for unit in units:
        key = bound_key(unit, current_sources)
        if key in seen:
            raise ValueError("Duplicate destination units")
        seen.add(key)
        mapping.append(old.get(key))
    return mapping


def copy_reusable_vectors(previous_run, manifest, policy, units, model_dir, vectors, mode="exact", admission=None):
    import numpy as np
    previous_report, previous_binding, prior_manifest, previous_input = load_completed_run(previous_run)
    reuse_compatible_policy(previous_binding["policy"], policy)
    if prior_manifest["spec_sha256"] != manifest["spec_sha256"]:
        raise ValueError("Reuse questions differ")
    verify_vector_runtime(previous_report, policy, model_dir)
    prior = np.load(previous_run / "vectors.npy", mmap_mode="r", allow_pickle=False)
    if (prior.shape != (prior_manifest["units"], policy["dimensions"]) or prior.dtype != np.float32 or
            vectors.shape != (len(units), policy["dimensions"]) or vectors.dtype != np.float32):
        raise ValueError("Reuse vector dimensions or type differ")
    if (mode == "profile-rebound") != (admission is not None):
        raise ValueError("Only explicit profile-rebound mode accepts a transition admission")
    if mode in ("augmented", "profile-rebound"):
        mapping = (profile_rebound_mapping(previous_input, prior_manifest, manifest, units, admission)
                   if admission is not None else augmented_reuse_mapping(previous_input, prior_manifest, manifest, units))
        changed = []
        for index, donor in enumerate(mapping):
            if donor is None:
                changed.append(index)
            else:
                if not np.isfinite(prior[donor]).all() or not np.isclose(np.linalg.norm(prior[donor]), 1, atol=1e-4):
                    raise ValueError("Invalid donor vector")
                vectors[index] = prior[donor]
        return changed, {"run": str(previous_run), "report_sha256": fingerprint(previous_run / "report.json"),
            "donor_input_manifest_sha256": previous_binding["input_manifest_sha256"],
            "destination_units_sha256": manifest["units_sha256"],
            "copied_vectors": len(units) - len(changed), "recomputed_vectors": len(changed),
            "unused_donor_vectors": len(prior) - (len(units) - len(changed)),
            "mapping_sha256": fingerprint_mapping(mapping),
            "profile_transition": admission,
            "policy": ("profile-rebound-v1: reviewed manifest pair; " if admission is not None else "augmented-v1: same project/profile; ") +
                      "exact full-unit text and provenance key; same model/runtime; destination order preserved"}
    if mode != "exact" or prior_manifest["projects"] != manifest["projects"] or prior.shape != vectors.shape:
        raise ValueError("Exact reuse requires identical corpus and row order")
    changed, count = [], 0
    hashed = hashlib.sha256()
    with (previous_input / "units.jsonl").open("rb") as stream:
        for index, line in enumerate(stream):
            hashed.update(line)
            if index >= len(units):
                raise ValueError("Reuse unit count differs")
            if same_vector_input(json.loads(line), units[index]):
                vectors[index] = prior[index]
            else:
                changed.append(index)
            count += 1
    if count != len(units) or hashed.hexdigest() != prior_manifest["units_sha256"]:
        raise ValueError("Reuse units changed or count differs")
    return changed, {"run": str(previous_run), "report_sha256": fingerprint(previous_run / "report.json"),
        "copied_vectors": count - len(changed), "recomputed_vectors": len(changed),
        "policy": "exact-text-and-source-binding; completed hash-verified model/index only"}


def fingerprint_mapping(mapping):
    return hashlib.sha256(json.dumps(mapping, separators=(",", ":")).encode()).hexdigest()


def run(input_dir, model_dir, output, reuse_run=None, reuse_mode="exact", evaluation_freeze=None, profile_transition=None):
    started = time.monotonic()
    manifest, spec, policy, units = load_input(input_dir)
    validate_spec(spec, output)
    if reuse_mode != "exact" and reuse_run is None:
        raise ValueError("Augmented reuse requires a completed donor run")
    if (reuse_mode == "profile-rebound") != (profile_transition is not None):
        raise ValueError("Profile-rebound mode requires its explicit transition file")
    admission = json.loads(profile_transition.read_text()) if profile_transition is not None else None
    freeze_hash = None
    if evaluation_freeze is not None:
        freeze = json.loads(evaluation_freeze.read_text())
        if freeze["input_manifest_sha256"] != fingerprint(input_dir / "input-manifest.json"):
            raise ValueError("Evaluation freeze belongs to a different corpus")
        freeze_hash = fingerprint(evaluation_freeze)
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    save(output / "run-binding.json", {"input_manifest_sha256": fingerprint(input_dir / "input-manifest.json"),
        "policy": policy, "runner_sha256": fingerprint(Path(__file__)), "input": str(input_dir),
        "reuse_mode": reuse_mode, "evaluation_freeze_sha256": freeze_hash,
        "profile_transition_sha256": fingerprint(profile_transition) if profile_transition else None})
    completed = 0
    try:
        model = load_model(model_dir, policy)
        import numpy as np
        from tokenizers import Tokenizer
        load_seconds = time.monotonic() - started
        tokenizer = Tokenizer.from_file(str(model_dir / "tokenizer.json"))
        tokenizer.no_truncation()
        tokenizer.no_padding()
        lengths = []
        for start in range(0, len(units), 128):
            texts = [policy["passage_prefix"] + unit["text"] for unit in units[start:start + 128]]
            lengths.extend(len(item.ids) for item in tokenizer.encode_batch(texts))
            check_budget(policy, started)
        vectors = np.lib.format.open_memmap(output / "vectors.npy", mode="w+", dtype=np.float32,
                                            shape=(len(units), policy["dimensions"]))
        changed = range(len(units))
        reuse = None
        if reuse_run is not None:
            changed, reuse = copy_reusable_vectors(reuse_run, manifest, policy, units, model_dir, vectors, reuse_mode, admission)
            completed = len(units) - len(changed)
            save(output / "reuse.json", reuse)
            print(json.dumps(reuse), flush=True)
        # Length ordering changes batching cost, never the enrolled units or retrieval policy.
        order = sorted(changed, key=lambda index: (min(lengths[index], policy["max_tokens"]), index))
        check_budget(policy, started)
        embedding_started = time.monotonic()
        for offset in range(0, len(order), policy["batch_size"]):
            batch = order[offset:offset + policy["batch_size"]]
            texts = [policy["passage_prefix"] + units[index]["text"] for index in batch]
            values = np.array(list(model.embed(texts, batch_size=policy["batch_size"], parallel=None)))
            if values.shape != (len(batch), policy["dimensions"]) or not np.isfinite(values).all():
                raise ValueError("Invalid model vector output")
            if not np.allclose(np.linalg.norm(values, axis=1), 1, atol=1e-4):
                raise ValueError("Model vectors are not normalized")
            vectors[batch] = values
            completed += len(batch)
            check_budget(policy, started)
            if (offset + len(batch)) % 1024 == 0 or completed == len(units):
                progress = {"completed": completed, "total": len(units),
                            "seconds": round(time.monotonic() - started, 2), "peak_rss_bytes": peak_rss_bytes()}
                save(output / "progress.json", progress)
                print(json.dumps(progress), flush=True)
        vectors.flush()
        embedding_seconds = time.monotonic() - embedding_started
        results = []
        for project in spec["projects"]:
            indexes = np.array([index for index, unit in enumerate(units) if unit["project"] == project["id"]])
            matrix = vectors[indexes]
            for case in project["cases"]:
                query_started = time.monotonic()
                query_vector = np.array(next(model.embed([policy["query_prefix"] + case["query"]])))
                if query_vector.shape != (policy["dimensions"],) or not np.isfinite(query_vector).all():
                    raise ValueError("Invalid query vector")
                scores = matrix @ query_vector
                selected = np.argsort(-scores, kind="stable")[:policy["candidate_limit"]]
                candidates = [candidate(units[int(indexes[index])], scores[index]) for index in selected]
                rank = next((index + 1 for index, item in enumerate(candidates)
                             if item["path"] in case["expected_paths"]), None)
                row = {"project": project["id"], "id": case["id"], "kind": case["kind"],
                       "expected_rank_at_20": rank, "expected_in_first_5": rank is not None and rank <= policy["report_limit"],
                       "query_ms": (time.monotonic() - query_started) * 1000, "candidates": candidates}
                results.append(row)
                print(json.dumps({key: row[key] for key in ("id", "expected_rank_at_20", "expected_in_first_5", "query_ms")}), flush=True)
                check_budget(policy, started)
        summary, quality_met = summarize_quality(results, policy)
        current_drift = {project["id"]: sources_current(Path(project["root"]), project["sources"])
                         for project in manifest["projects"]}
        report = {"schema_version": "shin.local-semantic-pilot-result.v1", "completed": True,
            "candidate_quality_threshold_met": quality_met, "summary": summary, "results": results,
            "units": len(units), "token_truncated_units": sum(value > policy["max_tokens"] for value in lengths),
            "model_load_seconds": load_seconds, "embedding_seconds": embedding_seconds,
            "total_seconds": time.monotonic() - started, "peak_rss_bytes": peak_rss_bytes(),
            "source_drift_since_freeze": current_drift, "vectors_sha256": fingerprint(output / "vectors.npy"),
            "model_manifest_sha256": fingerprint(model_dir / "manifest.json"),
            "runtime_versions": {name: importlib.metadata.version(name) for name in ("fastembed", "onnxruntime", "tokenizers", "numpy")},
            "runtime_options": {"enable_cpu_mem_arena": True, "providers": ["CPUExecutionProvider"],
                                "threads": policy["threads"], "batch_size": policy["batch_size"],
                                "input_loading": "streamed-jsonl"},
            "vector_reuse": reuse,
            "boundary": "Frozen published text candidate recall, not full raw code, final Core selection, equality with jbcontext enrollment, task success, current-source proof or Pack completion. Dense negatives remain navigation candidates. No exact-query routing or quality policy uses case labels; labels are evaluation only."}
        check_budget(policy, started)
        save(output / "report.json", report)
        print(json.dumps({"summary": summary, "candidate_quality_threshold_met": quality_met}), flush=True)
    except Exception as error:
        save(output / "failure.json", {"completed": False, "completed_units": completed,
             "error_type": type(error).__name__, "message": str(error),
             "seconds": time.monotonic() - started, "peak_rss_bytes": peak_rss_bytes()})
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    seal = subs.add_parser("seal-model")
    seal.add_argument("--model-dir", type=Path, required=True)
    seal.add_argument("--policy", type=Path, required=True)
    execute = subs.add_parser("run")
    for name in ("input", "model-dir", "output"):
        execute.add_argument("--" + name, type=Path, required=True)
    execute.add_argument("--reuse-run", type=Path)
    execute.add_argument("--reuse-mode", choices=("exact", "augmented", "profile-rebound"), default="exact")
    execute.add_argument("--evaluation-freeze", type=Path)
    execute.add_argument("--profile-transition", type=Path)
    args = parser.parse_args()
    if args.command == "seal-model":
        seal_model(args.model_dir.resolve(), json.loads(args.policy.read_text()))
    else:
        run(args.input.resolve(), args.model_dir.resolve(), args.output.resolve(),
            args.reuse_run.resolve() if args.reuse_run else None, args.reuse_mode,
            args.evaluation_freeze.resolve() if args.evaluation_freeze else None,
            args.profile_transition.resolve() if args.profile_transition else None)
