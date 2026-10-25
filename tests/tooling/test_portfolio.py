#!/usr/bin/env python3
"""Assessment semantics, no consumer writes, and private report admission."""

import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import shin_portfolio as portfolio
from test_eval_review import full, query


def fixture():
    return {"schema_version": "shin.portfolio.v1", "run_id": "a" * 32, "status": "completed",
            "started_at_unix_ms": 10, "updated_at_unix_ms": 20, "engines": {
                name: {"version": "1.27.4", "generator_version": "generator", "sha256": "a" * 64}
                for name in ("baseline", "candidate")}, "projects": [{
                    "id": "example", "profile": "example", "status": "compared", "stage": "finished", "reason": "",
                    "captured_at_unix_ms": 20, "comparable": True, "live": {}, "lanes": {}, "cases": [], "gates": []}],
            "excluded": []}


class PortfolioTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name).resolve()
        self.home.chmod(0o700)

    def test_comparison_does_not_call_resource_only_failure_quality_regression(self):
        key = ("eval", "case", -1)
        rows = portfolio.compare({key: (True, "passed")}, {key: (True, "resource-only-failure")})
        self.assertEqual(rows[0]["kind"], "passed")
        self.assertEqual(rows[0]["candidate"], "resource-only-failure")
        self.assertEqual(portfolio.compare({key: (True, "passed")}, {key: (False, "quality-failure")})[0]["kind"], "regression")
        self.assertEqual(portfolio.compare({key: (False, "failed")}, {key: (False, "failed")})[0]["kind"], "existing-failure")
        self.assertEqual(portfolio.compare({key: (False, "failed")}, {key: (True, "passed")})[0]["kind"], "improvement")
        self.assertEqual(portfolio.compare({key: (True, "passed")}, {})[0]["kind"], "unknown")

    def test_inventory_uses_content_not_generator_or_timestamps(self):
        source = {"path": "src/a.cpp", "content_hash": "fnv1a64:" + "a" * 16}
        a = {"sources": [source], "source_fact_sources": [dict(source)], "generator_version": "a"}
        b = copy.deepcopy(a)
        b["generator_version"] = "b"
        self.assertEqual(portfolio.inventory(a), portfolio.inventory(b))
        b["source_fact_sources"][0]["content_hash"] = "fnv1a64:" + "b" * 16
        with self.assertRaises(Exception):
            portfolio.inventory(b)
        b["source_fact_sources"][0].pop("content_hash")
        with self.assertRaises(Exception):
            portfolio.inventory(b)

    def test_excluded_large_body_is_not_claimed_as_hashed_evidence(self):
        source = {"path": "test.tsx", "content_hash": "", "status": "skipped-large", "bytes": 546755}
        value = portfolio.inventory({"sources": [], "source_fact_sources": [source]})
        self.assertEqual(value, {"test.tsx": {"excluded": "skipped-large", "bytes": 546755}})
        source["status"] = "scanned"
        with self.assertRaises(Exception):
            portfolio.inventory({"sources": [], "source_fact_sources": [source]})

    def test_native_validation_reuses_quality_and_resource_classifier(self):
        evaluation = full([query(candidates=900, passed=False)], ok=False)
        pack = {"schema_version": "shin.pack-eval.v1", "summary": {"passed": 1, "failed": 0, "total": 1},
                "cases": [{"id": "pack", "passed": True}]}
        validation = {"schema_version": "shin.validation.v1", "gates": [
            {"id": name, "attempted": True, "ok": name != "eval",
             "result": evaluation if name == "eval" else pack if name == "pack-eval" else {}}
            for name in ("profile-check", "check", "doctor", "vacuum", "eval", "pack-eval")]}
        summary, cases, gates = portfolio.summarize_validation(validation)
        self.assertEqual(summary["quality_failed"], 0)
        self.assertEqual(summary["resource_exceeded"], 1)
        self.assertEqual(summary["checks"]["failed"], 1)
        self.assertEqual(cases[("eval", "fixture", -1)], (True, "resource-only-failure"))
        self.assertFalse(gates["eval"])

    def test_reader_strips_unknown_private_fields_and_never_writes(self):
        value = fixture()
        value["private"] = "/private/root"
        value["projects"][0]["raw_query"] = "secret-query"
        portfolio.save_report(self.home, value)
        before = (self.home / "report.json").read_bytes()
        projected = portfolio.report(self.home)
        self.assertIsNone(projected["agent_usefulness"])
        self.assertIsNone(projected["semantic_quality"])
        self.assertNotIn(b"private", portfolio.encode(projected))
        self.assertNotIn(b"secret-query", portfolio.encode(projected))
        self.assertEqual(before, (self.home / "report.json").read_bytes())

    def test_rejects_unsafe_mode_symlink_hardlink_and_malformed(self):
        portfolio.save_report(self.home, fixture())
        file = self.home / "report.json"
        file.chmod(0o644)
        with self.assertRaises(Exception):
            portfolio.report(self.home)
        file.chmod(0o600)
        os.link(file, self.home / "other")
        with self.assertRaises(Exception):
            portfolio.report(self.home)
        file.unlink()
        file.symlink_to(self.home / "other")
        with self.assertRaises(Exception):
            portfolio.report(self.home)
        file.unlink()
        file.write_text('{"schema_version":"private-corruption"}')
        file.chmod(0o600)
        with self.assertRaises(Exception):
            portfolio.report(self.home)

    def test_new_output_must_not_overlap_any_consumer_or_canonical_index(self):
        root = self.home / "consumer"
        root.mkdir()
        (root / "example.json").write_text("{}")
        binary = str(Path(sys.executable).resolve())
        config = {"schema_version": "shin.portfolio-config.v1", "baseline": binary,
                  "candidate": binary, "projects": [{"id": "example", "profile": "example",
                      "root": str(root), "registry": str(root), "canonical_index": str(root),
                      "marker": None, "live_engine": "baseline"}]}
        with self.assertRaisesRegex(Exception, "portfolio-route-overlap"):
            portfolio.prepare(config, root / "new")
        with self.assertRaisesRegex(Exception, "portfolio-route-overlap"):
            portfolio.prepare(config, self.home)
        self.assertEqual(len(list(root.iterdir())), 1)

    def test_bad_counts_and_unbounded_projects_rejected(self):
        with self.assertRaises(Exception):
            portfolio._count({"passed": 2, "failed": 1, "total": 2})
        value = fixture()
        value["projects"] *= 25
        with self.assertRaises(Exception):
            portfolio.panel_projection(value)

    def test_timed_out_child_is_reaped(self):
        with self.assertRaisesRegex(Exception, "command-timeout"):
            portfolio.execute([sys.executable, "-c", "import time; time.sleep(30)"], self.home / "slow", .1, lambda: None)

    def test_output_budget_is_enforced_even_for_fast_exiting_child(self):
        with patch.object(portfolio, "MAX_RAW", 32), self.assertRaisesRegex(Exception, "command-output-budget"):
            portfolio.execute([sys.executable, "-c", "print('x' * 10000)"], self.home / "oversized", 5, lambda: None)

    def test_disk_reserve_stops_the_child_instead_of_filling_disk(self):
        with patch.object(portfolio.shutil, "disk_usage", return_value=SimpleNamespace(free=0)), \
                self.assertRaisesRegex(Exception, "free-disk-reserve"):
            portfolio.execute([sys.executable, "-c", "import time; time.sleep(30)"], self.home / "disk", 5, lambda: None)

    def test_runner_detects_drift_and_inventory_change_without_consumer_writes(self):
        root = self.home / "consumer"
        root.mkdir()
        (root / "example.json").write_text("{}")
        binary = str(Path(sys.executable).resolve())
        config = {"schema_version": "shin.portfolio-config.v1", "baseline": binary, "candidate": binary,
                  "projects": [{"id": "example", "profile": "example", "root": str(root), "registry": str(root),
                                "canonical_index": str(root), "marker": None, "live_engine": "baseline"}]}
        before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in root.iterdir()}
        for scenario, expected in (("same", "compared"), ("drift", "source-changed"), ("inventory", "blocked")):
            def execute(command, prefix, timeout, pulse):
                if command[-1] == "--version":
                    return {"version": "1.27.4", "generator_version": "test"}, 0, 1
                if "ready" in command:
                    return {"command": "ready", "ready": True}, 0, 1
                if "build" in command:
                    out = Path(command[command.index("--out-dir") + 1]) / "docs/kb/generated"
                    out.mkdir(parents=True)
                    content = "b" if scenario == "inventory" and prefix.name.startswith("candidate") else "a"
                    (out / "manifest.json").write_text(json.dumps({"sources": [
                        {"path": "example.json", "content_hash": "fnv1a64:" + content * 16}], "source_fact_sources": []}))
                    return {"ok": True}, 0, 1
                if "worktree-diff" in command:
                    return {"current": scenario != "drift", "comparison": {"snapshot_integrity": "verified"}}, 0, 1
                evaluation = full()
                pack = {"schema_version": "shin.pack-eval.v1", "summary": {"passed": 0, "failed": 0, "total": 0}, "cases": []}
                return {"schema_version": "shin.validation.v1", "profile": "example", "gates": [
                    {"id": name, "attempted": True, "ok": True,
                     "result": evaluation if name == "eval" else pack if name == "pack-eval" else {}}
                    for name in ("profile-check", "check", "doctor", "vacuum", "eval", "pack-eval")]}, 0, 1
            with patch.object(portfolio, "execute", side_effect=execute), patch("builtins.print"):
                value = portfolio.run(config, self.home / scenario)
            project = value["projects"][0]
            self.assertEqual(project["status"], expected)
            self.assertEqual(project["comparable"], scenario == "same")
            if scenario != "same":
                self.assertEqual(project["cases"], [])
            self.assertEqual(before, {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in root.iterdir()})


if __name__ == "__main__":
    unittest.main()
