#!/usr/bin/env python3
"""Real PostgreSQL and HTTP coverage for the isolated server retrieval slice."""

from __future__ import annotations

import argparse
import copy
import json
import os
import re
from pathlib import Path
import shutil
import secrets
import stat
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any

from integration_test import BUILD_ROOT, IntegrationFailure, IntegrationSuite, under_build


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CORE = REPOSITORY_ROOT / "build" / "core-p2" / "shin-context"
PROFILE_ID = "generic-docs"
PROJECT_ID = "visible"
ROUTE_ID = "visible-retrieval"
MAX_BODY_BYTES = 32 * 1024


class RetrievalIntegrationSuite(IntegrationSuite):
    def __init__(self, binary: Path, core: Path, state_root: Path, report_path: Path) -> None:
        super().__init__(binary, state_root, report_path)
        self.core = core
        self.fixture_root = state_root / "fixtures" / "roots" / PROJECT_ID
        self.fixture_root_before_replacement = self.fixture_root.with_name("visible-before-root-replacement")
        self.retrieval_root = state_root / "fixtures" / "retrieval"
        self.profile_dir = self.retrieval_root / "profiles"
        self.out_dir = self.retrieval_root / "out"
        self.other_root = state_root / "fixtures" / "roots" / "other"
        self.other_out_dir = self.retrieval_root / "other-out"
        self.managed_state_dir = self.retrieval_root / "managed-artifacts"
        self.task_path = self.retrieval_root / "task-request.json"
        self.runtime_config_path = state_root / "config" / "runtime.json"
        self.migrator_config_path = state_root / "config" / "migrator.json"
        self._original_configs: dict[Path, bytes] = {}
        self.retrieval_benchmark: dict[str, Any] = {"status": "not-run"}
        self.active_generations: dict[tuple[str, str], dict[str, Any]] = {}

    def run_cli_json(self, *arguments: str) -> dict[str, Any]:
        completed = self.run_process(
            [
                str(self.core),
                "--profile",
                PROFILE_ID,
                "--profile-dir",
                str(self.profile_dir),
                "--root",
                str(self.fixture_root),
                "--out-dir",
                str(self.out_dir),
                *arguments,
            ],
            f"candidate Core {' '.join(arguments)}",
            timeout=45.0,
        )
        try:
            decoded = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise IntegrationFailure("candidate Core did not return a JSON result") from error
        if not isinstance(decoded, dict) or decoded.get("ok") is not True:
            raise IntegrationFailure("candidate Core did not return a successful result")
        return decoded

    def run_external_core_json(
        self, root: Path, out_dir: Path, *arguments: str, expected_success: bool
    ) -> dict[str, Any]:
        environment = {name: value for name, value in os.environ.items() if not name.startswith("GIT_")}
        environment["PATH"] = ""
        completed = subprocess.run(
            [
                str(self.core),
                "--profile",
                PROFILE_ID,
                "--profile-dir",
                str(self.profile_dir),
                "--root",
                str(root),
                "--out-dir",
                str(out_dir),
                *arguments,
            ],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=45.0,
            env=environment,
        )
        try:
            payload = completed.stdout if completed.stdout.strip() else completed.stderr
            decoded = json.loads(payload)
        except json.JSONDecodeError as error:
            raise IntegrationFailure(f"external no-git {arguments[0]} did not return JSON") from error
        if not isinstance(decoded, dict) or (completed.returncode == 0) != expected_success:
            raise IntegrationFailure("external no-git Core command returned an unexpected status")
        return decoded

    @staticmethod
    def write_fixture_file(path: Path, content: str) -> None:
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(path.parent, 0o700)
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(content)

    @staticmethod
    def replace_private_json(path: Path, value: dict[str, Any]) -> None:
        temporary = path.with_name(f".{path.name}.retrieval-test.tmp")
        temporary.unlink(missing_ok=True)
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
                json.dump(value, handle, sort_keys=True, separators=(",", ":"))
                handle.write("\n")
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary, path)
            os.chmod(path, 0o600)
        finally:
            temporary.unlink(missing_ok=True)

    def prepare_retrieval_fixture(self) -> None:
        self.fixture_root.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(self.fixture_root, 0o700)
        if self.retrieval_root.exists():
            shutil.rmtree(self.retrieval_root)
        self.profile_dir.mkdir(mode=0o700, parents=True)
        self.out_dir.mkdir(mode=0o700)
        self.other_out_dir.mkdir(mode=0o700)
        self.managed_state_dir.mkdir(mode=0o700)

        profile = json.loads((REPOSITORY_ROOT / "profiles" / "generic-docs.json").read_text(encoding="utf-8"))
        # Generic routes need real neutral goldens before they can become managed active generations.
        profile["evalCases"] = [{
            "id": "retrieval-fixture-policy-eval",
            "query": "project identity profile fingerprint",
            "mustPaths": ["docs/policy.md"],
            "maxCandidateRows": -1,
            "limit": 6,
            "perDocLimit": 2,
        }]
        profile["packEvalCases"] = [{
            "id": "retrieval-fixture-policy-pack",
            "request": {
                "schema_version": "shin.task-request.v1",
                "request_id": "retrieval-fixture-policy-pack",
                "task": {
                    "objective": "Verify neutral managed-route policy evidence.",
                    "must": [{"id": "route-policy", "query": "project identity profile fingerprint"}],
                },
                "budget": {
                    "max_evidence_bytes": 8192,
                    "max_records": 6,
                    "per_query_records": 6,
                    "per_document_records": 2,
                },
            },
            "mustPaths": ["docs/policy.md"],
            "expectedComplete": True,
            "maxEvidenceBytes": 8192,
            "maxRecords": 6,
            "maxCompactBytes": 16000,
            "requireSerialParallelIdentity": True,
        }]
        self.write_fixture_file(
            self.profile_dir / f"{PROFILE_ID}.json",
            json.dumps(profile, sort_keys=True, separators=(",", ":")) + "\n",
        )
        self.write_fixture_file(
            self.fixture_root / "README.md",
            "# Retrieval Fixture\n\n"
            "The retrieval fixture documents immutable snapshot admission and deterministic evidence.\n",
        )
        self.write_fixture_file(
            self.fixture_root / "docs" / "runtime.md",
            "# Snapshot Runtime\n\n"
            "A verified snapshot supports compact Brief and Pack evidence.\n"
            "Exact-current admission rejects source drift without a snapshot-only fallback.\n",
        )
        self.write_fixture_file(
            self.fixture_root / "docs" / "policy.md",
            "# Route Policy\n\n"
            "Routes bind a project identity, profile fingerprint, and generated output.\n",
        )
        self.write_fixture_file(
            self.fixture_root / "docs" / "long-body.md",
            "# Long Evidence Body\n" + "ordinary filler " * 600 + "LateServiceNeedle tailalpha\n",
        )

        self.run_cli_json("build")
        self.write_fixture_file(self.other_root / "README.md", "# Other Project\n\nOtherProjectOnly isolated evidence.\n")
        self.write_fixture_file(
            self.other_root / "docs" / "policy.md",
            "# Other Route Policy\n\nRoutes bind a project identity, profile fingerprint, and generated output.\n",
        )
        self.run_process(
            [str(self.core), "--profile", PROFILE_ID, "--profile-dir", str(self.profile_dir),
             "--root", str(self.other_root), "--out-dir", str(self.other_out_dir), "build"],
            "publish other neutral fixture", timeout=45.0,
        )

    def configure_retrieval(self) -> None:
        route = {
            "id": ROUTE_ID,
            "project_id": PROJECT_ID,
            "workspace_id": "primary",
            "root": str(self.fixture_root),
            "profile": PROFILE_ID,
            "profile_dir": str(self.profile_dir),
        }
        for path in (self.runtime_config_path, self.migrator_config_path):
            original = path.read_bytes()
            self._original_configs.setdefault(path, original)
            config = json.loads(original)
            if config.get("schema_version") != "shin.server-config.v10" or config.get("source_host_id") != "local-dev":
                raise IntegrationFailure("developer helper did not create the server v9 source-host configuration")
            config["retrieval"] = {
                "routes": [route, {**route, "id": "other-retrieval", "project_id": "other",
                                    "workspace_id": "primary", "root": str(self.other_root)}],
                "max_payload_bytes": 536870912,
                "max_generation_bytes": 134217728,
                "max_file_bytes": 67108864,
                "max_generations": 8,
            }
            config["preparations"] = {
                "enabled": True,
                "state_dir": str(self.managed_state_dir),
                "max_retained_jobs": 8,
                "lease_seconds": 3,
            }
            self.replace_private_json(path, config)

    def check_external_nogit_workspace_and_currentness(self) -> None:
        temporary = Path(tempfile.mkdtemp(prefix="shin-server-nogit-", dir=tempfile.gettempdir())).resolve()
        source_root = temporary / "source"
        out_dir = temporary / "out"
        runtime_original = self.runtime_config_path.read_bytes()
        try:
            for directory in (source_root, out_dir):
                directory.mkdir(mode=0o700)
                os.chmod(directory, 0o700)
                status = directory.stat()
                if status.st_uid != os.geteuid() or stat.S_IMODE(status.st_mode) != 0o700:
                    raise IntegrationFailure("external no-git fixture is not private and owned")
            if (source_root / ".git").exists():
                raise IntegrationFailure("external no-git fixture unexpectedly contains a Git directory")
            discovery = subprocess.run(
                ["git", "-C", str(source_root), "rev-parse", "--show-toplevel"],
                check=False,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=10.0,
            )
            if discovery.returncode == 0:
                raise IntegrationFailure("external fixture inherited a discoverable Git worktree")

            self.write_fixture_file(
                source_root / "README.md",
                "# Neutral External Fixture\n\nStandalone source evidence without repository metadata.\n",
            )
            source = source_root / "docs" / "policy.md"
            self.write_fixture_file(source, "# Neutral Policy\n\nCurrentness is content-derived.\n")

            config = json.loads(runtime_original)
            config["allowed_roots"] = [str(source_root)]
            config["retrieval"]["routes"] = []
            self.replace_private_json(self.runtime_config_path, config)
            self.start_server()
            try:
                self.assert_register("external-nogit", "register-external-nogit")
                self.assert_workspace_register("external-nogit", "primary", source_root, "register-external-nogit-primary")
            finally:
                self.stop_server(expect_clean=True)
            self.replace_private_json(self.runtime_config_path, json.loads(runtime_original))

            built = self.run_external_core_json(source_root, out_dir, "build", expected_success=True)
            if built.get("ok") is not True:
                raise IntegrationFailure("external no-git Core build did not succeed")
            current = self.run_external_core_json(source_root, out_dir, "worktree-diff", "--strict", expected_success=True)
            if current.get("current") is not True or current.get("comparison", {}).get("git_required") is not False:
                raise IntegrationFailure("external no-git worktree proof is not Git-independent and current")
            brief = self.run_external_core_json(
                source_root, out_dir, "brief", "currentness content", "--projection", "compact", "--require-current",
                expected_success=True,
            )
            if brief.get("ok") is not True or "docs/policy.md" not in {
                item.get("path") for item in brief.get("verification_paths", [])
            }:
                raise IntegrationFailure("external no-git current Brief did not succeed")

            self.write_fixture_file(source, "# Neutral Policy\n\nCurrentness is content-derived after an edit.\n")
            stale = self.run_external_core_json(source_root, out_dir, "worktree-diff", "--strict", expected_success=False)
            if stale.get("current") is not False or stale.get("strict") is not True:
                raise IntegrationFailure("external no-git source edit did not make worktree-diff strictly stale")
            stale_brief = self.run_external_core_json(
                source_root, out_dir, "brief", "currentness content", "--projection", "compact", "--require-current",
                expected_success=False,
            )
            if stale_brief.get("error", {}).get("code") != "stale-index":
                raise IntegrationFailure("external no-git strict Brief did not reject stale source")

            self.run_external_core_json(source_root, out_dir, "build", expected_success=True)
            rebuilt = self.run_external_core_json(source_root, out_dir, "worktree-diff", "--strict", expected_success=True)
            if (rebuilt.get("current") is not True or
                    rebuilt.get("worktree", {}).get("worktree_id") == current.get("worktree", {}).get("worktree_id")):
                raise IntegrationFailure("external no-git rebuild did not restore currentness")
            rebuilt_brief = self.run_external_core_json(
                source_root, out_dir, "brief", "currentness content", "--projection", "compact", "--require-current",
                expected_success=True,
            )
            if (rebuilt_brief.get("snapshot_id") == brief.get("snapshot_id") or
                    "docs/policy.md" not in {item.get("path") for item in rebuilt_brief.get("verification_paths", [])}):
                raise IntegrationFailure("external no-git rebuild did not publish changed policy evidence")
        finally:
            self.stop_server(expect_clean=False)
            self.replace_private_json(self.runtime_config_path, json.loads(runtime_original))
            shutil.rmtree(temporary)

    def restore_configs(self) -> None:
        for path, content in self._original_configs.items():
            temporary = path.with_name(f".{path.name}.retrieval-test.restore.tmp")
            temporary.unlink(missing_ok=True)
            descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            try:
                with os.fdopen(descriptor, "wb") as handle:
                    handle.write(content)
                    handle.flush()
                    os.fsync(handle.fileno())
                os.replace(temporary, path)
                os.chmod(path, 0o600)
            finally:
                temporary.unlink(missing_ok=True)

    def post_retrieval(
        self,
        project_id: str,
        action: str,
        payload: Any,
        *,
        route_id: str = ROUTE_ID,
        role: str = "admin",
        content_type: str | None = "application/json",
    ) -> tuple[int, dict[str, Any]]:
        headers = self.auth_headers(role)
        if content_type is not None:
            headers["Content-Type"] = content_type
        body = payload if isinstance(payload, bytes) else json.dumps(payload, separators=(",", ":")).encode("utf-8")
        return self.request(
            "POST",
            f"/api/v1/projects/{project_id}/routes/{route_id}/{action}",
            headers=headers,
            body=body,
        )

    @staticmethod
    def _activation_fields() -> set[str]:
        return {"project_id", "route_id", "revision", "preparation_id", "snapshot_id"}

    def activation(self, project_id: str, route_id: str, *, expected_status: int = 200) -> dict[str, Any]:
        envelope = self.expect_status(
            expected_status,
            "GET",
            f"/api/v1/projects/{project_id}/routes/{route_id}/activation",
            headers=self.auth_headers("admin"),
        )
        result = envelope.get("result")
        if not isinstance(result, dict) or set(result) != self._activation_fields():
            raise IntegrationFailure("activation lookup does not match the public contract")
        if result["project_id"] != project_id or result["route_id"] != route_id:
            raise IntegrationFailure("activation lookup crossed its configured route boundary")
        if type(result["revision"]) is not int or result["revision"] < 0:
            raise IntegrationFailure("activation lookup has an invalid revision")
        if result["preparation_id"] is not None and (type(result["preparation_id"]) is not int or result["preparation_id"] <= 0):
            raise IntegrationFailure("activation lookup has an invalid preparation identity")
        if result["snapshot_id"] is not None and not isinstance(result["snapshot_id"], str):
            raise IntegrationFailure("activation lookup has an invalid snapshot identity")
        if (result["preparation_id"] is None) != (result["snapshot_id"] is None):
            raise IntegrationFailure("activation lookup has a partial active target")
        return result

    def post_seed_preparation(self, project_id: str, route_id: str, expected_snapshot_id: str | None,
                              key: str) -> dict[str, Any]:
        status, response = self.request(
            "POST",
            f"/api/v1/projects/{project_id}/routes/{route_id}/preparations",
            headers={**self.auth_headers("admin"), "Content-Type": "application/json", "Idempotency-Key": key},
            body=json.dumps({"expected_snapshot_id": expected_snapshot_id}, separators=(",", ":")).encode("utf-8"),
        )
        if status != 202:
            raise IntegrationFailure("managed retrieval seed preparation was not accepted")
        result = response.get("result")
        if not isinstance(result, dict) or result.get("replayed") is not False or not isinstance(result.get("job"), dict):
            raise IntegrationFailure("managed retrieval seed preparation has an invalid response")
        job = result["job"]
        if (type(job.get("id")) is not int or job["id"] <= 0 or job.get("project_id") != project_id or
                job.get("route_id") != route_id):
            raise IntegrationFailure("managed retrieval seed preparation has the wrong public job identity")
        return job

    def wait_for_seed_preparation(self, project_id: str, route_id: str, job_id: int) -> dict[str, Any]:
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            result = self.expect_status(
                200,
                "GET",
                f"/api/v1/preparations/{job_id}",
                headers=self.auth_headers("admin"),
            ).get("result")
            if not isinstance(result, dict) or result.get("id") != job_id:
                raise IntegrationFailure("managed retrieval seed preparation lookup is malformed")
            if result.get("state") == "prepared":
                if not isinstance(result.get("artifact_snapshot_id"), str):
                    raise IntegrationFailure("managed retrieval seed preparation has no artifact snapshot")
                return result
            if result.get("state") in {"failed", "cancelled"}:
                raise IntegrationFailure("managed retrieval seed preparation did not complete")
            time.sleep(0.05)
        raise IntegrationFailure("managed retrieval seed preparation did not finish")

    def activate_seed(self, project_id: str, route_id: str, preparation_id: int,
                      expected_revision: int) -> dict[str, Any]:
        status, response = self.request(
            "POST",
            f"/api/v1/projects/{project_id}/routes/{route_id}/activation",
            headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
            body=json.dumps({"preparation_id": preparation_id, "expected_revision": expected_revision},
                            separators=(",", ":")).encode("utf-8"),
        )
        if status != 200:
            raise IntegrationFailure("managed retrieval seed activation was not accepted")
        result = response.get("result")
        if not isinstance(result, dict) or result.get("replayed") is not False or not isinstance(result.get("receipt"), dict):
            raise IntegrationFailure("managed retrieval seed activation has an invalid response")
        receipt = result["receipt"]
        if (set(receipt) != {"project_id", "route_id", "previous_revision", "revision", "preparation_id", "snapshot_id", "created_at"} or
                receipt["project_id"] != project_id or receipt["route_id"] != route_id or
                receipt["previous_revision"] != expected_revision or receipt["revision"] != expected_revision + 1 or
                receipt["preparation_id"] != preparation_id or not isinstance(receipt["snapshot_id"], str) or
                not isinstance(receipt["created_at"], str)):
            raise IntegrationFailure("managed retrieval seed activation receipt is invalid")
        return receipt

    def seed_managed_retrieval_routes(self, *, with_scoped_publications: bool = False) -> None:
        for project_id, route_id in ((PROJECT_ID, ROUTE_ID), ("other", "other-retrieval")):
            initial = self.activation(project_id, route_id)
            if initial["revision"] != 0 or initial["preparation_id"] is not None or initial["snapshot_id"] is not None:
                raise IntegrationFailure("new retrieval route was implicitly published before activation")
            unavailable, response = self.post_retrieval(
                project_id,
                "brief",
                {"query": "snapshot admission evidence"},
                route_id=route_id,
            )
            if unavailable != 503 or response.get("error", {}).get("code") != "active-generation-missing":
                raise IntegrationFailure("unpublished retrieval route did not fail closed")
            job = self.post_seed_preparation(project_id, route_id, None, "seed-" + route_id)
            prepared = self.wait_for_seed_preparation(project_id, route_id, job["id"])
            if with_scoped_publications:
                # Publish before route activation changes the source-base revision.
                # Other stand users keep their original preparation-only fixture.
                publication = f"/api/v1/projects/{project_id}/publications/scoped-publication"
                proposal = self.expect_status(201, "POST", publication + "/proposals", headers={
                    **self.auth_headers("admin"), "Content-Type": "application/json", "Idempotency-Key": "scoped-publication-" + route_id},
                    body=json.dumps({"preparation_id": prepared["id"], "base_revision": 0}).encode())
                proposal_id = proposal["result"]["proposal"]["proposal"]["id"]
                self.expect_status(200, "POST", publication + f"/proposals/{proposal_id}/resolution",
                    headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
                    body=json.dumps({"decision": "accept", "expected_revision": 0}).encode())
            receipt = self.activate_seed(project_id, route_id, prepared["id"], initial["revision"])
            active = self.activation(project_id, route_id)
            if (active["revision"] != receipt["revision"] or active["preparation_id"] != prepared["id"] or
                    active["snapshot_id"] != prepared["artifact_snapshot_id"]):
                raise IntegrationFailure("managed retrieval seed activation was not persisted")
            self.active_generations[(project_id, route_id)] = active
            artifact = self.managed_artifact(project_id, route_id)
            for entry in (artifact, *artifact.rglob("*")):
                expected_mode = 0o700 if entry.is_dir() else 0o600
                if stat.S_IMODE(entry.lstat().st_mode) != expected_mode:
                    raise IntegrationFailure("managed artifact inherited non-private creation permissions")

    def disable_preparation_worker(self) -> None:
        if self.server is not None:
            raise IntegrationFailure("preparation worker can only be disabled while the owned server is stopped")
        for path in (self.runtime_config_path, self.migrator_config_path):
            config = json.loads(path.read_text(encoding="utf-8"))
            config["preparations"]["enabled"] = False
            self.replace_private_json(path, config)

    def managed_artifact(self, project_id: str, route_id: str) -> Path:
        active = self.activation(project_id, route_id)
        preparation_id = active["preparation_id"]
        if preparation_id is None:
            raise IntegrationFailure("managed artifact lookup requires an active preparation")
        key = self.scalar("SELECT artifact_key FROM shin_server.preparations WHERE id = " + str(preparation_id))
        if not re.fullmatch(r"[0-9a-f]{64}", key):
            raise IntegrationFailure("managed artifact lookup found no bound candidate key")
        return self.managed_state_dir / f"job-{preparation_id}-{key}"

    @staticmethod
    def transport_evidence(value: Any) -> Any:
        """Server omits CLI-local routing coordinates, never canonical evidence."""
        if not isinstance(value, dict):
            return value
        return {key: item for key, item in value.items() if key not in {"root", "out_root"}}

    @staticmethod
    def snapshot_only_parity_document(value: Any) -> Any:
        """Only snapshot-only admission intentionally does not share CLI freshness assessment."""
        if not isinstance(value, dict):
            return value
        result = copy.deepcopy(RetrievalIntegrationSuite.transport_evidence(value))
        result.pop("index_freshness", None)
        result.pop("freshness_scope", None)
        return result

    def assert_wrapped_core(
        self,
        response: dict[str, Any],
        expected: dict[str, Any],
        *,
        admission: str,
    ) -> dict[str, Any]:
        result = response.get("result")
        if not isinstance(result, dict):
            raise IntegrationFailure("retrieval response has no result wrapper")
        if result.get("project_id") != PROJECT_ID or result.get("route_id") != ROUTE_ID:
            raise IntegrationFailure("retrieval wrapper returned the wrong route identity")
        active = self.active_generations.get((PROJECT_ID, ROUTE_ID))
        if active is None:
            raise IntegrationFailure("retrieval response was observed before an explicit managed activation")
        if result.get("revision") != active["revision"] or result.get("preparation_id") != active["preparation_id"]:
            raise IntegrationFailure("retrieval wrapper did not bind evidence to its managed activation")
        if result.get("admission") != admission:
            raise IntegrationFailure("retrieval wrapper reported the wrong admission mode")
        if result.get("snapshot_id") != expected.get("snapshot_id"):
            raise IntegrationFailure("retrieval wrapper and Core result disagree on snapshot identity")
        evidence = result.get("evidence")
        if not isinstance(evidence, dict):
            raise IntegrationFailure("Core result is not nested at response.result.evidence")
        if "root" in evidence or "out_root" in evidence:
            raise IntegrationFailure("server evidence exposed CLI-local routing coordinates")
        if admission == "exact-current" and evidence != self.transport_evidence(expected):
            raise IntegrationFailure("server result differs from the same compact Core retrieval")
        if admission == "snapshot-only":
            freshness = evidence.get("index_freshness")
            if (not isinstance(freshness, dict) or freshness.get("status") != "not-assessed" or
                    freshness.get("current") is not False or freshness.get("fully_verified") is not False):
                raise IntegrationFailure("snapshot-only response did not retain unassessed freshness")
            if self.snapshot_only_parity_document(evidence) != self.snapshot_only_parity_document(expected):
                raise IntegrationFailure("snapshot-only result differs from the same compact Core retrieval")
        return evidence

    def assert_failure_without_paths(self, status: int, response: dict[str, Any], *paths: Path) -> None:
        if status < 400 or response.get("ok") is not False:
            raise IntegrationFailure("invalid retrieval state did not fail")
        error = response.get("error")
        if not isinstance(error, dict):
            raise IntegrationFailure("retrieval failure has no error envelope")
        rendered = json.dumps(error, sort_keys=True)
        if any(str(path) in rendered for path in paths):
            raise IntegrationFailure("retrieval error leaked a filesystem path")

    def assert_error_code(self, response: dict[str, Any], expected: str) -> None:
        error = response.get("error")
        if not isinstance(error, dict) or error.get("code") != expected:
            raise IntegrationFailure(f"retrieval failure did not return {expected}")

    def compact_brief(self, *, require_current: bool = True, limit: int | None = None) -> dict[str, Any]:
        body: dict[str, Any] = {"query": "snapshot admission evidence"}
        if limit is not None:
            body["limit"] = limit
        if not require_current:
            body["require_current"] = False
        status, response = self.post_retrieval(PROJECT_ID, "brief", body)
        if status != 200:
            raise IntegrationFailure(f"compact Brief unexpectedly returned HTTP {status}")
        arguments = ["brief", body["query"], "--projection", "compact"]
        if limit is not None:
            arguments.extend(["--limit", str(limit)])
        if require_current:
            arguments.append("--require-current")
        return self.assert_wrapped_core(
            response,
            self.run_cli_json(*arguments),
            admission="exact-current" if require_current else "snapshot-only",
        )

    def task_request(self) -> dict[str, Any]:
        return {
            "schema_version": "shin.task-request.v1",
            "request_id": "retrieval-http-fixture",
            "task": {
                "objective": "Retrieve snapshot admission evidence.",
                "scope": [{"id": "route", "query": "route project identity profile"}],
                "must": [{"id": "snapshot", "query": "snapshot admission evidence"}],
                "should": [],
            },
            "budget": {
                "max_evidence_bytes": 65536,
                "max_records": 32,
                "per_query_records": 16,
                "per_document_records": 8,
            },
        }

    def check_cli_server_parity_and_cache(self) -> None:
        search_status, search_response = self.post_retrieval(PROJECT_ID, "search", {"query": "snapshot admission"})
        if search_status != 200:
            raise IntegrationFailure("default Search did not succeed")
        search = self.assert_wrapped_core(
            search_response,
            self.run_cli_json("search", "snapshot admission", "--require-current"),
            admission="exact-current",
        )
        if search.get("limit") != 10:
            raise IntegrationFailure("Search did not retain the Core default limit of 10")

        brief = self.compact_brief()
        brief_budget = brief.get("evidence_budget")
        if not isinstance(brief_budget, dict) or brief_budget.get("max_records") != 6:
            raise IntegrationFailure("Brief did not retain compact Core semantics and default limit 6")

        task = self.task_request()
        self.write_fixture_file(self.task_path, json.dumps(task, separators=(",", ":")) + "\n")
        pack_status, pack_response = self.post_retrieval(PROJECT_ID, "pack", {"task_request": task})
        if pack_status != 200:
            raise IntegrationFailure("compact Pack did not succeed")
        pack = self.assert_wrapped_core(
            pack_response,
            self.run_cli_json("pack", "--input", str(self.task_path), "--jobs", "1", "--projection", "compact", "--require-current"),
            admission="exact-current",
        )
        if pack.get("schema_version") != "shin.agent-context-pack-compact.v1" or pack.get("projection") != "compact":
            raise IntegrationFailure("Pack did not retain compact projection")

        self.expect_status(403, "GET", "/api/v1/cache", headers=self.auth_headers("reader"))
        cache = self.expect_status(200, "GET", "/api/v1/cache", headers=self.auth_headers("admin")).get("result")
        counters = {
            "resident_generations",
            "resident_payload_bytes",
            "reserved_payload_bytes",
            "hits",
            "loads",
            "evictions",
            "maximum_payload_bytes",
            "maximum_generations",
        }
        expected = counters | {"load_in_progress", "accounting"}
        if not isinstance(cache, dict) or set(cache) != expected:
            raise IntegrationFailure("admin cache result does not match the counter contract")
        if any(type(cache[name]) is not int or cache[name] < 0 for name in counters):
            raise IntegrationFailure("admin cache result contains an invalid counter")
        if cache["load_in_progress"] is not False and cache["load_in_progress"] is not True:
            raise IntegrationFailure("admin cache result has an invalid load-in-progress value")
        if cache["accounting"] != "stored-artifact-bytes-not-process-rss":
            raise IntegrationFailure("admin cache result has an unexpected accounting boundary")
        if cache["maximum_payload_bytes"] != 536870912 or cache["maximum_generations"] != 8:
            raise IntegrationFailure("admin cache result lost the configured bounds")

    def check_full_section_publication(self) -> None:
        query = "LateServiceNeedle"
        status, response = self.post_retrieval(PROJECT_ID, "brief", {"query": query})
        if status != 200:
            raise IntegrationFailure("late section body was not retrievable through the service")
        brief = self.assert_wrapped_core(
            response, self.run_cli_json("brief", query, "--projection", "compact", "--require-current"),
            admission="exact-current",
        )
        rows = brief.get("evidence", [])
        if (len(rows) != 2 or any(row.get("path") != "docs/long-body.md" for row in rows) or
                {(row.get("start_line"), row.get("end_line")) for row in rows} != {(1, 2), (2, 2)}):
            raise IntegrationFailure("late section retrieval did not retain its exact source")
        if any(row.get("indexed_text_bytes", 0) <= 4000 or
               row.get("indexed_text_omitted_bytes") != 0 or
               query in row.get("excerpt", "") for row in rows):
            raise IntegrationFailure("compact section bounds or preview differ from the published body")
        task = {
            "schema_version": "shin.task-request.v1", "request_id": "service-full-section",
            "task": {"objective": "Verify late source evidence", "must": [
                {"id": "late", "query": query, "require_full_coverage": True},
                {"id": "same", "query": query, "require_full_coverage": True},
            ]},
            "evidence_types": ["section"],
            "budget": {"max_evidence_bytes": 4000, "max_records": 1,
                       "per_query_records": 1, "per_document_records": 1},
        }
        self.write_fixture_file(self.task_path, json.dumps(task) + "\n")
        status, response = self.post_retrieval(PROJECT_ID, "pack", {"task_request": task})
        if status != 200:
            raise IntegrationFailure("late-body Pack did not succeed through the service")
        pack = self.assert_wrapped_core(
            response, self.run_cli_json("pack", "--input", str(self.task_path), "--jobs", "1",
                                       "--projection", "compact", "--require-current"),
            admission="exact-current",
        )
        evidence = pack.get("evidence", [])
        if (pack.get("completion", {}).get("complete") is not True or len(evidence) != 1 or
                len(evidence[0].get("matches", [])) != 2 or
                "indexed_text" in evidence[0].get("record", {}) or
                evidence[0].get("record", {}).get("indexed_text_bytes", 0) <= 4000 or
                len(json.dumps(evidence, separators=(",", ":")).encode()) > 4000):
            raise IntegrationFailure("service Pack lost exact sharing or exposed search-only text")

    def check_request_guards_and_budgets(self) -> None:
        endpoint = f"/api/v1/projects/{PROJECT_ID}/routes/{ROUTE_ID}/brief"
        headers = {**self.auth_headers("admin"), "Content-Type": "application/json"}
        invalid_bodies: list[Any] = [
            b"",
            [],
            {"query": 1},
            {"query": "snapshot", "require_current": "true"},
            {"query": "snapshot", "limit": 0},
            {"query": "snapshot", "limit": 33},
            {"query": "snapshot", "unexpected": True},
            {"query": "snapshot", "input": str(self.task_path)},
        ]
        for body in invalid_bodies:
            encoded = body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
            self.expect_status(400, "POST", endpoint, headers=headers, body=encoded)
        self.expect_status(413, "POST", endpoint, headers=headers, body=json.dumps({"query": "x" * MAX_BODY_BYTES}).encode("utf-8"))
        self.expect_status(404, "POST", f"/api/v1/projects/{PROJECT_ID}/routes/missing/brief", headers=headers, body=b"{}")

        denied_status, denied = self.post_retrieval("other", "brief", {"query": "snapshot"}, role="reader")
        if denied_status != 404:
            raise IntegrationFailure("denied project lookup did not return HTTP 404 before route I/O")
        self.assert_failure_without_paths(denied_status, denied, self.out_dir)

        task = self.task_request()
        pack_endpoint = f"/api/v1/projects/{PROJECT_ID}/routes/{ROUTE_ID}/pack"
        pack_headers = {**self.auth_headers("admin"), "Content-Type": "application/json"}
        invalid_pack: list[Any] = [
            {},
            {"task_request": []},
            {"task_request": task, "input": str(self.task_path)},
        ]
        oversized = copy.deepcopy(task)
        oversized["budget"]["max_evidence_bytes"] = 65537
        invalid_pack.append({"task_request": oversized})
        oversized = copy.deepcopy(task)
        oversized["budget"]["max_records"] = 33
        invalid_pack.append({"task_request": oversized})
        oversized = copy.deepcopy(task)
        oversized["budget"]["per_query_records"] = 17
        invalid_pack.append({"task_request": oversized})
        too_many = copy.deepcopy(task)
        too_many["task"]["must"] = [
            {"id": f"requirement-{index}", "query": "snapshot admission"} for index in range(17)
        ]
        invalid_pack.append({"task_request": too_many})
        for payload in invalid_pack:
            self.expect_status(400, "POST", pack_endpoint, headers=pack_headers, body=json.dumps(payload).encode("utf-8"))
        self.expect_status(413, "POST", pack_endpoint, headers=pack_headers, body=json.dumps({"task_request": {"padding": "x" * MAX_BODY_BYTES}}).encode("utf-8"))

    def check_project_route_isolation(self) -> None:
        headers = {**self.auth_headers("admin"), "Content-Type": "application/json"}
        path = "/api/v1/projects/other/routes/other-retrieval/search"
        payload = b'{"query":"OtherProjectOnly"}'
        response = self.expect_status(200, "POST", path, headers=headers, body=payload)
        result = response["result"]
        if result["project_id"] != "other" or result["route_id"] != "other-retrieval":
            raise IntegrationFailure("retrieval crossed its configured project route")
        if "OtherProjectOnly" not in json.dumps(result["evidence"]):
            raise IntegrationFailure("other project did not return its own evidence")
        self.expect_status(404, "POST", "/api/v1/projects/visible/routes/other-retrieval/search",
                           headers=headers, body=payload)
        hidden = self.other_out_dir.with_name("other-hidden")
        self.other_out_dir.rename(hidden)
        try:
            self.expect_status(404, "POST", path,
                               headers={**self.auth_headers("reader"), "Content-Type": "application/json"}, body=payload)
        finally:
            hidden.rename(self.other_out_dir)
        self.expect_status(200, "POST", f"/api/v1/projects/{PROJECT_ID}/routes/{ROUTE_ID}/brief",
                           headers={**self.auth_headers("reader"), "Content-Type": "application/json"},
                           body=b'{"query":"snapshot admission"}')

    def check_route_workspace_fails_closed(self) -> None:
        self.stop_server(expect_clean=True)
        config = json.loads(self.runtime_config_path.read_text(encoding="utf-8"))
        route = next(route for route in config["retrieval"]["routes"] if route["id"] == ROUTE_ID)
        route["root"] = str(self.other_root)
        self.replace_private_json(self.runtime_config_path, config)
        self.start_server()
        try:
            self.expect_status(
                409,
                "POST",
                f"/api/v1/projects/{PROJECT_ID}/routes/{ROUTE_ID}/brief",
                headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
                body=b'{"query":"snapshot admission"}',
            )
        finally:
            self.stop_server(expect_clean=True)
            route["root"] = str(self.fixture_root)
            self.replace_private_json(self.runtime_config_path, config)
            self.start_server()
        try:
            self.stop_server(expect_clean=True)
            route["workspace_id"] = "missing-workspace"
            self.replace_private_json(self.runtime_config_path, config)
            self.start_server()
            self.expect_status(
                404,
                "POST",
                f"/api/v1/projects/{PROJECT_ID}/routes/{ROUTE_ID}/brief",
                headers={**self.auth_headers("admin"), "Content-Type": "application/json"},
                body=b'{"query":"snapshot admission"}',
            )
        finally:
            self.stop_server(expect_clean=True)
            route["workspace_id"] = "primary"
            self.replace_private_json(self.runtime_config_path, config)
            self.start_server()

    def check_serial_and_concurrent_determinism(self) -> None:
        payload = {"query": "snapshot admission evidence", "limit": 6}
        serial: list[dict[str, Any]] = []
        for _ in range(3):
            status, response = self.post_retrieval(PROJECT_ID, "brief", payload)
            if status != 200:
                raise IntegrationFailure("serial retrieval did not succeed")
            serial.append(response["result"]["evidence"])
        if any(value != serial[0] for value in serial[1:]):
            raise IntegrationFailure("serial retrieval results are not deterministic")

        barrier = threading.Barrier(5)
        results: list[dict[str, Any]] = []
        failures: list[BaseException] = []
        lock = threading.Lock()

        def retrieve() -> None:
            try:
                barrier.wait(timeout=5.0)
                status, response = self.post_retrieval(PROJECT_ID, "brief", payload)
                if status != 200:
                    raise IntegrationFailure("concurrent retrieval did not succeed")
                with lock:
                    results.append(response["result"]["evidence"])
            except BaseException as error:
                with lock:
                    failures.append(error)

        workers = [threading.Thread(target=retrieve) for _ in range(4)]
        for worker in workers:
            worker.start()
        barrier.wait(timeout=5.0)
        for worker in workers:
            worker.join(timeout=10.0)
        if any(worker.is_alive() for worker in workers) or failures or len(results) != 4:
            raise IntegrationFailure("concurrent retrieval workers did not complete")
        if any(value != serial[0] for value in results):
            raise IntegrationFailure("concurrent retrieval changed the selected evidence")

    def check_warm_artifact_corruption(self) -> None:
        payload = {"query": "snapshot admission evidence", "require_current": False}
        status, before = self.post_retrieval(PROJECT_ID, "brief", payload)
        if status != 200:
            raise IntegrationFailure("snapshot-only baseline failed")
        artifact = self.managed_artifact(PROJECT_ID, ROUTE_ID) / "docs" / "kb" / "generated" / "sections.jsonl"
        original = artifact.read_text(encoding="utf-8")
        try:
            self.write_fixture_file(artifact, original + "{corrupted artifact\n")
            strict_status, strict = self.post_retrieval(PROJECT_ID, "brief", {"query": payload["query"]})
            if strict_status != 409:
                raise IntegrationFailure("warm strict retrieval ignored generated artifact corruption")
            self.assert_failure_without_paths(strict_status, strict, self.fixture_root, self.managed_state_dir)
            status, after = self.post_retrieval(PROJECT_ID, "brief", payload)
            if status != 200 or before != after:
                raise IntegrationFailure("resident snapshot-only view read corrupted live artifact bytes")
        finally:
            self.write_fixture_file(artifact, original)
        self.compact_brief()

    def check_staleness_generation_and_route_identity(self) -> None:
        original = (self.fixture_root / "docs" / "runtime.md").read_text(encoding="utf-8")
        self.write_fixture_file(self.fixture_root / "docs" / "runtime.md", original + "\nSource drift is deliberate.\n")
        strict_status, strict = self.post_retrieval(PROJECT_ID, "brief", {"query": "snapshot admission evidence"})
        if strict_status != 409:
            raise IntegrationFailure("strict source drift did not return HTTP 409")
        self.assert_failure_without_paths(strict_status, strict, self.fixture_root, self.out_dir)
        self.assert_error_code(strict, "stale-index")

        snapshot_only = self.compact_brief(require_current=False)
        if snapshot_only.get("snapshot_id") is None:
            raise IntegrationFailure("snapshot-only retrieval lost its immutable snapshot identity")

        active_before_rebuild = self.activation(PROJECT_ID, ROUTE_ID)
        old_snapshot = snapshot_only["snapshot_id"]
        self.run_cli_json("build")
        rebuilt = self.run_cli_json("brief", "snapshot admission evidence", "--projection", "compact", "--require-current")
        if not isinstance(rebuilt.get("snapshot_id"), str) or rebuilt["snapshot_id"] == old_snapshot:
            raise IntegrationFailure("explicit CLI oracle rebuild did not change its own generation")
        if self.activation(PROJECT_ID, ROUTE_ID) != active_before_rebuild:
            raise IntegrationFailure("plain CLI build changed the server-managed active generation")
        still_stale, stale = self.post_retrieval(PROJECT_ID, "brief", {"query": "snapshot admission evidence"})
        if still_stale != 409 or stale.get("error", {}).get("code") != "stale-index":
            raise IntegrationFailure("CLI oracle rebuild implicitly changed managed retrieval selection")

        profile_path = self.profile_dir / f"{PROFILE_ID}.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        original_profile = json.dumps(profile, sort_keys=True, separators=(",", ":")) + "\n"
        profile["markdownChunkLines"] = 25
        self.write_fixture_file(profile_path, json.dumps(profile, sort_keys=True, separators=(",", ":")) + "\n")
        profile_status, profile_error = self.post_retrieval(PROJECT_ID, "brief", {"query": "snapshot admission evidence"})
        if profile_status != 409:
            raise IntegrationFailure("profile mutation did not return HTTP 409")
        self.assert_failure_without_paths(profile_status, profile_error, profile_path, self.out_dir)
        self.assert_error_code(profile_error, "route-profile-changed")
        self.write_fixture_file(profile_path, original_profile)
        self.write_fixture_file(self.fixture_root / "docs" / "runtime.md", original)
        self.run_cli_json("build")
        if self.activation(PROJECT_ID, ROUTE_ID) != active_before_rebuild:
            raise IntegrationFailure("restoring the CLI oracle changed managed retrieval selection")
        self.compact_brief()

    def check_corrupt_output_and_root_replacement(self) -> None:
        manifest = self.managed_artifact(PROJECT_ID, ROUTE_ID) / "docs" / "kb" / "generated" / "manifest.json"
        self.stop_server(expect_clean=True)
        original_manifest = manifest.read_text(encoding="utf-8")
        try:
            self.write_fixture_file(manifest, "{corrupt snapshot\n")
            self.start_server()
            status, response = self.post_retrieval(PROJECT_ID, "brief", {"query": "snapshot admission evidence"})
            self.assert_failure_without_paths(status, response, self.fixture_root, self.out_dir, manifest)
        finally:
            self.stop_server(expect_clean=True)
            self.write_fixture_file(manifest, original_manifest)

        self.start_server()
        self.compact_brief()
        root_replaced = False
        try:
            if self.fixture_root_before_replacement.exists():
                shutil.rmtree(self.fixture_root_before_replacement)
            self.fixture_root.rename(self.fixture_root_before_replacement)
            root_replaced = True
            self.fixture_root.mkdir(mode=0o700)
            self.write_fixture_file(
                self.fixture_root / "README.md",
                "# Replacement Root\n\nThis is a different directory identity.\n",
            )
            status, response = self.post_retrieval(PROJECT_ID, "brief", {"query": "snapshot admission evidence"})
            if status != 409:
                raise IntegrationFailure("root replacement did not return HTTP 409")
            self.assert_failure_without_paths(status, response, self.fixture_root, self.out_dir)
            self.assert_error_code(response, "root-identity-changed")
        finally:
            self.stop_server(expect_clean=True)
            if root_replaced:
                if self.fixture_root.exists():
                    shutil.rmtree(self.fixture_root)
                if self.fixture_root_before_replacement.exists():
                    self.fixture_root_before_replacement.rename(self.fixture_root)
            self.start_server()

    @staticmethod
    def percentile_ms(samples: list[float], percentile: int) -> float:
        if not samples:
            raise IntegrationFailure("latency benchmark received no samples")
        rank = max(0, min(len(samples) - 1, (len(samples) * percentile + 99) // 100 - 1))
        return round(sorted(samples)[rank], 3)

    def timed_retrieval(self, payload: dict[str, Any], admission: str) -> float:
        started = time.perf_counter_ns()
        status, response = self.post_retrieval(PROJECT_ID, "brief", payload)
        elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
        if status != 200:
            raise IntegrationFailure("latency benchmark retrieval did not succeed")
        result = response.get("result")
        if not isinstance(result, dict) or result.get("admission") != admission or not isinstance(result.get("evidence"), dict):
            raise IntegrationFailure("latency benchmark retrieval returned an invalid result wrapper")
        return elapsed_ms

    def check_retrieval_latency_benchmark(self) -> None:
        strict_payload = {"query": "snapshot admission evidence"}
        snapshot_payload = {"query": "snapshot admission evidence", "require_current": False}
        for _ in range(4):
            self.timed_retrieval(strict_payload, "exact-current")
            self.timed_retrieval(snapshot_payload, "snapshot-only")

        strict_samples: list[float] = []
        snapshot_samples: list[float] = []
        for _ in range(30):
            strict_samples.append(self.timed_retrieval(strict_payload, "exact-current"))
            snapshot_samples.append(self.timed_retrieval(snapshot_payload, "snapshot-only"))
        cache = self.expect_status(200, "GET", "/api/v1/cache", headers=self.auth_headers("admin")).get("result")
        if not isinstance(cache, dict):
            raise IntegrationFailure("latency benchmark could not read cache statistics")
        self.retrieval_benchmark = {
            "status": "completed",
            "transport": "loopback-http-one-connection-per-request",
            "warmup_pairs": 4,
            "alternating_pairs": 30,
            "strict_exact_current": {
                "samples": len(strict_samples),
                "p50_ms": self.percentile_ms(strict_samples, 50),
                "p95_ms": self.percentile_ms(strict_samples, 95),
            },
            "snapshot_only": {
                "samples": len(snapshot_samples),
                "p50_ms": self.percentile_ms(snapshot_samples, 50),
                "p95_ms": self.percentile_ms(snapshot_samples, 95),
            },
            "cache": cache,
        }

    def write_report(self, *, ok: bool, failure: str | None = None) -> None:
        super().write_report(ok=ok, failure=failure)
        report = json.loads(self.report_path.read_text(encoding="utf-8"))
        report["retrieval_benchmark"] = self.retrieval_benchmark
        report["oidc_http_measurement"] = getattr(self, "oidc_http_measurement", None)
        report["personal_http_measurement"] = getattr(self, "personal_http_measurement", None)
        report["personal_mutation_measurement"] = getattr(self, "personal_mutation_measurement", None)
        self.replace_private_json(self.report_path, report)

    def check_scoped_workspace_retrieval(self) -> None:
        self.stop_server(expect_clean=True)
        baseline_active = self.scalar("SELECT COALESCE(jsonb_agg(to_jsonb(a)),'[]'::jsonb)::text "
                                      "FROM shin_server.active_routes a WHERE project_id IN ('visible','other')")
        try:
            probe = self.binary.parent / "shin-server-scoped-retrieval-probe"
            result = self.run_process([str(probe), str(self.runtime_config_path)], "native scoped retrieval", timeout=60.0)
            receipt = json.loads(result.stdout)
            if receipt.get("ok") is not True or receipt.get("checks", 0) < 60 or receipt.get("scoped_publications") is not True:
                raise IntegrationFailure("scoped retrieval receipt is incomplete")
            from personal_http_checks import check_personal_http
            self.record("personal-http-retrieval-journal-isolation-revocation-restart", lambda: check_personal_http(self))
            from oidc_http_checks import check_oidc_http
            self.record("oidc-http-device-isolation-revocation-dynamic-workers", lambda: check_oidc_http(self))
            from scoped_history_checks import check_scoped_history
            self.record("native-scoped-publication-history-all-source-authorization", lambda: check_scoped_history(self))
            self.record("native-scoped-checkpoint-read-write-revocation-resume-impact", self.check_scoped_checkpoints)
            from checkpoint_rls_checks import check_checkpoint_rls
            self.record("checkpoint-rls-unfiltered-runtime-sql-and-transaction-reset", lambda: check_checkpoint_rls(self))
            self.record("native-scoped-preparation-promotion-lifecycle-revocation", self.check_scoped_preparations)
        finally:
            # Restore this test's previously qualified route pointers, not a product rollback.
            self.psql("UPDATE shin_server.active_routes a SET revision=b.revision,preparation_id=b.preparation_id,"
                      "snapshot_id=b.snapshot_id,route_fingerprint=b.route_fingerprint,scope_workspace_id=b.scope_workspace_id FROM jsonb_populate_recordset("
                      "NULL::shin_server.active_routes,'" + baseline_active.replace("'", "''") + "'::jsonb) b "
                      "WHERE a.project_id=b.project_id AND a.route_id=b.route_id")
            self.psql("DELETE FROM shin_server.checkpoint_slots WHERE project_id IN ('visible','other') "
                      "AND owner_id IN ('scoped-alice','scoped-bob')")
            # Only this dedicated fixture's publication rows; source candidates
            # and all prior registry/audit fixtures remain available to the suite.
            self.psql("BEGIN; DELETE FROM shin_server.audit_events WHERE publication_proposal_id IN "
                      "(SELECT id FROM shin_server.publication_proposals WHERE project_id IN ('visible','other') "
                      "AND publication_id IN ('scoped-publication','scoped-promotion-a','scoped-promotion-b','scoped-promotion-mixed')); "
                      "DELETE FROM shin_server.publication_heads WHERE project_id IN ('visible','other') AND publication_id IN ('scoped-publication','scoped-promotion-a','scoped-promotion-b','scoped-promotion-mixed'); "
                      "DELETE FROM shin_server.publication_withdrawals WHERE project_id IN ('visible','other') AND publication_id IN ('scoped-publication','scoped-promotion-a','scoped-promotion-b','scoped-promotion-mixed'); "
                      "DELETE FROM shin_server.publication_resolutions WHERE project_id IN ('visible','other') AND publication_id IN ('scoped-publication','scoped-promotion-a','scoped-promotion-b','scoped-promotion-mixed'); "
                      "DELETE FROM shin_server.publication_proposals WHERE project_id IN ('visible','other') AND publication_id IN ('scoped-publication','scoped-promotion-a','scoped-promotion-b','scoped-promotion-mixed'); COMMIT")
            self.psql("BEGIN; DELETE FROM shin_server.audit_events WHERE principal_id IN ('scoped-alice','scoped-bob') "
                      "AND activation_route_id IS NOT NULL; "
                      "DELETE FROM shin_server.activation_receipts WHERE principal_id IN ('scoped-alice','scoped-bob'); "
                      "CREATE TEMP TABLE scoped_reclaimed ON COMMIT DROP AS SELECT p.id FROM shin_server.preparations p "
                      "JOIN shin_server.preparation_retention r ON r.preparation_id=p.id AND r.state='released' "
                      "WHERE p.principal_id IN ('scoped-alice','scoped-bob'); "
                      "DELETE FROM shin_server.audit_events WHERE preparation_id IN (SELECT id FROM scoped_reclaimed); "
                      "DELETE FROM shin_server.preparation_retention WHERE preparation_id IN (SELECT id FROM scoped_reclaimed); "
                      "DELETE FROM shin_server.preparations WHERE id IN (SELECT id FROM scoped_reclaimed); COMMIT")
            # These two projects contain declared request-ledger metadata only, no disk artifacts.
            self.psql("DELETE FROM shin_server.audit_events WHERE project_id IN ('keys-project-a','keys-project-b'); "
                      "DELETE FROM shin_server.publication_proposals WHERE project_id IN ('keys-project-a','keys-project-b'); "
                      "DELETE FROM shin_server.preparations WHERE project_id IN ('keys-project-a','keys-project-b')")
            self.psql("DELETE FROM shin_server.workspace_publication_history; "
                      "DELETE FROM shin_server.retrieval_journal_attempts WHERE scope_principal_id IN ('scoped-alice','scoped-bob'); "
                      "DELETE FROM shin_server.identity_ownership_audit; DELETE FROM shin_server.identity_workspace_owners; "
                      "DELETE FROM shin_server.identity_project_owners; DELETE FROM shin_server.identity_audit; "
                      "DELETE FROM shin_server.identity_memberships; DELETE FROM shin_server.identity_tenants; "
                      "DELETE FROM shin_server.identity_accounts")
            self.psql("DELETE FROM shin_server.workspace_requests WHERE project_id IN ('keys-project-a','keys-project-b'); "
                      "DELETE FROM shin_server.workspaces WHERE project_id IN ('keys-project-a','keys-project-b'); "
                      "DELETE FROM shin_server.idempotency_requests WHERE key IN ('keys-project-a','keys-project-b'); "
                      "DELETE FROM shin_server.projects WHERE id IN ('keys-project-a','keys-project-b')")
            self.start_server()

    def check_scoped_checkpoints(self) -> None:
        probe = self.binary.parent / "shin-server-scoped-checkpoint-probe"
        result = self.run_process([str(probe), str(self.runtime_config_path)], "native scoped checkpoints", timeout=60.0)
        receipt = json.loads(result.stdout)
        if (receipt.get("ok") is not True or receipt.get("scoped_checkpoints") is not True
                or receipt.get("scoped_mutations") is not True or receipt.get("checks", 0) < 60):
            raise IntegrationFailure("scoped checkpoint receipt is incomplete")

    def check_scoped_preparations(self) -> None:
        probe = self.binary.parent / "shin-server-scoped-preparation-probe"
        result = self.run_process([str(probe), str(self.runtime_config_path)], "native scoped preparations", timeout=60.0)
        receipt = json.loads(result.stdout)
        if (receipt.get("ok") is not True or receipt.get("scoped_jobs") is not True
                or receipt.get("scoped_promotion") is not True or receipt.get("scoped_retention") is not True
                or receipt.get("scoped_request_keys") is not True or receipt.get("scoped_cleanup") is not True
                or receipt.get("personal_routes_without_publications") is not True
                or receipt.get("checks", 0) < 150):
            raise IntegrationFailure("scoped preparation receipt is incomplete")

    def run(self) -> None:
        # Preserve the P1 safety checks, real PostgreSQL lifecycle, and subprocess cleanup first.
        super().run()
        try:
            self.prepare_retrieval_fixture()
            self.record("registry-external-nogit-workspace-and-currentness", self.check_external_nogit_workspace_and_currentness)
            self.configure_retrieval()
            self.start_server()
            self.record("retrieval-explicit-managed-seed", lambda: self.seed_managed_retrieval_routes(with_scoped_publications=True))
            self.stop_server(expect_clean=True)
            self.disable_preparation_worker()
            self.start_server()
            self.record("retrieval-cli-server-parity-and-cache", self.check_cli_server_parity_and_cache)
            self.record("native-scoped-workspace-retrieval-cache-revocation", self.check_scoped_workspace_retrieval)
            self.record("retrieval-full-section-publication", self.check_full_section_publication)
            self.record("retrieval-request-guards-and-budgets", self.check_request_guards_and_budgets)
            self.record("retrieval-project-route-isolation", self.check_project_route_isolation)
            self.record("retrieval-route-workspace-fails-closed", self.check_route_workspace_fails_closed)
            self.record("retrieval-serial-and-concurrent-determinism", self.check_serial_and_concurrent_determinism)
            self.record("retrieval-warm-artifact-corruption", self.check_warm_artifact_corruption)
            self.record("retrieval-staleness-generation-and-profile-identity", self.check_staleness_generation_and_route_identity)
            self.record("retrieval-corrupt-output-and-root-replacement", self.check_corrupt_output_and_root_replacement)
            self.record("retrieval-latency-benchmark", self.check_retrieval_latency_benchmark)
            self.stop_server(expect_clean=True)
        finally:
            self.stop_server(expect_clean=False)
            self.restore_configs()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="native shin-server executable")
    parser.add_argument("--core", default=str(DEFAULT_CORE), help="candidate shin-context executable")
    parser.add_argument("--state-root", default=str(BUILD_ROOT / "server-dev"), help="existing isolated server development state")
    parser.add_argument("--report", help="new report path below this worktree's build directory")
    return parser.parse_args(argv)


def validate_candidate_core(core: Path) -> bool:
    if not under_build(core):
        return False
    expected_version = (REPOSITORY_ROOT / "VERSION").read_text(encoding="utf-8").strip()
    try:
        completed = subprocess.run(
            [str(core), "--version"],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10.0,
        )
        version = json.loads(completed.stdout)
    except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError):
        return False
    return (
        completed.returncode == 0
        and isinstance(version, dict)
        and version.get("tool") == "shin-context"
        and version.get("version") == expected_version
    )


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    binary = Path(args.binary).resolve(strict=False)
    core = Path(args.core).resolve(strict=False)
    state_root = Path(args.state_root).resolve(strict=False)
    report_path = (Path(args.report).resolve(strict=False) if args.report else
                   state_root / "reports" / f"retrieval-integration-{secrets.token_hex(8)}.json")
    if not binary.is_file() or not os.access(binary, os.X_OK):
        print("error: --binary must name an executable native shin-server", file=sys.stderr)
        return 2
    if not core.is_file() or not os.access(core, os.X_OK):
        print("error: --core must name an executable candidate shin-context", file=sys.stderr)
        return 2
    if not validate_candidate_core(core):
        print("error: --core must be this worktree's build executable at the current VERSION", file=sys.stderr)
        return 2
    if not under_build(state_root) or not under_build(report_path):
        print("error: --state-root and --report must be below this worktree's build directory", file=sys.stderr)
        return 2
    if report_path.exists():
        print("error: --report already exists", file=sys.stderr)
        return 2

    suite = RetrievalIntegrationSuite(binary, core, state_root, report_path)
    try:
        suite.run()
    except Exception as error:
        suite.stop_server(expect_clean=False)
        if isinstance(error, IntegrationFailure):
            suite.failure_assertion = str(error)
        try:
            suite.write_report(ok=False, failure=type(error).__name__)
        except OSError:
            pass
        print(f"error: retrieval integration test failed: {type(error).__name__}", file=sys.stderr)
        return 1
    suite.write_report(ok=True)
    print("retrieval integration test passed; compact report written below the isolated build state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
