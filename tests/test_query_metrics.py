#!/usr/bin/env python3
"""Metadata privacy, retention, failure isolation and read-only reporting."""

from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
import fcntl
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("semantic_entry", ROOT / "tools/shin_semantic.py")
entry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(entry)
from shin_semantic import telemetry as t
from shin_semantic.artifact import Rejected, decode, encode


class MetricsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="shin-query-metrics-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        self.home = self.base / "metrics"
        t.initialize(self.home)
        self.config = {"root": str(self.base / "source"), "root_identity": "1:2", "project": "fixture",
            "profile": "fixture", "profile_sha256": "a" * 64, "core_sha256": "b" * 64,
            "core_binary": str(self.base / "core"), "profile_path": str(self.base / "profiles/fixture.json"),
            "out_dir": str(self.base / "index"), "model_dir": str(self.base / "model"),
            "artifact_dir": str(self.base / "artifact")}
        self.config_path = self.base / "route.json"

    def row(self, duration=10, **changes):
        value = {"attempt_id": uuid.uuid4().hex, "observed_at_unix_ms": 1, "elapsed_ns": duration,
            "phase_ns": {"core-brief": 1}, "failed_phase": None, "route_key": "c" * 64,
            "project": "fixture", "profile": "fixture", "core_sha256": "b" * 64, "profile_sha256": "a" * 64,
            "artifact_sha256": None, "snapshot_id": "fnv1a64:0123456789abcdef", "ok": True,
            "reason": None, "semantic_status": "not-needed-exact-or-owner", "projection": "compact",
            "model_state": "not-used-or-incomplete",
            "response_bytes": 100, "native_records": 2, "navigation_candidates": None}
        value.update(changes)
        return value

    def append(self, row):
        t.append(self.home, row, self.config, self.config_path)

    def test_init_private_and_never_resets_existing_home(self):
        self.assertEqual(self.home.stat().st_mode & 0o777, 0o700)
        for file in self.home.iterdir():
            self.assertEqual(file.stat().st_mode & 0o777, 0o600)
        before = (self.home / "journal.json").read_bytes()
        with self.assertRaises(FileExistsError):
            t.initialize(self.home)
        self.assertEqual((self.home / "journal.json").read_bytes(), before)

    def test_trace_records_disjoint_durations_and_failed_phase(self):
        with patch.object(t.time, "perf_counter_ns", side_effect=[0, 1, 4, 5, 7, 10]):
            trace = t.Trace()
            with trace.phase("core-brief"):
                pass
            with self.assertRaises(ValueError), trace.phase("model-load"):
                raise ValueError("private exception")
            row = trace.record(self.config, None, {"ok": False, "error": {"code": "private exception"}}, 80, "audit")
        self.assertEqual(row["phase_ns"], {"core-brief": 3, "model-load": 2})
        self.assertEqual(row["elapsed_ns"], 10)
        self.assertEqual(row["failed_phase"], "model-load")
        self.assertEqual(row["reason"], "query-rejected")
        t.validate_record(row)

    def test_metadata_does_not_copy_query_paths_source_or_credentials(self):
        sentinel = "never-persist-this-private-text"
        raw = {"query": sentinel, "core": {"query": sentinel, "snapshot_id": "fnv1a64:0123456789abcdef",
            "evidence": [{"path": sentinel, "excerpt": sentinel}]}, "navigation": {"candidates": [{"path": sentinel}]},
            "semantic_status": "available", "artifact_sha256": "d" * 64}
        before = deepcopy(raw)
        row = t.Trace().record(self.config, raw, {"ok": True, "token": sentinel}, 120, "compact")
        self.append(row)
        data = (self.home / "journal.json").read_bytes()
        self.assertNotIn(sentinel.encode(), data)
        self.assertNotIn(str(self.base).encode(), data)
        self.assertEqual(raw, before)
        self.assertEqual(row["native_records"], 1)

    def test_report_percentiles_and_missing_population_are_explicit(self):
        for duration in range(1, 21):
            self.append(self.row(duration))
        before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.home.iterdir()}
        report = t.report(self.home)
        self.assertEqual(report["retained"], 20)
        self.assertEqual(report["cohorts"][0]["p50_ns"], 10)
        self.assertEqual(report["cohorts"][0]["p95_ns"], 19)
        self.assertIsNone(report["unrecorded_attempts"])
        self.assertIsNone(report["agent_usefulness"])
        self.assertEqual(before, {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.home.iterdir()})

    def test_expanded_read_limit_is_bounded_and_default_stays_twenty(self):
        for _ in range(25):
            self.append(self.row())
        self.assertEqual(len(t.report(self.home)["recent"]), 20)
        self.assertEqual(len(t.report(self.home, recent_limit=256)["recent"]), 25)
        for invalid in (0, 257, -1, True, 20.5):
            with self.assertRaises(Rejected):
                t.report(self.home, recent_limit=invalid)

    def test_failure_fallback_generation_and_projection_are_separate_cohorts(self):
        for changes in ({}, {"ok": False, "reason": "core-admission-rejected", "snapshot_id": None},
                        {"semantic_status": "runtime-unavailable-lexical-only"},
                        {"snapshot_id": "fnv1a64:0000000000000000"}, {"projection": "audit"}):
            self.append(self.row(**changes))
        self.assertEqual(len(t.report(self.home)["cohorts"]), 5)

    def test_unknown_observed_client_files_never_merge_with_pinned_files(self):
        self.append(self.row())
        self.append(self.row(client_files_sha256="d" * 64))
        self.assertEqual(len(t.report(self.home)["cohorts"]), 2)

    def test_unavailable_code_fingerprint_does_not_block_the_trace(self):
        with patch.object(t, "read", side_effect=OSError("private")):
            trace = t.Trace()
        row = trace.record(self.config, None, {"ok": True}, 5, "compact")
        self.assertIsNone(row["client_files_sha256"])
        self.append(row)
        self.assertIsNone(t.report(self.home)["cohorts"][0]["client_files_sha256"])

    def test_retention_counts_evicted_without_claiming_all_traffic(self):
        with patch.object(t, "MAX_RECORDS", 3):
            for duration in range(1, 6):
                self.append(self.row(duration))
            result = t.report(self.home, recent_limit=3)
        self.assertEqual((result["appended"], result["retained"], result["evicted"]), (5, 3, 2))
        self.assertEqual([r["elapsed_ns"] for r in result["recent"]], [3, 4, 5])

    def test_symlink_hardlink_and_public_files_rejected(self):
        journal = self.home / "journal.json"
        donor = self.base / "donor"
        donor.write_bytes(journal.read_bytes())
        donor.chmod(0o600)
        for kind in ("symlink", "hardlink", "public"):
            journal.unlink()
            if kind == "symlink":
                journal.symlink_to(donor)
            elif kind == "hardlink":
                os.link(donor, journal)
            else:
                journal.write_bytes(donor.read_bytes())
                journal.chmod(0o644)
            with self.assertRaises((Rejected, OSError)):
                self.append(self.row())
            with self.assertRaises((Rejected, OSError)):
                t.report(self.home)

    def test_staging_symlink_is_not_followed(self):
        outside = self.base / "outside"
        outside.write_bytes(b"preserve")
        (self.home / "journal.pending").symlink_to(outside)
        with self.assertRaises(OSError):
            self.append(self.row())
        self.assertEqual(outside.read_bytes(), b"preserve")
        self.assertEqual(t.report(self.home)["retained"], 0)

    def test_unsafe_directory_and_symlink_directory_rejected(self):
        self.home.chmod(0o755)
        with self.assertRaises(Rejected):
            t.report(self.home)
        self.home.chmod(0o700)
        alias = self.base / "alias"
        alias.symlink_to(self.home)
        with self.assertRaises(Rejected):
            t.report(alias)

    def test_route_overlap_never_writes_to_source_or_artifacts(self):
        for key in ("root", "out_dir", "model_dir", "artifact_dir", "profile_path", "core_binary"):
            config = dict(self.config, **{key: str(self.home)})
            with self.assertRaises(Rejected):
                t.append(self.home, self.row(), config, self.config_path)
        config = dict(self.config, profile_path=str(self.home / "fixture.json"))
        with self.assertRaises(Rejected):
            t.append(self.home, self.row(), config, self.config_path)
        self.assertEqual(t.report(self.home)["retained"], 0)

    def test_busy_writer_does_not_wait_or_change_journal(self):
        with (self.home / "writer.lock").open("rb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(BlockingIOError):
                self.append(self.row())
        self.assertEqual(t.report(self.home)["appended"], 0)

    def test_concurrent_acknowledged_appends_are_not_lost(self):
        def attempt(_):
            try:
                self.append(self.row())
                return 1
            except BlockingIOError:
                return 0
        with ThreadPoolExecutor(max_workers=8) as pool:
            acknowledged = sum(pool.map(attempt, range(24)))
        self.assertGreater(acknowledged, 0)
        self.assertEqual(t.report(self.home)["appended"], acknowledged)

    def test_corrupt_oversized_and_extra_field_records_are_not_repaired(self):
        journal = self.home / "journal.json"
        for data in (b"{broken", b"x" * (t.MAX_BYTES + 1)):
            journal.write_bytes(data)
            with self.assertRaises((Rejected, ValueError)):
                self.append(self.row())
            self.assertEqual(journal.read_bytes(), data)
        with self.assertRaises(Rejected):
            t.validate_record(self.row(query="private"))

    def test_duplicate_attempt_rejected(self):
        row = self.row()
        self.append(row)
        with self.assertRaises(Rejected):
            self.append(row)
        self.assertEqual(t.report(self.home)["appended"], 1)

    def test_atomic_write_failure_preserves_previous_complete_state(self):
        self.append(self.row())
        before = (self.home / "journal.json").read_bytes()
        with patch.object(t.os, "replace", side_effect=OSError("private")), self.assertRaises(OSError):
            self.append(self.row())
        self.assertEqual((self.home / "journal.json").read_bytes(), before)
        self.append(self.row())
        self.assertEqual(t.report(self.home)["appended"], 2)

    def test_post_rename_fsync_failure_is_unknown_not_automatic_retry(self):
        trace = t.Trace()
        session = type("Session", (), {"config": self.config, "path": self.config_path})()
        with patch.object(t.os, "fsync", side_effect=[None, OSError("private")]):
            warning = t.save(self.home, trace, session, None, {"ok": True}, 5, "compact")
        self.assertEqual(warning["code"], "query-metrics-not-confirmed")
        self.assertEqual(t.report(self.home)["appended"], 1)

    def test_save_failure_never_leaks_exception_or_changes_result(self):
        result = {"ok": False, "error": {"code": "core-admission-rejected"}}
        before = deepcopy(result)
        warning = t.save(self.home, t.Trace(), None, None, result, 100, "compact")
        self.assertEqual(result, before)
        self.assertFalse(warning["retrieval_changed"])
        self.assertNotIn("error", warning)


if __name__ == "__main__":
    unittest.main()
