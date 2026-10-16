#!/usr/bin/env python3
"""Read-only structural coverage probe; persist counters, never source text."""
import argparse
import json
from pathlib import Path
import subprocess
import time

from reproject_semantic_identifiers import publication_fingerprint


def require(value):
    if not value:
        raise RuntimeError("structural-probe-validation-failed")


def probe(exporter, root, index, on_chunk=None):
    start = time.monotonic()
    summary = {"schema_version": "shin.structural-chunk-probe.v1", "scope": "representation-not-agent-quality",
               "files": 0, "chunks": 0, "source_bytes": 0, "redacted_files": 0,
               "syntax_complete_files": 0, "unsupported_files": 0, "fallback_chunks": 0,
               "incomplete_files": 0, "omitted_bytes": 0, "max_chunk_bytes": 0,
               "disclosed_source_bytes": 0, "elapsed_scope": "export-and-independent-validation"}
    process = subprocess.Popen([str(exporter), str(root), str(index), "--structural-chunks"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    binding = None
    file = None
    raw = b""
    position = chunks = 0
    completed = False
    def finish_file():
        if file is not None:
            require(position == file["covered_bytes"] and chunks == file["chunks"])
            require(file["coverage_complete"] == (position == len(raw)))
            require((root / file["path"]).read_bytes() == raw)
    try:
        for line in process.stdout:
            require(not completed and len(line) <= 131072)
            row = json.loads(line)
            kind = row["type"]
            if kind == "binding":
                require(binding is None and summary["files"] == 0)
                require(row["projection"] == "core-bound-structural-chunks-v1")
                binding = row
            elif kind == "file":
                require(binding is not None)
                finish_file()
                file, position, chunks = row, 0, 0
                path = (root / row["path"]).resolve(strict=True)
                require(path.is_relative_to(root) and path.is_file())
                raw = path.read_bytes()
                require(len(raw) == row["source_bytes"] and len(raw) <= binding["max_source_file_bytes"])
                require(publication_fingerprint(raw) == row["source_content_hash"])
                require(row["omitted_bytes"] == len(raw) - row["covered_bytes"])
                summary["files"] += 1
                summary["source_bytes"] += len(raw)
                summary["redacted_files"] += row["redacted"]
                if not row["redacted"]:
                    summary["disclosed_source_bytes"] += row["covered_bytes"]
                summary["syntax_complete_files"] += row["syntax_complete"]
                summary["unsupported_files"] += not row["supported"]
                summary["incomplete_files"] += not row["coverage_complete"]
                summary["omitted_bytes"] += row["omitted_bytes"]
            elif kind == "chunk":
                require(file is not None and row["path"] == file["path"] and row["index"] == chunks)
                require(row["source_content_hash"] == file["source_content_hash"])
                require(row["byte_start"] == position and position < row["byte_end"] <= len(raw))
                part = raw[position:row["byte_end"]]
                require(len(part) <= binding["chunk_policy"]["max_bytes"])
                require(publication_fingerprint(part) == row["span_hash"])
                require(row["redacted"] == file["redacted"])
                require(row["text"] == ("[REDACTED:source-file]" if row["redacted"] else part.decode("utf-8")))
                require(row["start_line"] == raw[:position].count(b"\n") + 1)
                require(row["end_line"] == raw[:row["byte_end"] - 1].count(b"\n") + 1)
                require(row["range_origin"] in ("syntax-partition", "bounded-text-fallback"))
                if on_chunk is not None:
                    on_chunk(row)
                position = row["byte_end"]
                chunks += 1
                summary["chunks"] += 1
                summary["fallback_chunks"] += row["range_origin"] == "bounded-text-fallback"
                summary["max_chunk_bytes"] = max(summary["max_chunk_bytes"], len(part))
            elif kind == "complete":
                finish_file()
                require(binding is not None and row["snapshot_id"] == binding["snapshot_id"])
                require(row["records"] == row["source_files"] == summary["files"] == binding["selected_source_files"])
                completed = True
            else:
                require(False)
        require(process.wait(timeout=10) == 0 and completed)
        summary.update(ok=summary["incomplete_files"] == 0, snapshot_id=binding["snapshot_id"],
                       elapsed_ms=round((time.monotonic() - start) * 1000, 3))
        return summary
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
        process.stdout.close()
        process.stderr.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exporter", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--index", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = probe(args.exporter.resolve(strict=True), args.root.resolve(strict=True), args.index.resolve(strict=True))
        print(json.dumps(result, sort_keys=True))
        return 0 if result["ok"] else 1
    except (ValueError, KeyError, OSError, RuntimeError, subprocess.SubprocessError):
        print(json.dumps({"ok": False, "error": "structural-probe-failed"}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
