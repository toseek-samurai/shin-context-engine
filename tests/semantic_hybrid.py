"""Candidate-only combination. No Core scoring, coverage or authority changes."""

from collections import Counter
import math

from run_local_semantic_pilot import candidate
from run_project_search_comparison import metadata_path


def parent_lookup(units):
    lookup = {}
    for index, unit in enumerate(units):
        if unit.get("unit_kind") == "semantic-subrange":
            continue
        key = (unit["project"], unit["path"], unit["start_line"], unit["end_line"])
        if key in lookup:
            raise ValueError("Ambiguous parent unit range")
        lookup[key] = index
    return lookup


def map_lexical(project, payload, units, lookup):
    if payload.get("ok") is not True or len(payload["evidence"]) > 20:
        raise ValueError("Invalid or unbounded Core Brief")
    if payload["evidence_budget"]["max_records"] != 20:
        raise ValueError("Core output budget differs")
    mapped, excluded = {}, Counter()
    for row in payload["evidence"]:
        if metadata_path(row["path"]):
            excluded["evaluation-metadata"] += 1
            continue
        if row["type"] not in ("fact", "section"):
            excluded["non-parent-record-type"] += 1
            continue
        key = (project, row["path"], row["start_line"], row["end_line"])
        if key not in lookup:
            raise ValueError("Lexical source range is outside the frozen parent corpus")
        index = lookup[key]
        if row["type"] == "fact" and row["id"] not in units[index]["record_ids"]:
            raise ValueError("Lexical fact identity differs from the frozen parent")
        mapped.setdefault(index, {"rank": len(mapped) + 1, "evidence": []})["evidence"].append(row)
    return mapped, dict(excluded)


def combine(units, lexical, semantic, identifier_confirmed, policy, owner_confirmed=False):
    if len(lexical) > policy["candidate_limit"] or len(semantic) > policy["candidate_limit"]:
        raise ValueError("Channel candidate budget exceeded")
    for channel in (lexical, semantic):
        ranks = sorted(row["rank"] for row in channel.values())
        if ranks != list(range(1, len(channel) + 1)):
            raise ValueError("Channel ranks must be unique and contiguous")
    if any(not math.isfinite(row["similarity"]) for row in semantic.values()):
        raise ValueError("Invalid semantic score")
    indexes = set(lexical) | set(semantic)
    if any(type(i) is not int or not 0 <= i < len(units) for i in indexes):
        raise ValueError("Candidate index escapes its frozen corpus")
    if len({units[i]["project"] for i in indexes}) > 1:
        raise ValueError("Combination cannot cross project boundaries")
    if any(units[i].get("unit_kind") == "semantic-subrange" for i in indexes):
        raise ValueError("Children are not admitted by this experiment")
    code_anchor = policy["schema_version"] == "shin.semantic-code-anchor-experiment.v1"
    document_first = code_anchor or policy["schema_version"] == "shin.semantic-document-first-experiment.v1"
    anchor = {"status": "not-applicable"}
    scoped = identifier_confirmed or (document_first and owner_confirmed)
    if scoped or not semantic:
        mode = ("confirmed-identifier-lexical" if identifier_confirmed else
                "confirmed-owner-lexical" if scoped else "lexical-only")
        order = sorted(lexical, key=lambda i: lexical[i]["rank"])
        scores = {}
    elif document_first:
        mode = "semantic-document-first"
        semantic_order = sorted(semantic, key=lambda i: semantic[i]["rank"])
        lexical_order = sorted(lexical, key=lambda i: lexical[i]["rank"])
        leading = []
        if code_anchor:
            anchor = {"status": "no-eligible-source-fact-in-core-first5"}
            for index in lexical_order:
                eligible = [row for row in lexical[index]["evidence"]
                            if row.get("type") == "fact" and row.get("authority_level") == "source-fact"
                            and row.get("verification_level") == "source-observed"
                            and row.get("evidence_kind") in ("implementation-source", "test-source")
                            and type(row.get("rank")) is int and 1 <= row["rank"] <= policy["report_limit"]]
                if not eligible:
                    continue
                semantic_paths = {units[i]["path"] for i in semantic}
                anchor = {"status": "selected", "path": units[index]["path"],
                          "core_rank": min(row["rank"] for row in eligible)}
                if units[index]["path"] not in semantic_paths and len(semantic_paths) == policy["candidate_limit"]:
                    anchor["status"] = "semantic-path-budget-full"
                else:
                    leading = [index]
                    mode = "semantic-document-first-with-code-anchor"
                break
        # Preserve every semantic document before adding lexical-only documents.
        # Extra ranges are useful for inspection but must not displace navigation.
        paths, first, extra = set(), [], []
        for index in dict.fromkeys(leading + semantic_order + lexical_order):
            if units[index]["path"] in paths:
                extra.append(index)
            else:
                first.append(index)
                paths.add(units[index]["path"])
        order, scores = first + extra, {}
    else:
        mode = "reciprocal-rank-union"
        scores = {i: sum(1 / (policy["rank_constant"] + c[i]["rank"])
                         for c in (lexical, semantic) if i in c) for i in indexes}
        order = sorted(indexes, key=lambda i: (-scores[i],
            min(c[i]["rank"] for c in (lexical, semantic) if i in c), lexical.get(i, {}).get("rank", 10**9), i))
    selected, counts = [], Counter()
    for index in order:
        unit = units[index]
        if counts[unit["path"]] >= policy["per_document"]:
            continue
        counts[unit["path"]] += 1
        result = candidate(unit, 0)
        result.pop("similarity")
        result["channel"] = "hybrid-candidate-experiment"
        result["signals"] = {}
        if index in lexical:
            result["signals"]["lexical"] = lexical[index]
        if index in semantic and not scoped:
            result["signals"]["semantic"] = semantic[index]
        if index in scores:
            result["fusion_score"] = scores[index]
        selected.append(result)
        if len(selected) == policy["candidate_limit"]:
            break
    result = {"mode": mode, "candidates": selected, "semantic_grants_lexical_coverage": False,
            "symbol_existence_asserted": False, "current_source_verified": False,
            "pack_completion_asserted": False}
    if code_anchor:
        result["code_anchor"] = anchor
    return result


def regression_gate(rows, protect_ordinary_lexical=False):
    failures = []
    for row in rows:
        if row["kind"] == "negative":
            continue
        before, after = row["semantic_rank"], row["hybrid_rank"]
        if row["kind"] in ("english", "russian"):
            if before is not None and after is None:
                failures.append({"id": row["id"], "reason": "semantic-top20-hit-lost"})
            if before is not None and before <= 5 and (after is None or after > 5):
                failures.append({"id": row["id"], "reason": "semantic-first5-hit-lost"})
            if protect_ordinary_lexical:
                for channel in ("lexical", "document_first"):
                    prior = row[channel + "_rank"]
                    if prior is not None and prior <= 5 and (after is None or after > 5):
                        failures.append({"id": row["id"], "reason": channel + "-language-first5-hit-lost"})
        if row["kind"] == "exact" and row["lexical_rank"] is not None and row["lexical_rank"] <= 5:
            if after is None or after > 5:
                failures.append({"id": row["id"], "reason": "lexical-exact-first5-hit-lost"})
    return {"passed": not failures, "regressions": failures}
