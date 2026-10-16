#!/usr/bin/env python3
"""Capture bounded current pilot inputs before changing the native representation."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def digest(data):
    return hashlib.sha256(data).hexdigest()


def fnv(data):
    # Preserve Core's published legacy offset; this is not a new hash format.
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f"fnv1a64:{value:016x}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "core", "index", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    root, core, index = args.root.resolve(), args.core.resolve(), args.index.resolve()
    profile = root / "profiles/shin-server.json"
    policy = root / "tests/fixtures/code-context-pilot-policy.json"
    manifest_path = index / "docs/kb/generated/manifest.json"
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    command = [str(core), "--root", str(root), "--profile", "shin-server", "--profile-dir",
               str(profile.parent), "--out-dir", str(index)]
    proof = subprocess.run(command + ["validate", "--require-current", "--strict", "--summary"],
                           check=True, capture_output=True, timeout=60)
    validation = json.loads(proof.stdout)
    if not validation["ok"]:
        raise ValueError("Baseline is not current and valid")
    paths = {}
    for row in manifest["sources"] + manifest["source_fact_sources"]:
        if row.get("content_hash"):
            paths[row["path"]] = row["content_hash"]
    sources, total = [], 0
    for relative, expected in sorted(paths.items()):
        path = root / relative
        if path.resolve() != path or not path.is_relative_to(root) or not path.is_file():
            raise ValueError("Unsafe frozen source")
        if path.stat().st_size > 2 * 1024 * 1024:
            raise ValueError("Source budget exceeded")
        content = path.read_bytes()
        total += len(content)
        if total > 32 * 1024 * 1024 or fnv(content) != expected:
            raise ValueError("Changed or oversized input corpus")
        sources.append({"path": relative, "sha256": digest(content), "text": content.decode("utf-8")})
    for topic in json.loads(policy.read_bytes())["topics"]:
        for span in topic["evidence"]:
            source = next(row for row in sources if row["path"] == span["path"])
            if not 1 <= span["start"] <= span["end"] <= len(source["text"].splitlines()):
                raise ValueError("Invalid review span")
    if manifest_path.read_bytes() != manifest_bytes:
        raise ValueError("Baseline publication changed")
    result = {"schema_version": "shin.code-context-freeze.v1", "root": str(root),
              "core": str(core), "core_sha256": digest(core.read_bytes()),
              "profile": json.loads(profile.read_bytes()), "profile_sha256": digest(profile.read_bytes()),
              "policy_sha256": digest(policy.read_bytes()), "manifest_sha256": digest(manifest_bytes),
              "manifest": manifest, "sources": sources, "source_bytes": total,
              "validation": validation, "scope": "captured-inputs-not-a-new-retrieval-score"}
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    target = args.output / "freeze.json"
    with target.open("x") as stream:
        target.chmod(0o600)
        json.dump(result, stream, ensure_ascii=True, indent=2)
        stream.write("\n")
    print(json.dumps({"sources": len(sources), "source_bytes": total,
                      "freeze_sha256": digest(target.read_bytes()), "policy_sha256": result["policy_sha256"]}))


if __name__ == "__main__":
    main()
