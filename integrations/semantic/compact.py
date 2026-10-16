"""Lossless evidence grouping for the source-owned agent query entrypoint."""

from copy import deepcopy

from .artifact import encode, relative, require

MAX_BYTES = 128 * 1024
READ_TEXT_FORMAT = "physical-line-number-tab-v1"
RECORD_DEFAULTS = ("type", "record_type", "status", "authority_level", "evidence_kind",
                   "verification_level", "source", "range_origin")
NAVIGATION_DEFAULTS = ("project", "channel", "status", "current_source_verified", "range_kind")


def physical_lines(text):
    parts = text.split("\n")
    lines = [part + "\n" for part in parts[:-1]]
    if parts[-1] or not lines:
        lines.append(parts[-1])
    return lines


def number_read(result):
    """Display physical line labels without duplicating or changing source bytes."""
    require(result.get("schema_version") == "shin.agent-read.v1" and result.get("ok") is True
            and "body_text_format" not in result, "invalid-read-projection")
    output = deepcopy(result)
    for body in output["native"]["evidence"]["bodies"]:
        require("numbered_text" not in body, "invalid-read-projection")
        if body.get("state") != "complete":
            require(body.get("text") is None, "invalid-read-projection")
            continue
        require(isinstance(body.get("text"), str), "invalid-read-projection")
        lines = physical_lines(body.pop("text"))
        start, end = body.get("start_line"), body.get("end_line")
        require(type(start) is int and type(end) is int and 1 <= start <= end
                and len(lines) == end - start + 1, "invalid-read-line-range")
        body["numbered_text"] = "".join(f"{start + offset}\t{line}" for offset, line in enumerate(lines))
    output["schema_version"] = "shin.agent-read.v2"
    output["body_text_format"] = READ_TEXT_FORMAT
    require(len(encode(output)) + 1 <= MAX_BYTES, "agent-read-output-budget")
    return output


def expand_read(result):
    """Restore exact native body text before hashing, literal matching or audit."""
    if result.get("schema_version") == "shin.agent-read.v1":
        return deepcopy(result)
    require(result.get("schema_version") == "shin.agent-read.v2" and result.get("ok") is True
            and result.get("body_text_format") == READ_TEXT_FORMAT
            and len(encode(result)) + 1 <= MAX_BYTES, "invalid-read-projection")
    output = deepcopy(result)
    for body in output["native"]["evidence"]["bodies"]:
        if body.get("state") != "complete":
            require(body.get("text") is None and "numbered_text" not in body, "invalid-read-projection")
            continue
        require("text" not in body and isinstance(body.get("numbered_text"), str), "invalid-read-projection")
        start, end = body.get("start_line"), body.get("end_line")
        lines = physical_lines(body.pop("numbered_text"))
        require(type(start) is int and type(end) is int and 1 <= start <= end
                and len(lines) == end - start + 1, "invalid-read-line-range")
        raw = []
        for offset, line in enumerate(lines):
            prefix = f"{start + offset}\t"
            require(line.startswith(prefix), "invalid-read-line-label")
            raw.append(line[len(prefix):])
        body["text"] = "".join(raw)
    output["schema_version"] = "shin.agent-read.v1"
    del output["body_text_format"]
    return output


def factor_defaults(rows, fields):
    defaults = {}
    if len(rows) > 1:
        for field in fields:
            if field in rows[0] and all(field in row and encode(row[field]) == encode(rows[0][field]) for row in rows):
                defaults[field] = rows[0][field]
        for row in rows:
            for field in defaults:
                del row[field]
    return defaults


def condense_query(result):
    """Readable common fields only; no record, text, ranking or provenance loss."""
    require(result.get("schema_version") == "shin.agent-query.v1" and result.get("ok") is True,
            "invalid-agent-projection")
    output = deepcopy(result)
    native = output["native"]
    require(native.get("projection") == "grouped-native-records-v1"
            and not {"record_defaults", "range_defaults"}.intersection(native), "invalid-agent-projection")
    rows = [row for file in native["files"] for row in file["records"]]
    native["record_defaults"] = factor_defaults(rows, RECORD_DEFAULTS)
    native["range_defaults"] = factor_defaults([row["range"] for row in rows], ("kind", "read_scope"))
    for row in rows:
        if row.get("id") == row.get("evidence_id") and "id" in row:
            del row["evidence_id"]
    native["projection"] = "grouped-native-records-v2"
    navigation = output["navigation"]
    if navigation is not None:
        require("candidate_defaults" not in navigation, "reserved-projection-field")
        navigation["candidate_defaults"] = factor_defaults(navigation["candidates"], NAVIGATION_DEFAULTS)
    output["schema_version"] = "shin.agent-query.v2"
    require(len(encode(output)) + 1 <= MAX_BYTES, "agent-query-output-budget")
    return output


