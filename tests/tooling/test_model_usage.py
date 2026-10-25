import copy
from datetime import date
import fcntl
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import shin_model_usage as usage


def raw(client="codex", **changes):
    row = {"provider": client, "project": "/workspace/one", "sessionId": "mixed-chat",
           "timestamp": "2026-10-03T12:00:00Z", "model": "test-model", "inputTokens": 100,
           "outputTokens": 20, "cacheReadTokens": 70, "cacheWriteTokens": 0, "reasoningTokens": 10,
           "query": "private prompt", "cost": 55, "shellCommands": "SECRET"}
    row.update(changes)
    return {"schema": "codeburn.export.v2", "records": [row], "shellCommands": "PRIVATE"}


class UsageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name).resolve()
        self.home.chmod(0o700)
        self.cfg = {"schema_version": "shin.model-usage-config.v1", "codeburn": "/bin/codeburn",
                    "projects": [{"id": "one", "name": "One", "paths": ["/workspace/one"]},
                                 {"id": "two", "name": "Two", "paths": ["/workspace/two"]}], "session_bindings": []}

    def collect(self, loader=None, today=date(2026, 10, 3)):
        return usage.collect(self.cfg, self.home, today=today,
                             loader=loader or (lambda c, client, *args: raw(client)))

    def test_metadata_only_exact_project_binding_and_separate_counters(self):
        value = self.collect()
        self.assertEqual(len(value["buckets"]), 3)
        text = json.dumps(value)
        for secret in ("/workspace", "mixed-chat", "PRIVATE", "private prompt", "cost", "shellCommands"):
            self.assertNotIn(secret, text)
        row = value["buckets"][0]
        self.assertEqual((row["input_tokens"], row["output_tokens"], row["reasoning_tokens"]), (100, 20, 10))
        self.assertEqual((self.home / "usage.json").stat().st_mode & 0o777, 0o600)

    def test_repeated_collection_replaces_overlap_without_double_count(self):
        a, b = self.collect(), self.collect()
        self.assertEqual(a["buckets"], b["buckets"])
        updated = self.collect(lambda c, client, *args: raw(client, inputTokens=125))
        self.assertTrue(all(r["input_tokens"] == 125 for r in updated["buckets"]))

    def test_identical_legitimate_rows_are_not_deduplicated_by_token_value(self):
        data = raw()
        data["records"] *= 2
        rows, _ = usage.normalize(data, self.cfg, "codex", "2026-10-01", "2026-10-03")
        self.assertEqual(rows[0]["records"], 2)
        self.assertEqual(rows[0]["input_tokens"], 200)

    def test_failure_keeps_last_good_history_and_other_clients_progress(self):
        before = self.collect()
        def loader(c, client, *args):
            if client == "hermes": raise OSError("PRIVATE failed")
            return raw(client, inputTokens=150)
        after = self.collect(loader)
        self.assertEqual(after["sources"]["hermes"]["state"], "unavailable")
        self.assertEqual(next(r for r in after["buckets"] if r["client"] == "hermes")["input_tokens"], 100)
        self.assertEqual(after["sources"]["hermes"]["last_success_unix_ms"], before["sources"]["hermes"]["last_success_unix_ms"])
        self.assertNotIn("PRIVATE", json.dumps(after))

    def test_empty_after_nonempty_does_not_erase_history(self):
        before = self.collect()
        after = self.collect(lambda *args: {"schema": "codeburn.export.v2", "records": []})
        self.assertEqual(before["buckets"], after["buckets"])
        self.assertTrue(all(s["state"] == "unavailable" for s in after["sources"].values()))

    def test_empty_first_import_is_not_an_invented_bucket(self):
        value = self.collect(lambda *args: {"schema": "codeburn.export.v2", "records": []})
        self.assertEqual(value["buckets"], [])

    def test_mixed_session_stays_unassigned_without_explicit_interval(self):
        data = raw(project="/archive/SLJS")
        rows, counts = usage.normalize(data, self.cfg, "codex", "2026-10-01", "2026-10-03")
        self.assertEqual(rows, [])
        self.assertEqual(counts["unassigned_records"], 1)
        self.cfg["session_bindings"] = [{"client": "codex", "session_id": "mixed-chat", "project": "two",
                                        "from": "2026-10-03T12:00:00Z", "to": "2026-10-03T13:00:00Z"}]
        rows, _ = usage.normalize(data, self.cfg, "codex", "2026-10-01", "2026-10-03")
        self.assertEqual(rows[0]["project"], "two")
        self.assertEqual(rows[0]["manual_binding_records"], 1)
        data["records"][0]["timestamp"] = "2026-10-03T13:00:00Z"
        self.assertEqual(usage.normalize(data, self.cfg, "codex", "2026-10-01", "2026-10-03")[0], [])

    def test_config_rejects_ambiguous_paths_and_overlapping_intervals(self):
        path = self.home / "config.json"
        def check():
            path.write_text(json.dumps(self.cfg)); path.chmod(0o600)
            return usage.config(path)
        check()
        self.cfg["projects"][1]["paths"] = ["/workspace/one"]
        with self.assertRaises(Exception): check()
        self.cfg["projects"][1]["paths"] = []
        self.cfg["session_bindings"] = [{"client": "codex", "session_id": "s", "project": "one",
                                        "from": "2026-10-03T00:00:00Z", "to": "2026-10-04T00:00:00Z"}] * 2
        with self.assertRaises(Exception): check()

    def test_rebinding_existing_history_requires_new_home(self):
        self.collect()
        before = (self.home / "usage.json").read_bytes()
        self.cfg["projects"][0]["paths"] = ["/changed"]
        with self.assertRaises(Exception): self.collect()
        self.assertEqual(before, (self.home / "usage.json").read_bytes())

    def test_private_files_corruption_symlinks_and_writer_lock(self):
        self.collect()
        path = self.home / "usage.json"
        path.chmod(0o644)
        with self.assertRaises(Exception): usage.report(self.home)
        path.chmod(0o600)
        with (self.home / "writer.lock").open("rb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            with self.assertRaises(BlockingIOError): self.collect()
        original = path.read_bytes()
        path.unlink(); path.symlink_to(self.home / "elsewhere")
        with self.assertRaises(Exception): usage.report(self.home)
        path.unlink(); path.write_bytes(original); path.chmod(0o600)
        value = usage.report(self.home); value["buckets"].append(copy.deepcopy(value["buckets"][0]))
        with self.assertRaises(Exception): usage.validate(value)

    def test_invalid_numeric_or_foreign_schema_never_replaces_history(self):
        self.collect()
        before = usage.report(self.home)["buckets"]
        for bad in (True, -1, float("nan"), 2**54):
            value = self.collect(lambda c, client, *args: raw(client, inputTokens=bad))
            self.assertEqual(before, value["buckets"])
        value = self.collect(lambda *args: {"schema": "foreign", "records": []})
        self.assertEqual(before, value["buckets"])

    def test_retention_and_dates_use_utc_day_not_host_timezone(self):
        value = self.collect(lambda c, client, *args: raw(client, timestamp="2026-10-04T00:30:00+03:00"))
        self.assertTrue(all(r["date"] == "2026-10-03" for r in value["buckets"]))
        later = self.collect(lambda *args: {"schema": "codeburn.export.v2", "records": []}, date(2027, 1, 3))
        self.assertEqual(later["buckets"], [])


if __name__ == "__main__": unittest.main()
