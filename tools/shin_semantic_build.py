#!/usr/bin/env python3
"""Build and atomically activate one explicitly configured semantic generation."""

import argparse
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("semantic_entry", Path(__file__).with_name("shin_semantic.py"))
entry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(entry)
from shin_semantic.artifact import Rejected, encode
from shin_semantic.producer import Producer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--reuse", type=Path, help="explicit donor route, first build only")
    args = parser.parse_args()
    try:
        result = Producer(args.config).build(args.reuse)
    except Rejected as error:
        result = {"schema_version": "shin.semantic-build-result.v1", "ok": False, "error": {"code": str(error)}}
    except Exception:
        result = {"schema_version": "shin.semantic-build-result.v1", "ok": False, "error": {"code": "semantic-build-rejected"}}
    print(encode(result).decode())
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
