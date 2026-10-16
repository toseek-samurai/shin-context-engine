#!/usr/bin/env python3
"""Acceptance must be recomputed, not inferred from a timer or producer ok flag."""

from copy import deepcopy
import unittest

from soak_integration_test import (ACTIONS, ORDER, POLICY, MACOS_MEMORY, RSS_MEMORY, FD_ACCEPTANCE, IntegrationFailure,
    fd_growth, validate_fd_windows,
    cpu_seconds, parse_heap_summary, paths, rss_growth, validate_report, validate_resources, validate_frozen_inputs)


def warmup():
    return {name: {"expected_snapshot_id": "snapshot-" + name, "health": {
        "project_id": name, "route_id": name, "state": "healthy", "checking": False, "selection_aligned": True,
        "current_generation": {"snapshot_id": "snapshot-" + name}, "assessment": {
            "healthy": True, "index_current": True, "generation": {"snapshot_id": "snapshot-" + name}}}} for name in ORDER}


def receipt(mode="smoke"):
    duration = POLICY["duration_seconds" if mode == "soak" else "smoke_seconds"]
    requests = [{"project": project, "action": ACTIONS[(i + j) % 3], "ok": True, "elapsed_ms": 5}
                for i in range(duration // 2) for j, project in enumerate(ORDER)]
    total = len(requests) + 48
    value = {"schema_version": "shin.soak-report.v4", "ok": True, "failure": None, "mode": mode, "warmup": warmup(),
        "platform": "linux", "memory_acceptance": deepcopy(RSS_MEMORY), "heap_samples": [],
        "qualification": "60-minute-soak" if mode == "soak" else "smoke-only", "policy": deepcopy(POLICY),
        "elapsed_seconds": duration, "requests": requests,
        "samples": [{"elapsed_ms": i * 10000, "rss_bytes": 20000000, "fds": 20, "cpu_seconds": i}
                    for i in range(duration // 10 + 1)], "max_schedule_lag_ms": 1,
        "telemetry_completed": total, "journal_rows": total, "total_requests": total,
        "source_preserved": True, "recovered": True,
        "observer_samples": (duration // 2 // POLICY["observer_batches"] + 1) * 4,
        "stop": {"exit_code": 0, "elapsed_ms": 15, "high_water_rss_bytes": 25000000}, "restart_ms": 50}
    value["rss_growth"] = rss_growth(value["samples"], RSS_MEMORY)
    value["fd_acceptance"] = deepcopy(FD_ACCEPTANCE)
    value["fd_growth"] = fd_growth(value["samples"], duration)
    return value


def mac_receipt(mode="smoke"):
    value = receipt(mode)
    duration = POLICY["duration_seconds" if mode == "soak" else "smoke_seconds"]
    value.update(platform="darwin", memory_acceptance=deepcopy(MACOS_MEMORY), heap_samples=[{
        "slot_seconds": slot, "started_elapsed_ms": slot * 1000, "tool_elapsed_ms": 100,
        "exit_code": 0, "live_nodes": 20000, "live_bytes": 18000000, "sha256": "a" * 64}
        for slot in range(0, duration + 1, 60)])
    value["samples"][-1]["rss_bytes"] += POLICY["rss_growth_bytes"] + 1
    value["stop"]["high_water_rss_bytes"] = value["samples"][-1]["rss_bytes"]
    value["rss_growth"] = rss_growth(value["samples"], MACOS_MEMORY)
    return value


class SoakAcceptanceTests(unittest.TestCase):
    def test_valid_receipts(self):
        for mode in ("smoke", "soak"):
            validate_report(receipt(mode))

    def test_smoke_cannot_claim_hour(self):
        value = receipt()
        value.update(mode="soak", qualification="60-minute-soak")
        with self.assertRaises(IntegrationFailure):
            validate_report(value)

    def test_missing_failed_or_changed_proof(self):
        for mutate in (
            lambda v: v.update(ok=False),
            lambda v: v["requests"].pop(),
            lambda v: v["requests"][0].update(ok=False),
            lambda v: v["requests"][0].update(elapsed_ms=4001),
            lambda v: v["samples"].pop(),
            lambda v: v["samples"][-1].update(rss_bytes=POLICY["rss_bytes"] + 1),
            lambda v: v["samples"][-1].update(rss_bytes=20000000 + POLICY["rss_growth_bytes"] + 1),
            lambda v: v["samples"][-1].update(fds=25),
            lambda v: v["samples"][-1].update(cpu_seconds=121),
            lambda v: v.update(journal_rows=v["journal_rows"] - 1),
            lambda v: v.update(telemetry_completed=v["total_requests"] - 1),
            lambda v: v.update(observer_samples=0),
            lambda v: v.update(source_preserved=False),
            lambda v: v.update(recovered=False),
            lambda v: v["stop"].update(exit_code=1),
            lambda v: v["stop"].update(high_water_rss_bytes=1),
            lambda v: v["policy"].update(rss_growth_bytes=999999999),
        ):
            value = receipt()
            mutate(value)
            with self.assertRaises(IntegrationFailure):
                validate_report(value)

    def test_cpu_formats(self):
        for value, expected in (("00:01.25", 1.25), ("1:02:03", 3723), ("2-01:02:03", 176523)):
            self.assertEqual(cpu_seconds(value), expected)
        for value in ("1", "nan:2", "1:inf"):
            with self.assertRaises(IntegrationFailure):
                cpu_seconds(value)

    def test_exact_path_evidence(self):
        self.assertEqual(paths({"records": [{"path": "src/a.cpp"}], "query": "src/b.cpp"}), {"src/a.cpp"})

    def test_growth_stops_without_waiting_for_an_hour(self):
        baseline = {"rss_bytes": 100000000, "fds": 20}
        with self.assertRaisesRegex(IntegrationFailure, "soak-rss-growth-budget"):
            validate_resources(baseline, {**baseline, "rss_bytes": 100000000 + POLICY["rss_growth_bytes"] + 1})

    def test_changed_build_requires_a_new_smoke_not_a_waiver(self):
        previous = {"inputs": {"a": "abc"}, "policy": POLICY, "binaries": {"server": "old"}, "harness_sha256": "old"}
        current = {**previous, "binaries": {"server": "new"}, "harness_sha256": "new"}
        for mode in ("smoke", "diagnostic"):
            validate_frozen_inputs(previous, current, mode)
        with self.assertRaisesRegex(IntegrationFailure, "smoke-executables-changed"):
            validate_frozen_inputs(previous, current, "soak")
        current["inputs"] = {"a": "changed"}
        with self.assertRaisesRegex(IntegrationFailure, "frozen-inputs-changed"):
            validate_frozen_inputs(previous, current, "diagnostic")

    def test_diagnostic_cannot_qualify(self):
        with self.assertRaises(IntegrationFailure):
            validate_report({"schema_version": "shin.memory-diagnostic.v1", "ok": True, "qualification": False})

    def test_all_routes_must_finish_current_background_work(self):
        for mutate in (lambda v: v.pop("warmup"), lambda v: v["warmup"].pop("route-b"),
                       lambda v: v["warmup"]["route-b"]["health"].update(state="pending"),
                       lambda v: v["warmup"]["route-b"]["health"].update(checking=True),
                       lambda v: v["warmup"]["route-b"]["health"].update(selection_aligned=False),
                       lambda v: v["warmup"]["route-b"].update(expected_snapshot_id="old")):
            value = receipt()
            mutate(value)
            with self.assertRaises(IntegrationFailure):
                validate_report(value)

    def test_old_receipt_is_only_an_explicit_historical_input(self):
        for schema in ("shin.soak-report.v1", "shin.soak-report.v2", "shin.soak-report.v3"):
            value = receipt()
            value["schema_version"] = schema
            if schema.endswith("v1"):
                value.pop("warmup")
            with self.assertRaises(IntegrationFailure):
                validate_report(value)
            validate_report(value, allow_legacy=True)

    def test_macos_keeps_original_rss_failure_visible(self):
        for mode in ("smoke", "soak"):
            value = mac_receipt(mode)
            self.assertFalse(value["rss_growth"]["within_original_limit"])
            self.assertFalse(value["rss_growth"]["qualification_required"])
            validate_report(value)

    def test_macos_requires_every_bounded_heap_snapshot(self):
        for mutate in (
            lambda v: v.pop("heap_samples"),
            lambda v: v["heap_samples"].pop(),
            lambda v: v["heap_samples"][-1].update(slot_seconds=59),
            lambda v: v["heap_samples"][-1].update(started_elapsed_ms=65000),
            lambda v: v["heap_samples"][-1].update(tool_elapsed_ms=4001),
            lambda v: v["heap_samples"][-1].update(exit_code=1),
            lambda v: v["heap_samples"][-1].update(live_nodes=0),
            lambda v: v["heap_samples"][-1].update(live_bytes=True),
            lambda v: v["heap_samples"][-1].update(live_bytes=18000000 + 33554433),
            lambda v: v["heap_samples"][-1].update(sha256=""),
            lambda v: v.pop("rss_growth"),
            lambda v: v["rss_growth"].update(within_original_limit=True),
            lambda v: v["samples"][-1].update(rss_bytes=POLICY["rss_bytes"] + 1),
            lambda v: v["memory_acceptance"].update(live_growth_bytes=999999999),
            lambda v: v.update(platform="linux"),
        ):
            value = mac_receipt()
            mutate(value)
            with self.assertRaises(IntegrationFailure):
                validate_report(value)

    def test_linux_still_enforces_rss_growth(self):
        value = receipt()
        value["samples"][-1]["rss_bytes"] += POLICY["rss_growth_bytes"] + 1
        value["rss_growth"] = rss_growth(value["samples"], RSS_MEMORY)
        with self.assertRaisesRegex(IntegrationFailure, "soak-rss-growth-budget"):
            validate_report(value)

    def test_heap_summary_is_exact_and_unambiguous(self):
        text = "Process: shin-server\nAll zones: 22880 nodes (18094672 bytes)\n"
        self.assertEqual(parse_heap_summary(text), {"live_nodes": 22880, "live_bytes": 18094672})
        for invalid in ("", text + text, "All zones: 0 nodes (0 bytes)", "All zones: 4 nodes (18M bytes)"):
            with self.assertRaises(IntegrationFailure):
                parse_heap_summary(invalid)

    def test_hour_cannot_change_platform_or_memory_acceptance(self):
        previous = {"inputs": {}, "policy": POLICY, "binaries": {}, "harness_sha256": "same",
                    "platform": "darwin", "memory_acceptance": MACOS_MEMORY}
        validate_frozen_inputs(previous, deepcopy(previous), "soak")
        for key, value in (("platform", "linux"), ("memory_acceptance", RSS_MEMORY)):
            with self.assertRaisesRegex(IntegrationFailure, "smoke-memory-acceptance-changed"):
                validate_frozen_inputs(previous, {**previous, key: value}, "soak")

    def test_transient_descriptors_remain_visible_but_are_not_sustained_growth(self):
        value = receipt()
        value["samples"][2]["fds"] = 40
        value["fd_growth"] = fd_growth(value["samples"], 60)
        self.assertFalse(value["fd_growth"]["original_within_limit"])
        validate_report(value)

    def test_baseline_cannot_hide_persistent_growth_in_first_window(self):
        value = receipt()
        for row in value["samples"][1:]:
            row["fds"] = 25
        value["fd_growth"] = fd_growth(value["samples"], 60)
        with self.assertRaisesRegex(IntegrationFailure, "sustained-fd-growth"):
            validate_report(value)

    def test_each_window_not_global_minimum_must_pass(self):
        value = receipt("soak")
        for row in value["samples"]:
            if 60000 <= row["elapsed_ms"] < 120000:
                row["fds"] = 25
        value["fd_growth"] = fd_growth(value["samples"], 3600)
        with self.assertRaisesRegex(IntegrationFailure, "sustained-fd-growth"):
            validate_report(value)

    def test_missing_sparse_duplicate_or_excessive_fd_samples_are_rejected(self):
        baseline = receipt()["samples"]
        for rows in (baseline[:3], [baseline[0], baseline[1], baseline[1], *baseline[2:]],
                     [{**row, "fds": 97} for row in baseline]):
            with self.subTest(rows=rows), self.assertRaises(IntegrationFailure):
                validate_fd_windows(rows, 60, complete=True)

    def test_window_fails_as_soon_as_closed_without_waiting_for_hour(self):
        rows = [{"elapsed_ms": i * 10000, "fds": 20 if i == 0 else 25} for i in range(8)]
        with self.assertRaisesRegex(IntegrationFailure, "sustained-fd-growth"):
            validate_fd_windows(rows, 3600, complete=False)

    def test_fd_acceptance_and_original_diagnostic_cannot_be_forged(self):
        for mutate in (lambda v: v.pop("fd_growth"),
                       lambda v: v["fd_growth"].update(original_within_limit=False),
                       lambda v: v["fd_acceptance"].update(window_seconds=120)):
            value = receipt()
            mutate(value)
            with self.assertRaises(IntegrationFailure):
                validate_report(value)

    def test_legacy_v3_keeps_its_original_per_sample_fd_gate(self):
        value = receipt()
        value["schema_version"] = "shin.soak-report.v3"
        value["samples"][2]["fds"] = 40
        with self.assertRaisesRegex(IntegrationFailure, "soak-fd-growth-budget"):
            validate_report(value, allow_legacy=True)

    def test_hour_cannot_change_fd_policy(self):
        value = {"inputs": {}, "policy": POLICY, "binaries": {}, "harness_sha256": "same",
                 "platform": "linux", "memory_acceptance": RSS_MEMORY, "fd_acceptance": FD_ACCEPTANCE}
        with self.assertRaisesRegex(IntegrationFailure, "smoke-fd-acceptance-changed"):
            validate_frozen_inputs(value, {**value, "fd_acceptance": None}, "soak")


if __name__ == "__main__":
    unittest.main()
