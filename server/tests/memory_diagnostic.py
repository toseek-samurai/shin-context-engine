#!/usr/bin/env python3
"""Bounded macOS heap/RSS diagnosis on the frozen soak corpus, never qualification."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from soak_integration_test import (BUILD_ROOT, POLICY, SoakSuite, IntegrationFailure,
                                   RSS_MEMORY, digest, inventory, require, under_build, validate_report)
from operations_budget_integration_test import WRITER

DURATION = 600
IDLE_SECONDS = 60
PROFILE_SLOTS = {90, 180, 300}


class MemoryDiagnostic(SoakSuite):
    def __init__(self, *args, warm_background=False, **kwargs):
        super().__init__(*args, **kwargs)
        self.memory_policy = dict(RSS_MEMORY)
        self.violations, self.profiles = [], []
        self.before = None
        self.workload_seconds = 0
        self.diagnostic_hash = digest(Path(__file__))
        self.warm_background_enabled = warm_background

    def sample(self, start):
        try:
            super().sample(start)
        except IntegrationFailure as error:
            # Observe the known growth failure for a finite ten-minute diagnosis.
            # Absolute RSS, descriptors, telemetry and all other guards still stop.
            if str(error) != "soak-rss-growth-budget":
                raise
            self.violations.append({"reason": str(error), "sample": len(self.samples) - 1})

    def profile(self, label):
        self.phase = "profile-" + label
        self.progress()
        row = {"label": label, "elapsed_seconds": self.elapsed_seconds, "tools": []}
        for tool, options in (("vmmap", ["-summary"]), ("heap", ["--noContent", "-s"])):
            path = self.output / (label + "-" + tool + ".txt")
            descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "w") as output:
                completed = subprocess.run(["/usr/bin/" + tool, *options, str(self.server.pid)],
                    stdout=output, stderr=subprocess.STDOUT, timeout=30, stdin=subprocess.DEVNULL)
            row["tools"].append({"tool": tool, "exit_code": completed.returncode,
                                  "path": str(path), "sha256": digest(path)})
        self.profiles.append(row)
        self.write("profiles.json", self.profiles)
        require(all(t["exit_code"] == 0 for t in row["tools"]), "native-memory-inspection-unavailable")

    def protected_roots(self):
        return [c[key] for c in self.corpora.values() for key in ("root", "profiles", "out")] + [self.managed_state_dir]

    def run(self):
        self.setup()
        self.before = inventory(self.protected_roots())
        with ThreadPoolExecutor(max_workers=4) as pool:
            self.phase = "cold-warm-baseline"
            for index in range(POLICY["warm_batches"]):
                self.baseline_requests.extend(self.batch(pool, index))
            self.observe()
            self.write("baseline.json", self.baseline_requests)
            if self.warm_background_enabled:
                self.warm_background()
            start = time.monotonic()
            self.sample(start)
            self.profile("warm")
            offset = time.monotonic() - start
            for slot in range(DURATION // POLICY["cadence_seconds"]):
                self.phase = "diagnostic-load"
                target = start + offset + slot * POLICY["cadence_seconds"]
                time.sleep(max(0, target - time.monotonic()))
                self.requests.extend(self.batch(pool, slot))
                self.workload_seconds = (slot + 1) * POLICY["cadence_seconds"]
                if (slot + 1) % POLICY["observer_batches"] == 0:
                    self.observe()
                if (slot + 1) % POLICY["sample_batches"] == 0:
                    self.sample(start)
                if slot + 1 in PROFILE_SLOTS:
                    before_profile = time.monotonic()
                    self.profile("load-" + str(self.workload_seconds))
                    offset += time.monotonic() - before_profile
            self.phase = "idle-background-enabled"
            for _ in range(IDLE_SECONDS // 10):
                time.sleep(10)
                self.sample(start)
            self.profile("idle")
        writer = self.await_value(WRITER, lambda value: value["pending_batches"] == 0 and
                                  value["acknowledged_through"] == self.total_requests)
        self.journal_rows = int(self.scalar("SELECT count(*) FROM shin_server.retrieval_journal_attempts"))
        require(writer["new_missing_sequences"] == 0 and self.journal_rows == self.total_requests, "diagnostic-journal-incomplete")
        self.source_preserved = self.before == inventory(self.protected_roots())
        require(self.source_preserved and self.diagnostic_hash == digest(Path(__file__)), "diagnostic-input-drift")
        self.stop_receipt = self.stop_measured()

    def report(self, failure):
        value = {"schema_version": "shin.memory-diagnostic.v1", "ok": failure is None,
            "qualification": False, "failure": failure, "duration_slots_seconds": self.workload_seconds,
            "diagnostic_sha256": self.diagnostic_hash, "freeze": self.freeze,
            "elapsed_seconds": self.elapsed_seconds, "policy": POLICY,
            "budget_violations": self.violations, "requests": self.requests,
            "background_warmup_required": self.warm_background_enabled, "warmup": self.warmup,
            "samples": self.samples, "profiles": self.profiles, "stop": self.stop_receipt,
            "total_requests": self.total_requests, "journal_rows": self.journal_rows,
            "source_preserved": self.source_preserved,
            "boundary": "Instrumented diagnostic only; profiler pauses excluded from request slots, included in wall time. Growth violations retained, other safety guards unchanged. No soak pass, timing qualification, allocator mutation or release approval."}
        self.write("report.json", value)
        self.phase = "diagnostic-complete" if failure is None else "diagnostic-failed"
        self.progress()
        return failure is None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "core", "observer", "output", "frozen-from"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--warm-background", action="store_true")
    args = parser.parse_args()
    require(sys.platform == "darwin", "macos-memory-tools-required")
    args.output, args.frozen_from = args.output.resolve(), args.frozen_from.resolve()
    require(under_build(args.output) and not args.output.exists() and under_build(args.frozen_from), "private-new-diagnostic-output-required")
    validate_report(json.loads((args.frozen_from / "report.json").read_text()), allow_legacy=True)
    args.output.mkdir(mode=0o700, parents=True)
    suite = MemoryDiagnostic(args.binary.resolve(), args.core.resolve(), args.observer.resolve(),
        BUILD_ROOT / "server-dev", args.output, "diagnostic", args.frozen_from, warm_background=args.warm_background)
    def interrupt(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupt)
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
    print(json.dumps({"ok": ok, "qualification": False, "failure": failure, "report": str(suite.report_path)}))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
