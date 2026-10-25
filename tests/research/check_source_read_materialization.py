#!/usr/bin/env python3
"""Verify explicit P2 materialization against the existing source-bound rubric."""

import argparse
import hashlib
import json
from pathlib import Path


def check(root, response_path, rubric_path):
    raw = response_path.read_bytes()
    if len(raw) > 128 * 1024:
        raise ValueError("Read response exceeds its envelope bound")
    response = json.loads(raw)
    if response["schema_version"] not in ("shin.source-read.v1", "shin.source-read.v2") or not response["ok"] or not response["complete"]:
        raise ValueError("Expected a complete native source read")
    if not response["index_freshness"]["current"] or not response["index_freshness"]["fully_verified"]:
        raise ValueError("Expected exact current-source admission")
    if response["evidence"]["omissions"]:
        raise ValueError("Explicit selection has omissions")
    review = json.loads(rubric_path.read_bytes())["reviews"][0]
    source = (root / review["path"]).read_bytes()
    source_hash = hashlib.sha256(source).hexdigest()
    if source_hash != review["source_sha256"]:
        raise ValueError("Source changed from the preexisting reviewed rubric")
    lines = source.splitlines(keepends=True)
    ranges = []
    for body in response["evidence"]["bodies"]:
        if body["path"] != review["path"] or body["state"] != "complete":
            raise ValueError("Unexpected or redacted body")
        first, last = body["start_line"], body["end_line"]
        text = body["text"].encode()
        if first < 1 or last < first or last > len(lines) or text != b"".join(lines[first - 1:last]):
            raise ValueError("Delivered text differs from its physical source range")
        if len(text) != body["source_range_bytes"]:
            raise ValueError("Physical byte count differs")
        ranges.append((first, last))
    clauses = [{"id": clause["id"], "supported": all(
        any(start <= first and end >= last for start, end in ranges)
        for first, last in clause["spans"])} for clause in review["clauses"]]
    ok = all(clause["supported"] for clause in clauses)
    if not ok:
        raise ValueError("Delivered bodies do not cover every preexisting reviewed clause")
    return {"schema_version": "shin.source-read-materialization-check.v1", "ok": True,
            "snapshot_id": response["snapshot_id"], "source_sha256": source_hash,
            "response_sha256": hashlib.sha256(raw).hexdigest(), "clauses": clauses,
            "complete_bodies": len(ranges), "evidence_bytes": response["evidence_bytes"],
            "output_bytes": len(raw),
            "boundary": "Explicit published-source selection with exact delivered bodies; not an agent run, query-ranking gain or executed proof"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--response", type=Path, required=True)
    parser.add_argument("--rubric", type=Path, required=True)
    arguments = parser.parse_args()
    print(json.dumps(check(arguments.root, arguments.response, arguments.rubric), indent=2))
