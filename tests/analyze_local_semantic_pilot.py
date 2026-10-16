#!/usr/bin/env python3
"""Compare completed control vectors and summarize the unchanged pilot outcome."""

import argparse
import json
from pathlib import Path

from run_carry_forward_comparison import save
from run_local_semantic_pilot import fingerprint


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("control", "candidate", "input", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError("Preserve earlier analysis")
    report = json.loads((args.candidate / "report.json").read_text())
    if not report.get("completed") or fingerprint(args.candidate / "vectors.npy") != report["vectors_sha256"]:
        raise ValueError("Candidate vector index is not a completed bound result")
    candidate_binding = json.loads((args.candidate / "run-binding.json").read_text())
    if fingerprint(args.input / "input-manifest.json") != candidate_binding["input_manifest_sha256"]:
        raise ValueError("Analysis input differs from the candidate binding")
    inputs = []
    for directory in (args.control, args.candidate):
        binding = json.loads((directory / "run-binding.json").read_text())
        path = Path(binding["input"]) / "input-manifest.json"
        if fingerprint(path) != binding["input_manifest_sha256"]:
            raise ValueError("Run input binding changed")
        inputs.append(json.loads(path.read_text()))
    if inputs[0]["units_sha256"] != inputs[1]["units_sha256"]:
        raise ValueError("Different vector corpora")
    import numpy as np
    old = np.load(args.control / "vectors.npy", mmap_mode="r", allow_pickle=False)
    new = np.load(args.candidate / "vectors.npy", mmap_mode="r", allow_pickle=False)
    if old.shape != new.shape:
        raise ValueError("Different vector dimensions")
    compared = 0
    maximum = 0.0
    for start in range(0, len(old), 1024):
        left, right = old[start:start + 1024], new[start:start + 1024]
        written = np.any(left != 0, axis=1)
        if written.any():
            compared += int(written.sum())
            maximum = max(maximum, float(np.abs(left[written] - right[written]).max()))
    failure = json.loads((args.control / "failure.json").read_text())
    if compared != failure["completed_units"]:
        raise ValueError("Partial control rows do not match the failure receipt")
    spec = json.loads((args.input / "spec.json").read_text())
    units = [json.loads(line) for line in (args.input / "units.jsonl").open()]
    observations = []
    for project in spec["projects"]:
        for case in project["cases"]:
            result = next(row for row in report["results"] if row["id"] == case["id"])
            targets = [unit for unit in units if unit["project"] == project["id"] and unit["path"] in case["expected_paths"]]
            observations.append({"id": case["id"], "rank": result["expected_rank_at_20"],
                "candidate_paths": len({row["path"] for row in result["candidates"]}),
                "target_units_enrolled": len(targets),
                "target_text_bytes": sum(len(unit["text"].encode()) for unit in targets)})
    result = {"control_completed": False, "candidate_completed": True,
        "compared_completed_control_rows": compared, "maximum_absolute_vector_difference": maximum,
        "within_float32_tolerance_1e-5": maximum <= 1e-5,
        "candidate_report_sha256": fingerprint(args.candidate / "report.json"),
        "control_partial_vectors_sha256": fingerprint(args.control / "vectors.npy"),
        "observations": observations,
        "boundary": "Only already computed control rows are compared; the control remains failed. Candidate enrollment, not causal proof that a different projection fixes the misses. No acceptance threshold or query changes."}
    save(args.output, result)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
