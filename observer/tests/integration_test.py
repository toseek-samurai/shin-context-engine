#!/usr/bin/env python3
"""Black-box integration harness for the standalone Shin Observer CLI.

The harness intentionally owns every fixture below a temporary directory outside
the repository.  It invokes only the absolute Observer and Core binaries with
an empty PATH, so a passing run cannot depend on Git, a shell, or ambient tools.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
OBSERVER_VERSION = (REPOSITORY_ROOT / "observer" / "VERSION").read_text(encoding="utf-8").strip()
OBSERVER_STABILITY = "development" if "-dev." in OBSERVER_VERSION or "-rc." in OBSERVER_VERSION else "stable"
CORE_VERSION = (REPOSITORY_ROOT / "VERSION").read_text(encoding="utf-8").strip()
RESPONSE_SCHEMA = "shin.observer-response.v1"
STATE_SCHEMA = "shin.observer-state.v3"
REPORT_SCHEMA = "shin.observer-integration-report.v1"
CONFIG_SCHEMA = "shin.observer-config.v2"
SECRET_TEXT = "observer-fixture-secret-6bde0ab8b0e44e60"


class IntegrationFailure(RuntimeError):
    pass


def assert_observer_envelope(value: Any, *, expected_ok: bool) -> dict[str, Any]:
    """Validate the complete public Observer response envelope."""
    common = {"schema_version", "tool", "version", "core_version", "stability", "ok"}
    required = common | ({"result"} if expected_ok else {"error"})
    if not isinstance(value, dict) or set(value) != required or value.get("schema_version") != RESPONSE_SCHEMA or \
            value.get("tool") != "shin-observer" or value.get("version") != OBSERVER_VERSION or \
            value.get("core_version") != CORE_VERSION or value.get("stability") != OBSERVER_STABILITY or \
            value.get("ok") is not expected_ok:
        raise IntegrationFailure("Observer response does not match shin.observer-response.v1")
    payload = value["result"] if expected_ok else value["error"]
    if expected_ok:
        if not isinstance(payload, dict):
            raise IntegrationFailure("Observer success envelope result is not an object")
    elif not isinstance(payload, dict) or set(payload) != {"code", "message"} or \
            not isinstance(payload["code"], str) or not payload["code"] or \
            not isinstance(payload["message"], str) or not payload["message"]:
        raise IntegrationFailure("Observer error envelope is malformed")
    return payload


def canonical(path: Path) -> Path:
    return path.resolve(strict=False)


def write_private_json(path: Path, value: dict[str, Any]) -> None:
    path.write_text(json.dumps(value, sort_keys=True) + "\n", encoding="utf-8")
    path.chmod(0o600)


def tree_metadata(root: Path) -> list[tuple[str, int, int, int]]:
    """Return mtime-oriented metadata without reading fixture source contents."""
    rows: list[tuple[str, int, int, int]] = []
    for path in sorted([root, *root.rglob("*")], key=lambda item: item.as_posix()):
        entry = path.lstat()
        rows.append((str(path.relative_to(root)) if path != root else ".",
                     stat.S_IFMT(entry.st_mode), entry.st_size, entry.st_mtime_ns))
    return rows


def state_from_result(result: Any) -> dict[str, Any]:
    if isinstance(result, dict) and result.get("schema_version") == STATE_SCHEMA:
        return result
    if isinstance(result, dict) and isinstance(result.get("state"), dict):
        state = result["state"]
        if state.get("schema_version") == STATE_SCHEMA:
            return state
    raise IntegrationFailure("Observer response did not contain the persisted state")


class Fixture:
    def __init__(self, parent: Path, name: str) -> None:
        self.base = parent / name
        self.root = self.base / "source"
        self.out = self.base / "index"
        self.profiles = self.base / "profiles"
        self.state = self.base / "state"
        self.config = self.base / "observer.json"
        for directory in (self.base, self.root, self.out, self.profiles, self.state):
            directory.mkdir(parents=True, exist_ok=False)
            directory.chmod(0o700)
        self.profile_path = self.profiles / "observer-fixture.json"
        self.write_profile()
        self.write_source("Initial observer fixture document.\n")
        self.write_config()

    def write_source(self, text: str) -> None:
        (self.root / "README.md").write_text(text, encoding="utf-8")

    def write_profile(self, *, source_label: str = "Observer fixture documentation") -> None:
        profile = {
            "schemaVersion": "shin.profile.v1",
            "document": "shin-profile-observer-fixture",
            "status": "active",
            "summary": "Small external fixture for Observer black-box integration.",
            "id": "observer-fixture",
            "sourceLabel": source_label,
            "rootFiles": ["README.md"],
            "optionalRootFiles": [],
            "sourceExtensions": [".md", ".json"],
            "sourceFactRoots": [],
            "sourceFactExtensions": [],
            "sourceFactExtractors": ["language"],
            "agentStartPaths": ["README.md"],
            "bootstrapOnly": True,
            "markdownMaxHeadingLevel": 4,
            "markdownChunkLines": 16,
            "markdownChunkMinLines": 1,
            "markdownChunkChars": 1200,
            "jsonKeyMaxDepth": 3,
            "jsonKeyMaxSectionsPerFile": 32,
            "jsonlMaxRowsPerFile": 100,
            "sortTopics": True,
            "defaultTopic": "documentation",
            "evalCases": [],
            "topicRules": [{
                "id": "documentation",
                "description": "Fixture documentation.",
                "keywords": ["fixture", "observer", "documentation"],
            }],
        }
        self.profile_path.write_text(json.dumps(profile, sort_keys=True) + "\n", encoding="utf-8")

    def config_data(self) -> dict[str, Any]:
        return {
            "schema_version": CONFIG_SCHEMA,
            "observer_id": "observer-test-agent",
            "source_host_id": "observer-test-host",
            "project_id": "observer-test-project",
            "workspace_id": "observer-test-workspace",
            "root": str(canonical(self.root)),
            "profile": "observer-fixture",
            "profile_dir": str(canonical(self.profiles)),
            "out_dir": str(canonical(self.out)),
            "state_dir": str(canonical(self.state)),
            "poll_interval_ms": 100,
            "max_snapshot_bytes": 1024 * 1024,
            "max_file_bytes": 256 * 1024,
            "outbox": {"enabled": False, "max_entries": 64, "max_bytes": 128 * 1024},
        }

    def write_config(self, data: dict[str, Any] | None = None) -> None:
        write_private_json(self.config, data or self.config_data())


class ObserverIntegrationSuite:
    def __init__(self, observer: Path, core: Path, report_path: Path) -> None:
        self.observer = observer
        self.core = core
        self.report_path = report_path
        self.started_at = time.monotonic()
        self.cases: list[dict[str, Any]] = []
        self.latencies_ms: list[float] = []
        self.active_processes: list[subprocess.Popen[str]] = []
        self.fixture_root: Path | None = None

    @property
    def empty_path_environment(self) -> dict[str, str]:
        # Core and Observer must be self-contained native binaries.  Keep locale only
        # when the caller selected one; no tool lookup may be available to either CLI.
        environment = {"PATH": "", "LC_ALL": os.environ.get("LC_ALL", "C")}
        if "TZ" in os.environ:
            environment["TZ"] = os.environ["TZ"]
        for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS", "ASAN_SYMBOLIZER_PATH"):
            if name in os.environ:
                environment[name] = os.environ[name]
        return environment

    def record(self, name: str, operation: Callable[[], None]) -> None:
        started = time.monotonic()
        try:
            operation()
        except BaseException as error:
            self.cases.append({"name": name, "ok": False, "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
                               "failure": type(error).__name__})
            raise
        self.cases.append({"name": name, "ok": True, "elapsed_ms": round((time.monotonic() - started) * 1000, 3)})

    def run_json(self, command: list[str], name: str, *, expect_success: bool = True,
                 timeout: float = 30.0) -> tuple[dict[str, Any], subprocess.CompletedProcess[str]]:
        completed = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, env=self.empty_path_environment,
                                   timeout=timeout, check=False)
        if expect_success and completed.returncode != 0:
            raise IntegrationFailure(f"{name} failed with exit status {completed.returncode}: {completed.stderr}")
        if not expect_success and completed.returncode == 0:
            raise IntegrationFailure(f"{name} unexpectedly succeeded")
        output = completed.stdout if expect_success else completed.stderr
        try:
            value = json.loads(output)
        except json.JSONDecodeError as error:
            raise IntegrationFailure(f"{name} did not emit one JSON document on the expected stream") from error
        if not isinstance(value, dict):
            raise IntegrationFailure(f"{name} JSON document is not an object")
        return value, completed

    def assert_success_envelope(self, value: dict[str, Any]) -> dict[str, Any]:
        return assert_observer_envelope(value, expected_ok=True)

    @staticmethod
    def assert_error_envelope(value: dict[str, Any]) -> None:
        assert_observer_envelope(value, expected_ok=False)

    def observer_command(self, fixture: Fixture, command: str, *arguments: str,
                         expect_success: bool = True) -> tuple[dict[str, Any], subprocess.CompletedProcess[str]]:
        value, completed = self.run_json([str(self.observer), command, "--config", str(fixture.config), *arguments],
                                         f"Observer {command}", expect_success=expect_success)
        if expect_success:
            if completed.stderr:
                raise IntegrationFailure(f"Observer {command} wrote stderr on success")
            return self.assert_success_envelope(value), completed
        if completed.stdout:
            raise IntegrationFailure(f"Observer {command} wrote stdout on failure")
        self.assert_error_envelope(value)
        return value, completed

    def core_command(self, fixture: Fixture, command: str, *arguments: str,
                     expect_success: bool = True) -> dict[str, Any]:
        value, _ = self.run_json([str(self.core), "--profile", "observer-fixture", "--profile-dir", str(fixture.profiles),
                                  "--root", str(fixture.root), "--out-dir", str(fixture.out), command, *arguments],
                                 f"Core {command}", expect_success=expect_success)
        if value.get("tool") != "shin-context":
            raise IntegrationFailure("Core command did not identify shin-context")
        return value

    def core_route(self, fixture: Fixture, command: str, *arguments: str) -> list[str]:
        return [str(self.core), "--profile", "observer-fixture", "--profile-dir", str(fixture.profiles),
                "--root", str(fixture.root), "--out-dir", str(fixture.out), command, *arguments]

    def core_stale_worktree(self, fixture: Fixture) -> None:
        completed = subprocess.run(self.core_route(fixture, "worktree-diff", "--strict"), stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                   env=self.empty_path_environment, timeout=30.0, check=False)
        if completed.returncode != 1 or completed.stderr:
            raise IntegrationFailure("stale Core worktree-diff did not use its negative stdout result")
        try:
            result = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise IntegrationFailure("stale Core worktree-diff did not emit JSON on stdout") from error
        if not isinstance(result, dict) or result.get("tool") != "shin-context" or result.get("ok") is not False or \
                result.get("current") is not False:
            raise IntegrationFailure("stale Core worktree-diff result is malformed")

    def core_currentness_error(self, fixture: Fixture) -> None:
        completed = subprocess.run(self.core_route(fixture, "brief", "observer fixture", "--require-current"),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                   env=self.empty_path_environment, timeout=30.0, check=False)
        if completed.returncode == 0 or completed.stdout:
            raise IntegrationFailure("stale Core Brief did not use its stderr error envelope")
        try:
            error = json.loads(completed.stderr)
        except json.JSONDecodeError as parse_error:
            raise IntegrationFailure("stale Core Brief did not emit a JSON error envelope") from parse_error
        expected = {"schema_version", "tool", "command", "ok", "error"}
        if not isinstance(error, dict) or set(error) != expected or error.get("schema_version") != "shin.error.v1" or \
                error.get("tool") != "shin-context" or error.get("command") != "brief" or error.get("ok") is not False or \
                not isinstance(error.get("error"), dict):
            raise IntegrationFailure("Core stderr error envelope is malformed")

    def status(self, fixture: Fixture) -> dict[str, Any]:
        result, _ = self.observer_command(fixture, "status")
        expected = {"state", "writer_lock_held", "live_source_verified", "observation_age_ms", "observation_age",
                    "observer_state_mutated", "consumer_mutated", "index_mutated", "synchronization"}
        if set(result) != expected or result["live_source_verified"] is not False or not isinstance(result["writer_lock_held"], bool):
            raise IntegrationFailure("Observer status is not the required read-only status result")
        if result["observer_state_mutated"] is not False or result["consumer_mutated"] is not False or \
                result["index_mutated"] is not False or result["synchronization"] != "explicit-metadata-delivery":
            raise IntegrationFailure("Observer status did not retain its read-only local-only boundary")
        if result["state"] is not None and not isinstance(result["state"], dict):
            raise IntegrationFailure("Observer status state is neither null nor an object")
        if result["observation_age_ms"] is not None and (not isinstance(result["observation_age_ms"], (int, float)) or
                                                          result["observation_age_ms"] < 0):
            raise IntegrationFailure("Observer status age is not null or a non-negative number")
        if result["observation_age"] not in {"within-ttl", "expired", "time-uncertain", "unavailable"}:
            raise IntegrationFailure("Observer status age classification is invalid")
        return result

    @staticmethod
    def is_fingerprint(value: Any) -> bool:
        return isinstance(value, str) and len(value) == 24 and value.startswith("fnv1a64:") and \
            all(character in "0123456789abcdef" for character in value[8:])

    @classmethod
    def assert_state_shape(cls, state: dict[str, Any], fixture: Fixture) -> None:
        required = {"schema_version", "binding_key", "binding", "root_identity", "sequence", "epoch", "lifecycle",
                    "previous_run_interrupted", "observation", "producer_version", "core_version", "outbox", "metrics"}
        binding_fields = {"observer_id", "source_host_id", "project_id", "workspace_id", "root", "profile",
                          "profile_dir", "out_dir", "state_dir", "state_identity", "outbox_policy"}
        if set(state) != required or state["schema_version"] != STATE_SCHEMA or not isinstance(state["binding_key"], str) or \
                len(state["binding_key"]) != 64 or not isinstance(state["binding"], dict) or \
                set(state["binding"]) != binding_fields or not isinstance(state["root_identity"], str) or \
                not state["root_identity"] or not isinstance(state["sequence"], int) or state["sequence"] < 1 or \
                not isinstance(state["epoch"], str) or len(state["epoch"]) != 32 or \
                not all(character in "0123456789abcdef" for character in state["epoch"]) or \
                state["lifecycle"] not in {"stopped", "waiting", "assessing"} or \
                not isinstance(state["previous_run_interrupted"], bool) or state["producer_version"] != OBSERVER_VERSION or \
                state["core_version"] != CORE_VERSION:
            raise IntegrationFailure("persisted state does not match shin.observer-state.v1")
        binding = state["binding"]
        configured = fixture.config_data()
        for key in ("observer_id", "source_host_id", "project_id", "workspace_id", "root", "profile", "profile_dir", "out_dir", "state_dir"):
            if binding[key] != configured[key]:
                raise IntegrationFailure("persisted state binding does not match the active Observer configuration")
        if not isinstance(binding["state_identity"], str) or not binding["state_identity"]:
            raise IntegrationFailure("persisted state does not bind the private state directory identity")

        observation = state["observation"]
        expected_observation = {"status", "reason_code", "started_at_ms", "snapshot_id", "worktree_id",
                                "snapshot_current", "profile_config_hash", "source_counts", "changes", "details_omitted",
                                "policy", "verification_level", "source_mutated", "index_mutated", "git_required",
                                "readiness_assessed", "observed_at_ms", "duration_ms"}
        count_fields = {"documents", "source_facts", "candidate_files", "capped_files", "skipped_files"}
        change_fields = {"metadata_changed", "documents_added", "documents_removed", "documents_changed",
                         "source_facts_added", "source_facts_removed", "source_facts_changed", "total_changes"}
        if not isinstance(observation, dict) or set(observation) - {"diagnostic"} != expected_observation or \
                observation["status"] not in {"current", "stale", "unpublished", "incomplete", "error"} or \
                (observation["reason_code"] is not None and (not isinstance(observation["reason_code"], str) or not observation["reason_code"])) or \
                any(observation[key] is not None and not cls.is_fingerprint(observation[key])
                    for key in ("snapshot_id", "worktree_id", "profile_config_hash")) or \
                (observation["snapshot_current"] is not None and not isinstance(observation["snapshot_current"], bool)) or \
                not all(isinstance(observation[key], int) and observation[key] >= 0
                        for key in ("observed_at_ms", "started_at_ms", "details_omitted")) or \
                not isinstance(observation["duration_ms"], (int, float)) or observation["duration_ms"] < 0 or \
                observation["policy"] != "periodic-profile-content-comparison" or \
                observation["verification_level"] != "local-source-observed" or observation["source_mutated"] is not False or \
                observation["index_mutated"] is not False or observation["git_required"] is not False or observation["readiness_assessed"] is not False:
            raise IntegrationFailure("persisted observation does not match the bounded Observer contract")
        if observation["source_counts"] is not None and (not isinstance(observation["source_counts"], dict) or \
                set(observation["source_counts"]) != count_fields or not all(isinstance(value, int) and value >= 0
                for value in observation["source_counts"].values())):
            raise IntegrationFailure("persisted observation source counts are invalid")
        if observation["changes"] is not None and (not isinstance(observation["changes"], dict) or \
                set(observation["changes"]) != change_fields or not all(isinstance(value, int) and value >= 0
                for value in observation["changes"].values())):
            raise IntegrationFailure("persisted observation change counts are invalid")
        if observation["status"] == "current" and (observation["snapshot_current"] is not True or
                                                     observation["reason_code"] is not None or observation["changes"] is None or
                                                     observation["changes"]["total_changes"] != 0):
            raise IntegrationFailure("current observation is internally inconsistent")
        if observation["status"] == "error" and any(observation[key] is not None for key in
                                                       ("snapshot_id", "worktree_id", "snapshot_current", "profile_config_hash",
                                                        "source_counts", "changes")):
            raise IntegrationFailure("error observation retained computed source or snapshot metadata")

    def sample_state(self, fixture: Fixture) -> dict[str, Any]:
        started = time.monotonic()
        result, _ = self.observer_command(fixture, "sample")
        self.latencies_ms.append(round((time.monotonic() - started) * 1000, 3))
        state = state_from_result(result)
        self.assert_state_shape(state, fixture)
        entries = {entry.name: entry for entry in fixture.state.iterdir()}
        if set(entries) != {"observer.lock", "state.json"}:
            raise IntegrationFailure("Observer state store retained more than its latest state and lock")
        for entry in entries.values():
            if not entry.is_file() or stat.S_IMODE(entry.lstat().st_mode) != 0o600:
                raise IntegrationFailure("Observer state store contains an unsafe latest-state artifact")
        status = self.status(fixture)
        if status["state"] != state:
            raise IntegrationFailure("sample did not return the state it persisted")
        return state

    def build_core(self, fixture: Fixture) -> None:
        result = self.core_command(fixture, "build")
        if result.get("ok") is not True:
            raise IntegrationFailure("explicit Core build did not succeed")

    def core_worktree(self, fixture: Fixture) -> dict[str, Any]:
        result = self.core_command(fixture, "worktree-diff", "--strict")
        if result.get("ok") is not True or result.get("current") is not True or not isinstance(result.get("snapshot"), dict) or \
                not isinstance(result["snapshot"].get("snapshot_id"), str) or \
                not isinstance(result.get("worktree"), dict) or not isinstance(result["worktree"].get("worktree_id"), str):
            raise IntegrationFailure("Core worktree-diff did not establish a current snapshot binding")
        return result

    def start_watch(self, fixture: Fixture, *arguments: str) -> subprocess.Popen[str]:
        process = subprocess.Popen([str(self.observer), "watch", "--config", str(fixture.config), *arguments],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                   env=self.empty_path_environment, start_new_session=True)
        self.active_processes.append(process)
        return process

    def wait_for_lifecycle(self, fixture: Fixture, wanted: set[str], process: subprocess.Popen[str]) -> dict[str, Any]:
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            if process.poll() is not None:
                stdout, stderr = process.communicate(timeout=1.0)
                raise IntegrationFailure(f"watch exited before reaching {wanted}: {process.returncode}, {stdout}, {stderr}")
            status = self.status(fixture)
            state = status["state"]
            if isinstance(state, dict) and state.get("lifecycle") in wanted:
                self.assert_state_shape(state, fixture)
                return state
            time.sleep(0.04)
        raise IntegrationFailure(f"watch did not persist lifecycle {wanted}")

    def finish_watch(self, process: subprocess.Popen[str], *, signal_number: int | None = signal.SIGTERM,
                     expect_clean: bool = True) -> tuple[dict[str, Any] | None, str]:
        if signal_number is not None and process.poll() is None:
            os.killpg(process.pid, signal_number)
        try:
            stdout, stderr = process.communicate(timeout=15.0)
        except subprocess.TimeoutExpired as error:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5.0)
            raise IntegrationFailure("owned Observer watch did not stop") from error
        finally:
            if process in self.active_processes:
                self.active_processes.remove(process)
        if expect_clean and process.returncode != 0:
            raise IntegrationFailure(f"watch did not exit cleanly after SIGTERM: {stderr}")
        if not expect_clean:
            return None, stderr
        try:
            response = json.loads(stdout)
        except json.JSONDecodeError as error:
            raise IntegrationFailure("watch did not emit exactly one final JSON summary") from error
        if not isinstance(response, dict):
            raise IntegrationFailure("watch final summary is not a JSON object")
        result = self.assert_success_envelope(response)
        return state_from_result(result), stderr

    def check_version_and_invalid_watch_bounds(self) -> None:
        result, _ = self.run_json([str(self.observer), "--version"], "Observer --version")
        self.assert_success_envelope(result)
        core, _ = self.run_json([str(self.core), "--version"], "Core --version")
        if core.get("tool") != "shin-context" or core.get("version") != CORE_VERSION or core.get("ok") is not True:
            raise IntegrationFailure("--core did not identify the required development Core")
        with tempfile.TemporaryDirectory(prefix="shin-observer-invalid-watch-") as temporary:
            fixture = Fixture(Path(temporary), "fixture")
            for samples in ("0", "1001"):
                _, completed = self.observer_command(fixture, "watch", "--samples", samples, expect_success=False)
                if completed.stdout:
                    raise IntegrationFailure("invalid watch bounds wrote stdout")

    def check_unpublished_and_current_parity(self) -> None:
        assert self.fixture_root is not None
        fixture = Fixture(self.fixture_root, "parity")
        root_before, out_before = tree_metadata(fixture.root), tree_metadata(fixture.out)
        initial = self.sample_state(fixture)
        observation = initial["observation"]
        if observation["status"] != "unpublished" or observation["snapshot_id"] is not None or observation["snapshot_current"] is not None:
            raise IntegrationFailure("sample before an index did not persist an unpublished observation")
        if tree_metadata(fixture.root) != root_before or tree_metadata(fixture.out) != out_before:
            raise IntegrationFailure("unpublished sample modified source or index output")
        if any("shin" in path.name.lower() for path in [*fixture.root.rglob("*"), *fixture.out.rglob("*")]):
            raise IntegrationFailure("Observer created a lock or state artifact under source or output")

        self.build_core(fixture)
        current = self.sample_state(fixture)
        core = self.core_worktree(fixture)
        observation = current["observation"]
        if observation["status"] != "current" or observation["snapshot_current"] is not True or \
                observation["snapshot_id"] != core["snapshot"]["snapshot_id"] or observation["worktree_id"] != core["worktree"]["worktree_id"]:
            raise IntegrationFailure("Observer current observation does not match Core worktree-diff")

        output_before = tree_metadata(fixture.out)
        fixture.write_source("Changed source after explicit Core build.\n")
        stale = self.sample_state(fixture)
        if stale["observation"]["status"] != "stale" or stale["observation"]["snapshot_current"] is not False:
            raise IntegrationFailure("source edit did not produce a stale observation")
        self.core_stale_worktree(fixture)
        self.core_currentness_error(fixture)
        if tree_metadata(fixture.out) != output_before:
            raise IntegrationFailure("Observer rebuilt or mutated the Core output after a stale sample")
        self.build_core(fixture)
        rebuilt = self.sample_state(fixture)
        if rebuilt["observation"]["status"] != "current":
            raise IntegrationFailure("explicit Core rebuild was not observed as current")

        source_before = tree_metadata(fixture.root)
        out_before = tree_metadata(fixture.out)
        status = self.status(fixture)
        if status["live_source_verified"] or status["writer_lock_held"]:
            raise IntegrationFailure("status made a live source freshness or writer claim while idle")
        if tree_metadata(fixture.root) != source_before or tree_metadata(fixture.out) != out_before:
            raise IntegrationFailure("status mutated source or Core output")

    def check_watch_lock_recovery_and_ttl(self) -> None:
        assert self.fixture_root is not None
        fixture = Fixture(self.fixture_root, "watch")
        self.build_core(fixture)
        self.sample_state(fixture)

        watch = self.start_watch(fixture)
        self.wait_for_lifecycle(fixture, {"waiting"}, watch)
        if self.status(fixture)["writer_lock_held"] is not True:
            raise IntegrationFailure("watch waiting state did not retain the exclusive writer lock")
        readable, _, _ = select.select([watch.stdout], [], [], 0)  # type: ignore[list-item]
        if readable:
            raise IntegrationFailure("watch wrote a per-sample stdout response")
        self.observer_command(fixture, "sample", expect_success=False)
        brief = self.core_command(fixture, "brief", "observer fixture", "--require-current")
        if brief.get("ok") is not True:
            raise IntegrationFailure("local Core brief was blocked by an idle Observer watch")
        graceful, _ = self.finish_watch(watch)
        assert graceful is not None
        self.assert_state_shape(graceful, fixture)
        if graceful["lifecycle"] != "stopped":
            raise IntegrationFailure("graceful watch stop did not persist stopped lifecycle")
        stopped_status = self.status(fixture)
        if stopped_status["writer_lock_held"]:
            raise IntegrationFailure("graceful watch left the writer lock held")

        interrupted = self.start_watch(fixture)
        before_kill = self.wait_for_lifecycle(fixture, {"waiting"}, interrupted)
        self.finish_watch(interrupted, signal_number=signal.SIGKILL, expect_clean=False)
        replacement = self.start_watch(fixture, "--samples", "1")
        _, stderr = self.finish_watch(replacement, signal_number=None)
        if stderr:
            raise IntegrationFailure("finite watch wrote diagnostics on successful completion")
        after = self.status(fixture)["state"]
        if not isinstance(after, dict):
            raise IntegrationFailure("watch restart did not preserve state")
        self.assert_state_shape(after, fixture)
        if after["epoch"] == before_kill["epoch"] or after["sequence"] <= before_kill["sequence"] or \
                after["previous_run_interrupted"] is not True or after["lifecycle"] != "stopped":
            raise IntegrationFailure("SIGKILL recovery did not record a new interrupted writer epoch")

        ttl_seconds = max(3 * 0.100, 1.0)
        time.sleep(ttl_seconds + 0.15)
        ttl_status = self.status(fixture)
        if ttl_status["observation_age"] != "expired" or ttl_status["live_source_verified"]:
            raise IntegrationFailure("status TTL after stop did not remain metadata-only and expire")

    def check_binding_and_root_failures(self) -> None:
        assert self.fixture_root is not None
        fixture = Fixture(self.fixture_root, "root-binding")
        bound = self.sample_state(fixture)
        original_identity = bound["root_identity"]
        moved = fixture.base / "source-original"
        fixture.root.rename(moved)
        fixture.root.mkdir(mode=0o700)
        fixture.write_source("Replacement root must not rebind automatically.\n")
        replaced = self.sample_state(fixture)
        if replaced["observation"]["status"] != "error" or replaced["observation"]["reason_code"] != "root-identity-changed" or \
                replaced["root_identity"] != original_identity:
            raise IntegrationFailure("replacement root was not rejected without automatic rebinding")
        shutil.rmtree(fixture.root)
        missing = self.sample_state(fixture)
        if missing["observation"]["status"] != "error" or missing["observation"]["reason_code"] != "root-unavailable" or \
                missing["root_identity"] != original_identity:
            raise IntegrationFailure("missing bound root was not recorded as root-unavailable")

        output = Fixture(self.fixture_root, "output-disappeared")
        self.build_core(output)
        self.sample_state(output)
        shutil.rmtree(output.out)
        missing_output = self.sample_state(output)
        if missing_output["observation"]["status"] == "current":
            raise IntegrationFailure("missing bound output was incorrectly observed as current")

        mismatch = Fixture(self.fixture_root, "config-mismatch")
        before = self.sample_state(mismatch)
        changed = mismatch.config_data()
        changed["workspace_id"] = "different-workspace"
        mismatch.write_config(changed)
        pending = mismatch.state / "state.pending"
        write_private_json(pending, {"uncommitted": True})
        before_pending = pending.read_bytes()
        self.observer_command(mismatch, "sample", expect_success=False)
        self.observer_command(mismatch, "status", expect_success=False)
        if pending.read_bytes() != before_pending:
            raise IntegrationFailure("wrong binding recovered another workspace's pending state")
        mismatch.write_config()
        state = self.status(mismatch)["state"]
        if state != before:
            raise IntegrationFailure("rejected binding mismatch altered the persisted binding")
        self.sample_state(mismatch)
        if pending.exists():
            raise IntegrationFailure("authorized writer did not recover its interrupted pending state")

    def check_initial_pending_recovery(self) -> None:
        assert self.fixture_root is not None
        fixture = Fixture(self.fixture_root, "initial-pending")
        committed = self.sample_state(fixture)
        state_file = fixture.state / "state.json"
        pending_file = fixture.state / "state.pending"
        state_file.unlink()
        initial = dict(committed, sequence=0, observation=None, lifecycle="assessing")
        initial["metrics"] = {"schema_version": "shin.observer-metrics.v1", "scope": "retained-committed-samples",
                              "sample_count": 0, "outcomes": {key: 0 for key in
                                  ("current", "stale", "unpublished", "incomplete", "error")},
                              "scan_duration_ms": {"last": 0.0, "max": 0.0, "total": 0.0}}
        malformed = {"partial": True}
        foreign = json.loads(json.dumps(initial))
        foreign["binding"]["workspace_id"] = "foreign-workspace"
        inconsistent = dict(initial, sequence=1)
        for rejected in (malformed, foreign, committed, inconsistent):
            write_private_json(pending_file, rejected)
            before = pending_file.read_bytes()
            if self.status(fixture)["state"] is not None:
                raise IntegrationFailure("read-only status promoted uncommitted initial state")
            self.observer_command(fixture, "sample", expect_success=False)
            if state_file.exists() or pending_file.read_bytes() != before:
                raise IntegrationFailure("rejected initial pending state was changed or promoted")
        write_private_json(pending_file, initial)
        recovered = self.sample_state(fixture)
        if recovered["sequence"] != 1 or recovered["epoch"] == initial["epoch"] or pending_file.exists():
            raise IntegrationFailure("same-binding initial scratch did not recover into a fresh observation")

    def check_profile_snapshot_and_metadata_bounds(self) -> None:
        assert self.fixture_root is not None
        fixture = Fixture(self.fixture_root, "metadata")
        fixture.write_source(f"Documentation includes {SECRET_TEXT} but Observer metadata must not.\n")
        self.build_core(fixture)
        self.sample_state(fixture)

        fixture.write_profile(source_label="Changed profile policy")
        profile_changed = self.sample_state(fixture)
        if profile_changed["observation"]["status"] not in {"stale", "incomplete", "error"} or \
                profile_changed["observation"]["status"] == "current":
            raise IntegrationFailure("profile change was incorrectly observed as current")
        fixture.write_profile()
        self.build_core(fixture)
        self.sample_state(fixture)

        manifest = fixture.out / "docs" / "kb" / "generated" / "manifest.json"
        if not manifest.is_file():
            raise IntegrationFailure("Core fixture did not produce the expected generated manifest")
        manifest.write_text("{not valid json\n", encoding="utf-8")
        corrupt = self.sample_state(fixture)
        if corrupt["observation"]["status"] not in {"incomplete", "error"} or not corrupt["observation"]["reason_code"]:
            raise IntegrationFailure("corrupt snapshot was not represented as a persisted observation error")

        status = self.status(fixture)
        rendered = json.dumps(status["state"], sort_keys=True)
        if SECRET_TEXT in rendered:
            raise IntegrationFailure("persisted Observer metadata exposed raw source text")
        for path in fixture.state.rglob("*"):
            if path.is_file() and SECRET_TEXT in path.read_text(encoding="utf-8", errors="replace"):
                raise IntegrationFailure("Observer state files exposed raw source text")
        observation = status["state"]["observation"] if isinstance(status["state"], dict) else {}
        if not isinstance(observation.get("details_omitted"), int) or observation["details_omitted"] < 0:
            raise IntegrationFailure("Observer metadata does not retain a bounded detail count")

    def check_invalid_configs(self) -> None:
        assert self.fixture_root is not None
        fixture = Fixture(self.fixture_root, "invalid-config")
        cases: list[tuple[str, str]] = []
        unsafe_mode = fixture.config_data()
        fixture.write_config(unsafe_mode)
        fixture.config.chmod(0o644)
        cases.append(("unsafe-config-mode", fixture.config.read_text(encoding="utf-8")))
        fixture.config.chmod(0o600)
        invalid_interval = fixture.config_data()
        invalid_interval["poll_interval_ms"] = 99
        cases.append(("invalid-poll-interval", json.dumps(invalid_interval)))
        invalid_file_limit = fixture.config_data()
        invalid_file_limit["max_file_bytes"] = invalid_file_limit["max_snapshot_bytes"] + 1
        cases.append(("invalid-file-snapshot-limit", json.dumps(invalid_file_limit)))
        invalid_snapshot_limit = fixture.config_data()
        invalid_snapshot_limit["max_snapshot_bytes"] = 1024 * 1024 * 1024 + 1
        cases.append(("invalid-snapshot-limit", json.dumps(invalid_snapshot_limit)))
        unknown_field = fixture.config_data()
        unknown_field["unexpected"] = True
        cases.append(("unknown-config-field", json.dumps(unknown_field)))
        overlap = fixture.config_data()
        overlap["state_dir"] = str(fixture.root)
        cases.append(("unsafe-source-state-overlap", json.dumps(overlap)))
        overlap_out = fixture.config_data()
        overlap_out["state_dir"] = str(fixture.out)
        cases.append(("unsafe-output-state-overlap", json.dumps(overlap_out)))
        overlap_profile = fixture.config_data()
        overlap_profile["state_dir"] = str(fixture.profiles)
        cases.append(("unsafe-profile-state-overlap", json.dumps(overlap_profile)))
        duplicate = json.dumps(fixture.config_data())[:-1] + ',"observer_id":"duplicate"}'
        cases.append(("duplicate-config-field", duplicate))

        for name, content in cases:
            fixture.config.write_text(content + "\n", encoding="utf-8")
            fixture.config.chmod(0o600 if name != "unsafe-config-mode" else 0o644)
            self.observer_command(fixture, "sample", expect_success=False)
            fixture.config.chmod(0o600)

        fixture.write_config()
        fixture.state.chmod(0o755)
        try:
            self.observer_command(fixture, "sample", expect_success=False)
        finally:
            fixture.state.chmod(0o700)

        source_config = fixture.root / "observer.json"
        write_private_json(source_config, fixture.config_data())
        value, completed = self.run_json([str(self.observer), "sample", "--config", str(source_config)],
                                         "Observer source-contained config", expect_success=False)
        if completed.stdout:
            raise IntegrationFailure("source-contained configuration failure wrote stdout")
        self.assert_error_envelope(value)

    def check_incomplete_inventory_and_state_guards(self) -> None:
        assert self.fixture_root is not None
        fixture = Fixture(self.fixture_root, "incomplete")
        source = fixture.root / "src"
        source.mkdir()
        (source / "one.cpp").write_text("int one() { return 1; }\n", encoding="utf-8")
        (source / "two.cpp").write_text("int two() { return 2; }\n", encoding="utf-8")
        profile = json.loads(fixture.profile_path.read_text(encoding="utf-8"))
        profile.update({"sourceFactRoots": ["src"], "sourceFactExtensions": [".cpp"], "sourceFactMaxFiles": 1})
        fixture.profile_path.write_text(json.dumps(profile), encoding="utf-8")
        unpublished = self.sample_state(fixture)
        if unpublished["observation"]["status"] != "incomplete" or unpublished["observation"]["snapshot_id"] is not None:
            raise IntegrationFailure("capped unpublished inventory was not explicitly incomplete")
        self.build_core(fixture)
        incomplete = self.sample_state(fixture)
        if incomplete["observation"]["status"] != "incomplete" or incomplete["observation"]["snapshot_current"] is not False:
            raise IntegrationFailure("published capped inventory was promoted to current")
        state_file = fixture.state / "state.json"
        for field, value in (("snapshot_current", True), ("readiness_assessed", True), ("raw_source", SECRET_TEXT)):
            changed = json.loads(json.dumps(incomplete))
            changed["observation"][field] = value
            write_private_json(state_file, changed)
            before = state_file.read_bytes()
            self.observer_command(fixture, "status", expect_success=False)
            self.observer_command(fixture, "sample", expect_success=False)
            if state_file.read_bytes() != before:
                raise IntegrationFailure("invalid state was silently rewritten")
        write_private_json(state_file, incomplete)
        profile["sourceFactMaxFiles"] = 2
        fixture.profile_path.write_text(json.dumps(profile), encoding="utf-8")
        self.build_core(fixture)
        if self.sample_state(fixture)["observation"]["status"] != "current":
            raise IntegrationFailure("complete rebuilt inventory did not recover")

    def check_outbox_capacity_restart_and_metrics(self) -> None:
        assert self.fixture_root is not None
        fixture = Fixture(self.fixture_root, "durable-outbox")
        config = fixture.config_data()
        config["outbox"].update(enabled=True, max_entries=2)
        fixture.write_config(config)
        first = self.sample_state(fixture)
        queue = first["outbox"]
        if len(queue["entries"]) != 1 or queue["rejected_observations"] != 0:
            raise IntegrationFailure("first committed observation was not queued")
        entry = queue["entries"][0]
        expected_id = hashlib.sha256(f'{queue["stream_id"]}:1'.encode()).hexdigest()
        if entry["submission_id"] != expected_id or entry["payload"]["verification_level"] != "producer-observed":
            raise IntegrationFailure("outbox submission identity/provenance was not explicit")
        if any(str(path) in json.dumps(entry) for path in (fixture.root, fixture.out, fixture.state, fixture.profiles)):
            raise IntegrationFailure("outbound payload included a local filesystem binding path")
        unchanged = self.sample_state(fixture)
        if unchanged["outbox"]["entries"] != queue["entries"] or unchanged["outbox"]["coalesced_observations"] != 1:
            raise IntegrationFailure("equal observations did not coalesce without rewriting accepted capture time")
        fixture.write_source("Source edit creates a different immutable queued observation.\n")
        second = self.sample_state(fixture)
        saved_entries = second["outbox"]["entries"]
        if len(saved_entries) != 2 or saved_entries[0] != entry or saved_entries[1]["sequence"] != 2:
            raise IntegrationFailure("queued order or accepted predecessor was changed")
        self.build_core(fixture)
        full = self.sample_state(fixture)
        if full["outbox"]["entries"] != saved_entries or full["outbox"]["rejected_observations"] != 1 or \
                full["outbox"]["first_rejected_observation_sequence"] != 4 or full["observation"]["status"] != "current":
            raise IntegrationFailure("capacity did not preserve accepted entries and independent local currentness")
        watch = self.start_watch(fixture)
        self.wait_for_lifecycle(fixture, {"waiting"}, watch)
        self.finish_watch(watch, signal_number=signal.SIGKILL, expect_clean=False)
        restarted = self.sample_state(fixture)
        if restarted["outbox"]["entries"] != saved_entries or restarted["outbox"]["stream_id"] != queue["stream_id"] or \
                restarted["previous_run_interrupted"] is not True:
            raise IntegrationFailure("restart lost pending submissions or changed stable stream identity")
        self.core_worktree(fixture)
        self.core_command(fixture, "brief", "observer fixture", "--require-current")
        state_file = fixture.state / "state.json"
        before = tree_metadata(fixture.state)
        metrics, completed = self.observer_command(fixture, "metrics")
        if metrics["state_mutated"] or metrics["live_source_verified"] or metrics["synchronization"] != "explicit-metadata-delivery" or \
                metrics["outbox"]["resync_required"] is not True or metrics["outbox"]["server_acknowledged"] is not False or \
                metrics["metrics"]["sample_count"] != restarted["sequence"] or \
                sum(metrics["metrics"]["outcomes"].values()) != restarted["sequence"] or \
                len(completed.stdout.encode()) > 4096 or "entries" in metrics["outbox"]:
            raise IntegrationFailure("compact metrics confused local durability with delivery or lost counters")
        if tree_metadata(fixture.state) != before:
            raise IntegrationFailure("metrics mutated the private state")
        for field in ("outbox", "metrics"):
            corrupt = json.loads(json.dumps(restarted))
            if field == "outbox":
                corrupt[field]["entries"][0]["payload"]["project_id"] = "foreign-project"
            else:
                corrupt[field]["sample_count"] += 1
            write_private_json(state_file, corrupt)
            frozen = state_file.read_bytes()
            self.observer_command(fixture, "sample", expect_success=False)
            self.observer_command(fixture, "metrics", expect_success=False)
            if state_file.read_bytes() != frozen:
                raise IntegrationFailure("corrupt durable state was silently repaired")
        write_private_json(state_file, restarted)
        pending = fixture.state / "state.pending"
        write_private_json(pending, {"incomplete": True})
        recovered = self.sample_state(fixture)
        if pending.exists() or recovered["outbox"]["entries"] != saved_entries:
            raise IntegrationFailure("pending recovery changed previously committed submissions")

    def check_v1_rejection_and_outbox_policy_binding(self) -> None:
        assert self.fixture_root is not None
        fixture = Fixture(self.fixture_root, "v2-binding")
        committed = self.sample_state(fixture)
        state_file = fixture.state / "state.json"
        legacy = dict(committed, schema_version="shin.observer-state.v1")
        write_private_json(state_file, legacy)
        before = state_file.read_bytes()
        self.observer_command(fixture, "sample", expect_success=False)
        if state_file.read_bytes() != before:
            raise IntegrationFailure("legacy state was migrated implicitly")
        write_private_json(state_file, committed)
        config = fixture.config_data()
        config["outbox"]["enabled"] = True
        fixture.write_config(config)
        self.observer_command(fixture, "sample", expect_success=False)
        if json.loads(state_file.read_text()) != committed:
            raise IntegrationFailure("capture policy changed an existing state binding")
        for policy in ({"enabled": 1, "max_entries": 4, "max_bytes": 4096},
                       {"enabled": True, "max_entries": 129, "max_bytes": 4096},
                       {"enabled": True, "max_entries": 4, "max_bytes": 196609}):
            config["outbox"] = policy
            fixture.write_config(config)
            self.observer_command(fixture, "sample", expect_success=False)

    def run(self) -> None:
        with tempfile.TemporaryDirectory(prefix="shin-observer-integration-") as temporary:
            self.fixture_root = Path(temporary).resolve()
            self.record("version-and-watch-bounds", self.check_version_and_invalid_watch_bounds)
            self.record("unpublished-current-stale-and-status-parity", self.check_unpublished_and_current_parity)
            self.record("watch-lock-crash-recovery-and-ttl", self.check_watch_lock_recovery_and_ttl)
            self.record("binding-root-replacement-and-missing", self.check_binding_and_root_failures)
            self.record("initial-pending-binding-and-recovery", self.check_initial_pending_recovery)
            self.record("profile-corrupt-snapshot-and-bounded-metadata", self.check_profile_snapshot_and_metadata_bounds)
            self.record("invalid-config-duplicates-and-overlaps", self.check_invalid_configs)
            self.record("incomplete-inventory-and-typed-state-guards", self.check_incomplete_inventory_and_state_guards)
            self.record("outbox-capacity-restart-and-compact-metrics", self.check_outbox_capacity_restart_and_metrics)
            self.record("v1-rejection-and-outbox-policy-binding", self.check_v1_rejection_and_outbox_policy_binding)
            from outbox_archive_checks import check_outbox_archive
            self.record("outbox-archive-disposition", lambda: check_outbox_archive(self))

    def cleanup(self) -> None:
        for process in list(self.active_processes):
            try:
                self.finish_watch(process, signal_number=signal.SIGKILL, expect_clean=False)
            except (OSError, subprocess.SubprocessError):
                pass

    def write_report(self, *, ok: bool, failure: str | None = None) -> None:
        report: dict[str, Any] = {
            "schema_version": REPORT_SCHEMA,
            "ok": ok,
            "cases": self.cases,
            "elapsed_ms": round((time.monotonic() - self.started_at) * 1000, 3),
            "measured_per_sample_latency_ms": self.latencies_ms,
            "performance_note": "Measurements are local harness observations, not an SLO.",
        }
        if failure is not None:
            report["failure"] = failure
        descriptor = os.open(self.report_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(report, output, separators=(",", ":"), sort_keys=True)
            output.write("\n")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--observer", required=True, help="absolute shin-observer binary")
    parser.add_argument("--core", required=True, help="absolute build/core-p2/shin-context binary")
    parser.add_argument("--report", required=True, help="new explicit JSON report path outside source fixtures")
    return parser.parse_args(argv)


def require_executable(path: str, label: str) -> Path:
    candidate = Path(path)
    if not candidate.is_absolute() or not candidate.is_file() or not os.access(candidate, os.X_OK):
        raise IntegrationFailure(f"--{label} must name an absolute executable file")
    return candidate.resolve()


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        observer = require_executable(args.observer, "observer")
        core = require_executable(args.core, "core")
        report = Path(args.report)
        if not report.is_absolute() or report.exists() or not report.parent.is_dir():
            raise IntegrationFailure("--report must be a new absolute path with an existing parent directory")
        suite = ObserverIntegrationSuite(observer, core, report)
        try:
            suite.run()
        except BaseException as error:
            suite.cleanup()
            try:
                suite.write_report(ok=False, failure=type(error).__name__)
            except OSError:
                pass
            print(f"error: Observer integration test failed: {error}", file=sys.stderr)
            return 1
        suite.cleanup()
        suite.write_report(ok=True)
        print("Observer integration test passed; compact report written to --report")
        return 0
    except IntegrationFailure as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
