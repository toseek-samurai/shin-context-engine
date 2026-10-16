#!/usr/bin/env python3
"""Frozen-corpus SQLite FTS5/BM25 reference, never imported by production retrieval."""

import argparse
from collections import Counter
import json
from pathlib import Path
import sqlite3
import sys
import time

sys.dont_write_bytecode = True

from check_semantic_artifact_parity import CONTROL_SHA, FREEZE_SHA
from run_local_semantic_pilot import fingerprint, load_completed_run, peak_rss_bytes
from semantic_hybrid import combine

POLICY = {"schema_version": "shin.semantic-hybrid-experiment.v1", "candidate_limit": 20,
          "report_limit": 5, "per_document": 2, "rank_constant": 60}
MAX_SECONDS = 120
MAX_RSS = 3 * 1024 ** 3


class BM25:
    """Use SQLite's tokenizer and scorer, without reproducing either algorithm."""
    def __init__(self, units, deadline):
        self.units = units
        self.db = sqlite3.connect(":memory:")
        self.db.set_progress_handler(lambda: int(time.monotonic() > deadline), 10000)
        self.db.execute("PRAGMA temp_store=MEMORY")
        self.db.execute("CREATE VIRTUAL TABLE documents USING fts5(body, tokenize='unicode61')")
        self.db.execute("CREATE VIRTUAL TABLE query_text USING fts5(body, tokenize='unicode61')")
        self.db.execute("CREATE VIRTUAL TABLE query_vocab USING fts5vocab(query_text, 'row')")
        with self.db:
            self.db.executemany("INSERT INTO documents(rowid,body) VALUES (?,?)",
                                ((i + 1, unit["text"]) for i, unit in enumerate(units)))

    def close(self):
        self.db.close()

    def search(self, query):
        if not isinstance(query, str) or not 0 < len(query.encode()) <= 4096 or "\0" in query:
            raise ValueError("Invalid bounded query")
        self.db.execute("DELETE FROM query_text")
        self.db.execute("INSERT INTO query_text(body) VALUES (?)", (query,))
        terms = [row[0] for row in self.db.execute("SELECT term FROM query_vocab ORDER BY term")]
        if len(terms) > 64:
            raise ValueError("Query token budget exceeded")
        if not terms:
            return []
        expression = " OR ".join('"' + term.replace('"', '""') + '"' for term in terms)
        rows = self.db.execute("SELECT rowid, bm25(documents) AS score FROM documents "
                               "WHERE documents MATCH ? ORDER BY score, rowid", (expression,))
        result, counts = [], Counter()
        for rowid, score in rows:
            index = rowid - 1
            path = self.units[index]["path"]
            if counts[path] == POLICY["per_document"]:
                continue
            counts[path] += 1
            result.append({"index": index, "rank": len(result) + 1, "score": score})
            if len(result) == POLICY["candidate_limit"]:
                break
        return result


def rank(rows, expected):
    return next((i for i, row in enumerate(rows, 1) if row["path"] in expected), None)


def identity(unit):
    return unit["path"], unit["start_line"], unit["end_line"], unit["source_sha256"]


