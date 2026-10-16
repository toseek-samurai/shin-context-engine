"""Compose the established parent-only input from verified Core publications."""

from pathlib import Path

from .artifact import MAX_BYTES, MAX_UNITS, canonical, decode, publication_id, read, relative, require, sha
from .selection import metadata_path


def capture_publication(route):
    generated = canonical(route["out_dir"]) / "docs/kb/generated"
    publication = read(generated / "manifest.json", 8 * 1024 * 1024)
    manifest = decode(publication)
    require(manifest["profile"] == route["profile"], "publication-profile-mismatch")
    integrity = manifest["generated_file_integrity"]
    require(integrity["algorithm"] == "fnv1a64", "unsupported-publication-integrity")
    files, total = {}, len(publication)
    for name in ("facts.jsonl", "sections.jsonl", "links.jsonl"):
        entries = [item for item in integrity["files"] if item["path"] == "docs/kb/generated/" + name]
        require(len(entries) == 1 and type(entries[0]["bytes"]) is int and 0 <= entries[0]["bytes"] <= MAX_BYTES,
                "invalid-publication-file")
        total += entries[0]["bytes"]
        require(total <= MAX_BYTES, "projection-input-budget")
        data = read(generated / name, entries[0]["bytes"])
        require(len(data) == entries[0]["bytes"] and publication_id(data) == entries[0]["content_hash"], "publication-file-changed")
        files[name] = data
    require(read(generated / "manifest.json", 8 * 1024 * 1024) == publication, "publication-changed")
    return publication, manifest, files


def project_units(route, publication, files, exported):
    published = decode(publication)
    source_hashes = {}
    for source in [*published["sources"], *published["source_fact_sources"]]:
        if source.get("content_hash"):
            binding = (source["content_hash"], source["bytes"])
            require(source_hashes.setdefault(source["path"], binding) == binding, "conflicting-published-source")
    records = [decode(line) for line in exported.split(b"\n") if line]
    snapshot = publication_id(publication)
    require(len(records) >= 2 and records[0].get("type") == "binding" and records[-1].get("type") == "complete",
            "incomplete-source-export")
    require(records[0]["snapshot_id"] == records[-1]["snapshot_id"] == snapshot
            and records[-1]["records"] == len(records) - 2 and records[0]["projection"] == "core-bound-range-v1"
            and records[0]["max_text_bytes"] == 2048, "source-export-binding-mismatch")
    ranges = {}
    for row in records[1:-1]:
        require(row["type"] == "range", "invalid-source-export")
        if metadata_path(row["path"]):
            continue
        key = row["path"], row["start_line"], row["end_line"]
        group = ranges.setdefault(key, {"ids": set(), "text": row["text"]})
        require(group["text"] == row["text"] and row["record_id"] not in group["ids"], "conflicting-source-export")
        group["ids"].add(row["record_id"])
    groups, sources, source_bytes = {}, {}, 0
    root = Path(route["root"])
    for name in ("facts.jsonl", "sections.jsonl"):
        for line in files[name].split(b"\n"):
            if not line:
                continue
            row = decode(line)
            path = row["path"]
            require(relative(path), "unsafe-source-path")
            if metadata_path(path):
                continue
            if path not in sources:
                raw = read(canonical(str(root / path)), 16 * 1024 * 1024)
                require(source_hashes.get(path) == (publication_id(raw), len(raw)), "source-changed-during-projection")
                source_bytes += len(raw)
                require(source_bytes <= MAX_BYTES, "source-byte-budget")
                sources[path] = {"sha256": sha(raw), "lines": raw.count(b"\n") + 1}
            start, end = row["start_line"], row["end_line"]
            require(type(start) is int and type(end) is int and 1 <= start <= end <= sources[path]["lines"], "invalid-source-range")
            key = path, start, end
            unit = groups.setdefault(key, {"project": route["project"], "path": path, "start_line": start, "end_line": end,
                "source_sha256": sources[path]["sha256"], "record_ids": [], "parts": []})
            require(len(groups) <= MAX_UNITS, "unit-count-budget")
            identity = row.get("id") or f"{row['document_id']}:{start}:{end}:{row.get('anchor', '')}"
            unit["record_ids"].append(identity)
            for field in ("title", "heading", "summary", "indexed_text"):
                value = row.get(field)
                if isinstance(value, str) and value and value not in unit["parts"]:
                    unit["parts"].append(value)
            if not row.get("indexed_text") and row.get("preview") and row["preview"] not in unit["parts"]:
                unit["parts"].append(row["preview"])
    units, texts, text_bytes = [], [], 0
    for key in sorted(groups):
        unit = groups[key]
        text = unit["path"] + "\n" + "\n".join(unit.pop("parts"))
        unit["record_ids"] = sorted(set(unit["record_ids"]))
        if key in ranges:
            require(ranges[key]["ids"] <= set(unit["record_ids"]), "source-export-record-mismatch")
            text += "\nSource range: " + ranges[key]["text"]
        text_bytes += len(text.encode())
        require(text_bytes <= MAX_BYTES, "model-input-budget")
        unit["text_sha256"] = sha(text.encode())
        units.append(unit)
        texts.append(text)
    require(units, "empty-semantic-corpus")
    require(set(ranges) <= set(groups), "source-export-outside-corpus")
    return units, texts, {"model_input_bytes": text_bytes, "source_bytes": source_bytes, "source_files": len(sources)}
