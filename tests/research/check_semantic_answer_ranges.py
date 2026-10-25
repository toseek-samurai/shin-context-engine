#!/usr/bin/env python3
"""Review source-navigation ranges separately from preserved filename hit gates."""

import argparse
from itertools import combinations
import json
from pathlib import Path

from check_semantic_artifact_parity import CONTROL_SHA, FREEZE_SHA, ROOT
from run_local_semantic_pilot import fingerprint, load_completed_run
from shin_semantic.artifact import encode, relative, sha


def covered(spans, start, end):
    cursor = start
    for left, right in sorted(spans):
        if right < cursor:
            continue
        if left > cursor:
            return False
        cursor = right + 1
        if cursor > end:
            return True
    return False


def inspect(candidates, review, limit):
    selected, first = [], None
    for rank, row in enumerate(candidates[:limit], 1):
        if row["project"] != review["project"] or row["path"] != review["path"]:
            continue
        if row["source_sha256"] != review["source_sha256"]:
            raise ValueError("Candidate source differs from reviewed source")
        start, end = row["start_line"], row["end_line"]
        if type(start) is not int or type(end) is not int or not 1 <= start <= end:
            raise ValueError("Invalid selected source range")
        first = first or rank
        selected.append((start, end))
    clauses = [{"id": item["id"], "required_spans": item["spans"],
                "covered_by_navigation": all(covered(selected, start, end) for start, end in item["spans"])}
               for item in review["clauses"]]
    return {"limit": limit, "first_expected_path_rank": first, "selected_ranges": selected,
            "clauses": clauses, "covered_clauses": sum(item["covered_by_navigation"] for item in clauses),
            "required_clauses": len(clauses), "all_required_ranges_covered": all(item["covered_by_navigation"] for item in clauses),
            "supplied_snippet_sufficiency": "not-assessed-navigation-does-not-deliver-these-source-bodies",
            "expanded_source_sufficiency": "requires-source-read-and-review", "runtime_proof": False}


def attainability(units, review, cap):
    """Oracle diagnosis only: expected spans must never participate in retrieval."""
    if type(cap) is not int or not 1 <= cap <= 4:
        raise ValueError("Unsupported review capacity")
    needs = sorted(span for clause in review["clauses"] for span in clause["spans"])
    available = []
    for unit in units:
        if unit["project"] != review["project"] or unit["path"] != review["path"]:
            continue
        inspect([unit], review, 1)
        if any(unit["start_line"] <= end and unit["end_line"] >= start for start, end in needs):
            available.append(unit)
    if len(available) > 24:
        raise ValueError("Answer-range review unit budget")
    witness, through = [], 0
    # Farthest-reaching interval at the next required line minimizes interval count.
    for start, end in needs:
        cursor = max(start, through + 1)
        while cursor <= end:
            eligible = [unit for unit in available if unit["start_line"] <= cursor <= unit["end_line"]]
            if not eligible:
                return {"status": "required-range-not-enrolled", "first_uncovered_line": cursor,
                        "minimum_parent_ranges": None, "per_document_cap": cap,
                        "complete_navigation_attainable": False}
            selected = max(eligible, key=lambda unit: (unit["end_line"], -unit["start_line"]))
            witness.append(selected)
            through = selected["end_line"]
            cursor = through + 1
    best, attempts = 0, 0
    for count in range(1, min(cap, len(available)) + 1):
        for subset in combinations(available, count):
            attempts += 1
            if attempts > 4096:
                raise ValueError("Answer-range review combination budget")
            best = max(best, inspect(subset, review, count)["covered_clauses"])
    return {"status": "complete", "per_document_cap": cap, "minimum_parent_ranges": len(witness),
            "complete_navigation_attainable": len(witness) <= cap,
            "maximum_covered_clauses_with_cap": best, "required_clauses": len(review["clauses"]),
            "minimum_range_witness": [[row["start_line"], row["end_line"]] for row in witness],
            "boundary": "best-possible-range-coverage-not-retrieved-ranks-or-supplied-source"}


