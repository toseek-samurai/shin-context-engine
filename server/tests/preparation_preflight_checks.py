#!/usr/bin/env python3
"""HTTP checks for read-only prepared-candidate quality preflight.

This mixin deliberately owns only its private fixture profile registry.  The
main preparation runner owns lifecycle ordering, database reset and report
composition.
"""

from __future__ import annotations

import fcntl
import json
import os
from pathlib import Path
import secrets
import subprocess
import threading
import time
from typing import Any

from integration_test import IntegrationFailure
from retrieval_integration_test import PROJECT_ID, ROUTE_ID


_PUBLIC_JOB_FIELDS = {
    "id",
    "project_id",
    "route_id",
    "state",
    "cancel_requested",
    "artifact_snapshot_id",
    "artifact_bytes",
    "reserved_artifact_bytes",
    "reason_code",
    "created_at",
    "updated_at",
}
_CHECKS_PASSED = {
    "route": "matched",
    "writer": "quiescent",
    "artifact": "verified",
    "ledger": "matched",
    "doctor": "passed",
    "source": "current",
    "base": "matched",
}
_SUITE_FIELDS = {"state", "total", "passed", "failed", "query_total", "query_passed", "resource_check"}
_PREFLIGHT_FIELDS = {
    "schema_version",
    "job",
    "scope",
    "started_at",
    "finished_at",
    "checks",
    "candidate_verified",
    "activation_supported",
    "activation_ready",
    "project_quality_assessed",
    "mutations_performed",
    "artifact_snapshot_id",
    "artifact_bytes",
    "profile_check",
    "quality",
    "quality_passed",
    "preflight_passed",
    "reason_codes",
}


