#!/usr/bin/env python3
"""Derive a body-text pilot from a frozen summary corpus via the native exporter."""

import argparse
import json
from pathlib import Path

from reproject_semantic_identifiers import publication_fingerprint
from run_carry_forward_comparison import save
from run_local_semantic_pilot import fingerprint, load_input
from run_project_search_comparison import validate_spec
from semantic_chunk_projection import child_units


def apply_ranges(units, records):
    by_range = {}
    for row in records:
        key = row["path"], row["start_line"], row["end_line"]
        group = by_range.setdefault(key, {"ids": set(), "text": row["text"]})
        if group["text"] != row["text"] or row["record_id"] in group["ids"]:
            raise ValueError("Conflicting or duplicate exported range")
        group["ids"].add(row["record_id"])
    changed = truncated = redacted = 0
    result = []
    flags = {(row["path"], row["start_line"], row["end_line"]): row for row in records}
    for original in units:
        unit = dict(original)
        key = unit["path"], unit["start_line"], unit["end_line"]
        group = by_range.get(key)
        if group:
            if not group["ids"].issubset(unit["record_ids"]):
                raise ValueError("Exported record does not belong to frozen unit")
            unit["text"] += "\nSource range: " + group["text"]
            changed += 1
            truncated += flags[key]["truncated"]
            redacted += flags[key]["redacted"]
        result.append(unit)
    return result, {"changed_units": changed, "byte_truncated_units": truncated, "redacted_units": redacted}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("input", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--chunks", action="store_true")
    args = parser.parse_args()
    source, output = args.input.resolve(), args.output.resolve()
    manifest, spec, policy, units = load_input(source)
    if manifest.get("projection_version") not in (None, "published-summary-v1"):
        raise ValueError("Expected an unmodified summary control")
    if not manifest.get("source_exporter_sha256"):
        raise ValueError("Source ranges must be captured in the original common freeze")
    validate_spec(spec, output)
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    policy["projection"] += (" Controlled core-bound-range-v1 amendment: append up to 2048 UTF-8 bytes "
        "of Core-redacted, compact source text inside each published bound fact range. "
        "No adjacent comments, aliases, query selection or lexical-summary changes. "
        "The native exporter verifies publication, full source hash and physical span before redaction/clipping.")
    if args.chunks:
        policy["projection"] += (" Add native syntax-owned child ranges only for byte-clipped parents, excluding descendant scopes: "
            "at most 24 lines, 1024 text bytes and 16 children per parent. Preserve parent IDs, physical "
            "coordinates, source/span hashes and explicit omissions. No query-specific selection.")
    save(output / "spec.json", spec)
    save(output / "policy.json", policy)
    receipts, total_bytes, total_units = [], 0, 0
    with (output / "units.jsonl").open("x") as stream:
        for project in manifest["projects"]:
            index = source / project["id"] / "index"
            publication = index / "docs/kb/generated/manifest.json"
            if fingerprint(publication) != project["publication_sha256"]:
                raise ValueError("Frozen publication changed")
            path = source / project["id"] / "source-ranges.jsonl"
            if fingerprint(path) != project["source_projection_sha256"]:
                raise ValueError("Frozen source projection changed")
            # JSONL is LF-delimited; Unicode line separators may be string data.
            with path.open("rb") as exported:
                rows = [json.loads(line) for line in exported]
            identity = publication_fingerprint(publication.read_bytes())
            if (not rows or rows[0]["type"] != "binding" or rows[-1]["type"] != "complete"
                    or rows[0]["snapshot_id"] != identity or rows[-1]["snapshot_id"] != identity
                    or rows[-1]["records"] != len(rows) - 2 or rows[0]["max_text_bytes"] != 2048
                    or any(row["type"] != "range" for row in rows[1:-1])):
                raise ValueError("Incomplete or mismatched native export")
            projection = rows[0].get("projection")
            if projection not in ("core-bound-range-v1", "core-bound-chunks-v1"):
                raise ValueError("Unsupported source export projection")
            if args.chunks and (projection != "core-bound-chunks-v1" or rows[0].get("chunk_policy") != {
                    "max_lines": 24, "max_text_bytes": 1024, "max_chunks_per_range": 16,
                    "ownership": "syntax-range-minus-descendants"}):
                raise ValueError("Chunk policy differs from the declared bounds")
            selected = [unit for unit in units if unit["project"] == project["id"]]
            enriched, receipt = apply_ranges(selected, rows[1:-1])
            if args.chunks:
                children, child_receipt = child_units(selected, rows[1:-1])
                enriched.extend(children)
                receipt.update(child_receipt)
                project["base_units"] = project["units"]
                project["units"] = len(enriched)
            receipt["output_units"] = len(enriched)
            receipt.update(project=project["id"], export_sha256=fingerprint(path), exported_records=len(rows) - 2)
            receipts.append(receipt)
            for unit in enriched:
                serialized = json.dumps(unit, ensure_ascii=False, separators=(",", ":")) + "\n"
                total_bytes += len(serialized.encode())
                total_units += 1
                if total_bytes > policy["max_input_bytes"] or total_units > policy["max_units"]:
                    save(output / "rejected.json", {"completed": False, "project": project["id"],
                         "units_at_rejection": total_units, "bytes_at_rejection": total_bytes,
                         "project_receipts": receipts, "reason": "unchanged-corpus-budget-exceeded"})
                    raise ValueError("Body input exceeds the unchanged corpus budget")
                stream.write(serialized)
            print(json.dumps(receipt), flush=True)
    manifest.update({"spec_sha256": fingerprint(output / "spec.json"),
        "policy_sha256": fingerprint(output / "policy.json"), "units_sha256": fingerprint(output / "units.jsonl"),
        "bytes": total_bytes, "units": total_units,
        "derived_from_manifest_sha256": fingerprint(source / "input-manifest.json"),
        "projection_version": "core-bound-chunks-v1" if args.chunks else "core-bound-range-v1", "projection_amendment": {
            "exporter_sha256": manifest["source_exporter_sha256"], "max_text_bytes": 2048, "projects": receipts,
            "query_changes": False, "raw_source_in_python_projection": False}})
    if args.chunks:
        manifest["projection_amendment"]["chunk_policy"] = {
            "max_lines": 24, "max_text_bytes": 1024, "max_chunks_per_range": 16,
            "ownership": "syntax-range-minus-descendants", "enrollment": "byte-clipped-parents-only"}
    save(output / "input-manifest.json", manifest)
    print(json.dumps({"units": total_units, "bytes": total_bytes}), flush=True)


if __name__ == "__main__":
    main()
