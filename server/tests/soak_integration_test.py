#!/usr/bin/env python3
"""Finite four-corpus R3 workload; smoke mode is never an hour-soak receipt."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

from integration_test import BUILD_ROOT, REPOSITORY_ROOT, IntegrationFailure, under_build
from operations_budget_integration_test import digest, inventory, resources, WRITER, KNOWLEDGE
from retrieval_integration_test import RetrievalIntegrationSuite
from retrieval_observability_checks import RetrievalObservabilityChecks
from observer_ingestion_integration_test import load_observer_fixture_module


# Fixed before the first measured run; unchanged R2 Release memory/FD ceilings.
POLICY = {"duration_seconds": 3600, "smoke_seconds": 60, "clients": 4,
          "cadence_seconds": 2, "warm_batches": 12, "sample_batches": 5,
          "observer_batches": 15, "max_sample_gap_ms": 15000,
          "request_ms": 4000, "p95_ms": 1000, "schedule_lag_ms": 4000,
          "rss_bytes": 268435456, "rss_growth_bytes": 33554432,
          "fds": 96, "fd_growth": 4, "mean_cpu_cores": 2,
          "shutdown_ms": 10000, "restart_ms": 10000,
          "preparation_seconds": 120, "observer_ms": 4000,
          "source_max_files": 1000, "source_max_bytes": 33554432,
          "source_max_file_bytes": 2097152, "journal_rows": 8192}
CORPORA = {
    "route-a": {"query": "synthetic route alpha anchor", "target": "src/alpha_anchor.cpp"},
    "route-b": {"query": "synthetic route bravo anchor", "target": "src/bravo_anchor.cpp"},
    "route-c": {"query": "synthetic route charlie anchor", "target": "src/charlie_anchor.cpp"},
    "route-d": {"query": "synthetic route delta anchor", "target": "src/delta_anchor.cpp"}}
ORDER = tuple(CORPORA)
ACTIONS = ("search", "brief", "pack")
RSS_MEMORY = {"id": "original-rss/v1", "rss_growth_required": True}
MACOS_MEMORY = {"id": "macos-live-heap/v1", "rss_growth_required": False,
                "live_growth_bytes": 33554432, "sample_seconds": 60, "tool_timeout_seconds": 4}
FD_ACCEPTANCE = {"id": "window-floor/v1", "window_seconds": 60, "minimum_samples": 4,
                 "baseline_in_window": False, "original_peak_growth_required": False}


def memory_policy(platform):
    require(platform in ("darwin", "linux"), "unsupported-soak-platform")
    return dict(MACOS_MEMORY if platform == "darwin" else RSS_MEMORY)


def parse_heap_summary(text):
    matches = re.findall(r"^All zones: ([0-9]+) nodes \(([0-9]+) bytes\)$", text, re.MULTILINE)
    require(len(matches) == 1, "missing-or-ambiguous-live-heap-summary")
    nodes, allocated = map(int, matches[0])
    require(nodes > 0 and allocated > 0, "invalid-live-heap-summary")
    return {"live_nodes": nodes, "live_bytes": allocated}


def validate_heap_samples(rows, duration):
    require(isinstance(rows, list) and [r["slot_seconds"] for r in rows]
            == list(range(0, duration + 1, MACOS_MEMORY["sample_seconds"])), "missing-live-heap-samples")
    for row in rows:
        require(row["exit_code"] == 0 and type(row["live_nodes"]) is int and row["live_nodes"] > 0
                and type(row["live_bytes"]) is int and row["live_bytes"] > 0
                and 0 <= row["tool_elapsed_ms"] <= MACOS_MEMORY["tool_timeout_seconds"] * 1000
                and 0 <= row["started_elapsed_ms"] - row["slot_seconds"] * 1000 <= POLICY["schedule_lag_ms"]
                and re.fullmatch(r"[a-f0-9]{64}", row["sha256"]) is not None, "invalid-live-heap-sample")
        require(row["live_bytes"] - rows[0]["live_bytes"] <= MACOS_MEMORY["live_growth_bytes"],
                "soak-live-heap-growth-budget")


def rss_growth(samples, policy):
    growth = max(row["rss_bytes"] for row in samples) - samples[0]["rss_bytes"]
    return {"bytes": growth, "original_limit_bytes": POLICY["rss_growth_bytes"],
            "within_original_limit": growth <= POLICY["rss_growth_bytes"],
            "qualification_required": policy["rss_growth_required"]}


def fd_growth(samples, duration):
    baseline = samples[0]["fds"]
    windows = [[] for _ in range(duration // FD_ACCEPTANCE["window_seconds"])]
    for row in samples[1:]:
        slot = min(int(row["elapsed_ms"] // (FD_ACCEPTANCE["window_seconds"] * 1000)), len(windows) - 1)
        require(slot >= 0, "invalid-fd-sample-time")
        windows[slot].append(row["fds"])
    peak = max(row["fds"] for row in samples)
    return {"baseline_fds": baseline, "peak_fds": peak, "peak_growth": peak - baseline,
            "original_limit": POLICY["fd_growth"], "original_within_limit": peak - baseline <= POLICY["fd_growth"],
            "original_qualification_required": False, "windows": [
                {"start_seconds": index * FD_ACCEPTANCE["window_seconds"],
                 "end_seconds": (index + 1) * FD_ACCEPTANCE["window_seconds"],
                 "samples": len(values), "minimum_fds": min(values) if values else None,
                 "growth": min(values) - baseline if values else None}
                for index, values in enumerate(windows)]}


def validate_fd_windows(samples, duration, *, complete):
    require(samples and all(type(row["fds"]) is int and 0 < row["fds"] <= POLICY["fds"]
            and 0 <= row["elapsed_ms"] <= duration * 1000 + POLICY["request_ms"] for row in samples),
            "invalid-fd-samples")
    require(all(0 < b["elapsed_ms"] - a["elapsed_ms"] <= POLICY["max_sample_gap_ms"]
                for a, b in zip(samples, samples[1:])), "fd-sample-gap-or-duplicate")
    result = fd_growth(samples, duration)
    for window in result["windows"]:
        if complete or window["end_seconds"] * 1000 <= samples[-1]["elapsed_ms"]:
            require(window["samples"] >= FD_ACCEPTANCE["minimum_samples"], "missing-fd-window-samples")
            require(window["growth"] <= POLICY["fd_growth"], "soak-sustained-fd-growth-budget")
    return result


def require(condition, message):
    if not condition:
        raise IntegrationFailure(message)


def cpu_seconds(value):
    """Portable ps cumulative CPU: [[days-]hours:]minutes:seconds."""
    days, separator, clock = value.strip().partition("-")
    fields = [float(part) for part in (clock if separator else days).split(":")]
    require(len(fields) in (2, 3) and all(math.isfinite(n) and n >= 0 for n in fields), "invalid-cpu-time")
    seconds = sum(n * 60 ** i for i, n in enumerate(reversed(fields)))
    return seconds + (int(days) * 86400 if separator else 0)


def paths(value):
    if isinstance(value, dict):
        return ({value["path"]} if isinstance(value.get("path"), str) else set()).union(
            *(paths(item) for item in value.values()))
    if isinstance(value, list):
        return set().union(*(paths(item) for item in value))
    return set()


def task(query):
    return {"schema_version": "shin.task-request.v1", "request_id": "r3-soak",
            "task": {"objective": "Inspect the frozen source anchor.", "must": [{"id": "anchor", "query": query}]},
            "budget": {"max_evidence_bytes": 8192, "max_records": 6,
                       "per_query_records": 6, "per_document_records": 2}}


def validate_resources(baseline, row, memory=RSS_MEMORY, *, fd_growth_required=True):
    require(0 < row["rss_bytes"] <= POLICY["rss_bytes"] and 0 < row["fds"] <= POLICY["fds"],
            "soak-absolute-resource-budget")
    if memory["rss_growth_required"]:
        require(row["rss_bytes"] - baseline["rss_bytes"] <= POLICY["rss_growth_bytes"], "soak-rss-growth-budget")
    if fd_growth_required:
        require(row["fds"] - baseline["fds"] <= POLICY["fd_growth"], "soak-fd-growth-budget")


def validate_frozen_inputs(previous, current, mode):
    require(current["inputs"] == previous["inputs"] and current["policy"] == previous["policy"],
            "frozen-inputs-changed")
    if mode == "soak":
        require(current["binaries"] == previous["binaries"] and
                current["harness_sha256"] == previous["harness_sha256"], "smoke-executables-changed")
        require(current.get("memory_acceptance") == previous.get("memory_acceptance")
                and current.get("platform") == previous.get("platform"), "smoke-memory-acceptance-changed")
        require(current.get("fd_acceptance") == previous.get("fd_acceptance"), "smoke-fd-acceptance-changed")


def background_warm(name, health, snapshot):
    if not isinstance(health, dict) or not isinstance(snapshot, str) or not snapshot:
        return False
    assessment, generation = health.get("assessment"), health.get("current_generation")
    return (health.get("project_id") == name and health.get("route_id") == name
            and health.get("state") == "healthy" and health.get("checking") is False
            and health.get("selection_aligned") is True and isinstance(assessment, dict)
            and isinstance(generation, dict) and generation.get("snapshot_id") == snapshot
            and assessment.get("healthy") is True and assessment.get("index_current") is True
            and assessment.get("generation") == generation)


def validate_report(value, *, allow_legacy=False):
    schemas = {"shin.soak-report.v4"}
    if allow_legacy:
        schemas.update(("shin.soak-report.v1", "shin.soak-report.v2", "shin.soak-report.v3"))
    require(value.get("schema_version") in schemas and value.get("ok") is True
            and value.get("failure") is None and value.get("policy") == POLICY, "invalid-soak-receipt")
    current = value["schema_version"] in ("shin.soak-report.v3", "shin.soak-report.v4")
    window_fds = value["schema_version"] == "shin.soak-report.v4"
    memory = memory_policy(value.get("platform")) if current else RSS_MEMORY
    if current:
        require(value.get("memory_acceptance") == memory, "invalid-soak-memory-acceptance")
    if value["schema_version"] != "shin.soak-report.v1":
        warmup = value.get("warmup")
        require(isinstance(warmup, dict) and set(warmup) == set(ORDER), "missing-background-warmup")
        require(all(background_warm(name, row.get("health"), row.get("expected_snapshot_id"))
                    for name, row in warmup.items()), "incomplete-background-warmup")
    mode = value.get("mode")
    require(mode in ("smoke", "soak"), "invalid-soak-mode")
    duration = POLICY["duration_seconds" if mode == "soak" else "smoke_seconds"]
    if current:
        if memory == MACOS_MEMORY:
            validate_heap_samples(value.get("heap_samples"), duration)
        else:
            require(value.get("heap_samples") == [], "unexpected-live-heap-samples")
    require(value.get("qualification") == ("60-minute-soak" if mode == "soak" else "smoke-only")
            and duration <= value["elapsed_seconds"] <= duration + POLICY["request_ms"] / 1000,
            "soak-duration-not-proven")
    rows = value["requests"]
    require(len(rows) == duration // POLICY["cadence_seconds"] * POLICY["clients"], "soak-request-count")
    for project in ORDER:
        for action in ACTIONS:
            subset = [r for r in rows if r["project"] == project and r["action"] == action]
            require(len(subset) == duration // POLICY["cadence_seconds"] // 3, "soak-mix-incomplete")
            require(all(r["ok"] is True and 0 <= r["elapsed_ms"] <= POLICY["request_ms"] for r in subset),
                    "soak-request-failed")
            require(RetrievalIntegrationSuite.percentile_ms([r["elapsed_ms"] for r in subset], 95)
                    <= POLICY["p95_ms"], "soak-latency-budget")
    samples = value["samples"]
    require(len(samples) >= duration // 10 + 1, "soak-missing-resource-samples")
    require(all(0 <= b["elapsed_ms"] - a["elapsed_ms"] <= POLICY["max_sample_gap_ms"]
                for a, b in zip(samples, samples[1:])), "soak-sample-gap")
    baseline = samples[0]
    for row in samples:
        validate_resources(baseline, row, memory, fd_growth_required=not window_fds)
    if window_fds:
        require(value.get("fd_acceptance") == FD_ACCEPTANCE, "invalid-fd-acceptance")
        require(value.get("fd_growth") == validate_fd_windows(samples, duration, complete=True),
                "missing-original-fd-comparison")
    if current:
        require(value.get("rss_growth") == rss_growth(samples, memory), "missing-original-rss-comparison")
    require(0 <= (samples[-1]["cpu_seconds"] - baseline["cpu_seconds"]) / value["elapsed_seconds"]
            <= POLICY["mean_cpu_cores"], "soak-cpu-budget")
    require(0 <= value["max_schedule_lag_ms"] <= POLICY["schedule_lag_ms"], "soak-schedule-lag")
    require(value["telemetry_completed"] == value["journal_rows"] == value["total_requests"]
            <= POLICY["journal_rows"] and value["source_preserved"] is True
            and value["recovered"] is True, "soak-completeness-or-preservation")
    require(value["observer_samples"] == (duration // POLICY["cadence_seconds"] // POLICY["observer_batches"] + 1) * 4,
            "soak-observation-incomplete")
    require(value["stop"]["exit_code"] == 0 and 0 <= value["stop"]["elapsed_ms"] <= POLICY["shutdown_ms"]
            and value["stop"]["high_water_rss_bytes"] >= max(r["rss_bytes"] for r in samples)
            and 0 <= value["restart_ms"] <= POLICY["restart_ms"], "soak-stop-or-recovery")


class SoakSuite(RetrievalIntegrationSuite):
    def __init__(self, binary, core, observer, state_root, output, mode, frozen_from=None):
        super().__init__(binary, core, state_root, output / "report.json")
        self.observer, self.output, self.mode = observer, output, mode
        self.corpora, self.samples, self.requests, self.baseline_requests = {}, [], [], []
        self.observer_samples, self.total_requests = 0, 0
        self.provisioned, self.recovered, self.source_preserved = False, False, False
        self.elapsed_seconds, self.max_lag = 0, 0
        self.stop_receipt, self.restart_ms = {}, None
        self.telemetry_completed, self.journal_rows = None, None
        self.freeze = None
        self.phase = "prepare"
        self.frozen_from = frozen_from
        self.observer_fixture = load_observer_fixture_module()
        self.warmup = {}
        self.memory_policy = memory_policy(sys.platform)
        self.heap_samples = []

    def write(self, name, value):
        self.replace_private_json(self.output / name, value)

    def progress(self):
        self.write("progress.json", {"phase": self.phase, "mode": self.mode,
            "harness_pid": os.getpid(), "server_pid": self.server.pid if self.server else None,
            "updated_unix_seconds": time.time(), "completed_requests": len(self.requests),
            "elapsed_seconds": self.elapsed_seconds, "report": str(self.report_path)})

    def prepare_corpora(self):
        owner_path = REPOSITORY_ROOT / "profiles/generic-cpp.json"
        if self.frozen_from:
            owner_raw_by_route = {
                name: (self.frozen_from / name / "owner-profile.json").read_bytes() for name in ORDER}
        else:
            owner_raw = owner_path.read_bytes()
            owner_raw_by_route = {name: owner_raw for name in ORDER}
        for name in ORDER:
            base = self.output / name
            root, profiles, out, state = (base / n for n in ("source", "profiles", "index", "observer-state"))
            for directory in (root, profiles, out, state):
                directory.mkdir(mode=0o700, parents=True)
            spec = CORPORA[name]
            anchor = spec["query"].replace(" ", "_")
            self.write_fixture_file(root / spec["target"],
                f"namespace soak_fixture {{ constexpr const char* {anchor} = \"{name}\"; }}\n")
            owner_raw = owner_raw_by_route[name]
            owner = json.loads(owner_raw)
            self.write_fixture_file(base / "owner-profile.json", owner_raw.decode("utf-8"))
            profile = deepcopy(owner)
            profile.update(id="soak-" + name, document="shin-profile-soak-" + name,
                summary="Generated local source fixture for runtime qualification.",
                rootFiles=[], optionalRootFiles=[], agentStartPaths=[], bootstrapOnly=True,
                documentIncludePrefixes=[], sourceFactRoots=["src"],
                evalCases=[{"id": "soak-anchor", "query": spec["query"], "mustPaths": [spec["target"]],
                            "limit": 6, "perDocLimit": 2}],
                packEvalCases=[{"id": "soak-anchor-pack", "request": task(spec["query"]),
                    "mustPaths": [spec["target"]], "expectedComplete": True, "maxEvidenceBytes": 8192,
                    "maxRecords": 6, "maxCompactBytes": 16000, "requireSerialParallelIdentity": True}])
            self.write_fixture_file(profiles / (profile["id"] + ".json"), json.dumps(profile))
            config = {"schema_version": "shin.observer-config.v2", "observer_id": "soak-" + name,
                "source_host_id": "local-dev", "project_id": name, "workspace_id": "primary",
                "root": str(root), "profile": profile["id"], "profile_dir": str(profiles),
                "out_dir": str(out), "state_dir": str(state), "poll_interval_ms": 1000,
                "max_snapshot_bytes": 134217728, "max_file_bytes": 67108864,
                "outbox": {"enabled": False, "max_entries": 64, "max_bytes": 131072}}
            self.replace_private_json(base / "observer.json", config)
            self.corpora[name] = {"root": root, "profiles": profiles, "out": out,
                "profile": profile["id"], "query": spec["query"], "target": spec["target"],
                "observer": base / "observer.json"}
        protected = [c[key] for c in self.corpora.values() for key in ("root", "profiles")]
        protected += [self.output / name / "owner-profile.json" for name in ORDER]
        inputs = {str(Path(path).relative_to(self.output)): sha for path, sha in inventory(protected).items()}
        inputs.update({name + "/owner-profile.json": digest(self.output / name / "owner-profile.json") for name in ORDER})
        self.freeze = {"policy": POLICY, "scope": CORPORA, "inputs": inputs,
                       "platform": sys.platform, "memory_acceptance": self.memory_policy,
                       "fd_acceptance": FD_ACCEPTANCE,
                       "binaries": {name: digest(path) for name, path in
                                    (("core", self.core), ("server", self.binary), ("observer", self.observer))},
                       "harness_sha256": digest(Path(__file__))}
        if self.frozen_from:
            validate_frozen_inputs(json.loads((self.frozen_from / "freeze.json").read_text()), self.freeze, self.mode)
        self.write("freeze.json", self.freeze)
        for name, c in self.corpora.items():
            prefix = [str(self.core), "--root", str(c["root"]), "--profile", c["profile"],
                      "--profile-dir", str(c["profiles"]), "--out-dir", str(c["out"])]
            for action in (("build",), ("validate", "--strict")):
                result = subprocess.run(prefix + list(action), capture_output=True, text=True, timeout=120)
                self.write(name + "-" + action[0] + ".json", json.loads(result.stdout or result.stderr))
                require(result.returncode == 0, "frozen-corpus-" + name + "-" + action[0])

    def result(self, path):
        return self.expect_status(200, "GET", path, headers=self.auth_headers("admin"))["result"]

    def warm_background(self):
        self.phase = "warm-background"
        self.progress()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            observations = {name: {"expected_snapshot_id": self.active_generations[(name, name)]["snapshot_id"],
                "health": self.result(f"/api/v1/projects/{name}/routes/{name}/knowledge-health")} for name in ORDER}
            if all(background_warm(name, row["health"], row["expected_snapshot_id"]) for name, row in observations.items()):
                self.warmup = observations
                self.write("warmup.json", observations)
                return
            time.sleep(.05)
        raise IntegrationFailure("background-warmup-deadline")

    def configure(self):
        self.managed_state_dir = self.output / "managed"
        self.managed_state_dir.mkdir(mode=0o700)
        for path in (self.runtime_config_path, self.migrator_config_path):
            raw = path.read_bytes()
            self._original_configs[path] = raw
            value = json.loads(raw)
            value.update(workers=2, queue_limit=4, allowed_roots=[str(c["root"]) for c in self.corpora.values()])
            value["retrieval"] = {"routes": [{"id": name, "project_id": name, "workspace_id": "primary",
                "root": str(c["root"]), "profile": c["profile"], "profile_dir": str(c["profiles"])}
                for name, c in self.corpora.items()], "max_payload_bytes": 134217728,
                "max_generation_bytes": 33554432, "max_file_bytes": 16777216, "max_generations": 4}
            value["preparations"] = {"enabled": True, "state_dir": str(self.managed_state_dir),
                                     "max_retained_jobs": 8, "lease_seconds": 3}
            value["knowledge"] = {"enabled": True, "principal_id": "dev-admin", "route_ids": list(ORDER),
                "interval_seconds": 5, "max_age_seconds": 15, "prepare_on_drift": False}
            value["journal_writer"] = {"enabled": True, "principal_id": "dev-admin",
                "interval_ms": 100, "max_retry_ms": 400, "flush_on_shutdown": True}
            self.replace_private_json(path, value)

    def await_value(self, path, predicate, seconds=10):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            value = self.result(path)
            if predicate(value):
                return value
            time.sleep(.05)
        raise IntegrationFailure("background-progress-deadline")

    def request_one(self, name, action):
        c = self.corpora[name]
        body = {"task_request": task(c["query"])} if action == "pack" else {"query": c["query"], "limit": 6}
        started = time.monotonic()
        status, value = self.post_retrieval(name, action, body, route_id=name)
        elapsed = (time.monotonic() - started) * 1000
        result = value.get("result", {})
        require(status == 200 and value.get("ok") is True and result.get("project_id") == name
                and result.get("admission") == "exact-current" and c["target"] in paths(result.get("evidence"))
                and result.get("snapshot_id") == self.active_generations[(name, name)]["snapshot_id"],
                "retrieval-evidence-" + name + "-" + action)
        return {"project": name, "action": action, "elapsed_ms": elapsed, "ok": True}

    def batch(self, pool, index):
        values = list(pool.map(lambda item: self.request_one(item[1], ACTIONS[(index + item[0]) % 3]), enumerate(ORDER)))
        self.total_requests += len(values)
        return values

    def observe(self):
        for c in self.corpora.values():
            started = time.monotonic()
            value = self.run_process([str(self.observer), "sample", "--config", str(c["observer"])],
                                     "Observer sample", timeout=4)
            result = json.loads(value.stdout)
            require(result.get("ok") is True and (time.monotonic() - started) * 1000 <= POLICY["observer_ms"],
                    "observer-sample-failed")
            # State is persisted by the existing local Observer, not this harness.
            state = self.observer_fixture.state_from_result(result["result"])
            require(state["observation"]["status"] == "current", "observer-not-current")
            self.observer_samples += 1

    def sample(self, start):
        metrics = RetrievalObservabilityChecks(self).metrics()
        require(sum(cell["completed"] for cell in metrics["cells"]) == self.total_requests
                and sum(cell["failed"] for cell in metrics["cells"]) == 0, "missing-request-telemetry")
        writer, cache, knowledge = self.result(WRITER), self.result("/api/v1/cache"), self.result(KNOWLEDGE)
        require(writer["pending_batches"] <= 1 and writer["pending_records"] <= 256
                and writer["pending_bytes"] <= 4194304, "writer-bound")
        require(cache["resident_generations"] <= 4 and
                cache["resident_payload_bytes"] + cache["reserved_payload_bytes"] <= cache["maximum_payload_bytes"],
                "cache-bound")
        cpu = cpu_seconds(subprocess.check_output(["ps", "-o", "time=", "-p", str(self.server.pid)], text=True, timeout=2))
        row = {"elapsed_ms": (time.monotonic() - start) * 1000, **resources(self.server.pid), "cpu_seconds": cpu,
               "cache": cache, "writer": writer, "knowledge": knowledge, "retention": metrics["retention"]}
        self.samples.append(row)
        self.write("samples.json", self.samples)
        self.elapsed_seconds = time.monotonic() - start
        self.progress()
        validate_resources(self.samples[0], row, self.memory_policy, fd_growth_required=False)
        duration = POLICY["duration_seconds" if self.mode == "soak" else "smoke_seconds"]
        validate_fd_windows(self.samples, duration, complete=False)

    def sample_heap(self, slot, start=None):
        if self.memory_policy != MACOS_MEMORY:
            return
        path = self.output / f"heap-{slot:04d}.txt"
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        started = time.monotonic()
        with os.fdopen(descriptor, "w") as output:
            completed = subprocess.run(["/usr/bin/heap", "--noContent", "-s", str(self.server.pid)],
                stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                timeout=MACOS_MEMORY["tool_timeout_seconds"])
        require(completed.returncode == 0, "live-heap-inspection-unavailable")
        row = {"slot_seconds": slot, "started_elapsed_ms": (started - start) * 1000 if start is not None else 0,
               "tool_elapsed_ms": (time.monotonic() - started) * 1000, "exit_code": completed.returncode,
               "sha256": digest(path), **parse_heap_summary(path.read_text())}
        self.heap_samples.append(row)
        self.write("heap-samples.json", self.heap_samples)
        validate_heap_samples(self.heap_samples, slot)

    def stop_measured(self):
        process = self.server
        process.terminate()
        started = time.monotonic()
        while time.monotonic() - started < POLICY["shutdown_ms"] / 1000:
            pid, status, usage = os.wait4(process.pid, os.WNOHANG)
            if pid:
                process.returncode = os.waitstatus_to_exitcode(status)
                self.server = None
                receipt = {"exit_code": process.returncode, "elapsed_ms": (time.monotonic() - started) * 1000,
                    "high_water_rss_bytes": usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024),
                    "user_cpu_seconds": usage.ru_utime, "system_cpu_seconds": usage.ru_stime}
                require(receipt["exit_code"] == 0, "unclean-soak-stop")
                return receipt
            time.sleep(.02)
        raise IntegrationFailure("soak-stop-timeout")

    def setup(self):
        self.progress()
        self.prepare_corpora()
        self.check_version()
        self.require_http_port_free()
        self.dev_command("init")
        self.provisioned = True
        self.dev_command("up")
        self.wait_for_postgres()
        self.reset_schema()
        self.migrate()
        self.admin_token = self.read_private_token("dev_admin.token")
        self.reader_token = self.read_private_token("dev_reader.token")
        self.configure()
        self.start_server()
        for name, c in self.corpora.items():
            self.assert_register(name, "r3-register-" + name)
            self.assert_workspace_register(name, "primary", c["root"], "r3-workspace-" + name)
            job = self.post_seed_preparation(name, name, None, "r3-seed-" + name)
            prepared = self.await_value("/api/v1/preparations/" + str(job["id"]),
                lambda value: value["state"] in ("prepared", "failed", "cancelled"), POLICY["preparation_seconds"])
            require(prepared["state"] == "prepared", "preparation-failed-" + name)
            self.activate_seed(name, name, job["id"], 0)
            self.active_generations[(name, name)] = self.activation(name, name)

    def run(self):
        self.setup()
        with ThreadPoolExecutor(max_workers=4) as pool:
            self.phase = "cold-warm-baseline"
            for index in range(POLICY["warm_batches"]):
                self.baseline_requests.extend(self.batch(pool, index))
            self.write("baseline.json", self.baseline_requests)
            self.observe()
            self.warm_background()
            before = inventory([c[key] for c in self.corpora.values() for key in ("root", "profiles", "out")]
                               + [self.managed_state_dir])
            identity = self.server_id
            self.phase = self.mode
            duration = POLICY["duration_seconds" if self.mode == "soak" else "smoke_seconds"]
            self.sample_heap(0)
            start = time.monotonic()
            self.sample(start)
            for index in range(duration // POLICY["cadence_seconds"]):
                target = start + index * POLICY["cadence_seconds"]
                time.sleep(max(0, target - time.monotonic()))
                slot = index * POLICY["cadence_seconds"]
                if slot and slot % MACOS_MEMORY["sample_seconds"] == 0:
                    self.sample_heap(slot, start)
                self.max_lag = max(self.max_lag, (time.monotonic() - target) * 1000)
                require(self.max_lag <= POLICY["schedule_lag_ms"], "workload-cadence-missed")
                self.requests.extend(self.batch(pool, index))
                if (index + 1) % POLICY["observer_batches"] == 0:
                    self.observe()
                if (index + 1) % POLICY["sample_batches"] == 0:
                    self.sample(start)
                    self.elapsed_seconds = time.monotonic() - start
                    self.progress()
            time.sleep(max(0, start + duration - time.monotonic()))
            self.sample_heap(duration, start)
            self.elapsed_seconds = time.monotonic() - start
            self.sample(start)
        self.phase = "verify-recovery"
        self.progress()
        self.await_value(WRITER, lambda value: value["pending_batches"] == 0 and
                         value["acknowledged_through"] == self.total_requests)
        metrics = RetrievalObservabilityChecks(self).metrics()
        self.telemetry_completed = sum(cell["completed"] for cell in metrics["cells"])
        self.journal_rows = int(self.scalar("SELECT count(*) FROM shin_server.retrieval_journal_attempts"))
        require(self.samples[-1]["knowledge"]["checks_completed"] > self.samples[0]["knowledge"]["checks_completed"],
                "no-health-progress")
        self.stop_receipt = self.stop_measured()
        started = time.monotonic()
        self.start_server()
        self.restart_ms = (time.monotonic() - started) * 1000
        require(identity == self.server_id, "identity-changed-after-restart")
        for name in ORDER:
            require(self.activation(name, name) == self.active_generations[(name, name)], "activation-changed-after-restart")
            self.request_one(name, "brief")
        self.recovered = True
        self.source_preserved = before == inventory([c[key] for c in self.corpora.values()
            for key in ("root", "profiles", "out")] + [self.managed_state_dir])
        require(self.freeze["binaries"] == {name: digest(path) for name, path in
            (("core", self.core), ("server", self.binary), ("observer", self.observer))}
            and self.freeze["harness_sha256"] == digest(Path(__file__)), "executables-changed-during-run")
        self.stop_server(expect_clean=True)

    def cleanup(self):
        try:
            self.stop_server(expect_clean=False)
        finally:
            self.restore_configs()
            if self.provisioned:
                self.compose("stop", "postgres")

    def report(self, failure):
        duration = POLICY["duration_seconds" if self.mode == "soak" else "smoke_seconds"]
        value = {"schema_version": "shin.soak-report.v4", "ok": failure is None, "failure": failure,
            "platform": sys.platform, "memory_acceptance": self.memory_policy, "heap_samples": self.heap_samples,
            "fd_acceptance": FD_ACCEPTANCE,
            "fd_growth": fd_growth(self.samples, duration) if self.samples else None,
            "rss_growth": rss_growth(self.samples, self.memory_policy) if self.samples else None,
            "mode": self.mode, "qualification": "60-minute-soak" if self.mode == "soak" else "smoke-only",
            "policy": POLICY, "freeze": self.freeze, "elapsed_seconds": self.elapsed_seconds,
            "warmup": self.warmup,
            "requests": self.requests, "samples": self.samples, "baseline_requests": self.baseline_requests,
            "max_schedule_lag_ms": self.max_lag, "observer_samples": self.observer_samples,
            "total_requests": self.total_requests, "telemetry_completed": self.telemetry_completed,
            "journal_rows": self.journal_rows, "source_preserved": self.source_preserved,
            "recovered": self.recovered, "stop": self.stop_receipt, "restart_ms": self.restart_ms,
            "boundary": "Frozen component slices, native lexical runtime only. Not full owner-profile or semantic relevance acceptance, production throughput, remote deployment, or universal leak freedom. CPU/RSS refer to Server, not Docker/PostgreSQL/Observer; high-water includes startup."}
        if failure is None:
            try:
                validate_report(value)
            except (IntegrationFailure, KeyError, TypeError, ValueError) as error:
                value.update(ok=False, failure={"stage": "receipt-validation", "type": type(error).__name__, "reason": str(error)})
        self.write("report.json", value)
        self.phase = "passed" if value["ok"] else "failed"
        self.progress()
        return value["ok"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "core", "observer", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--state-root", type=Path, default=BUILD_ROOT / "server-dev")
    parser.add_argument("--mode", choices=("smoke", "soak"), required=True)
    parser.add_argument("--frozen-from", type=Path, help="Reuse the exact smoke corpus and preserved owner profiles")
    args = parser.parse_args()
    args.output = args.output.resolve()
    if not under_build(args.output) or args.output.exists() or not under_build(args.state_root):
        parser.error("new output and isolated state must remain under the development build")
    if args.mode == "soak" and args.frozen_from is None:
        parser.error("hour qualification requires --frozen-from a passing same-input smoke")
    if args.frozen_from:
        args.frozen_from = args.frozen_from.resolve()
        if not under_build(args.frozen_from):
            parser.error("frozen corpus must remain under the development build")
        validate_report(json.loads((args.frozen_from / "report.json").read_text()), allow_legacy=args.mode == "smoke")
    for binary in (args.binary, args.core, args.observer):
        if not binary.is_absolute() or not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error("absolute native executable paths required")
    args.output.mkdir(mode=0o700, parents=True)
    suite = SoakSuite(args.binary, args.core, args.observer, args.state_root, args.output, args.mode, args.frozen_from)
    def interrupted(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    failure = None
    try:
        suite.run()
    except BaseException as error:
        failure = {"stage": suite.phase, "type": type(error).__name__,
                   "reason": str(error) if isinstance(error, IntegrationFailure) else None}
    finally:
        try:
            suite.cleanup()
        except Exception as error:
            failure = failure or {"stage": "cleanup", "type": type(error).__name__}
    ok = suite.report(failure)
    print(json.dumps({"ok": ok, "mode": args.mode, "report": str(suite.report_path), "failure": failure}))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