class PreparationPreflightChecks:
    """Checks mixed into ``PreparationIntegrationSuite`` by its main runner."""

    def _preflight_profiles_dir(self) -> Path:
        return self.retrieval_root / "preflight-profiles"

    def _preflight_state(self, job_id: int) -> dict[str, Any]:
        state = self._inspection_state(job_id)
        state["active_routes"] = self.scalar(
            "SELECT COALESCE(jsonb_agg(a ORDER BY project_id, route_id)::text, '[]') "
            "FROM shin_server.active_routes a"
        )
        state["activation_receipts"] = self.scalar(
            "SELECT COALESCE(jsonb_agg(a ORDER BY project_id, route_id, previous_revision)::text, '[]') "
            "FROM shin_server.activation_receipts a"
        )
        return state

    def _assert_preflight_state(self, job_id: int, expected: dict[str, Any]) -> None:
        actual = self._preflight_state(job_id)
        if actual != expected:
            changed = ",".join(key for key in expected if actual.get(key) != expected[key])
            raise IntegrationFailure("candidate preflight changed tracked fixture state: " + changed)

    @staticmethod
    def _first_regular_file(root: Path) -> Path:
        for path in sorted(root.rglob("*")):
            if path.is_file() and not path.is_symlink():
                return path
        raise IntegrationFailure("candidate preflight fixture has no regular file to tamper")

    def _preflight_artifact(self, job_id: int) -> Path:
        return self._inspection_artifact(job_id)

    def _build_preflight_active(self, profile_id: str) -> str:
        completed = self.run_process(
            [
                str(self.core),
                "--profile",
                profile_id,
                "--profile-dir",
                str(self._preflight_profiles_dir()),
                "--root",
                str(self.fixture_root),
                "--out-dir",
                str(self.out_dir),
                "build",
            ],
            "preflight active fixture build",
            timeout=45.0,
        )
        try:
            result = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise IntegrationFailure("preflight active fixture build did not return JSON") from error
        if not isinstance(result, dict) or result.get("ok") is not True or result.get("published") is not True:
            raise IntegrationFailure("preflight active fixture build did not publish its generation")
        current = self.run_process(
            [
                str(self.core),
                "--profile",
                profile_id,
                "--profile-dir",
                str(self._preflight_profiles_dir()),
                "--root",
                str(self.fixture_root),
                "--out-dir",
                str(self.out_dir),
                "brief",
                "preflight candidate",
                "--projection",
                "compact",
                "--require-current",
            ],
            "preflight active fixture snapshot verification",
            timeout=45.0,
        )
        try:
            current_result = json.loads(current.stdout)
        except json.JSONDecodeError as error:
            raise IntegrationFailure("preflight active fixture snapshot verification did not return JSON") from error
        snapshot_id = current_result.get("snapshot_id") if isinstance(current_result, dict) else None
        if not isinstance(snapshot_id, str) or not snapshot_id:
            raise IntegrationFailure("preflight active fixture build has no snapshot identity")
        return snapshot_id

    def _write_preflight_profiles(self) -> None:
        registry = self._preflight_profiles_dir()
        if registry.exists():
            raise IntegrationFailure("preflight private profile registry already exists")
        registry.mkdir(mode=0o700)
        os.chmod(registry, 0o700)
        base = json.loads((self.profile_dir / "generic-docs.json").read_text(encoding="utf-8"))

        def profile(profile_id: str, *, eval_failure: bool = False, pack_failure: bool = False,
                    configured: bool = True, malformed: bool = False) -> dict[str, Any]:
            value = json.loads(json.dumps(base))
            value["id"] = profile_id
            if not configured:
                value["evalCases"] = []
                value["packEvalCases"] = []
                return value
            query = "project identity profile fingerprint"
            value["evalCases"] = [{
                "id": profile_id + "-eval",
                "query": query,
                "mustPaths": ["docs/policy.md"],
                "maxCandidateRows": 0 if eval_failure else -1,
                "limit": 6,
                "perDocLimit": 2,
            }]
            value["packEvalCases"] = [{
                "id": profile_id + "-pack",
                "request": {
                    "schema_version": "shin.task-request.v1",
                    "request_id": profile_id + "-pack",
                    "task": {
                        "objective": "Verify the prepared route policy evidence.",
                        "must": [{"id": "route-policy", "query": query}],
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
                "maxRecords": 0 if pack_failure else 6,
                "maxCompactBytes": 16000,
                "requireSerialParallelIdentity": True,
            }]
            if malformed:
                value["evalCases"][0]["mustPaths"] = ["docs/not-in-fixture.md"]
            return value

        profiles = {
            "preflight-passing": profile("preflight-passing"),
            "preflight-eval-failing": profile("preflight-eval-failing", eval_failure=True),
            "preflight-pack-failing": profile("preflight-pack-failing", pack_failure=True),
            "preflight-not-configured": profile("preflight-not-configured", configured=False),
            "preflight-profile-failing": profile("preflight-profile-failing", malformed=True),
        }
        for suffix in ("resource", "coverage", "pack"):
            profile_id = "preflight-advisory-" + suffix
            value = profile(profile_id, eval_failure=True, pack_failure=suffix == "pack")
            value["evalResourcePolicy"] = "advisory"
            value["evalCases"][0]["queryVariants"] = ["fingerprint profile identity project"]
            value["evalCases"][0]["maxMatchedSections"] = 0
            if suffix == "coverage":
                value["evalCases"][0]["minCoveredGroups"] = 64
            profiles[profile_id] = value
        for profile_id, value in profiles.items():
            self.write_fixture_file(registry / f"{profile_id}.json", json.dumps(value, sort_keys=True) + "\n")

    def _configure_preflight_route(self, profile_id: str) -> None:
        if self.server is not None:
            raise IntegrationFailure("preflight routes may only change while the owned server is stopped")
        for path in (self.runtime_config_path, self.migrator_config_path):
            config = json.loads(path.read_text(encoding="utf-8"))
            retrieval = config.get("retrieval")
            if not isinstance(retrieval, dict) or not isinstance(retrieval.get("routes"), list):
                raise IntegrationFailure("preflight fixture has no configured retrieval routes")
            replaced = False
            routes = []
            for route in retrieval["routes"]:
                if route.get("project_id") == PROJECT_ID and route.get("id") == ROUTE_ID:
                    routes.append({**route, "profile": profile_id, "profile_dir": str(self._preflight_profiles_dir())})
                    replaced = True
                else:
                    routes.append(route)
            if not replaced:
                raise IntegrationFailure("preflight fixture could not find the visible retrieval route")
            retrieval["routes"] = routes
            self.replace_private_json(path, config)

    def _prepare_preflight_candidate(self, profile_id: str) -> dict[str, Any]:
        if self.server is not None:
            raise IntegrationFailure("preflight candidate preparation requires a stopped server")
        self._configure_preflight_route(profile_id)
        # This CLI build is an oracle for fixture construction only; it must not become server authority.
        self._build_preflight_active(profile_id)
        self.start_server()
        expected_snapshot = self.current_snapshot_id()
        status, response = self.post_preparation(
            PROJECT_ID,
            ROUTE_ID,
            expected_snapshot,
            "preflight-" + profile_id + "-" + secrets.token_hex(6),
        )
        if status != 202:
            raise IntegrationFailure("preflight candidate preparation was not accepted")
        result = response.get("result")
        if not isinstance(result, dict):
            raise IntegrationFailure("preflight candidate preparation returned no result")
        job = self.assert_public_job(result.get("job"), project_id=PROJECT_ID, route_id=ROUTE_ID)
        return self.wait_for_terminal(job["id"], "prepared")

    def _get_preflight(self, job_id: int, *, role: str = "admin") -> tuple[dict[str, Any], dict[str, Any]]:
        envelope = self.expect_status(
            200,
            "GET",
            f"/api/v1/preparations/{job_id}/preflight",
            headers=self.auth_headers(role),
        )
        report = envelope.get("result")
        if not isinstance(report, dict):
            raise IntegrationFailure("candidate preflight response has no report object")
        return envelope, report

    def _assert_safe_preflight(self, report: dict[str, Any], forbidden: tuple[str, ...] = ()) -> None:
        rendered = json.dumps(report, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        private_values = [str(self.fixture_root), str(self.out_dir), str(self._preflight_profiles_dir())]
        if self.preparation_root is not None:
            private_values.append(str(self.preparation_root))
        private_values.extend((self.admin_token, self.reader_token, *forbidden))
        if any(value and value in rendered for value in private_values):
            raise IntegrationFailure("candidate preflight exposed a path, secret or source content")

        def visit(value: Any) -> None:
            if isinstance(value, dict):
                for key, child in value.items():
                    if key.lower() in {"path", "paths", "message", "content", "detail", "exception", "snippet"}:
                        raise IntegrationFailure("candidate preflight exposed an unsafe diagnostic field")
                    visit(child)
            elif isinstance(value, list):
                for child in value:
                    visit(child)

        visit(report)

    @staticmethod
    def _assert_timestamp(value: Any) -> None:
        if not isinstance(value, str) or not value.endswith("Z"):
            raise IntegrationFailure("candidate preflight did not render a UTC timestamp")
        try:
            time.strptime(value, "%Y-%m-%dT%H:%M:%SZ")
        except ValueError as error:
            raise IntegrationFailure("candidate preflight rendered an invalid timestamp") from error

    def _assert_suite(self, value: Any, *, state: str, total: int | None = None,
                      passed: int | None = None, failed: int | None = None,
                      query_total: int | None = None, query_passed: int | None = None,
                      resource_policy: str = "strict", resource_failed_queries: int = 0,
                      pack_query_count_is_zero: bool = False) -> None:
        if not isinstance(value, dict) or set(value) != _SUITE_FIELDS or value.get("state") != state:
            raise IntegrationFailure("candidate preflight quality suite has an invalid public shape or state")
        if state in {"not-assessed", "unavailable"}:
            if any(value[name] is not None for name in _SUITE_FIELDS - {"state"}):
                raise IntegrationFailure("unassessed candidate preflight quality suite exposed counters")
            return
        counters = ("total", "passed", "failed", "query_total", "query_passed")
        if any(type(value[name]) is not int or value[name] < 0 for name in counters):
            raise IntegrationFailure("candidate preflight quality suite has invalid counters")
        if value["total"] != value["passed"] + value["failed"]:
            raise IntegrationFailure("candidate preflight quality case counters are inconsistent")
        if value["query_passed"] > value["query_total"]:
            raise IntegrationFailure("candidate preflight quality query counters are inconsistent")
        expected_resource = None if pack_query_count_is_zero else {
            "policy": resource_policy, "ok": value["query_total"] > 0 and resource_failed_queries == 0,
            "failed_queries": resource_failed_queries,
            "warning_queries": resource_failed_queries if resource_policy == "advisory" else 0}
        if value["resource_check"] != expected_resource:
            raise IntegrationFailure("candidate preflight resource observation is inconsistent")
        if expected_resource is not None:
            resource = value["resource_check"]
            if (type(resource["ok"]) is not bool or
                    type(resource["failed_queries"]) is not int or
                    type(resource["warning_queries"]) is not int):
                raise IntegrationFailure("candidate preflight resource counters have invalid types")
        if total is not None and value["total"] != total:
            raise IntegrationFailure("candidate preflight quality reported the wrong case total")
        if passed is not None and value["passed"] != passed:
            raise IntegrationFailure("candidate preflight quality reported the wrong passed case count")
        if failed is not None and value["failed"] != failed:
            raise IntegrationFailure("candidate preflight quality reported the wrong failed case count")
        if pack_query_count_is_zero:
            if value["query_total"] != 0 or value["query_passed"] != 0:
                raise IntegrationFailure("candidate preflight Pack query counters must remain zero")
        else:
            if query_total is not None and value["query_total"] != query_total:
                raise IntegrationFailure("candidate preflight quality reported the wrong query total")
            if query_passed is not None and value["query_passed"] != query_passed:
                raise IntegrationFailure("candidate preflight quality reported the wrong passed query count")

    @staticmethod
    def _assert_profile_check(value: Any, *, state: str) -> None:
        if not isinstance(value, dict) or set(value) != {"state", "errors", "warnings"} or value.get("state") != state:
            raise IntegrationFailure("candidate preflight profile check has an invalid public shape or state")
        if state in {"not-assessed", "unavailable"}:
            if value["errors"] is not None or value["warnings"] is not None:
                raise IntegrationFailure("unassessed candidate preflight profile check exposed counters")
            return
        if type(value["errors"]) is not int or type(value["warnings"]) is not int:
            raise IntegrationFailure("candidate preflight profile check has invalid counters")
        if value["errors"] < 0 or value["warnings"] < 0:
            raise IntegrationFailure("candidate preflight profile check has negative counters")
        if state == "passed" and (value["errors"] != 0 or value["warnings"] != 0):
            raise IntegrationFailure("passed candidate preflight profile check has findings")
        if state == "failed" and value["errors"] == 0 and value["warnings"] == 0:
            raise IntegrationFailure("failed candidate preflight profile check has no findings")

    def _assert_preflight(
        self,
        report: dict[str, Any],
        job: dict[str, Any],
        *,
        checks: dict[str, str],
        eval_state: str,
        pack_state: str,
        reason_codes: list[str],
        candidate_verified: bool,
        quality_passed: bool,
        project_quality_assessed: bool,
        preflight_passed: bool,
        profile_check_state: str = "passed",
        eval_counts: tuple[int, int, int, int, int] | None = None,
        pack_counts: tuple[int, int, int] | None = None,
        resource_policy: str = "strict",
        resource_failed_queries: int = 0,
    ) -> None:
        if set(report) != _PREFLIGHT_FIELDS:
            raise IntegrationFailure("candidate preflight report does not have the exact public schema")
        if report["schema_version"] != "shin.preparation-preflight.v1" or report["scope"] != "candidate-observation":
            raise IntegrationFailure("candidate preflight report has the wrong schema identity or scope")
        if not isinstance(report["job"], dict) or set(report["job"]) != _PUBLIC_JOB_FIELDS or report["job"] != job:
            raise IntegrationFailure("candidate preflight did not return exactly the public inspected job")
        if report["checks"] != checks or report["reason_codes"] != reason_codes:
            raise IntegrationFailure("candidate preflight reported unexpected checks or reason codes")
        for name, expected in {
            "candidate_verified": candidate_verified,
            "quality_passed": quality_passed,
            "project_quality_assessed": project_quality_assessed,
            "preflight_passed": preflight_passed,
            "activation_supported": True,
            "activation_ready": False,
            "mutations_performed": False,
        }.items():
            if report[name] is not expected:
                raise IntegrationFailure("candidate preflight has an invalid boolean summary")
        self._assert_timestamp(report["started_at"])
        self._assert_timestamp(report["finished_at"])
        if report["started_at"] > report["finished_at"]:
            raise IntegrationFailure("candidate preflight finished before it started")
        if checks["artifact"] == "verified" and checks["ledger"] == "matched":
            if report["artifact_snapshot_id"] != job["artifact_snapshot_id"] or report["artifact_bytes"] != job["artifact_bytes"]:
                raise IntegrationFailure("verified candidate preflight did not bind artifact metadata to the ledger")
        elif report["artifact_snapshot_id"] is not None or report["artifact_bytes"] is not None:
            raise IntegrationFailure("unloaded candidate preflight exposed artifact metadata")
        self._assert_profile_check(report["profile_check"], state=profile_check_state)
        quality = report["quality"]
        if not isinstance(quality, dict) or set(quality) != {"eval", "pack_eval"}:
            raise IntegrationFailure("candidate preflight quality has an invalid public shape")
        if eval_counts is None:
            self._assert_suite(quality["eval"], state=eval_state)
        else:
            self._assert_suite(quality["eval"], state=eval_state, total=eval_counts[0], passed=eval_counts[1],
                               failed=eval_counts[2], query_total=eval_counts[3], query_passed=eval_counts[4],
                               resource_policy=resource_policy, resource_failed_queries=resource_failed_queries)
        if pack_counts is None:
            self._assert_suite(quality["pack_eval"], state=pack_state)
        else:
            self._assert_suite(quality["pack_eval"], state=pack_state, total=pack_counts[0],
                               passed=pack_counts[1], failed=pack_counts[2], pack_query_count_is_zero=True)
        self._assert_safe_preflight(report)

    def _quality_from_cli(self, profile_id: str, artifact: Path, command: str) -> tuple[tuple[int, ...], dict[str, Any] | None]:
        completed = subprocess.run(
            [
                str(self.core),
                "--profile",
                profile_id,
                "--profile-dir",
                str(self._preflight_profiles_dir()),
                "--root",
                str(self.fixture_root),
                "--out-dir",
                str(artifact),
                command,
                "--strict",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=45.0,
        )
        if completed.returncode not in {0, 1}:
            raise IntegrationFailure("preflight quality CLI did not return a normal pass or fail status")
        try:
            output = json.loads(completed.stdout)
            summary = output["summary"]
        except (json.JSONDecodeError, KeyError, TypeError) as error:
            raise IntegrationFailure("preflight quality CLI did not return its compact summary") from error
        values = (summary.get("total"), summary.get("passed"), summary.get("failed"))
        if any(type(value) is not int or value < 0 for value in values):
            raise IntegrationFailure("preflight quality CLI returned invalid case counters")
        if command == "eval":
            query_summary = output.get("query_summary")
            if not isinstance(query_summary, dict):
                raise IntegrationFailure("preflight eval CLI returned no query summary")
            queries = (query_summary.get("total"), query_summary.get("passed"))
            if any(type(value) is not int or value < 0 for value in queries):
                raise IntegrationFailure("preflight eval CLI returned invalid query counters")
            resource = output.get("resource_check")
            if not isinstance(resource, dict):
                raise IntegrationFailure("preflight eval CLI returned no resource check")
            return (*values, *queries), resource
        return (*values, 0, 0), None

    def _assert_cli_quality(self, report: dict[str, Any], profile_id: str, job: dict[str, Any]) -> None:
        artifact = self._preflight_artifact(job["id"])
        eval_counts, resource = self._quality_from_cli(profile_id, artifact, "eval")
        pack_counts, _ = self._quality_from_cli(profile_id, artifact, "pack-eval")
        quality = report["quality"]
        if resource != quality["eval"]["resource_check"]:
            raise IntegrationFailure("preflight resource observation differs from Core CLI")
        self._assert_suite(quality["eval"], state="passed" if eval_counts[2] == 0 else "failed",
                           total=eval_counts[0], passed=eval_counts[1], failed=eval_counts[2],
                           query_total=eval_counts[3], query_passed=eval_counts[4],
                           resource_policy=resource["policy"], resource_failed_queries=resource["failed_queries"])
        self._assert_suite(quality["pack_eval"], state="passed" if pack_counts[2] == 0 else "failed",
                           total=pack_counts[0], passed=pack_counts[1], failed=pack_counts[2],
                           pack_query_count_is_zero=True)

    def _assert_ordinary_inspection_after_quality_failure(self, job: dict[str, Any]) -> None:
        _, report = self._get_inspection(job["id"])
        self._assert_report(
            report,
            job,
            checks=dict(_CHECKS_PASSED),
            reason_codes=[],
            candidate_verified=True,
        )

    def check_preflight_passing_candidate(self) -> None:
        job = self._prepare_preflight_candidate("preflight-passing")
        baseline = self._preflight_state(job["id"])
        _, report = self._get_preflight(job["id"])
        self._assert_preflight(
            report,
            job,
            checks=dict(_CHECKS_PASSED),
            eval_state="passed",
            pack_state="passed",
            reason_codes=[],
            candidate_verified=True,
            quality_passed=True,
            project_quality_assessed=True,
            preflight_passed=True,
            eval_counts=(1, 1, 0, 1, 1),
            pack_counts=(1, 1, 0),
        )
        self._assert_cli_quality(report, "preflight-passing", job)
        self._assert_preflight_state(job["id"], baseline)
        self._preflight_passing_job = job

    def check_preflight_eval_failure(self) -> None:
        self.stop_server(expect_clean=True)
        job = self._prepare_preflight_candidate("preflight-eval-failing")
        baseline = self._preflight_state(job["id"])
        _, report = self._get_preflight(job["id"])
        self._assert_preflight(
            report,
            job,
            checks=dict(_CHECKS_PASSED),
            eval_state="failed",
            pack_state="passed",
            reason_codes=["candidate-eval-failed"],
            candidate_verified=True,
            quality_passed=False,
            project_quality_assessed=True,
            preflight_passed=False,
            eval_counts=(1, 0, 1, 1, 0),
            pack_counts=(1, 1, 0),
            resource_failed_queries=1,
        )
        self._assert_cli_quality(report, "preflight-eval-failing", job)
        self._assert_ordinary_inspection_after_quality_failure(job)
        self._assert_preflight_state(job["id"], baseline)

    def check_preflight_pack_failure(self) -> None:
        self.stop_server(expect_clean=True)
        job = self._prepare_preflight_candidate("preflight-pack-failing")
        baseline = self._preflight_state(job["id"])
        _, report = self._get_preflight(job["id"])
        self._assert_preflight(
            report,
            job,
            checks=dict(_CHECKS_PASSED),
            eval_state="passed",
            pack_state="failed",
            reason_codes=["candidate-pack-eval-failed"],
            candidate_verified=True,
            quality_passed=False,
            project_quality_assessed=True,
            preflight_passed=False,
            eval_counts=(1, 1, 0, 1, 1),
            pack_counts=(1, 0, 1),
        )
        self._assert_cli_quality(report, "preflight-pack-failing", job)
        self._assert_ordinary_inspection_after_quality_failure(job)
        self._assert_preflight_state(job["id"], baseline)

    def check_preflight_advisory(self, suffix: str) -> None:
        self.stop_server(expect_clean=True)
        profile_id = "preflight-advisory-" + suffix
        job = self._prepare_preflight_candidate(profile_id)
        baseline = self._preflight_state(job["id"])
        _, report = self._get_preflight(job["id"])
        coverage_failure = suffix == "coverage"
        pack_failure = suffix == "pack"
        accepted = not coverage_failure and not pack_failure
        self._assert_preflight(
            report, job, checks=dict(_CHECKS_PASSED),
            eval_state="failed" if coverage_failure else "passed",
            pack_state="failed" if pack_failure else "passed",
            reason_codes=["candidate-eval-failed"] if coverage_failure else
                         (["candidate-pack-eval-failed"] if pack_failure else []),
            candidate_verified=True, quality_passed=accepted,
            project_quality_assessed=True, preflight_passed=accepted,
            eval_counts=(1, 0, 1, 2, 0) if coverage_failure else (1, 1, 0, 2, 2),
            pack_counts=(1, 0, 1) if pack_failure else (1, 1, 0),
            resource_policy="advisory", resource_failed_queries=2)
        self._assert_cli_quality(report, profile_id, job)
        self._assert_preflight_state(job["id"], baseline)
        active = self.activation(PROJECT_ID, ROUTE_ID)
        status, response = self._post_activation(PROJECT_ID, ROUTE_ID, job["id"], active["revision"])
        if accepted:
            if status != 200:
                raise IntegrationFailure("advisory resource-only candidate could not activate")
            activated = self.activation(PROJECT_ID, ROUTE_ID)
            if activated["preparation_id"] != job["id"]:
                raise IntegrationFailure("advisory activation selected a different candidate")
            status, _ = self._post_activation(PROJECT_ID, ROUTE_ID, None, activated["revision"])
            if status != 200:
                raise IntegrationFailure("advisory fixture deactivation failed")
        else:
            if status != 409 or response.get("error", {}).get("code") != "activation-preflight-failed":
                raise IntegrationFailure("advisory policy waived a quality or Pack failure")
            if self.activation(PROJECT_ID, ROUTE_ID) != active:
                raise IntegrationFailure("rejected advisory activation changed the active route")
        self._release_candidate(job)

    def check_preflight_not_configured(self) -> None:
        self.stop_server(expect_clean=True)
        job = self._prepare_preflight_candidate("preflight-not-configured")
        baseline = self._preflight_state(job["id"])
        _, report = self._get_preflight(job["id"])
        self._assert_preflight(
            report,
            job,
            checks=dict(_CHECKS_PASSED),
            eval_state="not-configured",
            pack_state="not-configured",
            reason_codes=["candidate-eval-not-configured", "candidate-pack-eval-not-configured"],
            candidate_verified=True,
            quality_passed=False,
            project_quality_assessed=False,
            preflight_passed=False,
            eval_counts=(0, 0, 0, 0, 0),
            pack_counts=(0, 0, 0),
        )
        self._assert_preflight_state(job["id"], baseline)

    def check_preflight_profile_failure(self) -> None:
        self.stop_server(expect_clean=True)
        job = self._prepare_preflight_candidate("preflight-profile-failing")
        baseline = self._preflight_state(job["id"])
        _, report = self._get_preflight(job["id"])
        self._assert_preflight(
            report,
            job,
            checks=dict(_CHECKS_PASSED),
            eval_state="not-assessed",
            pack_state="not-assessed",
            reason_codes=["candidate-profile-failed"],
            candidate_verified=True,
            quality_passed=False,
            project_quality_assessed=False,
            preflight_passed=False,
            profile_check_state="failed",
        )
        self._assert_preflight_state(job["id"], baseline)

    def _resume_passing_candidate(self) -> dict[str, Any]:
        self.stop_server(expect_clean=True)
        job = self._prepare_preflight_candidate("preflight-passing")
        self._preflight_passing_job = job
        return job

    def _check_preflight_mid_quality_drift(self, job: dict[str, Any], baseline: dict[str, Any]) -> None:
        if self.ledger_probe is None:
            raise IntegrationFailure("mid-quality observation probe requires the native probe directory")
        probe = self.ledger_probe.with_name("shin-server-preflight-observation-tests")
        if not probe.is_file() or not os.access(probe, os.X_OK):
            raise IntegrationFailure("native mid-quality observation probe is unavailable")
        source = self.fixture_root / "docs" / "policy.md"
        if not source.is_file() or source.is_symlink() or source.resolve() != source:
            raise IntegrationFailure("mid-quality observation source is not a canonical regular fixture file")
        original = source.read_bytes()
        try:
            completed = self.run_process(
                [str(probe), str(self.runtime_config_path.resolve()), str(job["id"]), str(source)],
                "native mid-quality observation probe",
                timeout=45.0,
            )
            if source.read_bytes() != original:
                raise IntegrationFailure("native observation probe did not restore its fixture source")
        finally:
            # Also restore on a harness timeout, when the native RAII destructor cannot run.
            if source.read_bytes() != original:
                source.write_bytes(original)
        try:
            result = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise IntegrationFailure("native observation probe did not return JSON") from error
        if (not isinstance(result, dict) or result.get("ok") is not True or
                not isinstance(result.get("preflight"), dict) or
                result.get("activation_error") != "activation-preflight-failed" or
                type(result.get("barrier_mutations")) is not int or result["barrier_mutations"] != 2 or
                result.get("source_restored") is not True or result.get("restored_preflight_passed") is not True):
            raise IntegrationFailure("native observation probe did not complete both controlled drift passes")
        self._assert_preflight(
            result.get("preflight"), job,
            checks={**_CHECKS_PASSED, "source": "drifted"},
            eval_state="passed", pack_state="passed",
            reason_codes=["candidate-source-drifted"],
            candidate_verified=False, quality_passed=True, project_quality_assessed=True, preflight_passed=False,
            eval_counts=(1, 1, 0, 1, 1), pack_counts=(1, 1, 0),
        )
        self._assert_preflight_state(job["id"], baseline)

    def check_preflight_source_drift_and_corruption(self) -> None:
        job = self._resume_passing_candidate()
        baseline = self._preflight_state(job["id"])
        _, baseline_report = self._get_preflight(job["id"])
        self._assert_preflight(
            baseline_report,
            job,
            checks=dict(_CHECKS_PASSED),
            eval_state="passed",
            pack_state="passed",
            reason_codes=[],
            candidate_verified=True,
            quality_passed=True,
            project_quality_assessed=True,
            preflight_passed=True,
            eval_counts=(1, 1, 0, 1, 1),
            pack_counts=(1, 1, 0),
        )
        self._assert_preflight_state(job["id"], baseline)
        self._check_preflight_mid_quality_drift(job, baseline)
        source = self._first_regular_file(self.fixture_root)
        original = source.read_bytes()
        try:
            source.write_bytes(original + b"\npreflight-source-drift\n")
            _, report = self._get_preflight(job["id"])
            self._assert_preflight(
                report,
                job,
                checks={**_CHECKS_PASSED, "source": "drifted"},
                eval_state="not-assessed",
                pack_state="not-assessed",
                reason_codes=["candidate-source-drifted"],
                candidate_verified=False,
                quality_passed=False,
                project_quality_assessed=False,
                preflight_passed=False,
                profile_check_state="not-assessed",
            )
        finally:
            source.write_bytes(original)
        self._assert_preflight_state(job["id"], baseline)

        artifact = self._preflight_artifact(job["id"])
        manifest = artifact / "docs" / "kb" / "generated" / "manifest.json"
        original_manifest = manifest.read_bytes()
        try:
            manifest.write_bytes(original_manifest + b"\npreflight-corruption\n")
            _, report = self._get_preflight(job["id"])
            self._assert_preflight(
                report,
                job,
                checks={"route": "matched", "writer": "quiescent", "artifact": "invalid",
                        "ledger": "not-assessed", "doctor": "not-assessed", "source": "not-assessed",
                        "base": "not-assessed"},
                eval_state="not-assessed",
                pack_state="not-assessed",
                reason_codes=["candidate-artifact-invalid"],
                candidate_verified=False,
                quality_passed=False,
                project_quality_assessed=False,
                preflight_passed=False,
                profile_check_state="not-assessed",
            )
        finally:
            manifest.write_bytes(original_manifest)
        self._assert_preflight_state(job["id"], baseline)

    def check_preflight_access_and_query_guards(self) -> None:
        job = self._preflight_passing_job
        baseline = self._preflight_state(job["id"])
        self.expect_status(404, "GET", "/api/v1/preparations/999999999/preflight", headers=self.auth_headers("admin"))
        for query in ("?unexpected=1", "?unexpected=1&unexpected=2", "?%75nexpected=1"):
            self.expect_status(400, "GET", f"/api/v1/preparations/{job['id']}/preflight{query}",
                               headers=self.auth_headers("admin"))
        other_snapshot = self.activation("other", "other-retrieval")["snapshot_id"]
        status, response = self.post_preparation("other", "other-retrieval", other_snapshot,
                                                 "preflight-other-" + secrets.token_hex(6))
        if status != 202:
            raise IntegrationFailure("other-project preflight fixture preparation was not accepted")
        other = self.assert_public_job(response.get("result", {}).get("job"), project_id="other", route_id="other-retrieval")
        self.wait_for_terminal(other["id"], "prepared", project_id="other", route_id="other-retrieval")
        self.expect_status(404, "GET", f"/api/v1/preparations/{other['id']}/preflight", headers=self.auth_headers("reader"))
        self._assert_preflight_state(job["id"], baseline)

    def check_preflight_shared_admission(self) -> None:
        job = self._preflight_passing_job
        baseline = self._preflight_state(job["id"])
        results: list[tuple[int, dict[str, Any]]] = []
        failures: list[BaseException] = []

        def preflight() -> None:
            try:
                results.append(self.request(
                    "GET",
                    f"/api/v1/preparations/{job['id']}/preflight",
                    headers=self.auth_headers("admin"),
                ))
            except BaseException as error:
                failures.append(error)

        artifact = self._preflight_artifact(job["id"])
        publication = os.open(artifact / "docs" / "kb" / ".shin-context.lock", os.O_RDONLY | os.O_NOFOLLOW)
        lifetime = os.open(self._inspection_lock(job["id"]), os.O_RDONLY | os.O_NOFOLLOW)
        thread = threading.Thread(target=preflight)
        try:
            fcntl.flock(publication, fcntl.LOCK_EX)
            thread.start()
            deadline = time.monotonic() + 5.0
            admitted = False
            while time.monotonic() < deadline:
                try:
                    fcntl.flock(lifetime, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    admitted = True
                    break
                else:
                    fcntl.flock(lifetime, fcntl.LOCK_UN)
                time.sleep(0.02)
            if not admitted:
                raise IntegrationFailure("preflight did not reach its held candidate artifact boundary")
            status, envelope = self.request(
                "GET",
                f"/api/v1/preparations/{job['id']}/inspection",
                headers=self.auth_headers("admin"),
            )
            if status != 503 or envelope.get("error", {}).get("code") != "preparation-inspection-busy":
                raise IntegrationFailure("ordinary inspection did not respect the shared preflight admission slot")
        finally:
            fcntl.flock(publication, fcntl.LOCK_UN)
            os.close(publication)
            os.close(lifetime)
            if thread.ident is not None:
                thread.join(timeout=10.0)
        if thread.is_alive() or failures or len(results) != 1:
            raise IntegrationFailure("shared preflight admission request did not complete cleanly")
        status, envelope = results[0]
        if status != 200:
            raise IntegrationFailure("shared-admission preflight did not return HTTP 200")
        report = envelope.get("result")
        if not isinstance(report, dict):
            raise IntegrationFailure("shared-admission preflight returned no report")
        self._assert_preflight(
            report,
            job,
            checks=dict(_CHECKS_PASSED),
            eval_state="passed",
            pack_state="passed",
            reason_codes=[],
            candidate_verified=True,
            quality_passed=True,
            project_quality_assessed=True,
            preflight_passed=True,
            eval_counts=(1, 1, 0, 1, 1),
            pack_counts=(1, 1, 0),
        )
        self._assert_preflight_state(job["id"], baseline)

    def check_preflight_small_fixture_baseline(self) -> None:
        job = self._preflight_passing_job
        baseline = self._preflight_state(job["id"])
        durations: list[float] = []
        response_bytes: list[float] = []
        for _ in range(3):
            started = time.perf_counter_ns()
            envelope, report = self._get_preflight(job["id"])
            durations.append((time.perf_counter_ns() - started) / 1_000_000)
            response_bytes.append(float(len(json.dumps(envelope, sort_keys=True, separators=(",", ":"),
                                                  ensure_ascii=True).encode("utf-8"))))
            self._assert_preflight(
                report,
                job,
                checks=dict(_CHECKS_PASSED),
                eval_state="passed",
                pack_state="passed",
                reason_codes=[],
                candidate_verified=True,
                quality_passed=True,
                project_quality_assessed=True,
                preflight_passed=True,
                eval_counts=(1, 1, 0, 1, 1),
                pack_counts=(1, 1, 0),
            )
            self._assert_preflight_state(job["id"], baseline)
        self.preflight_measurement = {
            "status": "completed",
            "samples": len(durations),
            "total_http_p50_ms": self.percentile_ms(durations, 50),
            "total_http_p95_ms": self.percentile_ms(durations, 95),
            "response_bytes_p50": self.percentile_ms(response_bytes, 50),
            "response_bytes_p95": self.percentile_ms(response_bytes, 95),
            "scope": "Three sequential unchanged prepared-candidate preflights; total HTTP time and compact response bytes, not throughput or soak.",
        }

    def run_preflight_checks(self) -> None:
        """Run after the existing suite stopped its server and reset only preparation rows."""
        if self.server is not None:
            raise IntegrationFailure("preflight checks require the owned server to be stopped")
        self._write_preflight_profiles()
        self.record("candidate-preflight-passing-candidate", self.check_preflight_passing_candidate)
        self.record("candidate-preflight-eval-failure", self.check_preflight_eval_failure)
        self.record("candidate-preflight-pack-failure", self.check_preflight_pack_failure)
        self.record("candidate-preflight-not-configured", self.check_preflight_not_configured)
        self.record("candidate-preflight-profile-failure", self.check_preflight_profile_failure)
        self.record("candidate-preflight-source-drift-and-corruption", self.check_preflight_source_drift_and_corruption)
        self.record("candidate-preflight-access-and-query-guards", self.check_preflight_access_and_query_guards)
        self.record("candidate-preflight-shared-admission", self.check_preflight_shared_admission)
        self.record("candidate-preflight-small-fixture-baseline", self.check_preflight_small_fixture_baseline)
        for suffix in ("resource", "coverage", "pack"):
            self.record("candidate-preflight-advisory-" + suffix,
                        lambda suffix=suffix: self.check_preflight_advisory(suffix))
        self.stop_server(expect_clean=True)