def frozen_units(freeze, fixture):
    if fingerprint(freeze / "freeze.json") != FREEZE_SHA:
        raise ValueError("Historical freeze changed")
    binding = json.loads((freeze / "freeze.json").read_bytes())
    if fingerprint(freeze / "policy.json") != binding["policy_sha256"]:
        raise ValueError("Historical capacity policy changed")
    policy = json.loads((freeze / "policy.json").read_bytes())
    _, _, _, source = load_completed_run(Path(binding["run"]))
    if fingerprint(source / "input-manifest.json") != binding["input_manifest_sha256"]:
        raise ValueError("Frozen range inventory changed")
    wanted = {(review["project"], review["path"]) for review in fixture["reviews"]}
    units = []
    with (source / "units.jsonl").open("rb") as stream:
        for line in stream:
            unit = json.loads(line)
            if (unit["project"], unit["path"]) in wanted:
                units.append({key: unit[key] for key in ("project", "path", "source_sha256", "start_line", "end_line")})
    return units, policy["per_document"], binding["input_manifest_sha256"]


def run(root, control, output, freeze=None):
    fixture_path = ROOT / "tests/fixtures/semantic-answer-range-cases.json"
    fixture = json.loads(fixture_path.read_bytes())
    if fingerprint(control) != CONTROL_SHA:
        raise ValueError("Historical control changed")
    data = json.loads(control.read_bytes())
    rows = {row["id"]: row for cohort in data["results"].values() for row in cohort}
    units, cap, manifest_sha = frozen_units(freeze, fixture) if freeze else (None, None, None)
    results = []
    for review in fixture["reviews"]:
        if review["project"] != "shin" or not relative(review["path"]):
            raise ValueError("Review is not a bound Shin source")
        path = root / review["path"]
        if path.resolve() != path or not path.is_relative_to(root):
            raise ValueError("Unsafe review source")
        raw = path.read_bytes()
        if sha(raw) != review["source_sha256"]:
            raise ValueError("Reviewed source changed; do not rebind silently")
        lines = raw.splitlines()
        if not review["clauses"] or len({item["id"] for item in review["clauses"]}) != len(review["clauses"]):
            raise ValueError("Invalid review clauses")
        for item in review["clauses"]:
            if not item["spans"] or any(type(a) is not int or type(b) is not int or not 1 <= a <= b <= len(lines) for a, b in item["spans"]):
                raise ValueError("Reviewed span outside bound source")
        for case in review["cases"]:
            row = rows[case]
            if row["project"] != review["project"]:
                raise ValueError("Wrong review project")
            results.append({"id": case, "historical_path_rank": row["hybrid_rank"],
                "source_sha256": review["source_sha256"],
                "attainability": attainability(units, review, cap) if units is not None else {"status": "not-assessed"},
                "navigation": [inspect(row["hybrid"]["candidates"], review, limit) for limit in (5, 20)]})
    report = {"schema_version": "shin.semantic-answer-range-result.v1", "review_completed": True,
        "scope": fixture["scope"], "fixture_sha256": fingerprint(fixture_path), "control_sha256": CONTROL_SHA,
        "reviewer_sha256": fingerprint(Path(__file__)), "source_mutated": False,
        "freeze_sha256": FREEZE_SHA if freeze else None, "input_manifest_sha256": manifest_sha,
        "historical_acceptance_changed": False, "results": results}
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as stream:
        stream.write(encode(report) + b"\n")
    output.chmod(0o600)
    print(json.dumps({"review_completed": True, "cases": len(results), "output": str(output)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "control", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--freeze", type=Path, help="Also diagnose capacity against the pinned frozen unit inventory")
    args = parser.parse_args()
    run(args.root.resolve(), args.control.resolve(), args.output.resolve(), args.freeze.resolve() if args.freeze else None)
