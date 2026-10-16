#!/usr/bin/env python3
"""Query one explicitly configured local semantic development route."""

import argparse
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True

PACKAGE = Path(__file__).resolve().parents[1] / "integrations/semantic"
spec = importlib.util.spec_from_file_location("shin_semantic", PACKAGE / "__init__.py", submodule_search_locations=[str(PACKAGE)])
package = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = package
spec.loader.exec_module(package)
from shin_semantic.artifact import Rejected, encode
from shin_semantic.runtime import Session


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("query")
    parser.add_argument("--config", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = Session(args.config).query(args.query)
    except Rejected as error:
        result = {"schema_version": "shin.semantic-query.v1", "ok": False, "error": {"code": str(error)}}
    except Exception:
        result = {"schema_version": "shin.semantic-query.v1", "ok": False, "error": {"code": "semantic-query-rejected"}}
    print(encode(result).decode())
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