def expand_query(result):
    """Restore the full grouped v1 envelope for audit/consumers needing row fields."""
    if result.get("schema_version") == "shin.agent-query.v1":
        return deepcopy(result)
    require(result.get("schema_version") == "shin.agent-query.v2", "invalid-agent-projection")
    output = deepcopy(result)
    native = output["native"]
    require(native.get("projection") == "grouped-native-records-v2", "invalid-agent-projection")

    def inherit(row, defaults, allowed):
        require(isinstance(defaults, dict) and set(defaults) <= set(allowed)
                and not set(row).intersection(defaults), "invalid-agent-defaults")
        row.update(deepcopy(defaults))

    defaults, ranges = native.pop("record_defaults"), native.pop("range_defaults")
    for file in native["files"]:
        for row in file["records"]:
            inherit(row, defaults, RECORD_DEFAULTS)
            inherit(row["range"], ranges, ("kind", "read_scope"))
            if "evidence_id" not in row:
                require(isinstance(row.get("id"), str), "invalid-evidence-binding")
                row["evidence_id"] = row["id"]
    native["projection"] = "grouped-native-records-v1"
    navigation = output["navigation"]
    if navigation is not None:
        defaults = navigation.pop("candidate_defaults")
        for row in navigation["candidates"]:
            inherit(row, defaults, NAVIGATION_DEFAULTS)
    output["schema_version"] = "shin.agent-query.v1"
    return output


def range_view(row):
    start, end = row["start_line"], row["end_line"]
    require(type(start) is int and type(end) is int and 1 <= start <= end, "invalid-evidence-range")
    if row["type"] == "section":
        kind, scope = "section-span", "range"
    elif row.get("range_origin") and row.get("source", "").startswith("source-fact:"):
        kind, scope = "source-span", "range"
    elif (row["type"] in ("decision", "contract", "runbook") and row.get("source") == row["path"]
          and start == end == row.get("line") and not row.get("range_origin")):
        # Core typed_query_fact publishes a document/heading anchor, not the
        # source span from which its summary was assembled.
        kind, scope = "document-anchor", "document"
    else:
        kind, scope = "unclassified-range", "document"
    return {"start_line": start, "end_line": end, "kind": kind, "read_scope": scope}


def group_native(core):
    require(not {"files", "source_schema"}.intersection(core), "reserved-projection-field")
    require(core.get("schema_version") == "shin.context-brief-compact.v1" and core.get("tool") == "shin-context"
            and core.get("command") == "brief" and core.get("ok") is True and core.get("projection") == "compact"
            and core.get("index_mode") == "generated-index", "invalid-native-envelope")
    evidence = core["evidence"]
    require(isinstance(evidence, list) and len(evidence) <= 20, "core-evidence-budget")
    paths = {}
    for path in core["verification_paths"]:
        require(relative(path["path"]) and path["path"] not in paths, "invalid-verification-path")
        paths[path["path"]] = path
    files, positions, references = {}, {}, {}
    for index, original in enumerate(evidence):
        require(not {"ref", "evidence_id", "range", "excerpt_ref"}.intersection(original), "reserved-projection-field")
        require(original.get("rank") == index + 1 and relative(original["path"]), "invalid-evidence-order")
        path = original["path"]
        require(path in paths and isinstance(original["excerpt"], str), "invalid-evidence-binding")
        position = positions.get(path, 0)
        ids = paths[path]["evidence_ids"]
        require(position < len(ids), "invalid-evidence-binding")
        identity = ids[position]
        require(isinstance(identity, str) and identity and
                (original["type"] == "section" or identity == original.get("id")), "invalid-evidence-binding")
        positions[path] = position + 1
        group = files.setdefault(path, {"path": path, "records": [], "excerpts": []})
        row = deepcopy(original)
        text = row.pop("excerpt")
        if text not in group["excerpts"]:
            group["excerpts"].append(text)
        view = range_view(original)
        reference = "n" + str(index + 1)
        row.update(ref=reference, evidence_id=identity, range=view, excerpt_ref=group["excerpts"].index(text))
        for field in ("path", "start_line", "end_line"):
            row.pop(field)
        group["records"].append(row)
        references[encode(original)] = reference
    require(list(paths) == list(files), "invalid-verification-path")
    for path, group in files.items():
        expected = sorted({(row["range"]["start_line"], row["range"]["end_line"]) for row in group["records"]})
        actual = [(row["start_line"], row["end_line"]) for row in paths[path]["ranges"]]
        require(expected == actual and positions[path] == len(paths[path]["evidence_ids"]), "invalid-evidence-binding")
    result = deepcopy({key: value for key, value in core.items() if key not in ("schema_version", "evidence", "verification_paths")})
    result.update(source_schema=core["schema_version"], projection="grouped-native-records-v1", files=list(files.values()))
    return result, references


