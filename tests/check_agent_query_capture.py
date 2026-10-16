#!/usr/bin/env python3
"""Compare two explicit CLI captures; do not run, repair or admit a project."""

import argparse
from collections import Counter
import json
from pathlib import Path

from test_compact_query import restore_native, restore_navigation
from shin_semantic.artifact import decode, read, sha


def check(compact_path, audit_path):
    compact_bytes = read(compact_path, 128 * 1024)
    audit_bytes = read(audit_path, 2 * 1024 * 1024)
    compact, audit = decode(compact_bytes), decode(audit_bytes)
    if compact.get("schema_version") != "shin.agent-query.v1" or compact.get("ok") is not True or audit.get("ok") is not True:
        raise ValueError("Only successful explicit captures can be compared")
    native, references = restore_native(compact)
    navigation = restore_navigation(compact, references)
    if audit.get("schema_version") == "shin.hermes-result.v1":
        expected, expected_navigation = audit["result"], None
    elif audit.get("schema_version") == "shin.semantic-query.v1":
        expected, expected_navigation = audit["core"], audit["navigation"]
    else:
        raise ValueError("Unknown audit capture")
    same = native == expected and navigation == expected_navigation
    result = {"schema_version": "shin.agent-query-capture-check.v1", "ok": same,
              "comparison": "exact-native-records-coordinates-grading-coverage-and-navigation-including-origin",
              "compact_sha256": sha(compact_bytes), "audit_sha256": sha(audit_bytes),
              "compact_bytes": len(compact_bytes), "audit_bytes": len(audit_bytes),
              "snapshot_id": native["snapshot_id"], "profile": native["profile"],
              "admission_kind": compact["admission"]["kind"], "semantic_status": compact["semantic_status"],
              "files": len(compact["native"]["files"]), "records": len(native["evidence"]),
              "excerpts": sum(len(group["excerpts"]) for group in compact["native"]["files"]),
              "range_kinds": dict(Counter(row["range"]["kind"] for group in compact["native"]["files"] for row in group["records"])),
              "navigation_candidates": len(navigation["candidates"]) if navigation else 0,
              "live_state_rechecked": False}
    print(json.dumps(result, indent=2))
    return 0 if same else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compact", type=Path, required=True)
    parser.add_argument("--audit", type=Path, required=True)
    args = parser.parse_args()
    raise SystemExit(check(args.compact, args.audit))
