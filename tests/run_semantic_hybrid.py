#!/usr/bin/env python3
"""One predeclared native-lexical/local-semantic union over frozen parent vectors."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import time

from evaluate_semantic_selection import select_by_path
from evaluate_semantic_chunks import validate_heldout
from prepare_semantic_pilot import sources_current
from reproject_semantic_identifiers import publication_fingerprint
from run_carry_forward_comparison import save
from run_local_semantic_pilot import (candidate, check_budget, fingerprint, load_completed_run,
    load_input, load_model, peak_rss_bytes, summarize_quality, verify_vector_runtime)
from semantic_hybrid import combine, map_lexical, parent_lookup, regression_gate
from run_project_search_comparison import validate_spec
from semantic_source_hop import follow_source_reference, read_bound_links


def load_policy(path):
    policy = json.loads(path.read_text())
    if (policy.get("schema_version") not in ("shin.semantic-hybrid-experiment.v1", "shin.semantic-document-first-experiment.v1",
                                            "shin.semantic-code-anchor-experiment.v1", "shin.semantic-source-hop-experiment.v1") or
            any(policy.get(k) != v for k, v in {"candidate_limit": 20, "report_limit": 5,
                "per_document": 2}.items()) or
            (policy["schema_version"] == "shin.semantic-hybrid-experiment.v1" and policy.get("rank_constant") != 60)):
        raise ValueError("Use the frozen single-policy hybrid experiment")
    return policy


def verify_publications(spec, manifest, publications, profiles=None):
    inventory = {p["id"]: p for p in manifest["projects"]}
    snapshots = {}
    for project in spec["projects"]:
        bound = inventory[project["id"]]
        profile = Path((profiles or {}).get(project["id"], project["profile"]))
        if project["root"] != bound["root"] or fingerprint(profile) != bound["profile_sha256"]:
            raise ValueError("Project/profile identity changed")
        path = publications / project["id"] / "index/docs/kb/generated/manifest.json"
        if fingerprint(path) != bound["publication_sha256"]:
            raise ValueError("Publication differs from the frozen vector corpus")
        snapshots[project["id"]] = publication_fingerprint(path.read_bytes())
    return snapshots


def freeze(run, publications, heldout_path, policy_path, exporter, parent_control, output, recovered_profiles=None, input_dir=None, extra_cases=None):
    if input_dir is not None:
        if run is not None or parent_control is not None:
            raise ValueError("A new corpus must not borrow old scores or vector bindings")
        manifest, _, _, units = load_input(input_dir)
        if any(u.get("unit_kind") == "semantic-subrange" for u in units):
            raise ValueError("New corpus selection is parent-only")
        source = input_dir
    else:
        _, _, manifest, source = load_completed_run(run)
    development = json.loads((source / "spec.json").read_text())
    heldout = json.loads(heldout_path.read_text())
    identity = lambda p: (p["id"], p["root"], p["profile"])
    if list(map(identity, development["projects"])) != list(map(identity, heldout["projects"])):
        raise ValueError("Cohort routes differ")
    cohorts = {"development": development, "heldout": heldout}
    targets = None
    if extra_cases is not None:
        extra = json.loads(extra_cases.read_text())
        validate_spec(extra, output)
        observed = {"projects": [{**a, "cases": a["cases"] + b["cases"]}
                                  for a, b in zip(development["projects"], heldout["projects"], strict=True)]}
        _, _, _, units = load_input(source)
        targets = validate_heldout(extra, observed, manifest, units)
        cohorts["challenge"] = extra
    control_format, control_binding_hash = "parent-matrix", None
    if parent_control is not None:
        previous = json.loads(parent_control.read_text())
        if previous.get("completed") is not True:
            raise ValueError("Prior control is incomplete")
        if isinstance(previous["results"]["development"], list):
            control_format = "selection-report"
            binding_path = parent_control.parent / "binding.json"
            if json.loads(binding_path.read_text())["run_report_sha256"] != fingerprint(run / "report.json"):
                raise ValueError("Selection control belongs to a different vector run")
            control_binding_hash = fingerprint(binding_path)
    profiles = {}
    for project in development["projects"]:
        original = Path(project["profile"])
        recovered = recovered_profiles / project["id"] / original.name if recovered_profiles else None
        captured = publications / project["id"] / "profiles" / original.name
        profiles[project["id"]] = str(recovered if recovered and recovered.is_file() else
                                      captured if captured.is_file() else original)
    snapshots = verify_publications(development, manifest, publications, profiles)
    policy = load_policy(policy_path)
    link_bindings = {}
    if policy["schema_version"] == "shin.semantic-source-hop-experiment.v1":
        remaining = json.loads((source / "policy.json").read_text())["max_input_bytes"] - manifest["bytes"]
        for project in manifest["projects"]:
            _, link_binding = read_bound_links(publications / project["id"] / "index/docs/kb/generated",
                                              project["publication_sha256"], remaining)
            remaining -= link_binding["bytes"]
            link_bindings[project["id"]] = link_binding
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    for project in development["projects"]:
        original = Path(profiles[project["id"]])
        saved = output / "profiles" / project["id"] / original.name
        saved.parent.mkdir(parents=True)
        saved.write_bytes(original.read_bytes())
        profiles[project["id"]] = str(saved)
    verify_publications(development, manifest, publications, profiles)
    save(output / "policy.json", policy)
    save(output / "cohorts.json", cohorts)
    receipt = {"schema_version": "shin.semantic-hybrid-freeze.v1",
        "created_at": datetime.now(timezone.utc).isoformat(), "run": str(run) if run else None,
        "run_report_sha256": fingerprint(run / "report.json") if run else None,
        "input_manifest_sha256": fingerprint(source / "input-manifest.json"),
        "policy_sha256": fingerprint(output / "policy.json"),
        "cohorts_sha256": fingerprint(output / "cohorts.json"),
        "publications": str(publications), "snapshots": snapshots, "profiles": profiles,
        "exporter": str(exporter), "exporter_sha256": fingerprint(exporter),
        "parent_control": str(parent_control) if parent_control else None,
        "parent_control_sha256": fingerprint(parent_control) if parent_control else None,
        "parent_control_format": control_format, "parent_control_binding_sha256": control_binding_hash,
        "new_topic_source_sha256": targets,
        "source_links": link_bindings,
        "source_hop_sha256": fingerprint(Path(__file__).with_name("semantic_source_hop.py")) if link_bindings else None,
        "baseline": "bound-parent-control" if parent_control else "fresh-same-matrix-observed-cohorts",
        "runner_sha256": fingerprint(Path(__file__)),
        "combiner_sha256": fingerprint(Path(__file__).with_name("semantic_hybrid.py"))}
    save(output / "freeze.json", receipt)
    print(json.dumps(receipt, indent=2))


def lexical_batch(exporter, project, index, queries, snapshot, require_current=False):
    profile_path = Path(project["profile"])
    args = [str(exporter), project["root"], str(index), str(profile_path.parent),
            json.loads(profile_path.read_text())["id"]]
    if require_current:
        args.append("--require-current")
    started = time.monotonic()
    result = subprocess.run(args, input=json.dumps(queries), text=True, capture_output=True, timeout=180)
    if result.returncode:
        raise ValueError("Native lexical admission failed; no semantic fallback is allowed")
    payload = json.loads(result.stdout)
    if (payload.get("completed") is not True or payload["snapshot_id"] != snapshot or
            len(payload["results"]) != len(queries)):
        raise ValueError("Native lexical batch binding differs")
    for query, row in zip(queries, payload["results"], strict=True):
        if row["query"] != query or row["snapshot_id"] != snapshot or row["profile"] != json.loads(profile_path.read_text())["id"]:
            raise ValueError("Native lexical query binding differs")
        if not require_current and row["index_freshness"]["status"] != "not-assessed":
            raise ValueError("Frozen lexical comparison must not imply currentness")
    return payload, time.monotonic() - started


def model_available(directory):
    # Absence is an optional-channel state. Partial/corrupt installations still fail verification.
    return directory.exists()


def evaluate(freeze_dir, model_dir, output, require_current=False, vector_run=None):
    started = time.monotonic()
    receipt = json.loads((freeze_dir / "freeze.json").read_text())
    for file, key in (("policy.json", "policy_sha256"), ("cohorts.json", "cohorts_sha256")):
        if fingerprint(freeze_dir / file) != receipt[key]:
            raise ValueError("Frozen policy or questions changed")
    exporter = Path(receipt["exporter"])
    for path, key in ((exporter, "exporter_sha256"), (Path(__file__), "runner_sha256"),
                      (Path(__file__).with_name("semantic_hybrid.py"), "combiner_sha256")):
        if fingerprint(path) != receipt[key]:
            raise ValueError("Frozen experiment implementation changed")
    if receipt.get("source_hop_sha256") is not None and fingerprint(Path(__file__).with_name("semantic_source_hop.py")) != receipt["source_hop_sha256"]:
        raise ValueError("Frozen source-hop implementation changed")
    if receipt["run"] is None and vector_run is None:
        raise ValueError("New corpus evaluation requires its pre-bound vector run")
    if receipt["run"] is not None and vector_run is not None:
        raise ValueError("Cannot override a frozen vector run")
    run = vector_run if vector_run is not None else Path(receipt["run"])
    report, binding, manifest, source = load_completed_run(run)
    if ((receipt["run_report_sha256"] is not None and fingerprint(run / "report.json") != receipt["run_report_sha256"]) or
            binding["input_manifest_sha256"] != receipt["input_manifest_sha256"]):
        raise ValueError("Completed vector run changed")
    if receipt["run"] is None and binding.get("evaluation_freeze_sha256") != fingerprint(freeze_dir / "freeze.json"):
        raise ValueError("New evaluation policy was not bound before the vector run")
    _, spec, model_policy, units = load_input(source)
    policy = load_policy(freeze_dir / "policy.json")
    cohorts = json.loads((freeze_dir / "cohorts.json").read_text())
    previous = None
    if receipt["parent_control"] is not None:
        parent_control = Path(receipt["parent_control"])
        if fingerprint(parent_control) != receipt["parent_control_sha256"]:
            raise ValueError("Prior parent-only control changed")
        previous = json.loads(parent_control.read_text())
        if previous.get("completed") is not True:
            raise ValueError("Prior parent-only control is incomplete")
        if receipt.get("parent_control_format") == "selection-report":
            path = parent_control.parent / "binding.json"
            if (fingerprint(path) != receipt["parent_control_binding_sha256"] or
                    json.loads(path.read_text())["run_report_sha256"] != fingerprint(run / "report.json")):
                raise ValueError("Selection control vector binding changed")
    elif any(u.get("unit_kind") == "semantic-subrange" for u in units):
        raise ValueError("New corpus selection is parent-only")
    if cohorts["development"] != spec:
        raise ValueError("Development questions changed")
    publications = Path(receipt["publications"])
    snapshots = verify_publications(spec, manifest, publications, receipt["profiles"])
    if snapshots != receipt["snapshots"]:
        raise ValueError("Snapshot identity changed")
    available = model_available(model_dir)
    if available:
        verify_vector_runtime(report, model_policy, model_dir)
        model = load_model(model_dir, model_policy)
        import numpy as np
        vectors = np.load(run / "vectors.npy", mmap_mode="r", allow_pickle=False)
        if vectors.shape != (len(units), model_policy["dimensions"]) or vectors.dtype != np.float32:
            raise ValueError("Invalid bound vector matrix")
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    save(output / "binding.json", {"freeze_sha256": fingerprint(freeze_dir / "freeze.json"),
        "run_report_sha256": fingerprint(run / "report.json"),
        "source_mode": "require-current" if require_current else "frozen-snapshot-only",
        "semantic_status": "available" if available else "unavailable-lexical-only"})
    lookup = parent_lookup(units)
    source_hop = policy["schema_version"] == "shin.semantic-source-hop-experiment.v1"
    anchored = source_hop or policy["schema_version"] == "shin.semantic-code-anchor-experiment.v1"
    selector_policy = {**policy, "schema_version": "shin.semantic-code-anchor-experiment.v1"} if source_hop else policy
    baseline_policy = {**policy, "schema_version": "shin.semantic-document-first-experiment.v1"}
    inventories = {p["id"]: p for p in manifest["projects"]}
    link_cache, link_budget = {}, model_policy["max_input_bytes"] - manifest["bytes"]
    results, batch_seconds, query_count, raw_replays, parent_replays = {}, 0, 0, 0, 0
    for cohort, case_spec in cohorts.items():
        rows = []
        for project in case_spec["projects"]:
            routed = {**project, "profile": receipt["profiles"][project["id"]]}
            payload, seconds = lexical_batch(exporter, routed, publications / project["id"] / "index",
                [c["query"] for c in project["cases"]], snapshots[project["id"]], require_current)
            batch_seconds += seconds
            save(output / f"{cohort}-{project['id']}-lexical.json", payload)
            indexes = [i for i, u in enumerate(units) if u["project"] == project["id"] and u.get("unit_kind") != "semantic-subrange"]
            paths = [units[i]["path"] for i in indexes]
            by_path = {}
            if source_hop:
                for i in indexes:
                    by_path.setdefault(units[i]["path"], []).append(i)
                if project["id"] not in link_cache:
                    links, link_binding = read_bound_links(publications / project["id"] / "index/docs/kb/generated",
                        inventories[project["id"]]["publication_sha256"], link_budget)
                    if link_binding != receipt["source_links"][project["id"]]:
                        raise ValueError("Frozen source links changed")
                    link_budget -= link_binding["bytes"]
                    link_cache[project["id"]] = links
            if available:
                matrix = vectors[indexes]
            for case, raw in zip(project["cases"], payload["results"], strict=True):
                lexical, excluded = map_lexical(project["id"], raw, units, lookup)
                semantic = {}
                if available:
                    query = np.array(next(model.embed([model_policy["query_prefix"] + case["query"]])))
                    if query.shape != (model_policy["dimensions"],) or not np.isfinite(query).all():
                        raise ValueError("Invalid query vector")
                    scores = matrix @ query
                    if previous is None and cohort == "development":
                        raw_candidates = [candidate(units[indexes[int(i)]], scores[i])
                                          for i in np.argsort(-scores, kind="stable")[:20]]
                        original = next(r for r in report["results"] if r["id"] == case["id"])
                        identity = lambda c: {k: v for k, v in c.items() if k != "similarity"}
                        if list(map(identity, raw_candidates)) != list(map(identity, original["candidates"])):
                            raise ValueError("Fresh raw semantic replay differs in provenance/order")
                        raw_replays += 1
                    order = select_by_path(np.argsort(-scores, kind="stable"), paths, 20, 2)
                    semantic = {indexes[int(i)]: {"rank": rank + 1, "similarity": float(scores[i])} for rank, i in enumerate(order)}
                    query_count += 1
                confirmed = raw["retrieval_mode"] == "identifier-scoped"
                hybrid = combine(units, lexical, semantic, confirmed, selector_policy, raw["retrieval_mode"] == "owner-scoped")
                code_anchor = hybrid
                if source_hop:
                    score_lookup = {i: float(score) for i, score in zip(indexes, scores, strict=True)} if available else {}
                    hybrid = follow_source_reference(code_anchor, units, by_path, score_lookup,
                        link_cache[project["id"]], inventories[project["id"]], policy)
                document_first = combine(units, lexical, semantic, confirmed, baseline_policy,
                                         raw["retrieval_mode"] == "owner-scoped") if anchored else None
                lexical_only = combine(units, lexical, {}, False, selector_policy)["candidates"]
                semantic_only = [candidate(units[i], row["similarity"]) for i, row in semantic.items()]
                if available and previous is not None and cohort in previous["results"]:
                    selection_control = receipt.get("parent_control_format") == "selection-report"
                    prior_rows = previous["results"][cohort] if selection_control else previous["results"][cohort]["parents"]
                    prior = next(r for r in prior_rows if r["id"] == case["id"])
                    prior_candidates = prior["semantic_candidates"] if selection_control else prior["candidates"]
                    identity = lambda c: {k: v for k, v in c.items() if k != "similarity"}
                    if (prior["project"] != project["id"] or prior["kind"] != case["kind"] or
                            list(map(identity, semantic_only)) != list(map(identity, prior_candidates))):
                        raise ValueError("Parent-only semantic replay differs in provenance/order")
                    if selection_control and anchored and document_first != (prior["document_first"] if source_hop else prior["hybrid"]):
                        raise ValueError("Document-first control differs in candidates or provenance")
                    if selection_control and source_hop and code_anchor != prior["hybrid"]:
                        raise ValueError("Code-anchor control differs in candidates or provenance")
                    parent_replays += 1
                rank = lambda items: next((i + 1 for i, c in enumerate(items) if c["path"] in case["expected_paths"]), None)
                rows.append({"id": case["id"], "project": project["id"], "kind": case["kind"],
                    "lexical_rank": rank(lexical_only), "semantic_rank": rank(semantic_only), "hybrid_rank": rank(hybrid["candidates"]),
                    **({"document_first_rank": rank(document_first["candidates"]), "document_first": document_first} if anchored else {}),
                    **({"code_anchor_rank": rank(code_anchor["candidates"]), "code_anchor": code_anchor} if source_hop else {}),
                    "lexical_excluded": excluded, "core_retrieval_mode": raw["retrieval_mode"],
                    "semantic_status": "available" if available else "unavailable-lexical-only",
                    "lexical_candidates": lexical_only, "semantic_candidates": semantic_only, "hybrid": hybrid})
                check_budget(model_policy, started)
        results[cohort] = rows
    summaries = {}
    for cohort, rows in results.items():
        summaries[cohort] = {"regression_gate": regression_gate(rows, protect_ordinary_lexical=anchored)}
        if source_hop:
            gate = summaries[cohort]["regression_gate"]
            for row in rows:
                before, after = row["code_anchor_rank"], row["hybrid_rank"]
                if row["kind"] != "negative" and before is not None and (after is None or (before <= 5 < after)):
                    gate["regressions"].append({"id": row["id"], "reason": "code-anchor-top20-or-first5-hit-lost"})
            gate["passed"] = not gate["regressions"]
        channels = ["lexical", "semantic", *(["document_first"] if anchored else []), *(["code_anchor"] if source_hop else []), "hybrid"]
        for channel in channels:
            metrics = [{"kind": r["kind"], "expected_rank_at_20": r[channel + "_rank"],
                        "expected_in_first_5": r[channel + "_rank"] is not None and r[channel + "_rank"] <= 5} for r in rows]
            summary, passed = summarize_quality(metrics, model_policy)
            summaries[cohort][channel] = {"summary": summary, "cohort_threshold_met": passed}
    verify_publications(spec, manifest, publications, receipt["profiles"])
    result = {"completed": True, "summaries": summaries, "results": results,
        "semantic_status": "available" if available else "unavailable-lexical-only",
        "combination_gate_passed": available and all(s["regression_gate"]["passed"] and s["hybrid"]["cohort_threshold_met"] for s in summaries.values()),
        "document_vectors_computed": 0, "query_vectors_computed": query_count,
        "parent_semantic_replays_verified": parent_replays,
        "fresh_raw_semantic_replays_verified": raw_replays,
        "lexical_batch_seconds": batch_seconds, "total_seconds": time.monotonic() - started,
        "peak_rss_bytes": peak_rss_bytes(),
        "source_drift_since_freeze": {p["id"]: sources_current(Path(p["root"]), p["sources"]) for p in manifest["projects"]},
        "boundary": policy["scope"] + " Same publication/eligible parent ranges, not equal query fields: semantics uses safe body text. Typed/metadata lexical exclusions are not refilled. Observed cohorts, no new heldout claim. Source freshness remains separate."}
    check_budget(model_policy, started)
    save(output / "report.json", result)
    print(json.dumps({k: v for k, v in result.items() if k not in ("results", "source_drift_since_freeze")}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    prepare = subs.add_parser("freeze")
    for name in ("run", "publications", "heldout", "policy", "exporter", "parent-control", "output"):
        prepare.add_argument("--" + name, type=Path, required=True)
    prepare.add_argument("--recovered-profiles", type=Path)
    prepare.add_argument("--extra-cases", type=Path)
    fresh = subs.add_parser("freeze-input")
    for name in ("input", "publications", "heldout", "policy", "exporter", "output"):
        fresh.add_argument("--" + name, type=Path, required=True)
    fresh.add_argument("--recovered-profiles", type=Path)
    score = subs.add_parser("evaluate")
    for name in ("freeze", "model-dir", "output"):
        score.add_argument("--" + name, type=Path, required=True)
    score.add_argument("--require-current", action="store_true")
    score.add_argument("--run", type=Path)
    args = parser.parse_args()
    if args.command == "freeze":
        freeze(args.run.resolve(), args.publications.resolve(), args.heldout.resolve(), args.policy.resolve(),
               args.exporter.resolve(), args.parent_control.resolve(), args.output.resolve(),
               args.recovered_profiles.resolve() if args.recovered_profiles else None,
               extra_cases=args.extra_cases.resolve() if args.extra_cases else None)
    elif args.command == "freeze-input":
        freeze(None, args.publications.resolve(), args.heldout.resolve(), args.policy.resolve(),
               args.exporter.resolve(), None, args.output.resolve(),
               args.recovered_profiles.resolve() if args.recovered_profiles else None, input_dir=args.input.resolve())
    else:
        try:
            evaluate(args.freeze.resolve(), args.model_dir.resolve(), args.output.resolve(), args.require_current,
                     args.run.resolve() if args.run else None)
        except Exception:
            if args.output.is_dir():
                save(args.output / "failure.json", {"completed": False, "error": "Hybrid experiment failed; no partial result admitted"})
            raise
