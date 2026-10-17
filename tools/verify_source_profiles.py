#!/usr/bin/env python3
"""Qualify self profiles in a new external directory, without enrolling projects."""

import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if output == root or root in output.parents or output in root.parents:
        parser.error("output must be disjoint from source")
    if args.output.is_symlink() or output.exists():
        parser.error("choose a new output directory; existing indexes are not replaced")
    core = args.core.resolve(strict=True)
    output.mkdir(parents=True, mode=0o700)
    failed = []
    for profile in ("shin", "shin-observer"):
        common = [str(core), "--root", str(root), "--profile-dir", str(root / "profiles"),
                  "--profile", profile, "--out-dir", str(output / profile)]
        for command in (["build"], ["validate", "--strict"],
                        ["brief", "source facts", "--require-current"]):
            result = subprocess.run(common + command, capture_output=True, text=True, timeout=180)
            (output / f"{profile}-{command[0]}.json").write_text(result.stdout, encoding="utf-8")
            (output / f"{profile}-{command[0]}.stderr").write_text(result.stderr, encoding="utf-8")
            value = json.loads(result.stdout)
            if result.returncode or value.get("ok") is not True:
                failed.append({"profile": profile, "command": command[0], "exit": result.returncode})
                break
    print(json.dumps({"ok": not failed, "failed": failed}))
    return bool(failed)


if __name__ == "__main__":
    raise SystemExit(main())
