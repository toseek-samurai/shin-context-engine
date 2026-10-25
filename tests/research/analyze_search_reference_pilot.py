#!/usr/bin/env python3
"""Derive bounded search observations without turning file hits into answer scores."""

import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--working-query", type=Path)
    parser.add_argument("--working-root", type=Path)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    observations = []
    for row in report["rows"]:
        if row["engine"] != "shin":
            continue
        raw = json.loads(args.report.with_name(row["id"] + "-shin.json").read_text())
        coverage = raw["query_coverage"]
        observations.append({"id": row["id"], "records": row["records"],
                             "normalized_groups": coverage["total_groups"],
                             "covered_groups": coverage["covered_groups"],
                             "paths": row["paths"]})
    provenance = []
    if args.working_query:
        if args.working_root is None:
            parser.error("--working-query requires --working-root")
        root = args.working_root.resolve()
        raw = json.loads(args.working_query.read_text())
        revision = raw["revision"]
        if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
            raise ValueError("expected an explicit Git commit revision")
        for row in raw["results"]:
            relative = row["result"]["sourcePosition"]["relativePath"]
            path = (root / relative).resolve()
            if not path.is_relative_to(root):
                raise ValueError("reference result escapes reviewed root")
            blob = subprocess.run(["git", "-C", str(root), "show", revision + ":" + relative],
                                  capture_output=True, text=True, timeout=10, check=True).stdout
            current = path.read_text()
            content = row["content"]
            offset = current.find(content)
            provenance.append({"path": relative, "reported_revision": revision,
                               "content_matches_git_blob": content in blob,
                               "content_matches_worktree": offset >= 0,
                               "reported_start_line": row.get("contentStartLine"),
                               "actual_worktree_start_line": current[:offset].count("\n") + 1 if offset >= 0 else None})
    result = {"schema_version": "shin.reference-search-observations.v1",
              "frozen_revision": report["revision"], "file_localization_only": report["summary"],
              "shin_query_group_observations": observations,
              "working_reference_provenance": provenance,
              "boundary": "Path localization is not answer quality. Query coverage describes normalized lexical groups, not the complete natural-language intent. A reference revision label alone is not verification of current worktree bytes. No product-wide winner or latency claim is inferred."}
    output = args.report.with_name("observations.json")
    if output.exists():
        raise FileExistsError("preserve earlier observations; choose a new run directory")
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
