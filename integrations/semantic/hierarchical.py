"""Bounded file discovery and member selection, not yet selected by Session.query."""

from copy import deepcopy
import math

from .artifact import encode, require, sha
from .selection import candidate

MAX_FILES = 100
FILE_PASSAGES = 4
MEMBER_POOL = 16
CONTEXT_FILES = 5
CONTEXT_MEMBERS = 3
NAVIGATION_RECORDS = 20
MAX_OUTPUT = 128 * 1024


def make_plan(artifact, scores, baseline):
    import numpy as np

    require(scores.shape == (len(artifact.units),) and np.isfinite(scores).all(), "invalid-query-scores")
    require(len(baseline["candidates"]) <= NAVIGATION_RECORDS, "navigation-budget")
    original = {}
    for row in baseline["candidates"]:
        identity = row["path"], row["start_line"], row["end_line"]
        require(identity in artifact.lookup, "candidate-not-enrolled")
        index = artifact.lookup[identity]
        unit = artifact.units[index]
        require(unit["project"] == row["project"] and unit["source_sha256"] == row["source_sha256"]
                and sorted(unit["record_ids"]) == sorted(row["record_ids"]), "candidate-binding-mismatch")
        original.setdefault(row["path"], []).append(index)
    require(all(len(set(indices)) == len(indices) <= 2 for indices in original.values()), "candidate-identity-budget")
    order = [int(i) for i in np.argsort(-scores, kind="stable")]
    # Retain all existing files before filling the internal pool; this is not an output ranking.
    paths = dict.fromkeys(original)
    for index in order:
        if len(paths) == MAX_FILES:
            break
        paths.setdefault(artifact.units[index]["path"])
    selected = {path: list(original.get(path, [])) for path in paths}
    for index in order:
        path = artifact.units[index]["path"]
        if path in selected and len(selected[path]) < MEMBER_POOL and index not in selected[path]:
            selected[path].append(index)
    files = []
    for path, indices in selected.items():
        require(indices, "empty-file-candidates")
        files.append({"path": path, "representatives": indices[:FILE_PASSAGES], "members": indices,
                      "indexed_members": len(artifact.by_path[path]),
                      "members_truncated": len(indices) < len(artifact.by_path[path]),
                      "semantic_score": max(float(scores[i]) for i in indices)})
    files.sort(key=lambda file: (-file["semantic_score"], min(file["members"])))
    indices = {i for file in files for i in file["members"]}
    return {"snapshot_id": artifact.manifest["binding"]["snapshot_id"],
            "project": artifact.manifest["binding"]["project"], "files": files,
            "units": {str(i): deepcopy(artifact.units[i]) for i in sorted(indices)},
            "baseline": deepcopy(baseline)}


def execute(plan, query, texts, reranker, *, scoped=False):
    require(isinstance(query, str) and 0 < len(query.encode()) <= 4096 and "\0" not in query, "invalid-query")
    if scoped:
        result = {"navigation": deepcopy(plan["baseline"]), "collection": None,
                  "work": {"bypassed": True, "files": 0, "reranked_passages": 0}}
        require(len(encode(result)) + 1 <= MAX_OUTPUT, "hierarchical-output-budget")
        return result
    files, units = plan["files"], plan["units"]
    require(len(files) <= MAX_FILES and len({f["path"] for f in files}) == len(files), "file-pool-budget")
    allowed = set()
    for file in files:
        members, representatives = file["members"], file["representatives"]
        require(0 < len(members) <= MEMBER_POOL and len(set(members)) == len(members)
                and 0 < len(representatives) <= FILE_PASSAGES and len(set(representatives)) == len(representatives)
                and set(representatives) <= set(members), "member-pool-budget")
        for index in members:
            unit = units[str(index)]
            require(unit["path"] == file["path"] and unit["project"] == plan["project"], "member-file-mismatch")
        allowed.update(members)
    require(set(units) == {str(i) for i in allowed}, "unbound-plan-unit")
    scores = {}

    def score(indices):
        pending = list(dict.fromkeys(i for i in indices if i not in scores))
        passages = []
        for index in pending:
            require(index in allowed, "member-not-admitted")
            text = texts(units[str(index)])
            require(isinstance(text, str) and 0 < len(text.encode()) <= 65536
                    and sha(text.encode()) == units[str(index)]["text_sha256"], "member-text-binding-mismatch")
            passages.append(text)
        if pending:
            values = list(reranker.rerank(query, passages, batch_size=2))
            require(len(values) == len(pending) and all(math.isfinite(v) for v in values), "invalid-reranker-scores")
            scores.update(zip(pending, values))

    score(i for file in files for i in file["representatives"])
    ordered = sorted(enumerate(files), key=lambda pair: (-max(scores[i] for i in pair[1]["representatives"]), pair[0]))
    leading = [file for _, file in ordered[:CONTEXT_FILES]]
    score(i for file in leading for i in file["members"])
    original = {row["path"]: row for row in plan["baseline"]["candidates"]
                if "source_reference" in row.get("signals", {})}

    def projected(index):
        unit = units[str(index)]
        row = candidate(unit)
        row["signals"] = {"reranker": {"score": scores[index], "scope": "admitted-passage"}}
        if unit["path"] in original:
            # Preserve the complete relation witness even when the selected member changes.
            row["signals"]["file_reference"] = {"scope": "file-binding-not-member-selection",
                                                "basis": deepcopy(original[unit["path"]])}
        return row

    navigation = []
    for _, file in ordered[:NAVIGATION_RECORDS]:
        best = min(file["representatives"], key=lambda i: (-scores[i], file["representatives"].index(i)))
        navigation.append(projected(best))
    collection = []
    for file in leading:
        members = sorted(file["members"], key=lambda i: (-scores[i], file["members"].index(i)))
        collection.append({"path": file["path"], "members": [projected(i) for i in members[:CONTEXT_MEMBERS]],
                           "indexed_members": file["indexed_members"], "members_examined": len(members),
                           "members_truncated": file["members_truncated"]})
    result = {"navigation": {"mode": "hierarchical-candidate", "candidates": navigation,
        "semantic_grants_lexical_coverage": False, "symbol_existence_asserted": False,
        "current_source_verified": False, "pack_completion_asserted": False},
        "collection": {"files": collection, "source_inspection_required": True,
            "answer_completeness": "not-asserted", "scope": "separate-member-navigation-not-native-pack",
            "limits": {"files": CONTEXT_FILES, "members_per_file": CONTEXT_MEMBERS}},
        "work": {"bypassed": False, "files": len(files), "reranked_passages": len(scores)},
        "snapshot_id": plan["snapshot_id"], "project": plan["project"]}
    require(len(encode(result)) + 1 <= MAX_OUTPUT, "hierarchical-output-budget")
    return result
