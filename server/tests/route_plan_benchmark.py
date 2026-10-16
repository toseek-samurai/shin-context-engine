"""Paired planning diagnostic; all policy changes and synthetic rows roll back."""
import argparse
import json
import math
import re
from pathlib import Path

from integration_test import IntegrationSuite
from preparation_rls_checks import SEED, SOURCE, context
from publication_test_support import require

QUERY = """SELECT a.revision,p.artifact_key,
 EXISTS(SELECT 1 FROM shin_server.preparation_retention r WHERE r.preparation_id=a.preparation_id) AS retained
 FROM shin_server.active_routes a LEFT JOIN shin_server.preparations p ON p.id=a.preparation_id
 WHERE a.project_id='rls-a' AND a.route_id='route'"""
ROUTE = """INSERT INTO shin_server.active_routes(project_id,route_id,revision,preparation_id,snapshot_id,route_fingerprint)
 SELECT project_id,route_id,1,id,'fnv1a64:0123456789abcdef',route_fingerprint FROM shin_server.preparations
 WHERE project_id='rls-a' AND idempotency_key='source';"""


def benchmark(s, pairs=12):
    old = (Path(__file__).resolve().parents[1] / "migrations/030_preparation_row_security.sql").read_text()
    restore = ""
    for name in ("preparations_owner", "preparations_source_read"):
        match = re.search(r"CREATE POLICY " + name + r"\b[^;]+;", old)
        require(match is not None, "baseline policy missing")
        restore += f"DROP POLICY {name} ON shin_server.preparations;" + match.group()
    samples = {"inline_directory": [], "statement_invoker": []}
    for pair in range(pairs):
        modes = list(samples) if pair % 2 == 0 else list(reversed(samples))
        for mode in modes:
            sql = "BEGIN;" + SEED + ROUTE + (restore if mode == "inline_directory" else "")
            sql += context() + SOURCE + "SET LOCAL shin.content_route='route';"
            sql += "DO $measure$ DECLARE p json; BEGIN EXECUTE $query$EXPLAIN(ANALYZE,TIMING OFF,FORMAT JSON) " + QUERY
            sql += "$query$ INTO p; PERFORM set_config('shin.plan_probe',p::text,true); END $measure$;"
            sql += "SELECT current_setting('shin.plan_probe')::jsonb::text;ROLLBACK"
            plan = json.loads(s.scalar(sql))[0]
            require(plan["Plan"]["Actual Rows"] == 1, "planning fixture lost its selected source")
            samples[mode].append({"planning_ms": plan["Planning Time"], "execution_ms": plan["Execution Time"]})
    result = {"scope": "synthetic-single-route-postgresql-planning-not-http", "alternating_pairs": pairs,
              "permissions_cached": False, "policy_changes_committed": False}
    for mode, values in samples.items():
        result[mode] = {}
        for field in ("planning_ms", "execution_ms"):
            timings = sorted(row[field] for row in values)
            result[mode][field] = {"p50": timings[math.ceil(len(timings) * .5) - 1],
                                  "p95": timings[math.ceil(len(timings) * .95) - 1]}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--state-root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    suite = IntegrationSuite(args.binary.resolve(), args.state_root.resolve(), args.report.resolve())
    suite.require_http_port_free()
    suite.wait_for_postgres()
    result = benchmark(suite)
    with args.report.open("x", encoding="utf-8") as handle:
        json.dump(result, handle, sort_keys=True)
        handle.write("\n")
    print(json.dumps(result, sort_keys=True))