def run(freeze, control, output):
    started = time.monotonic()
    if fingerprint(freeze / "freeze.json") != FREEZE_SHA or fingerprint(control / "report.json") != CONTROL_SHA:
        raise ValueError("Historical control changed")
    binding = json.loads((freeze / "freeze.json").read_bytes())
    if fingerprint(freeze / "cohorts.json") != binding["cohorts_sha256"]:
        raise ValueError("Frozen questions changed")
    _, _, manifest, source = load_completed_run(Path(binding["run"]))
    if fingerprint(source / "input-manifest.json") != binding["input_manifest_sha256"]:
        raise ValueError("Frozen corpus binding changed")
    groups = {}
    with (source / "units.jsonl").open() as stream:
        for line in stream:
            unit = json.loads(line)
            if unit.get("unit_kind") == "semantic-subrange":
                raise ValueError("Reference requires unchanged parent units")
            groups.setdefault(unit["project"], []).append(unit)
    cohorts = json.loads((freeze / "cohorts.json").read_bytes())
    prior = json.loads((control / "report.json").read_bytes())
    results = []
    for project, units in groups.items():
        lookup = {identity(unit): i for i, unit in enumerate(units)}
        if len(lookup) != len(units):
            raise ValueError("Duplicate source identity")
        engine = BM25(units, started + MAX_SECONDS)
        try:
            for name, cohort in cohorts.items():
                cases = next(p for p in cohort["projects"] if p["id"] == project)["cases"]
                for case in cases:
                    old = next(r for r in prior["results"][name] if r["id"] == case["id"])
                    if old["project"] != project or old["kind"] != case["kind"]:
                        raise ValueError("Historical case binding changed")
                    hits = engine.search(case["query"])
                    lexical = {hit["index"]: {"rank": hit["rank"], "evidence": []} for hit in hits}
                    semantic = {}
                    for i, row in enumerate(old["semantic_candidates"], 1):
                        index = lookup[identity(row)]
                        if sorted(units[index]["record_ids"]) != sorted(row["record_ids"]):
                            raise ValueError("Semantic record identity changed")
                        semantic[index] = {"rank": i, "similarity": row["similarity"]}
                    scoped = old["core_retrieval_mode"] in ("identifier-scoped", "owner-scoped")
                    fused = old["hybrid"] if scoped else combine(units, lexical, semantic, False, POLICY)
                    rows = [{**{k: units[h["index"]][k] for k in
                        ("path", "start_line", "end_line", "source_sha256", "record_ids")}, "bm25": h["score"]} for h in hits]
                    results.append({"id": case["id"], "project": project, "cohort": name, "kind": case["kind"],
                        "core_lexical_rank": old["lexical_rank"], "current_rank": old["hybrid_rank"],
                        "bm25_rank": rank(rows, case["expected_paths"]),
                        "bm25_rrf_rank": rank(fused["candidates"], case["expected_paths"]),
                        "returned": {"bm25": len(rows), "bm25_rrf": len(fused["candidates"]),
                                     "current": len(old["hybrid"]["candidates"])},
                        "scoped_native_bypass": scoped, "bm25_candidates": rows,
                        "bm25_rrf_candidates": fused["candidates"]})
                    if time.monotonic() - started > MAX_SECONDS or peak_rss_bytes() > MAX_RSS:
                        raise ValueError("Reference budget exceeded")
        finally:
            engine.close()
    if len(results) != 60:
        raise ValueError("Incomplete historical cohort")
    if fingerprint(source / "units.jsonl") != manifest["units_sha256"]:
        raise ValueError("Frozen corpus changed during the reference run")
    summary = {}
    for kind in ("exact", "english", "russian", "negative"):
        members = [r for r in results if r["kind"] == kind]
        summary[kind] = {channel: {"first5": sum(r[channel] is not None and r[channel] <= 5 for r in members),
                                  "top20": sum(r[channel] is not None for r in members)}
                         for channel in ("core_lexical_rank", "current_rank", "bm25_rank", "bm25_rrf_rank")}
    regressions = [r["id"] for r in results if r["kind"] != "negative" and r["current_rank"] is not None
                   and r["current_rank"] <= 5 and (r["bm25_rrf_rank"] is None or r["bm25_rrf_rank"] > 5)]
    report = {"schema_version": "shin.standard-retrieval-reference.v1", "completed": True,
        "selected_for_runtime": False, "scope": "frozen-library-reference-not-live-quality-or-release-proof",
        "engine": {"library": "SQLite FTS5", "version": sqlite3.sqlite_version, "tokenizer": "unicode61",
                   "scorer": "builtin-bm25-default-weights", "query": "quoted-token-OR-no-aliases-or-stemming",
                   "corpus": "unchanged-redacted-model-text-per-project-IDF", "fusion": POLICY},
        "control_sha256": CONTROL_SHA, "freeze_sha256": FREEZE_SHA,
        "input_manifest_sha256": binding["input_manifest_sha256"], "summaries": summary,
        "projection_boundary": "Same enrolled parent units, but BM25 sees frozen embedding text; Core scores its own lexical fields. This is not an isolated scorer-only comparison.",
        "protected_first5_regressions": regressions, "results": results,
        "negative_boundary": "neighbors-never-imply-existence-or-answer-completeness",
        "seconds": round(time.monotonic() - started, 3), "peak_rss_bytes": peak_rss_bytes(),
        "document_vectors_computed": 0, "source_mutated": False, "installed": False}
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    output.chmod(0o600)
    print(json.dumps({k: v for k, v in report.items() if k != "results"}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ("freeze", "control", "output"):
        parser.add_argument("--" + field, type=Path, required=True)
    args = parser.parse_args()
    run(args.freeze.resolve(), args.control.resolve(), args.output.resolve())
