"""The measured code-anchor/one-source-reference policy, without eval inputs."""

from collections import Counter
from pathlib import PurePosixPath

from .artifact import relative, require


def metadata_path(path):
    return ("shin-profiles" in PurePosixPath(path).parts or
            path.startswith(("profiles/", "tests/evidence/", "tests/fixtures/", "docs/kb/")) or
            path.endswith("-reference-cases.json"))


def candidate(unit, score=None):
    value = {key: unit[key] for key in ("project", "path", "start_line", "end_line", "source_sha256", "record_ids")}
    value["record_ids"] = list(unit["record_ids"])
    value.update(channel="semantic-navigation", status="navigation-candidate-not-an-answer", current_source_verified=False)
    if score is not None:
        value["similarity"] = float(score)
    return value


def navigate(artifact, core, scores):
    import numpy as np

    units = artifact.units
    require(scores.shape == (len(units),) and np.isfinite(scores).all(), "invalid-query-scores")
    require(len(core["evidence"]) <= 20 and core["evidence_budget"]["max_records"] == 20, "core-evidence-budget")
    lexical, excluded = {}, Counter()
    for row in core["evidence"]:
        if metadata_path(row["path"]):
            excluded["evaluation-metadata"] += 1
            continue
        if row["type"] not in ("fact", "section"):
            excluded["non-parent-record-type"] += 1
            continue
        key = row["path"], row["start_line"], row["end_line"]
        require(key in artifact.lookup, "lexical-range-not-enrolled")
        index = artifact.lookup[key]
        require(row["type"] != "fact" or row["id"] in units[index]["record_ids"], "lexical-record-mismatch")
        lexical.setdefault(index, {"rank": len(lexical) + 1, "evidence": []})["evidence"].append(row)
    semantic, counts = {}, Counter()
    for item in np.argsort(-scores, kind="stable"):
        index = int(item)
        path = units[index]["path"]
        if counts[path] == 2:
            continue
        counts[path] += 1
        semantic[index] = {"rank": len(semantic) + 1, "similarity": float(scores[index])}
        if len(semantic) == 20:
            break
    scoped = core["retrieval_mode"] in ("identifier-scoped", "owner-scoped")
    anchor = {"status": "not-applicable"}
    order = list(lexical)
    if scoped:
        mode = "confirmed-identifier-lexical" if core["retrieval_mode"] == "identifier-scoped" else "confirmed-owner-lexical"
    else:
        mode = "semantic-document-first"
        anchor = {"status": "no-eligible-source-fact-in-core-first5"}
        leading = []
        for index in lexical:
            eligible = [row for row in lexical[index]["evidence"] if row.get("type") == "fact"
                        and row.get("authority_level") == "source-fact" and row.get("verification_level") == "source-observed"
                        and row.get("evidence_kind") in ("implementation-source", "test-source")
                        and type(row.get("rank")) is int and 1 <= row["rank"] <= 5]
            if not eligible:
                continue
            paths = {units[i]["path"] for i in semantic}
            anchor = {"status": "selected", "path": units[index]["path"], "core_rank": min(row["rank"] for row in eligible)}
            if units[index]["path"] not in paths and len(paths) == 20:
                anchor["status"] = "semantic-path-budget-full"
            else:
                leading = [index]
                mode = "semantic-document-first-with-code-anchor"
            break
        seen, first, extra = set(), [], []
        for index in dict.fromkeys(leading + list(semantic) + list(lexical)):
            if units[index]["path"] in seen:
                extra.append(index)
            else:
                first.append(index)
                seen.add(units[index]["path"])
        order = first + extra
    selected, counts = [], Counter()
    for index in order:
        if counts[units[index]["path"]] == 2:
            continue
        counts[units[index]["path"]] += 1
        row = candidate(units[index])
        row["signals"] = {}
        if index in lexical:
            row["signals"]["lexical"] = lexical[index]
        if index in semantic and not scoped:
            row["signals"]["semantic"] = semantic[index]
        selected.append(row)
        if len(selected) == 20:
            break
    result = {"mode": mode, "candidates": selected, "code_anchor": anchor,
              "semantic_grants_lexical_coverage": False, "symbol_existence_asserted": False,
              "current_source_verified": False, "pack_completion_asserted": False,
              "source_hop": {"status": "not-applicable", "hops": 0}}
    if anchor["status"] == "selected":
        resolve_reference(artifact, result, scores)
    return result, dict(excluded)


def resolve_reference(artifact, result, scores):
    origin = result["candidates"][0]
    rows = [row for row in artifact.links.get(origin["path"], [])
            if origin["start_line"] <= row["line"] <= origin["end_line"]]
    hop = result["source_hop"]
    hop["status"] = "no-source-link-in-selected-range"
    if not rows:
        return
    if len(rows) > 20:
        hop["status"] = "source-link-budget-exceeded"
        return
    targets = set()
    for row in rows:
        path = row.get("resolved_path")
        if row.get("status") != "resolved" or not relative(path) or path not in artifact.by_path:
            hop["status"] = "unresolved-or-unenrolled-target"
            return
        if path == origin["path"]:
            hop["status"] = "self-reference"
            return
        targets.add(path)
    if len(targets) != 1:
        hop["status"] = "ambiguous-source-targets"
        return
    path = next(iter(targets))
    if path in {row["path"] for row in result["candidates"]}:
        hop["status"] = "target-already-selected"
        return
    index = min(artifact.by_path[path], key=lambda i: (-scores[i], i))
    link = min(rows, key=lambda r: (r["line"], r["kind"], r.get("target", "")))
    related = candidate(artifact.units[index], scores[index])
    related["channel"] = "resolved-source-reference-navigation"
    related["signals"] = {"source_reference": {
        "path": origin["path"], "start_line": origin["start_line"], "end_line": origin["end_line"],
        "source_sha256": origin["source_sha256"], "record_ids": origin["record_ids"], "line": link["line"],
        "kind": link["kind"], "resolved_path": path, "scope": "one-hop-same-frozen-publication",
        "selection": "best-query-similarity-within-linked-path", "origin": origin}}
    result["candidates"][0] = related
    result["mode"] = "code-anchor-resolved-source-reference"
    result["source_hop"] = {"status": "resolved-in-place", "hops": 1, "path": path,
                            "origin_path": origin["path"], "origin_primary_replaced": True,
                            "origin_retained_as_provenance": True}
