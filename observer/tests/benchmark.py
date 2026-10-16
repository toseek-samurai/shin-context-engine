#!/usr/bin/env python3
"""Bounded local Observer watch measurement; measurements are not an SLO."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import select
import signal
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any

from integration_test import Fixture, IntegrationFailure


PS = Path("/bin/ps")
MAX_STATE_BYTES = 256 * 1024
ENVIRONMENT = {"PATH": "", "LC_ALL": os.environ.get("LC_ALL", "C")}
ENVIRONMENT.update({name: os.environ[name] for name in
                    ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS", "ASAN_SYMBOLIZER_PATH") if name in os.environ})


def route(core: Path, fixture: Fixture, command: str, *args: str) -> list[str]:
    return [str(core), "--profile", "observer-fixture", "--profile-dir", str(fixture.profiles),
            "--root", str(fixture.root), "--out-dir", str(fixture.out), command, *args]


def run_json(command: list[str], label: str, *, timeout: float = 30.0, allow_state_changed: bool = False) -> dict[str, Any]:
    completed = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, env=ENVIRONMENT, timeout=timeout, check=False)
    if completed.returncode or completed.stderr:
        if allow_state_changed and completed.returncode == 1 and not completed.stdout:
            try:
                rejected = json.loads(completed.stderr)
            except json.JSONDecodeError:
                rejected = None
            if isinstance(rejected, dict) and rejected.get("ok") is False and rejected.get("error", {}).get("code") == "state-changed":
                return rejected
        raise IntegrationFailure(f"{label} failed: {completed.returncode} {completed.stderr}")
    try:
        value = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise IntegrationFailure(f"{label} did not emit JSON") from error
    if not isinstance(value, dict):
        raise IntegrationFailure(f"{label} emitted a non-object")
    return value


def tree_signature(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted([root, *root.rglob("*")], key=lambda item: item.as_posix()):
        entry = path.lstat()
        relative = "." if path == root else path.relative_to(root).as_posix()
        digest.update(f"{relative}\0{entry.st_mode:o}\0{entry.st_size}\0{entry.st_mtime_ns}\0".encode())
        if path.is_file():
            digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def read_state(fixture: Fixture) -> tuple[dict[str, Any] | None, dict[str, int]]:
    sizes: dict[str, int] = {}
    for entry in fixture.state.iterdir():
        if entry.name not in {"observer.lock", "state.json", "state.pending"}:
            raise IntegrationFailure("unexpected state-store entry")
        try:
            status = entry.lstat()
        except FileNotFoundError:
            if entry.name == "state.pending":
                continue
            raise
        if not stat.S_ISREG(status.st_mode) or stat.S_IMODE(status.st_mode) != 0o600 or status.st_size > MAX_STATE_BYTES:
            raise IntegrationFailure("unsafe or oversized state-store entry")
        sizes[entry.name] = status.st_size
    path = fixture.state / "state.json"
    if not path.exists():
        return None, sizes
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise IntegrationFailure("state.json is not complete JSON") from error
    if not isinstance(state, dict):
        raise IntegrationFailure("state.json is not an object")
    return state, sizes


def rss_kib(pid: int) -> int:
    completed = subprocess.run([str(PS), "-o", "rss=", "-p", str(pid)], stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=ENVIRONMENT, check=False)
    if completed.returncode or completed.stderr or not completed.stdout.strip().isdigit():
        raise IntegrationFailure("/bin/ps could not measure the owned Observer RSS")
    return int(completed.stdout.strip())


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    return round(ordered[max(0, math.ceil(len(ordered) * fraction) - 1)], 3)


def rss_summary(samples: list[int]) -> dict[str, Any]:
    midpoint = len(samples) // 2

    def ranges(values: list[int]) -> dict[str, int] | None:
        return {"min": min(values), "max": max(values)} if values else None

    return {"method": "/bin/ps -o rss= -p PID; KiB", "samples": len(samples),
            "min": min(samples) if samples else None, "max": max(samples) if samples else None,
            "last": samples[-1] if samples else None, "first_half": ranges(samples[:midpoint]),
            "second_half": ranges(samples[midpoint:])}


def write_report(path: Path, value: dict[str, Any]) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as output:
        json.dump(value, output, separators=(",", ":"), sort_keys=True)
        output.write("\n")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--observer", required=True)
    parser.add_argument("--core", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--documents", type=int, default=1000)
    parser.add_argument("--samples", type=int, default=50)
    parser.add_argument("--outbox-preload", type=int, default=0)
    return parser.parse_args(argv)


def executable(value: str, name: str) -> Path:
    path = Path(value)
    if not path.is_absolute() or not path.is_file() or not os.access(path, os.X_OK):
        raise IntegrationFailure(f"--{name} must name an absolute executable")
    return path.resolve()


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    process: subprocess.Popen[str] | None = None
    try:
        observer, core = executable(args.observer, "observer"), executable(args.core, "core")
        report = Path(args.report)
        if not 1 <= args.documents <= 10000 or not 1 <= args.samples <= 1000 or not 0 <= args.outbox_preload <= 128:
            raise IntegrationFailure("--documents must be 1..10000, --samples 1..1000 and --outbox-preload 0..128")
        if not report.is_absolute() or report.exists() or not report.parent.is_dir() or not PS.is_file():
            raise IntegrationFailure("--report must be a new absolute path and /bin/ps must exist")
        with tempfile.TemporaryDirectory(prefix="shin-observer-benchmark-") as temporary:
            fixture = Fixture(Path(temporary).resolve(), "fixture")
            (fixture.root / "docs").mkdir(mode=0o700)
            fixture.write_source("# Observer benchmark\n\nDocs are indexed below.\n")
            for index in range(args.documents):
                (fixture.root / "docs" / f"doc-{index:04d}.md").write_text(f"# Document {index}\nFixture content.\n", encoding="utf-8")
            config = fixture.config_data()
            config.update(max_snapshot_bytes=128 * 1024 * 1024, max_file_bytes=64 * 1024 * 1024)
            if args.outbox_preload:
                config["outbox"].update(enabled=True, max_entries=128, max_bytes=192 * 1024)
            fixture.write_config(config)
            queued_entries = None
            for index in range(args.outbox_preload):
                fixture.write_source(f"# Observer benchmark\n\nPreload observation {index}.\n")
                sampled = run_json([str(observer), "sample", "--config", str(fixture.config)], "outbox preload")
                queued_entries = sampled["result"]["outbox"]["entries"]
            built = run_json(route(core, fixture, "build"), "explicit Core build")
            if built.get("tool") != "shin-context" or built.get("ok") is not True:
                raise IntegrationFailure("explicit Core build did not succeed")
            source_before, out_before = tree_signature(fixture.root), tree_signature(fixture.out)
            budget_s = min(600.0, max(30.0, args.samples * (0.2 + args.documents * 0.0002) + 10.0))
            process = subprocess.Popen([str(observer), "watch", "--config", str(fixture.config), "--samples", str(args.samples)],
                                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                       env=ENVIRONMENT, start_new_session=True)
            watch_started = time.monotonic()
            deadline, rss, seen, state_sizes = time.monotonic() + budget_s, [], {}, {}
            metric_read_conflicts = 0
            final_output: tuple[str, str] | None = None
            while process.poll() is None:
                if time.monotonic() > deadline:
                    raise IntegrationFailure("watch exceeded its bounded operator budget")
                readable, _, _ = select.select([process.stdout], [], [], 0)  # type: ignore[list-item]
                if readable:
                    try:
                        # A bounded final state can exceed pipe capacity; drain while waiting for exit.
                        final_output = process.communicate(timeout=5.0)
                    except subprocess.TimeoutExpired as error:
                        raise IntegrationFailure("watch emitted output before bounded completion") from error
                    break
                state, sizes = read_state(fixture)
                state_sizes.update({name: max(size, state_sizes.get(name, 0)) for name, size in sizes.items()})
                measured = run_json([str(observer), "metrics", "--config", str(fixture.config)], "Observer metrics",
                                    timeout=5.0, allow_state_changed=True)
                status = measured.get("result")
                if measured.get("ok") is False:
                    metric_read_conflicts += 1
                elif not isinstance(status, dict) or status.get("live_source_verified") is not False:
                    raise IntegrationFailure("status did not remain metadata-only")
                if state is not None and state.get("sequence", 0) > args.outbox_preload and isinstance(state.get("observation"), dict):
                    observation = state["observation"]
                    if observation.get("status") != "current" or observation.get("reason_code") is not None or \
                            observation.get("source_counts", {}).get("documents") != args.documents + 1:
                        raise IntegrationFailure("watch recorded a non-current or incomplete benchmark observation")
                    sequence = state.get("sequence")
                    if not isinstance(sequence, int) or (seen and sequence < max(seen)):
                        raise IntegrationFailure("observed watch sequence is not unique and monotonic")
                    if sequence not in seen:
                        seen[sequence] = observation.get("duration_ms")
                try:
                    measured_rss = rss_kib(process.pid)
                    if measured_rss > 0:
                        rss.append(measured_rss)
                except IntegrationFailure:
                    if process.poll() is None:
                        raise
                time.sleep(0.04)
            stdout, stderr = final_output if final_output is not None else process.communicate(timeout=5.0)
            exit_code = process.returncode
            process = None
            if exit_code != 0 or stderr or not stdout:
                raise IntegrationFailure("watch did not provide a clean final summary")
            final = json.loads(stdout)
            state, sizes = read_state(fixture)
            state_sizes.update({name: max(size, state_sizes.get(name, 0)) for name, size in sizes.items()})
            final_status = run_json([str(observer), "status", "--config", str(fixture.config)], "final Observer status").get("result")
            if not isinstance(final, dict) or final.get("ok") is not True or not isinstance(state, dict) or \
                    state.get("lifecycle") != "stopped" or state.get("sequence") != args.samples + args.outbox_preload or \
                    not isinstance(state.get("observation"), dict) or state["observation"].get("status") != "current" or \
                    (fixture.state / "state.pending").exists() or not isinstance(final_status, dict) or \
                    final_status.get("writer_lock_held") is not False or final_status.get("state", {}).get("observation", {}).get("status") != "current":
                raise IntegrationFailure("watch final state did not contain exactly the requested current samples")
            if tree_signature(fixture.root) != source_before or tree_signature(fixture.out) != out_before:
                raise IntegrationFailure("watch mutated source or published Core output")
            if queued_entries is not None and state["outbox"]["entries"][:len(queued_entries)] != queued_entries:
                raise IntegrationFailure("watch changed or lost preloaded accepted outbox entries")
            seen[state["sequence"]] = state["observation"]["duration_ms"]
            durations = [float(value) for value in seen.values() if isinstance(value, (int, float))]
            write_report(report, {"schema_version": "shin.observer-benchmark-report.v1", "ok": True,
                                  "documents": args.documents, "samples_requested": args.samples,
                                  "sequence_contract": {"initial": args.outbox_preload, "final": args.samples + args.outbox_preload},
                                  "outbox": None if state["outbox"] is None else {
                                      "preload_samples": args.outbox_preload, "pending_entries": len(state["outbox"]["entries"]),
                                      "pending_entry_bytes": state["outbox"]["payload_bytes"],
                                      "capacity_rejections": state["outbox"]["rejected_observations"]},
                                  "observed_samples": len(seen), "observation_duration_ms": {"p50": percentile(durations, .50), "p95": percentile(durations, .95)},
                                  "elapsed_ms": round((time.monotonic() - watch_started) * 1000, 3), "operator_budget_s": budget_s,
                                  "state_file_max_bytes": state_sizes, "rss_kib": rss_summary(rss),
                                  "metric_read_conflicts": metric_read_conflicts,
                                  "note": "Local measurements include polling, waits and fsync; zero-RSS exiting processes are omitted. These are not an SLO or leak claim."})
        return 0
    except (IntegrationFailure, OSError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(f"error: Observer benchmark failed: {error}", file=sys.stderr)
        return 1
    finally:
        if process is not None and process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5.0)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
