#!/usr/bin/env python3
"""Check maintained guidance layout, not model obedience or semantic equivalence."""

import argparse
import hashlib
import json
from pathlib import Path
import re


BUNDLES = {
    "codex": ("integrations/codex/skills/shin-context", 6000),
    "hermes": ("integrations/hermes/plugins/shin-context/skills/workflow", 5000),
}
LINK = re.compile(r"\[[^\]]+\]\(([^)]+)\)")


def prose_lines(text):
    """Exclude frontmatter and fenced examples from prose/link checks."""
    frontmatter = text.startswith("---\n")
    fence = None
    for number, line in enumerate(text.splitlines()):
        if frontmatter:
            if number and line == "---":
                frontmatter = False
            continue
        marker = re.match(r"^\s*(`{3,}|~{3,})", line)
        if marker:
            run = marker.group(1)
            if fence is None:
                fence = run
            elif run[0] == fence[0] and len(run) >= len(fence):
                fence = None
            yield ""
        elif fence is None:
            yield line


def heading_anchors(text):
    counts = {}
    anchors = set()
    for line in prose_lines(text):
        heading = re.match(r"^#{1,6}\s+(.+?)\s*#*\s*$", line)
        if not heading:
            continue
        base = re.sub(r"[^\w -]", "", heading.group(1).lower()).replace(" ", "-")
        count = counts.get(base, 0)
        anchors.add(base if count == 0 else f"{base}-{count}")
        counts[base] = count + 1
    return anchors


def check_links(file, boundary):
    errors, targets = [], []
    source = "\n".join(prose_lines(file.read_text(encoding="utf-8")))
    for raw in LINK.findall(source):
        if raw.startswith(("https://", "http://")):
            continue
        raw = raw.removeprefix("<").removesuffix(">")
        relative, _, anchor = raw.partition("#")
        target = file.parent / relative if relative else file
        if ":" in relative or Path(relative).is_absolute() or not target.resolve().is_relative_to(boundary):
            errors.append({"id": "reference-outside-owner", "path": str(file), "target": raw})
        elif target.is_symlink() or not target.is_file():
            errors.append({"id": "missing-or-symlink-reference", "path": str(file), "target": raw})
        elif anchor and target.suffix == ".md" and anchor not in heading_anchors(target.read_text(encoding="utf-8")):
            errors.append({"id": "missing-reference-anchor", "path": str(file), "target": raw})
        else:
            targets.append(target.resolve())
    return errors, targets


def duplicate_paragraphs(files):
    seen, errors = {}, []
    for file in sorted(files):
        prose = "\n".join(prose_lines(file.read_text(encoding="utf-8")))
        for block in re.split(r"\n\s*\n", prose):
            if block.startswith(("#", "|")):
                continue
            paragraph = " ".join(block.split())
            if len(paragraph) < 220:
                continue
            digest = hashlib.sha256(paragraph.encode()).hexdigest()
            if digest in seen:
                errors.append({"id": "duplicate-prose", "path": str(file), "owner": str(seen[digest]), "sha256": digest})
            else:
                seen[digest] = file
    return errors


def check_bundle(root, maximum_entry_bytes):
    root = root.resolve()
    entry = root / "SKILL.md"
    errors, visited = [], set()
    if not entry.is_file() or entry.is_symlink():
        return {"entry_bytes": 0, "pages": [], "errors": [{"id": "missing-entry", "path": str(entry)}]}
    size = entry.stat().st_size
    if size > maximum_entry_bytes:
        errors.append({"id": "entry-byte-budget", "path": str(entry), "actual": size, "maximum": maximum_entry_bytes})
    pending = [entry]
    while pending:
        file = pending.pop()
        if file in visited:
            continue
        visited.add(file)
        text = file.read_text(encoding="utf-8")
        if re.search(r"/(?:Users|home)/[^/\s]+/", text):
            errors.append({"id": "machine-specific-guidance", "path": str(file)})
        issues, targets = check_links(file, root)
        errors.extend(issues)
        pending.extend(p for p in targets if p.suffix == ".md")
    for reference in (root / "references").rglob("*.md"):
        if reference.resolve() not in visited:
            errors.append({"id": "unreachable-reference", "path": str(reference)})
    errors.extend(duplicate_paragraphs(visited))
    return {"entry_bytes": size, "maximum_entry_bytes": maximum_entry_bytes,
            "pages": [str(p.relative_to(root)) for p in sorted(visited)], "errors": errors}


def check_repository(root):
    root = root.resolve()
    reports, errors = {}, []
    profile = json.loads((root / "profiles/shin.json").read_text())
    for name, (relative, budget) in BUNDLES.items():
        result = check_bundle(root / relative, budget)
        reports[name] = result
        errors.extend(result["errors"])
        for page in result["pages"]:
            expected = f"{relative}/{page}"
            if expected not in profile["rootFiles"]:
                errors.append({"id": "guidance-not-enrolled", "path": expected})
    agent = root / "AGENTS.md"
    agent_bytes = agent.stat().st_size
    if agent_bytes > 4000:
        errors.append({"id": "entry-byte-budget", "path": "AGENTS.md", "actual": agent_bytes, "maximum": 4000})
    for file in (agent, root / "docs/agent-maintenance.md"):
        issues, _ = check_links(file, root)
        errors.extend(issues)
    errors.extend(duplicate_paragraphs([agent, root / "docs/agent-maintenance.md"]))
    policy = json.loads((root / "docs/machine/project-adoption-v1.json").read_text())["informationArchitecture"]
    if set(policy["ownership"]) != {"entry", "rules", "contracts", "procedures", "projections"}:
        errors.append({"id": "incomplete-guidance-ownership"})
    if not policy["scope"] or not policy["workflow"] or not policy["formatBoundary"] or not policy["migrationBoundary"]:
        errors.append({"id": "incomplete-guidance-boundaries"})
    return {"ok": not errors, "scope": "owned-guidance-structure-not-semantic-or-model-validation",
            "agent_entry_bytes": agent_bytes, "bundles": reports, "errors": errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    report = check_repository(args.root)
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
