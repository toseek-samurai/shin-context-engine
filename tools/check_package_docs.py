#!/usr/bin/env python3
"""Check relative inline Markdown links in the two shipped operator guides."""

import argparse
import json
import posixpath
from pathlib import PurePosixPath
from urllib.parse import unquote, urlsplit
from zipfile import ZipFile

from check_agent_guidance import LINK, heading_anchors, prose_lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive")
    args = parser.parse_args()
    failures = []
    checked = 0
    with ZipFile(args.archive) as archive:
        members = {item.filename for item in archive.infolist() if not item.is_dir()}
        roots = {str(PurePosixPath(name).parent) for name in members
                 if name.endswith("/share/shin-observer/local-kit.md")}
        if len(roots) != 1:
            parser.error("archive must contain exactly one local-kit guide")
        root = roots.pop()
        for page in ("local-kit.md", "native-client-setup.md"):
            name = f"{root}/{page}"
            if name not in members:
                failures.append({"page": name, "reason": "missing-guide"})
                continue
            text = archive.read(name).decode("utf-8")
            for target in LINK.findall("\n".join(prose_lines(text))):
                link = urlsplit(target.strip("<>"))
                if link.scheme in ("https", "http", "mailto"):
                    continue
                checked += 1
                resolved = posixpath.normpath(posixpath.join(root, unquote(link.path))) if link.path else name
                if link.scheme or link.netloc or link.path.startswith("/") or resolved not in members:
                    failures.append({"page": name, "target": target, "reason": "unshipped-target"})
                elif link.fragment and resolved.endswith(".md"):
                    if unquote(link.fragment) not in heading_anchors(archive.read(resolved).decode("utf-8")):
                        failures.append({"page": name, "target": target, "reason": "missing-anchor"})
    print(json.dumps({"ok": not failures, "relative_inline_links": checked, "failures": failures}, indent=2))
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
