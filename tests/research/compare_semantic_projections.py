#!/usr/bin/env python3
"""Compare complete projection runs only when all non-text controls agree."""

import argparse
import json
from pathlib import Path

from run_carry_forward_comparison import save
from run_local_semantic_pilot import fingerprint, reuse_compatible_policy, same_vector_input, load_completed_run


def load_run(directory):
    return load_completed_run(directory)


def compare(control, candidate):
    old, left, before, left_input = load_run(control)
    new, right, after, right_input = load_run(candidate)
    reuse_compatible_policy(left["policy"], right["policy"])
    for key in ("runtime_versions", "runtime_options", "model_manifest_sha256"):
        if old[key] != new[key]:
            raise ValueError("Runtime or model changed")
    for key in ("projects", "spec_sha256", "units"):
        if before[key] != after[key]:
            raise ValueError("Corpus or questions changed")
    identical = changed = 0
    with (left_input / "units.jsonl").open() as a, (right_input / "units.jsonl").open() as b:
        for x, y in zip(a, b, strict=True):
            same = same_vector_input(json.loads(x), json.loads(y))
            identical += same
            changed += not same
    if identical + changed != before["units"]:
        raise ValueError("Wrong unit count")
    cases = [case for project in json.loads((left_input / "spec.json").read_text())["projects"]
             for case in project["cases"]]
    if len(old["results"]) != len(cases) or len(new["results"]) != len(cases):
        raise ValueError("Incomplete query results")
    deltas = []
    for a, b, case in zip(old["results"], new["results"], cases, strict=True):
        if (a["id"] != b["id"] or a["id"] != case["id"] or a["kind"] != b["kind"]
                or a["kind"] != case["kind"]):
            raise ValueError("Query identities differ")
        deltas.append({"id": a["id"], "kind": a["kind"],
                       "before": a["expected_rank_at_20"], "after": b["expected_rank_at_20"],
                       "before_unique_paths": len({c["path"] for c in a["candidates"]}),
                       "after_unique_paths": len({c["path"] for c in b["candidates"]})})
    return {"control_report_sha256": fingerprint(control / "report.json"),
            "candidate_report_sha256": fingerprint(candidate / "report.json"),
            "identical_units": identical, "changed_units": changed,
            "control_summary": old["summary"], "candidate_summary": new["summary"],
            "candidate_quality_threshold_met": new["candidate_quality_threshold_met"],
            "deltas": deltas, "boundary": "Same-source, same-model candidate recall only; not final selection, currentness or release qualification."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("control", "candidate", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError("Preserve earlier comparison")
    result = compare(args.control, args.candidate)
    save(args.output, result)
    print(json.dumps(result, indent=2))
