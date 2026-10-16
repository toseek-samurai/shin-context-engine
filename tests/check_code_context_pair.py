#!/usr/bin/env python3
"""Apply frozen P4 gates to pinned receipts and an explicit human/source review."""

import argparse
from pathlib import Path
from statistics import median

from code_context_pair import MODEL, PROVIDER, EFFORT, check_answer, cohort_policy
from run_agent_query_pilot import write
from shin_semantic.artifact import decode, encode, require, sha

ROOT = Path(__file__).resolve().parents[1]
QUOTA_STOPS = {None, "pilot-response-budget", "pilot-call-budget", "pilot-round-budget", "pilot-inspection-budget"}


def answer_available(receipt):
    # A refused extra lookup does not erase an answer supported by earlier delivered bodies.
    # Currentness/security refusal, no final answer and deadline failure remain disqualifying.
    return receipt["completed"] and receipt["failure"] is None and receipt["stop_reason"] in QUOTA_STOPS


def thresholds(rows, policy):
    by = {v: [r for r in rows if r["variant"] == v] for v in ("baseline", "candidate")}
    lookup = {(row["id"], row["variant"]): row for row in rows}
    controls = by["baseline"]
    candidate = by["candidate"]
    new = [row for row in candidate if row["group"] == "new"]
    writer = [row for row in candidate if row["group"] == "writer"]
    common = [row["id"] for row in controls if row["passed"] and lookup[row["id"], "candidate"]["passed"]]
    common_bytes = {v: sum(lookup[identity, v]["bytes"] for identity in common) for v in by}
    completed = {v: sum(row["passed"] for row in group) for v, group in by.items()}
    tool_medians = {v: median(row["tool_seconds"] for row in group) for v, group in by.items()}
    native = [seconds for row in candidate for seconds in row["native_seconds"]]
    limits = policy["acceptance"]
    languages = {language: sum(row["passed"] for row in new if row["language"] == language)
                 for language in ("english", "russian")}
    regressions = [row["id"] for row in controls if (row["passed"] and not lookup[row["id"], "candidate"]["passed"])
        or row["supported_clauses"] > lookup[row["id"], "candidate"]["supported_clauses"]]
    byte_gain = bool(common) and common_bytes["candidate"] * 5 <= common_bytes["baseline"] * 4
    checks = {
        "candidate_new_tasks": sum(row["passed"] for row in new) >= limits["minimum_passed"],
        "candidate_each_language": all(value >= limits["minimum_per_language"] for value in languages.values()),
        "candidate_writer": len(writer) == 2 and all(row["passed"] and row["supported_clauses"] ==
                                                      limits["writer_clauses_per_language"] for row in writer),
        "no_unsupported_candidate_claims": not any(row["unsupported_claims"] for row in candidate),
        "no_baseline_task_or_clause_regression": not regressions,
        "additional_task_or_twenty_percent_bytes": completed["candidate"] > completed["baseline"] or byte_gain,
        "candidate_native_median": bool(native) and median(native) <= limits["native_median_seconds"],
        "candidate_native_max": bool(native) and max(native) <= limits["native_max_seconds"],
        "task_tool_budget": all(row["tool_seconds"] <= limits["tool_task_seconds"] for row in rows),
        "tool_median_ratio": tool_medians["candidate"] <= tool_medians["baseline"] * limits["median_tool_time_ratio"],
        "task_deadline": all(row["seconds"] <= policy["budgets"]["task_seconds"] for row in rows),
        "response_budget": all(row["bytes"] <= policy["budgets"]["response_bytes"] for row in rows),
    }
    return {"passed": all(checks.values()), "checks": checks, "completed": completed, "new_by_language": languages,
        "regressions": regressions, "common_completed_tasks": common, "common_completed_bytes": common_bytes,
        "candidate_byte_reduction": 1 - common_bytes["candidate"] / common_bytes["baseline"] if common_bytes["baseline"] else None,
        "median_tool_seconds": tool_medians, "candidate_native_median_seconds": median(native) if native else None,
        "candidate_native_max_seconds": max(native) if native else None}


