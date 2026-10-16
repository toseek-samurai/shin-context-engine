"""Execute consumer-owned golden Brief cases without changing their rank contract."""

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys


CASE_KEYS = {"id", "query", "limit", "perDocLimit", "mustPaths", "mustTopPaths",
             "mustNotPaths", "mustTopN", "rationale", "rankBasis"}
RANK_BASES = {"first-evidence-rank", "unique-path-rank"}


def rank_basis(document):
    value = document.get("rankBasis", "first-evidence-rank")
    if not isinstance(value, str) or value not in RANK_BASES:
        raise ValueError("rankBasis must be first-evidence-rank or unique-path-rank")
    return value


def validate_cases(document):
    if not isinstance(document, dict):
        raise ValueError("golden definition must be an object")
    if "rankBasis" in document:
        raise ValueError("rankBasis belongs in each case, not at definition level")
    cases = document.get("goldenRetrievalCases")
    if not isinstance(cases, list) or not cases:
        raise ValueError("goldenRetrievalCases must be a nonempty array")
    seen = set()
    for case in cases:
        if not isinstance(case, dict) or set(case) - CASE_KEYS:
            raise ValueError("unsupported golden case fields")
        rank_basis(case)
        for key in ("id", "query"):
            if not isinstance(case.get(key), str) or not case[key].strip():
                raise ValueError(f"golden case requires {key}")
        if case["id"] in seen:
            raise ValueError("duplicate golden case id")
        seen.add(case["id"])
        for key in ("limit", "perDocLimit", "mustTopN"):
            if type(case.get(key)) is not int or case[key] <= 0:
                raise ValueError(f"golden case requires positive {key}")
        for key in ("mustPaths", "mustTopPaths", "mustNotPaths"):
            paths = case.get(key)
            if not isinstance(paths, list) or any(not isinstance(p, str) or not p for p in paths):
                raise ValueError(f"golden case requires path array {key}")
        if not case["mustPaths"] and not case["mustTopPaths"]:
            raise ValueError("golden case requires a positive source assertion")
    return cases


def check_observation(case, result, profile, basis=None):
    if basis is None:
        basis = rank_basis(case)
    if basis not in RANK_BASES:
        raise ValueError("unsupported rank basis")
    if result.get("ok") is not True or result.get("projection") != "compact":
        raise ValueError("Brief did not return successful compact evidence")
    if result.get("profile") != profile or result.get("query") != case["query"]:
        raise ValueError("Brief route/query differs from the requested case")
    freshness = result.get("index_freshness", {})
    if freshness.get("current") is not True or freshness.get("fully_verified") is not True:
        raise ValueError("Brief snapshot is not fully verified and current")
    if not result.get("snapshot_id") or result["snapshot_id"] != freshness.get("snapshot_id"):
        raise ValueError("Brief snapshot identities disagree")
    evidence = result.get("evidence")
    if not isinstance(evidence, list) or len(evidence) > case["limit"]:
        raise ValueError("Brief evidence is missing or exceeds the original record budget")
    ranks = {}
    counts = Counter()
    for rank, row in enumerate(evidence, 1):
        path = row.get("path") if isinstance(row, dict) else None
        if not isinstance(path, str) or not path:
            raise ValueError("Brief evidence has no source path")
        if path not in ranks:
            ranks[path] = rank if basis == "first-evidence-rank" else len(ranks) + 1
        counts[path] += 1
    if any(count > case["perDocLimit"] for count in counts.values()):
        raise ValueError("Brief exceeds the original per-document budget")
    missing = [path for path in case["mustPaths"] if path not in ranks]
    missing_top = [path for path in case["mustTopPaths"]
                   if path not in ranks or ranks[path] > case["mustTopN"]]
    forbidden = [path for path in case["mustNotPaths"] if path in ranks]
    return {"passed": not (missing or missing_top or forbidden),
            "missing": missing, "missing_top": missing_top, "forbidden": forbidden,
            "path_ranks": ranks, "rank_basis": basis,
            "snapshot_id": result["snapshot_id"]}


def run_replay(binary, profile_dir, routes, timeout):
    observations = []
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    for profile, root, out_dir, definition in routes:
        definition_path = Path(definition).resolve()
        source = definition_path.read_bytes()
        definition_document = json.loads(source)
        cases = validate_cases(definition_document)
        definition_hash = hashlib.sha256(source).hexdigest()
        snapshot_id = None
        for case in cases:
            argv = [str(binary), "--profile", profile, "--profile-dir", str(profile_dir),
                    "--root", str(Path(root).resolve()), "--out-dir", str(Path(out_dir).resolve()),
                    "brief", case["query"], "--limit", str(case["limit"]),
                    "--per-doc", str(case["perDocLimit"]), "--projection", "compact", "--require-current"]
            record = {"profile": profile, "id": case["id"], "query": case["query"], "argv": argv,
                      "definition": str(definition_path), "definition_sha256": definition_hash,
                      "case": case}
            try:
                process = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
                if process.returncode != 0:
                    raise ValueError(f"Brief exited with status {process.returncode}")
                result = json.loads(process.stdout)
                record.update(check_observation(case, result, profile))
                if snapshot_id is not None and result["snapshot_id"] != snapshot_id:
                    raise ValueError("snapshot changed during the route replay")
                snapshot_id = result["snapshot_id"]
                record["response_sha256"] = hashlib.sha256(process.stdout.encode()).hexdigest()
                record["verification_paths"] = result.get("verification_paths", [])
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                record.update(passed=False, error=str(error))
            observations.append(record)
        if definition_path.read_bytes() != source:
            raise ValueError("golden definition changed during replay")
    if hashlib.sha256(binary.read_bytes()).hexdigest() != binary_hash:
        raise ValueError("binary changed during replay")
    return {"schema_version": "shin.consumer-golden-replay.v1",
            "captured_at": datetime.now(timezone.utc).isoformat(),
            "binary": str(binary), "binary_sha256": binary_hash,
            "passed": all(row["passed"] for row in observations),
            "passed_cases": sum(row["passed"] for row in observations),
            "total_cases": len(observations), "observations": observations}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--profile-dir", type=Path, required=True)
    parser.add_argument("--route", nargs=4, action="append", required=True,
                        metavar=("PROFILE", "ROOT", "OUT_DIR", "DEFINITION"))
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    try:
        report = run_replay(args.binary.resolve(), args.profile_dir.resolve(), args.route, args.timeout)
    except (OSError, ValueError) as error:
        print(f"consumer golden replay failed: {error}", file=sys.stderr)
        return 1
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"consumer golden replay: {report['passed_cases']}/{report['total_cases']}; report: {args.report}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
