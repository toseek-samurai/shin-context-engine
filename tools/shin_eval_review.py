#!/usr/bin/env python3
"""Bounded, read-only review of saved Core eval output; never grants readiness."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import stat
import sys

MAX_INPUT = 8 * 1024 * 1024
MAX_OUTPUT = 24 * 1024
RESOURCE_METRICS = {
    "candidate_rows": ("max_candidate_rows", "candidate-discovery"),
    "matched_sections": ("max_matched_sections", "scored-matches"),
    "matched_facts": ("max_matched_facts", "scored-matches"),
}
QUALITY_FIELDS = ("missing_paths", "missing_top_paths", "unexpected_paths", "missing_record_types", "rank_violations")
QUALITY_REASONS = {"missing-required-path", "missing-top-path", "forbidden-path-returned",
                   "missing-record-type", "rank-order-violated"}


def require(value):
    if not value:
        raise ValueError("invalid-eval-report")


def integer(value, minimum=0):
    require(type(value) is int and minimum <= value <= sys.maxsize)
    return value


def encode(value):
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"), allow_nan=False).encode()


def load(path):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result)
            result[key] = value
        return result

    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_size <= MAX_INPUT)
        data = stream.read(MAX_INPUT + 1)
    require(len(data) <= MAX_INPUT)
    value = json.loads(data, object_pairs_hook=pairs, parse_constant=lambda _: require(False))
    return value, hashlib.sha256(data).hexdigest()


def metric(name, actual, allowed):
    integer(actual)
    integer(allowed)
    return {"metric": name, "stage": RESOURCE_METRICS[name][1], "actual": actual, "allowed": allowed,
            "headroom": allowed - actual, "headroom_percent": round(100 * (allowed - actual) / allowed, 2) if allowed else None,
            "exceeded": actual > allowed}


def review_query(row, full, policy="strict"):
    require(isinstance(row, dict))
    require(row.get("resource_policy", policy) == policy)
    query = row.get("query")
    require(query is None or isinstance(query, str))
    identity = row.get("id")
    require(isinstance(identity, str) and len(identity.encode()) <= 1024)
    variant = integer(row.get("variant_index", -1), -1)
    require(variant < 8)
    limits, quality, unknown = [], [], []
    if full:
        require(type(row["query_passed"]) is bool)
        diagnostics = row["query_diagnostics"]
        for name, (field, _) in RESOURCE_METRICS.items():
            cap = integer(row[field], -1)
            actual = diagnostics[name] if name == "candidate_rows" else row[name]
            integer(actual)
            if cap >= 0:
                limits.append(metric(name, actual, cap))
        coverage = integer(row["query_coverage"]["covered_groups"])
        minimum = integer(row["min_covered_groups"], -1)
        if minimum >= 0 and coverage < minimum:
            quality.append("covered_groups")
    else:
        require(row.get("detail_level") in ("full", "header"))
        for detail in row.get("ceiling_violation_details", []):
            name = detail["metric"]
            actual, allowed = integer(detail["actual"]), integer(detail["allowed"])
            if name in RESOURCE_METRICS and detail["comparator"] == "<=":
                require(detail["stage"] == RESOURCE_METRICS[name][1] and actual > allowed)
                limits.append(metric(name, actual, allowed))
            elif name == "covered_groups" and detail["comparator"] == ">=":
                require(actual < allowed)
                quality.append("covered_groups")
            else:
                unknown.append("unknown-ceiling-metric")
        reasons = row.get("failure_reasons", [])
        require(isinstance(reasons, list) and all(isinstance(reason, str) for reason in reasons))
        quality.extend(reason for reason in reasons if reason in QUALITY_REASONS)
        unknown.extend(reason for reason in reasons if reason not in QUALITY_REASONS | {"retrieval-ceiling-violated"})
        if "retrieval-ceiling-violated" in reasons and not (limits or quality):
            unknown.append("ceiling-detail-unavailable")
        if row["detail_level"] != "full":
            unknown.append("query-detail-omitted")
    for field in QUALITY_FIELDS:
        if field not in row:
            unknown.append("quality-detail-unavailable")
        else:
            require(isinstance(row[field], list))
            if row[field]:
                quality.append(field)
    known_metrics = set(RESOURCE_METRICS) | {"covered_groups"}
    require(isinstance(row.get("ceiling_violations", []), list))
    if set(row.get("ceiling_violations", [])) - known_metrics:
        unknown.append("unknown-ceiling-metric")
    exceeded = any(item["exceeded"] for item in limits)
    passed = row["query_passed"] if full else False
    if full:
        require(passed == (not quality and (not exceeded or policy == "advisory") and not unknown))
        if "quality_passed" in row:
            require(type(row["quality_passed"]) is bool and row["quality_passed"] == (not quality and not unknown))
        if "resources_passed" in row:
            require(type(row["resources_passed"]) is bool and row["resources_passed"] == (not exceeded))
    if unknown:
        classification, action = "incomplete-or-unknown", "inspect-complete-eval"
    elif quality:
        classification = "quality-and-resource-failure" if exceeded else "quality-failure"
        action = "inspect-quality-before-budget-policy"
    elif exceeded:
        classification = "resource-warning" if policy == "advisory" else "resource-only-failure"
        action = "review-resource-regression" if policy == "advisory" else "compare-explicit-pinned-publications"
    elif passed:
        classification, action = "passed", "review-headroom" if limits else "none"
    else:
        classification, action = "incomplete-or-unknown", "inspect-complete-eval"
    return {"id": identity, "variant_index": variant, "query": query[:512] if query is not None else None,
            "query_truncated": query is not None and len(query) > 512,
            "query_sha256": hashlib.sha256(query.encode()).hexdigest() if query is not None else None,
            "query_passed": passed, "classification": classification, "next_action": action,
            "quality_status": "failed" if quality else ("unknown" if unknown else "passed-declared-assertions"),
            "resource_limits": limits, "limits_complete": full}


def review(value, input_sha256, limit=8):
    require(type(limit) is int and 1 <= limit <= 32)
    require(isinstance(value, dict) and value.get("schema_version") in ("shin.eval.v1", "shin.eval-failures.v1")
            and value.get("tool") == "shin-context" and value.get("command") == "eval" and type(value.get("ok")) is bool)
    full = value["schema_version"] == "shin.eval.v1"
    resource_check = value.get("resource_check")
    policy = "strict"
    if resource_check is not None:
        require(isinstance(resource_check, dict))
        policy = resource_check["policy"]
        require(policy in ("strict", "advisory") and type(resource_check["ok"]) is bool)
        resource_failed = integer(resource_check["failed_queries"])
        require(integer(resource_check["warning_queries"]) == (resource_failed if policy == "advisory" else 0))
    rows = value["cases"] if full else value["failures"]
    require(isinstance(rows, list) and len(rows) <= 256)
    entries = []
    for row in rows:
        entries.append(review_query(row, full, policy))
        variants = row.get("query_variants", [])
        require(isinstance(variants, list) and len(variants) <= 8 and (full or not variants))
        entries.extend(review_query(variant, full, policy) for variant in variants)
    require(len(entries) <= 256)
    require(len({(row["id"], row["variant_index"]) for row in entries}) == len(entries))
    counts = {key: integer(value["query_summary"][key]) for key in ("total", "passed", "failed")}
    require(counts["total"] == counts["passed"] + counts["failed"])
    if resource_check is not None:
        require(resource_failed <= counts["total"])
        require(resource_check["ok"] == (counts["total"] > 0 and resource_failed == 0))
        if full:
            require(resource_failed == sum(any(metric["exceeded"] for metric in row["resource_limits"]) for row in entries))
    require(not value["ok"] or counts["failed"] == 0)
    if full:
        require(counts["total"] == len(entries) and counts["passed"] == sum(row["query_passed"] for row in entries))
    else:
        require(counts["failed"] >= len(entries))
    freshness = value.get("index_freshness", {})
    require(isinstance(freshness, dict))
    if value["ok"] and (value.get("strict") is True or value.get("require_current") is True):
        require(freshness.get("current") is True)
    output = {"schema_version": "shin.eval-review.v1", "ok": value["ok"],
              "scope": "saved-eval-advisory-not-live-readiness", "input_sha256": input_sha256,
              "source_schema": value["schema_version"], "profile": value["profile"], "snapshot_id": value.get("snapshot_id"),
              "freshness_at_capture": {key: freshness.get(key) for key in ("current", "fully_verified", "status")},
              "query_summary": counts, "observed_classifications": dict(sorted(Counter(row["classification"] for row in entries).items())),
              "source_failed_queries_omitted": counts["failed"] - sum(not row["query_passed"] for row in entries),
              "source_successful_queries_omitted": 0 if full else counts["passed"],
              "queries": [], "query_budget": {"eligible": len(entries), "returned": 0, "omitted": len(entries), "max_bytes": MAX_OUTPUT},
              "comparison": "not-performed-no-engine-profile-or-candidate-trace-binding",
              "gate_boundary": "Does not evaluate safety, integrity, adoption or live freshness; run the canonical ready gate.",
              "automatic_actions": False}
    if resource_check is not None:
        output["resource_check"] = resource_check
    require(isinstance(output["profile"], str) and len(output["profile"].encode()) <= 128)
    require(output["snapshot_id"] is None or (isinstance(output["snapshot_id"], str) and len(output["snapshot_id"]) <= 128))
    for item in output["freshness_at_capture"].values():
        require(item is None or type(item) is bool or (isinstance(item, str) and len(item) <= 128))

    def priority(row):
        pressures = [(float("inf") if item["actual"] else 1) if item["allowed"] == 0
                     else item["actual"] / item["allowed"] for item in row["resource_limits"]]
        return (row["query_passed"], -max(pressures, default=-1), row["id"], row["variant_index"])

    for row in sorted(entries, key=priority):
        if len(output["queries"]) == limit:
            break
        output["queries"].append(row)
        output["query_budget"].update(returned=len(output["queries"]), omitted=len(entries) - len(output["queries"]))
        if len(encode(output)) + 1 > MAX_OUTPUT:
            output["queries"].pop()
            output["query_budget"].update(returned=len(output["queries"]), omitted=len(entries) - len(output["queries"]))
    require(len(encode(output)) + 1 <= MAX_OUTPUT)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--eval", required=True, type=Path, dest="evaluation")
    parser.add_argument("--limit", type=int, choices=range(1, 33), default=8)
    args = parser.parse_args()
    try:
        value, digest = load(args.evaluation)
        result = review(value, digest, args.limit)
    except (OSError, ValueError, TypeError, KeyError, RecursionError, OverflowError):
        print('{"schema_version":"shin.eval-review.v1","ok":false,"error":"invalid-eval-report"}')
        return 2
    sys.stdout.buffer.write(encode(result) + b"\n")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
