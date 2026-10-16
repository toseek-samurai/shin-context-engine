#!/usr/bin/env python3
"""Add frozen published references, never live-source text, to a pilot corpus."""

import argparse
import hashlib
import json
from pathlib import Path

from prepare_semantic_pilot import published_identifiers
from run_carry_forward_comparison import save
from run_local_semantic_pilot import fingerprint, load_input
from run_project_search_comparison import validate_spec


def publication_fingerprint(content):
    # Match the existing Core/test_impact.py publication hash, including its seed.
    value = 1469598103934665603
    for byte in content:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"fnv1a64:{value:016x}"


def read_bound_facts(generated, expected_manifest):
    manifest_bytes = (generated / "manifest.json").read_bytes()
    if hashlib.sha256(manifest_bytes).hexdigest() != expected_manifest:
        raise ValueError("Original publication manifest changed")
    manifest = json.loads(manifest_bytes)
    integrity = manifest["generated_file_integrity"]
    if integrity["algorithm"] != "fnv1a64":
        raise ValueError("Unsupported publication integrity algorithm")
    entries = [item for item in integrity["files"] if item["path"] == "docs/kb/generated/facts.jsonl"]
    if len(entries) != 1:
        raise ValueError("Expected one facts integrity entry")
    content = (generated / "facts.jsonl").read_bytes()
    entry = entries[0]
    if len(content) != entry["bytes"] or publication_fingerprint(content) != entry["content_hash"]:
        raise ValueError("Original published facts changed")
    return [json.loads(line) for line in content.splitlines()], hashlib.sha256(content).hexdigest()


def enrich_units(units, facts):
    by_range = {}
    for row in facts:
        references = published_identifiers(row)
        if references:
            key = row["path"], row["start_line"], row["end_line"]
            by_range.setdefault(key, []).append((row["id"], references))
    changed = 0
    result = []
    for original in units:
        unit = dict(original)
        identifiers = []
        key = unit["path"], unit["start_line"], unit["end_line"]
        for identity, references in by_range.get(key, []):
            if identity not in unit["record_ids"]:
                raise ValueError("Published fact does not belong to the frozen unit")
            identifiers.extend(references)
        identifiers = list(dict.fromkeys(identifiers))
        if identifiers:
            unit["text"] += "\nIdentifier references: " + " ".join(identifiers)
            changed += 1
        result.append(unit)
    return result, changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("input", "publications", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    source, output = args.input.resolve(), args.output.resolve()
    manifest, spec, policy, units = load_input(source)
    if manifest.get("projection_version") not in (None, "published-summary-v1"):
        raise ValueError("Reprojection requires the preserved summary-only baseline")
    validate_spec(spec, output)
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    policy["projection"] += " Append deduplicated published identifier_terms in original order to each identical bound range. No raw source read, field reweighting, aliases, unit selection or query changes."
    save(output / "spec.json", spec)
    save(output / "policy.json", policy)
    total_bytes = changed = 0
    receipts = []
    with (output / "units.jsonl").open("x") as stream:
        for project in manifest["projects"]:
            generated = args.publications.resolve() / project["id"] / "index/docs/kb/generated"
            facts, sha256 = read_bound_facts(generated, project["publication_sha256"])
            selected = [unit for unit in units if unit["project"] == project["id"]]
            enriched, count = enrich_units(selected, facts)
            for unit in enriched:
                serialized = json.dumps(unit, ensure_ascii=False, separators=(",", ":")) + "\n"
                total_bytes += len(serialized.encode())
                if total_bytes > policy["max_input_bytes"]:
                    raise ValueError("Reprojected input exceeds the unchanged budget")
                stream.write(serialized)
            receipts.append({"project": project["id"], "changed_units": count, "facts_sha256": sha256})
            changed += count
    manifest.update({"spec_sha256": fingerprint(output / "spec.json"),
        "policy_sha256": fingerprint(output / "policy.json"), "units_sha256": fingerprint(output / "units.jsonl"),
        "bytes": total_bytes, "derived_from_manifest_sha256": fingerprint(source / "input-manifest.json"),
        "projection_version": "published-identifiers-v1",
        "projection_amendment": {"changed_units": changed, "publications": receipts,
                                  "live_source_text_in_projection": False, "query_changes": False}})
    save(output / "input-manifest.json", manifest)
    print(json.dumps({"units": len(units), "changed_units": changed, "bytes": total_bytes}))


if __name__ == "__main__":
    main()