def group_navigation(navigation, references):
    if navigation is None:
        return None
    result = deepcopy(navigation)
    require(len(result["candidates"]) <= 20 and result["semantic_grants_lexical_coverage"] is False
            and result["symbol_existence_asserted"] is False and result["current_source_verified"] is False
            and result["pack_completion_asserted"] is False, "invalid-navigation-claims")

    def pack(row, depth=0):
        require(relative(row["path"]) and row["current_source_verified"] is False, "invalid-navigation-claims")
        require("range_kind" not in row, "reserved-projection-field")
        row["range_kind"] = "unclassified-navigation-range"
        signals = row.get("signals", {})
        if "lexical" in signals:
            lexical = signals["lexical"]
            require("evidence_refs" not in lexical, "reserved-projection-field")
            evidence = lexical.pop("evidence")
            require(all(encode(item) in references for item in evidence), "navigation-evidence-not-in-core")
            lexical["evidence_refs"] = [references[encode(item)] for item in evidence]
        if "source_reference" in signals:
            require(depth == 0, "recursive-navigation-origin")
            pack(signals["source_reference"]["origin"], depth + 1)

    for row in result["candidates"]:
        pack(row)
    return result


def project(core, *, admission, semantic_status, navigation=None, metadata=None):
    freshness = core["index_freshness"]
    require(freshness.get("current") is True and freshness.get("fully_verified") is True
            and freshness.get("status") == "current" and freshness.get("snapshot_id") == core["snapshot_id"],
            "core-currentness-rejected")
    native, references = group_native(core)
    result = {"schema_version": "shin.agent-query.v1", "ok": True, "admission": deepcopy(admission),
              "native": native, "semantic_status": semantic_status,
              "navigation": group_navigation(navigation, references),
              "interpretation": {"file_order": "first-native-evidence-occurrence-not-a-new-ranking",
                  "excerpt_completeness": "not-asserted", "source_inspection_required": True,
                  "navigation_is_evidence": False, "record_merging": False, "text_deduplication": "per-file-excerpts-only"}}
    if metadata:
        result["semantic_binding"] = deepcopy(metadata)
    require(len(encode(result)) + 1 <= MAX_BYTES, "agent-query-output-budget")
    return result


def from_semantic(result):
    require(result.get("schema_version") == "shin.semantic-query.v1" and result.get("ok") is True, "invalid-semantic-envelope")
    return project(result["core"], admission={"kind": "core-current-only", "ready": None,
                   "readiness": "not-evaluated-use-canonical-adoption-gates", "source_admission": result["source_admission"]},
                   semantic_status=result["semantic_status"], navigation=result["navigation"],
                   metadata={key: result[key] for key in ("project", "artifact_sha256", "lexical_excluded") if key in result})


def from_adapter(result):
    require(result.get("schema_version") == "shin.hermes-result.v1" and result.get("operation") == "brief"
            and result.get("ok") is True, "invalid-adapter-envelope")
    readiness = result["readiness"]
    require(readiness.get("schema_version") == "shin.agent-readiness.v1" and readiness.get("ok") is True
            and readiness.get("ready") is True and readiness.get("strict") is True, "adapter-readiness-rejected")
    route = result["route"]
    require(result["result"]["profile"] == route["profile"] == readiness["profile"], "adapter-route-mismatch")
    require(readiness["root"] == route["root"] and readiness["out_root"] == route["out_dir"]
            and readiness["marker"] == route["marker"], "adapter-route-mismatch")
    snapshots = [step.get("result", {}).get("snapshot", {}).get("snapshot_id")
                 for step in readiness.get("steps", []) if step.get("id") == "worktree-diff" and step.get("status") == "passed"]
    require(len(snapshots) <= 1, "invalid-readiness-observation")
    return project(result["result"], admission={"kind": "strict-ready-and-current-brief", "ready": True,
                   "route": route, "readiness_schema": readiness["schema_version"],
                   "worktree_observation_snapshot_id": snapshots[0] if snapshots else None,
                   "readiness_and_query_atomic": False,
                   "source_admission": "current-at-core-admission-not-a-future-mutation-guarantee"},
                   semantic_status="not-requested")
