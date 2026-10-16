import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import shin_portfolio
import shin_dashboard
from test_portfolio import fixture


class DashboardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name).resolve()
        self.home.chmod(0o700)
        self.report_dir = self.home / "batch"
        self.report_dir.mkdir(mode=0o700)
        self.value = fixture()
        shin_portfolio.save_report(self.report_dir, self.value)
        self.config = {"schema_version": "shin.dashboard-config.v1", "projects": [
            {"id": "one", "name": "Project One", "routes": ["example"], "query_projects": ["one"]},
            {"id": "two", "name": "Project Two", "routes": ["other"], "query_projects": ["two"]}],
            "assessments": [str(self.report_dir)], "journals": []}
        self.file = self.home / "config.json"

    def reader(self):
        self.file.write_text(json.dumps(self.config))
        self.file.chmod(0o600)
        return shin_dashboard.Dashboard(self.file)

    def test_historical_checks_are_project_scoped_private_and_read_only(self):
        reader = self.reader()
        before = (self.report_dir / "report.json").read_bytes()
        data = reader.report()
        self.assertEqual(data["checks"][0]["project"], "one")
        self.assertEqual(data["events"], [])
        self.assertEqual(data["query_coverage"], [])
        self.assertNotIn(str(self.home), json.dumps(data))
        self.assertEqual(before, (self.report_dir / "report.json").read_bytes())

    def test_ambiguous_project_bindings_rejected(self):
        for field, value in (("routes", "example"), ("query_projects", "one")):
            saved = copy.deepcopy(self.config)
            self.config["projects"][1][field] = [value]
            with self.assertRaises(Exception): self.reader()
            self.config = saved

    def test_missing_source_is_unavailable_not_empty_success(self):
        self.config["assessments"].append(str(self.home / "future-run"))
        data = self.reader().report()
        self.assertEqual([s["state"] for s in data["sources"]], ["available", "unavailable"])

    def test_duplicate_runs_not_counted_twice(self):
        other = self.home / "duplicate"
        other.mkdir(mode=0o700)
        shin_portfolio.save_report(other, self.value)
        self.config["assessments"].append(str(other))
        data = self.reader().report()
        self.assertEqual(len(data["checks"]), 1)
        self.assertEqual(data["sources"][1]["state"], "unavailable")

    def test_events_are_actual_producer_times_not_reconstructed_from_polling(self):
        self.value["events"] = [{"project": "example", "stage": "candidate-build", "state": "completed",
            "started_at_unix_ms": 12, "ended_at_unix_ms": 15, "elapsed_ms": 3, "outcome": "passed"}]
        shin_portfolio.save_report(self.report_dir, self.value)
        data = self.reader().report()
        self.assertEqual(data["events"][0]["started_at_unix_ms"], 12)
        self.assertEqual(data["events"][0]["project"], "one")
        self.value["events"][0]["stage"] = "/private/path"
        shin_portfolio.save_report(self.report_dir, self.value)
        self.assertEqual(self.reader().report()["sources"][0]["state"], "unavailable")

    def test_unsafe_report_and_configuration_rejected(self):
        reader = self.reader()
        self.file.chmod(0o644)
        with self.assertRaises(Exception): shin_dashboard.Dashboard(self.file)
        (self.report_dir / "report.json").chmod(0o644)
        self.assertEqual(reader.report()["sources"][0]["state"], "unavailable")

    def test_inconsistent_or_duplicate_events_are_not_healthy_history(self):
        event = {"project": "example", "stage": "candidate-build", "state": "completed",
                 "started_at_unix_ms": 12, "ended_at_unix_ms": 15, "elapsed_ms": 3, "outcome": "passed"}
        for events in ([{**event, "outcome": None}], [event, event],
                       [{**event, "elapsed_ms": None}], [{**event, "state": "running"}]):
            self.value["events"] = events
            shin_portfolio.save_report(self.report_dir, self.value)
            self.assertEqual(self.reader().report()["sources"][0]["state"], "unavailable")

    def test_journal_identity_mapping_and_no_cross_project_guess(self):
        self.config["journals"] = [str(self.home / "journal")]
        journal = {"epoch": "a" * 32, "retained": 3, "evicted": 2, "recent": [
            {"project": project, "attempt_id": str(i), "observed_at_unix_ms": i, "elapsed_ns": 10,
             "query": "private", "source": "/private/path"} for i, project in enumerate(("one", "two", "unknown"))]}
        with patch.object(shin_dashboard, "journal_report", return_value=journal):
            data = self.reader().report()
        self.assertEqual([q["project"] for q in data["queries"]], ["one", "two"])
        self.assertEqual(data["query_coverage"], ["one", "two"])
        self.assertNotIn("private", json.dumps(data))
        self.assertEqual(data["sources"][-1]["evicted"], 2)


if __name__ == "__main__": unittest.main()
