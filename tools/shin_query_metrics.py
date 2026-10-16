#!/usr/bin/env python3
"""Initialize or inspect the opt-in bounded local query journal."""

import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

from shin import load_semantic

load_semantic()
from shin_semantic.artifact import encode
from shin_semantic.telemetry import initialize, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("init", "report"))
    parser.add_argument("--dir", required=True, type=Path)
    args = parser.parse_args()
    try:
        path = args.dir.expanduser().absolute()
        if args.command == "init":
            initialize(path)
            result = {"schema_version": "shin.local-query-metrics-init.v1", "ok": True}
        else:
            result = report(path)
    except Exception:
        print(encode({"ok": False, "error": {"code": "query-metrics-rejected"}}).decode())
        return 1
    print(encode(result).decode())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