def evaluate(binding, run, review_path, policy, output):
    reviews = decode(review_path.read_bytes())
    require(reviews["schema_version"] == "shin.code-context-source-review.v1", "invalid-paired-review")
    entries = reviews["cases"]
    require(len(entries) == 2 * len(binding["cases"]) and all(row["reviewed"] is True for row in entries), "incomplete-paired-review")
    require(len({(row["id"], row["variant"]) for row in entries}) == len(entries), "duplicate-paired-review")
    rows = []
    for case in binding["cases"]:
        expected = 4 if case["group"] == "writer" else len(next(topic for topic in policy["topics"]
            if case["id"] == topic["id"] + "-" + case["language"])["clauses"])
        for variant in ("baseline", "candidate"):
            folder = run / (case["id"] + "-" + variant + ".task")
            receipt_bytes, trace_bytes = (folder / "receipt.json").read_bytes(), (folder / "trace.json").read_bytes()
            receipt, trace = decode(receipt_bytes), decode(trace_bytes)
            reviewed = next(row for row in entries if row["id"] == case["id"] and row["variant"] == variant)
            require(reviewed["receipt_sha256"] == sha(receipt_bytes) and reviewed["trace_sha256"] == sha(trace_bytes), "paired-review-input-changed")
            require(len(reviewed["clauses_supported"]) == expected and all(type(x) is bool for x in reviewed["clauses_supported"]),
                    "invalid-paired-clause-review")
            require(isinstance(reviewed["unsupported_claims"], list) and isinstance(reviewed["reason"], str)
                    and reviewed["reason"], "invalid-paired-review-reason")
            require(receipt["model"] == MODEL and receipt["provider"] == PROVIDER and receipt["effort"] == EFFORT,
                    "paired-model-mismatch")
            require(receipt["snapshot_id"] in (None, binding["variants"][variant]["snapshot_id"]), "paired-snapshot-mismatch")
            require(receipt["delivered_bytes"] == sum(len(raw.encode()) for raw in trace["responses"]), "paired-byte-accounting-mismatch")
            sound = answer_available(receipt)
            if sound:
                check_answer(receipt["answer"], trace["responses"], receipt.get("answer_contract", "legacy-v1"))
                require(receipt["forwarded_tool_bytes"] == receipt["delivered_bytes"], "paired-unforwarded-success")
            supported = sum(reviewed["clauses_supported"])
            rows.append({"id": case["id"], "variant": variant, "group": case["group"], "language": case["language"],
                "passed": bool(sound and supported == expected and not reviewed["unsupported_claims"]),
                "supported_clauses": supported, "required_clauses": expected, "unsupported_claims": reviewed["unsupported_claims"],
                "reason": reviewed["reason"], "failure": receipt["failure"], "stop_reason": receipt["stop_reason"],
                "calls": receipt["calls"], "rounds": receipt["rounds"], "bytes": receipt["delivered_bytes"],
                "forwarded_bytes": receipt["forwarded_tool_bytes"], "tool_seconds": receipt["tool_wall_seconds"],
                "seconds": receipt["elapsed_seconds"], "setup_seconds": receipt["setup_seconds"],
                "native_seconds": [step["operation_wall_seconds"] for step in receipt["steps"] if step["operation"] in ("inspect", "read")],
                "provider_usage": receipt["provider_usage"], "receipt_sha256": sha(receipt_bytes), "trace_sha256": sha(trace_bytes)})
    assessment = thresholds(rows, policy)
    report = {"schema_version": "shin.code-context-pair-review.v1", "ok": True, "promotion_passed": assessment["passed"],
        "assessment": assessment, "cases": rows, "source_review_sha256": sha(review_path.read_bytes()),
        "policy_sha256": binding["policy_sha256"], "freeze_sha256": binding["freeze_sha256"],
        "cohort": binding.get("cohort", "p4-regression"),
        "case_policy_sha256": binding.get("case_policy_sha256", binding["policy_sha256"]),
        "boundary": "small source-only same-source paired cohort; new task-level gates, not historical query scores or a release"}
    write(output, report)
    print(encode({"ok": True, "promotion_passed": report["promotion_passed"], "assessment": assessment}).decode())
    return report


def template(binding, run, output):
    entries = []
    for case in binding["cases"]:
        for variant in ("baseline", "candidate"):
            folder = run / (case["id"] + "-" + variant + ".task")
            entries.append({"id": case["id"], "variant": variant, "reviewed": False,
                "receipt_sha256": sha((folder / "receipt.json").read_bytes()), "trace_sha256": sha((folder / "trace.json").read_bytes()),
                "clauses_supported": [False] * (4 if case["group"] == "writer" else 3), "unsupported_claims": [], "reason": "pending"})
    require(not output.exists(), "paired-review-already-exists")
    write(output, {"schema_version": "shin.code-context-source-review.v1", "cases": entries})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepared", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--review", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--init-review", action="store_true")
    args = parser.parse_args()
    try:
        binding = decode((args.prepared / "binding.json").read_bytes())
        policy_bytes = (ROOT / "tests/fixtures/code-context-pilot-policy.json").read_bytes()
        require(sha(policy_bytes) == binding["policy_sha256"], "paired-policy-changed")
        policy, case_policy_sha = cohort_policy(binding.get("cohort", "p4-regression"))
        require(case_policy_sha == binding.get("case_policy_sha256", binding["policy_sha256"]), "paired-case-policy-changed")
        if args.init_review:
            template(binding, args.run, args.review)
        else:
            require(args.output is not None, "paired-output-required")
            evaluate(binding, args.run, args.review, policy, args.output)
    except Exception as error:
        print(encode({"ok": False, "error": str(error) if type(error).__name__ == "Rejected" else type(error).__name__}).decode())
        raise SystemExit(1)
