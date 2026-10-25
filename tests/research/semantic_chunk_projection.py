"""Assemble native-exported child units without reading or reparsing source text."""

import re


def child_units(units, records):
    if len({unit["project"] for unit in units}) > 1:
        raise ValueError("Chunk projection must stay within one project")
    parents = {(unit["path"], unit["start_line"], unit["end_line"]): unit for unit in units}
    children, statuses, seen = {}, {}, set()
    omitted = clipped = eligible = skipped_unclipped = 0
    for row in records:
        key = row["path"], row["start_line"], row["end_line"]
        parent = parents.get(key)
        if parent is None:
            continue
        if row["record_id"] not in parent["record_ids"]:
            raise ValueError("Unbound parent record")
        part = row["chunking"]
        if len(part["chunks"]) > 16:
            raise ValueError("Per-parent chunk limit exceeded")
        if key not in seen:
            statuses[part["status"]] = statuses.get(part["status"], 0) + 1
            if row["truncated"]:
                eligible += 1
                omitted += part["omitted_lines"]
                clipped += part["clipped_chunks"]
            elif part["chunks"]:
                skipped_unclipped += 1
            seen.add(key)
        if row["redacted"] and part["chunks"]:
            raise ValueError("Redacted parent cannot export child text")
        if not row["truncated"]:
            continue
        prior_end = row["start_line"] - 1
        for child in part["chunks"]:
            start, end = child["start_line"], child["end_line"]
            if (type(start) is not int or type(end) is not int or
                    not row["start_line"] <= start <= end <= row["end_line"] or
                    start <= prior_end or end - start + 1 > 24 or
                    child["parent_start_line"] != row["start_line"] or
                    child["parent_end_line"] != row["end_line"] or
                    child["parent_span_hash"] != row["span_hash"] or
                    child["path"] != row["path"] or
                    not re.fullmatch(r"fnv1a64:[0-9a-f]{16}", child["span_hash"]) or
                    len(child["text"].encode()) > 1024 or
                    child["range_origin"] != "semantic-line-window"):
                raise ValueError("Invalid child coordinates, size or source binding")
            prior_end = end
            location = row["path"], start, end
            item = children.setdefault(location, {"project": parent["project"], "path": row["path"],
                "start_line": start, "end_line": end, "source_sha256": parent["source_sha256"],
                "span_hash": child["span_hash"], "unit_kind": "semantic-subrange",
                "body": child["text"], "truncated": child["truncated"], "redacted": child["redacted"],
                "parents": {}, "symbols": set(), "record_ids": set()})
            if any(item[field] != child[field] for field in ("span_hash", "truncated", "redacted")) or item["body"] != child["text"]:
                raise ValueError("Conflicting child text or binding")
            binding = item["parents"].setdefault(key[1:], {"start_line": row["start_line"],
                "end_line": row["end_line"], "span_hash": row["span_hash"], "record_ids": set(),
                "chunking_status": part["status"], "omitted_lines": part["omitted_lines"],
                "clipped_chunks": part["clipped_chunks"]})
            if binding["span_hash"] != row["span_hash"]:
                raise ValueError("Conflicting parent span")
            binding["record_ids"].add(row["record_id"])
            item["record_ids"].add(row["record_id"])
            if row.get("symbol"):
                item["symbols"].add(row["symbol"])
    result = []
    for location in sorted(children):
        child = children[location]
        child["parent_ranges"] = []
        for _, binding in sorted(child.pop("parents").items()):
            binding["record_ids"] = sorted(binding["record_ids"])
            child["parent_ranges"].append(binding)
        child["record_ids"] = sorted(child["record_ids"])
        child["text"] = child["path"] + "\n" + " ".join(sorted(child.pop("symbols"))) + "\n" + child.pop("body")
        result.append(child)
    return result, {"child_units": len(result), "parent_statuses": statuses,
                    "eligible_byte_clipped_parents": eligible, "skipped_unclipped_parents": skipped_unclipped,
                    "omitted_owned_lines": omitted, "clipped_chunks": clipped}
