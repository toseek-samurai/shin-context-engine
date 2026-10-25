"""One source-bound navigation edge, never a transitive graph search or proof."""

from collections import Counter
import hashlib
import json
import math
from pathlib import PurePosixPath

from reproject_semantic_identifiers import publication_fingerprint
from run_local_semantic_pilot import candidate


def read_bound_links(generated, expected_manifest, max_bytes=300 * 1024 * 1024):
    data = (generated / "manifest.json").read_bytes()
    if hashlib.sha256(data).hexdigest() != expected_manifest:
        raise ValueError("Link publication manifest changed")
    integrity = json.loads(data)["generated_file_integrity"]
    entries = [row for row in integrity["files"] if row["path"] == "docs/kb/generated/links.jsonl"]
    if integrity["algorithm"] != "fnv1a64" or len(entries) != 1:
        raise ValueError("Invalid link integrity entry")
    path = generated / "links.jsonl"
    if path.stat().st_size != entries[0]["bytes"] or path.stat().st_size > max_bytes:
        raise ValueError("Link file size differs or exceeds the existing input ceiling")
    data = path.read_bytes()
    if len(data) != entries[0]["bytes"] or publication_fingerprint(data) != entries[0]["content_hash"]:
        raise ValueError("Published links changed")
    by_path = {}
    # JSONL records are LF-delimited; Unicode separators may occur in JSON strings.
    for line in data.split(b"\n"):
        if not line:
            continue
        row = json.loads(line)
        if row.get("kind") not in ("source-reference", "source-payload"):
            continue
        if not isinstance(row.get("path"), str) or type(row.get("line")) is not int or row["line"] < 1:
            raise ValueError("Invalid source link position")
        by_path.setdefault(row["path"], []).append(row)
    return by_path, {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}


def owned_relative(path):
    return (isinstance(path, str) and bool(path) and "\\" not in path and "\x00" not in path
            and not PurePosixPath(path).is_absolute() and ".." not in PurePosixPath(path).parts
            and str(PurePosixPath(path)) == path)


def follow_source_reference(base, units, by_path, scores, links, project, policy):
    placement = policy.get("placement_mode", "append-preserve-paths")
    if placement not in ("append-preserve-paths", "resolve-origin"):
        raise ValueError("Unsupported source-hop placement")
    resolve_origin = placement == "resolve-origin"
    result = {**base, "source_hop": {"status": "not-applicable", "hops": 0}, "candidates": list(base["candidates"])}
    if base.get("code_anchor", {}).get("status") != "selected":
        return result
    anchor = result["candidates"][0]
    if anchor["path"] != base["code_anchor"]["path"] or anchor["project"] != project["id"]:
        raise ValueError("Source anchor binding differs")
    if anchor["source_sha256"] != project["sources"].get(anchor["path"], {}).get("sha256"):
        raise ValueError("Source anchor inventory binding differs")
    rows = [row for row in links.get(anchor["path"], [])
            if row.get("kind") in ("source-reference", "source-payload") and
            anchor["start_line"] <= row["line"] <= anchor["end_line"]]
    hop = result["source_hop"]
    hop["status"] = "no-source-link-in-selected-range"
    if not rows:
        return result
    if len(rows) > policy["candidate_limit"]:
        hop["status"] = "source-link-budget-exceeded"
        return result
    targets = set()
    for row in rows:
        path = row.get("resolved_path")
        if (row.get("status") != "resolved" or not owned_relative(path) or
                path not in project["sources"] or path not in by_path):
            hop["status"] = "unresolved-or-unenrolled-target"
            return result
        if path == anchor["path"]:
            hop["status"] = "self-reference"
            return result
        targets.add(path)
    if len(targets) != 1:
        hop["status"] = "ambiguous-source-targets"
        return result
    path = next(iter(targets))
    counts = Counter(row["path"] for row in result["candidates"])
    if path in counts:
        hop["status"] = "target-already-selected"
        return result
    removable = next((i for i in range(len(result["candidates"]) - 1, policy["report_limit"] - 1, -1)
                      if counts[result["candidates"][i]["path"]] > 1), None)
    if not resolve_origin and len(result["candidates"]) == policy["candidate_limit"] and removable is None:
        hop["status"] = "distinct-path-budget-full"
        return result
    indexes = by_path[path]
    if not indexes:
        raise ValueError("Empty linked path enrollment")
    for i in indexes:
        if (type(i) is not int or not 0 <= i < len(units) or
                units[i]["project"] != project["id"] or units[i]["path"] != path or
                units[i].get("unit_kind") == "semantic-subrange" or
                units[i]["source_sha256"] != project["sources"][path]["sha256"] or not math.isfinite(scores[i])):
            raise ValueError("Linked unit source or score binding differs")
    index = min(indexes, key=lambda i: (-scores[i], i))
    row = min(rows, key=lambda r: (r["line"], r["kind"], r.get("target", "")))
    related = candidate(units[index], scores[index])
    related["channel"] = "resolved-source-reference-navigation"
    related["signals"] = {"source_reference": {
        "path": anchor["path"], "start_line": anchor["start_line"], "end_line": anchor["end_line"],
        "source_sha256": anchor["source_sha256"], "record_ids": anchor["record_ids"],
        "line": row["line"], "kind": row["kind"], "resolved_path": path,
        "scope": "one-hop-same-frozen-publication", "selection": "best-query-similarity-within-linked-path"}}
    if resolve_origin:
        related["signals"]["source_reference"]["origin"] = anchor
        result["candidates"][0] = related
        result["mode"] = "code-anchor-resolved-source-reference"
        result["source_hop"] = {"status": "resolved-in-place", "hops": 1, "path": path,
                                "origin_path": anchor["path"], "origin_primary_replaced": True,
                                "origin_retained_as_provenance": True}
        return result
    if len(result["candidates"]) == policy["candidate_limit"]:
        result["candidates"].pop(removable)
    result["candidates"].insert(1, related)
    result["mode"] = "code-anchor-with-one-source-hop"
    result["source_hop"] = {"status": "selected", "hops": 1, "path": path,
                            "duplicate_range_removed": removable is not None and len(base["candidates"]) == policy["candidate_limit"]}
    return result
