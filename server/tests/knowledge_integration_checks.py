"""P3E17 focused proactive-knowledge checks for PublicationIntegrationSuite.

The parent harness owns the isolated PostgreSQL stand, migrations and fixture
lifetime.  This sidecar only exercises the opt-in service API against its
already prepared neutral fixtures.
"""

from __future__ import annotations

import json
import secrets
import time
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
from typing import Any, Callable

from integration_test import IntegrationFailure
from publication_integration_test import PEER_ROUTE
from publication_test_support import no_private_values, require
from retrieval_integration_test import PROJECT_ID, ROUTE_ID
from native_json_codec import NativeJsonCodec


OTHER_PROJECT = "other"
OTHER_ROUTE = "other-retrieval"
_STATUS_FIELDS = {
    "schema_version", "enabled", "state", "active_route", "targets",
    "checks_completed", "checks_failed", "checks_deferred", "refresh_requests",
    "coalesced_requests", "read_only", "scope",
}
_HEALTH_FIELDS = {
    "schema_version", "route_id", "state", "assessment", "age_ms", "max_age_ms",
    "observed_at_unix_ms", "completed_at_unix_ms", "elapsed_ms", "checking",
    "read_only", "exact_current_at_read", "project_id", "current_generation",
    "selection_aligned",
}
_GENERATION_FIELDS = {
    "project_id", "route_id", "workspace_id", "source_host_id", "profile_id",
    "route_fingerprint", "preparation_id", "snapshot_id", "revision",
}
_ASSESSMENT_FIELDS = {
    "healthy", "generation", "index_current", "expected_snapshot_id", "vacuum",
    "doctor", "profile", "quality", "reason_codes", "mutations_performed",
    "maintenance_action", "source_mutated", "publication_mutated",
}
_BOOTSTRAP_FIELDS = {
    "schema_version", "task_id", "ready", "context_ready", "health_ready", "members",
    "evidence_bytes", "max_evidence_bytes", "read_only", "snapshot_consistency",
    "checkpoint_restored",
}
_MEMBER_FIELDS = {
    "project_id", "selector", "required", "state", "reason_code", "context",
    "health", "coverage_complete", "health_ready",
    "health_reason_code",
}
_SELECTOR_FIELDS = {"kind", "id"}


class KnowledgeIntegrationChecks:
    """Focused API checks to be registered after retrieval-journal retention."""

    def __init__(self, suite: Any) -> None:
        self.suite = suite
        self.original_configs: dict[Path, bytes] = {}
        self.original_source: str | None = None
        self.knowledge_measurement: dict[str, Any] = {"status": "not-run"}
        self.codec = NativeJsonCodec(suite.binary.parent / "shin-server-json-codec-probe")
        self.server_started_ns = 0

    def run(self) -> None:
        try:
            self.suite.record("knowledge-disabled-and-guards", self.check_disabled_and_guards)
            self.suite.record("knowledge-multi-project-bootstrap-and-cache", self.check_multi_project_bootstrap_and_cache)
            self.suite.record("knowledge-publication-bootstrap-v2", self.check_publication_bootstrap_v2)
            self.suite.record("knowledge-drift-read-only-recovery", self.check_drift_read_only_recovery)
            self.suite.record("knowledge-corrupt-artifact-recovery", self.check_corrupt_artifact_recovery)
            self.suite.record("knowledge-drift-preparation-deduplication", self.check_drift_preparation_deduplication)
        finally:
            self._restore_source()
            self._restore_configs_and_restart()
            self.codec.close()

    def _forbidden(self) -> list[str]:
        values = [
            str(self.suite.fixture_root), str(self.suite.peer_root), str(self.suite.other_root),
            str(self.suite.out_dir), str(self.suite.other_out_dir), str(self.suite.preparation_root),
            self.suite.admin_token, self.suite.reader_token,
        ]
        return [value for value in values if value and value != "None"]

    def _safe_result(self, envelope: dict[str, Any]) -> Any:
        result = envelope.get("result")
        no_private_values(result, self._forbidden())
        return result

    def _status(self, *, role: str = "admin") -> dict[str, Any]:
        response = self.suite.expect_status(200, "GET", "/api/v1/knowledge-maintenance",
                                            headers=self.suite.auth_headers(role))
        result = self._safe_result(response)
        require(isinstance(result, dict) and set(result) == _STATUS_FIELDS,
                "knowledge maintenance status public fields differ")
        require(result["schema_version"] == "shin.knowledge-maintenance.v1" and
                type(result["enabled"]) is bool and result["read_only"] is True and
                result["scope"] == "process-local", "knowledge maintenance status markers differ")
        for field in ("targets", "checks_completed", "checks_failed", "checks_deferred",
                      "refresh_requests", "coalesced_requests"):
            require(type(result[field]) is int and result[field] >= 0,
                    "knowledge maintenance counter is not a nonnegative integer")
        require(result["state"] in {"disabled", "idle", "checking"}, "knowledge maintenance state differs")
        return result

    def _health(self, project: str, route: str, *, role: str = "admin") -> dict[str, Any]:
        response = self.suite.expect_status(
            200, "GET", f"/api/v1/projects/{project}/routes/{route}/knowledge-health",
            headers=self.suite.auth_headers(role),
        )
        result = self._safe_result(response)
        require(isinstance(result, dict) and set(result) == _HEALTH_FIELDS,
                "knowledge health public fields differ")
        require(result["schema_version"] == "shin.knowledge-health.v1" and
                result["project_id"] == project and result["route_id"] == route and
                result["read_only"] is True and result["exact_current_at_read"] is False,
                "knowledge health markers differ")
        require(type(result["selection_aligned"]) is bool and
                (result["age_ms"] is None or type(result["age_ms"]) is int) and
                (result["elapsed_ms"] is None or isinstance(result["elapsed_ms"], (int, float))),
                "knowledge health cache metadata differs")
        generation = result["current_generation"]
        require(isinstance(generation, dict) and set(generation) == _GENERATION_FIELDS and
                generation["project_id"] == project and generation["route_id"] == route,
                "knowledge health current generation differs")
        if result["assessment"] is not None:
            assessment = result["assessment"]
            if assessment.get("assessment_complete") is False:
                require(set(assessment) == {"healthy", "assessment_complete", "generation", "reason_codes", "mutations_performed"} and
                        assessment["healthy"] is False and assessment["generation"] is None and
                        isinstance(assessment["reason_codes"], list) and assessment["reason_codes"],
                        "unavailable health assessment fabricated completed checks")
                return result
            require(isinstance(assessment, dict) and set(assessment) == _ASSESSMENT_FIELDS,
                    "knowledge assessment public fields differ")
            require(type(assessment["healthy"]) is bool and type(assessment["index_current"]) is bool and
                    assessment["source_mutated"] is False and assessment["publication_mutated"] is False,
                    "knowledge assessment authority markers differ")
            require(isinstance(assessment["generation"], dict) and set(assessment["generation"]) == _GENERATION_FIELDS,
                    "knowledge assessment generation differs")
        return result

    @staticmethod
    def _member(project: str, kind: str, identifier: str, query: str, *, required: bool) -> dict[str, Any]:
        return {
            "project_id": project,
            "selector": {"kind": kind, "id": identifier},
            "query": query,
            "required": required,
        }

    @staticmethod
    def _bootstrap_payload(task_id: str, members: list[dict[str, Any]], budget: int) -> bytes:
        return json.dumps({
            "schema_version": "shin.session-bootstrap-request.v2",
            "task_id": task_id,
            "members": members,
            "max_evidence_bytes": budget,
        }, separators=(",", ":")).encode("utf-8")

    def _bootstrap(self, task_id: str, members: list[dict[str, Any]], budget: int = 65536) -> tuple[dict[str, Any], float]:
        started = time.perf_counter_ns()
        status, response = self.suite.request(
            "POST", "/api/v1/session-bootstrap",
            headers={**self.suite.auth_headers("admin"), "Content-Type": "application/json"},
            body=self._bootstrap_payload(task_id, members, budget),
        )
        elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
        require(status == 200, "session bootstrap did not complete with HTTP 200")
        result = self._safe_result(response)
        require(isinstance(result, dict) and set(result) == _BOOTSTRAP_FIELDS,
                "session bootstrap public fields differ")
        require(result["schema_version"] == "shin.session-bootstrap.v2" and result["task_id"] == task_id and
                result["read_only"] is True and result["snapshot_consistency"] == "independently-observed-members" and
                result["checkpoint_restored"] is False and result["max_evidence_bytes"] == budget,
                "session bootstrap authority markers differ")
        require(type(result["ready"]) is bool and type(result["context_ready"]) is bool and
                type(result["health_ready"]) is bool and type(result["evidence_bytes"]) is int and
                0 <= result["evidence_bytes"] <= budget and isinstance(result["members"], list),
                "session bootstrap readiness fields differ")
        for member in result["members"]:
            require(isinstance(member, dict) and set(member) == _MEMBER_FIELDS,
                    "session bootstrap member public fields differ")
            selector = member["selector"]
            require(isinstance(selector, dict) and set(selector) == _SELECTOR_FIELDS and
                    selector["kind"] in {"route", "publication"} and
                    isinstance(selector["id"], str) and selector["id"],
                    "session bootstrap member selector differs")
            require(type(member["required"]) is bool and member["state"] in {
                "unavailable", "budget-exceeded", "no-evidence", "ready", "partial-coverage"
            } and type(member["coverage_complete"]) is bool and type(member["health_ready"]) is bool,
                    "session bootstrap member state differs")
        return result, elapsed_ms

    def _remember_configs(self) -> None:
        for path in (self.suite.runtime_config_path, self.suite.migrator_config_path):
            if path not in self.original_configs:
                self.original_configs[path] = path.read_bytes()

    def _policy(self, route_ids: list[str], *, prepare_on_drift: bool,
                interval_seconds: int = 1, max_age_seconds: int = 3) -> None:
        if self.suite.server is not None:
            self.suite.stop_server(expect_clean=True)
        self._remember_configs()
        for path in (self.suite.runtime_config_path, self.suite.migrator_config_path):
            config = json.loads(path.read_text(encoding="utf-8"))
            require(config.get("journal_writer", {}).get("enabled") is False,
                    "knowledge bootstrap checks require the journal writer to remain disabled")
            config["knowledge"] = {
                "enabled": True,
                "principal_id": "dev-admin",
                "route_ids": route_ids,
                "interval_seconds": interval_seconds,
                "max_age_seconds": max_age_seconds,
                "prepare_on_drift": prepare_on_drift,
            }
            self.suite.replace_private_json(path, config)
        self.server_started_ns = time.perf_counter_ns()
        self.suite.start_server()

    def _install_route_aliases(self, route_ids: list[str]) -> None:
        if self.suite.server is not None:
            self.suite.stop_server(expect_clean=True)
        self._remember_configs()
        for path in (self.suite.runtime_config_path, self.suite.migrator_config_path):
            config = json.loads(path.read_text(encoding="utf-8"))
            routes = config["retrieval"]["routes"]
            source = next(route for route in routes if route["id"] == ROUTE_ID)
            for route_id in route_ids:
                require(all(route["id"] != route_id for route in routes),
                        "bootstrap route alias already exists")
                routes.append({**source, "id": route_id})
            self.suite.replace_private_json(path, config)

    def _seed_alias_route(self, route_id: str) -> dict[str, Any]:
        active = self.suite.activation(PROJECT_ID, route_id)
        if active["preparation_id"] is None:
            job = self.suite.prepare_candidate(route_id)
            self.suite.activate_seed(PROJECT_ID, route_id, job["id"], active["revision"])
            return job
        job = self.suite.jobs.get(active["preparation_id"])
        require(job is not None, "active alias route is not backed by the inherited job registry")
        return job

    def _publish(self, publication: str, job: dict[str, Any]) -> dict[str, Any]:
        current = self.suite.get_head(publication)
        proposal = self.suite.submit(job, current["revision"], "bootstrap-publication-" + secrets.token_hex(8), publication)
        self.suite.resolution(proposal["proposal"]["id"], "accept", current["revision"], publication)
        return self.suite.get_head(publication)

    def _restore_configs_and_restart(self) -> None:
        if not self.original_configs:
            return
        if self.suite.server is not None:
            self.suite.stop_server(expect_clean=False)
        for path, original in self.original_configs.items():
            self.suite.replace_private_json(path, json.loads(original))
        self.suite.start_server()
        self.original_configs.clear()

    def _source_path(self) -> Path:
        return self.suite.fixture_root / "docs" / "publication-a.md"

    def _drift_source(self) -> None:
        path = self._source_path()
        if self.original_source is None:
            self.original_source = path.read_text(encoding="utf-8")
        self.suite.write_fixture_file(path, self.original_source + "\nKnowledge drift fixture marker.\n")

    def _restore_source(self) -> None:
        if self.original_source is not None:
            self.suite.write_fixture_file(self._source_path(), self.original_source)
            self.original_source = None

    def _wait(self, description: str, predicate: Callable[[], Any], timeout: float = 12.0) -> Any:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.05)
        raise IntegrationFailure("timed out waiting for " + description)

    def _wait_healthy(self, project: str, route: str) -> dict[str, Any]:
        def ready() -> dict[str, Any] | None:
            health = self._health(project, route)
            if health["state"] != "healthy" or health["selection_aligned"] is not True:
                return None
            assessment = health["assessment"]
            if assessment is None or assessment["healthy"] is not True or assessment["index_current"] is not True:
                return None
            return health
        return self._wait("healthy cached knowledge assessment", ready)

    def _seed_other_route(self) -> None:
        active = self.suite.activation(OTHER_PROJECT, OTHER_ROUTE)
        if active["preparation_id"] is not None:
            return
        status, response = self.suite.post_preparation(
            OTHER_PROJECT, OTHER_ROUTE, None, "knowledge-other-seed-" + secrets.token_hex(8),
        )
        require(status == 202, "second-project seed preparation was not admitted")
        job = self.suite.assert_public_job(response["result"].get("job"), project_id=OTHER_PROJECT, route_id=OTHER_ROUTE)
        prepared = self.suite.wait_for_terminal(job["id"], "prepared", project_id=OTHER_PROJECT, route_id=OTHER_ROUTE)
        self.suite.activate_seed(OTHER_PROJECT, OTHER_ROUTE, prepared["id"], active["revision"])
        activated = self.suite.activation(OTHER_PROJECT, OTHER_ROUTE)
        require(activated["preparation_id"] == prepared["id"] and activated["snapshot_id"] == prepared["artifact_snapshot_id"],
                "second-project route was not explicitly activated")

    def _durable_and_tree_state(self) -> tuple[dict[str, Any], tuple[str, ...]]:
        require(self.suite.preparation_root is not None, "knowledge checks require the inherited preparation fixture")
        return self.suite.durable_state(), (
            self.suite.tree_digest(self.suite.fixture_root),
            self.suite.tree_digest(self.suite.peer_root),
            self.suite.tree_digest(self.suite.other_root),
            self.suite.tree_digest(self.suite.out_dir),
            self.suite.tree_digest(self.suite.preparation_root),
        )

    def check_disabled_and_guards(self) -> None:
        before = self.suite.durable_state()
        routes = self._safe_result(self.suite.expect_status(200, "GET", f"/api/v1/projects/{PROJECT_ID}/knowledge-routes",
                                                           headers=self.suite.auth_headers("reader")))
        require(routes["schema_version"] == "shin.knowledge-routes.v1" and routes["read_only"] is True and
                routes["health_assessed"] is False and routes["scope"] == "configured-server-local-routes" and
                any(item["route_id"] == ROUTE_ID and item["monitored"] is False for item in routes["routes"]),
                "read-only route discovery did not identify the configured bootstrap route")
        self.suite.expect_status(404, "GET", "/api/v1/projects/other/knowledge-routes", headers=self.suite.auth_headers("reader"))
        self.suite.unchanged(before, "knowledge route inventory")
        status = self._status()
        require(status["enabled"] is False and status["state"] == "disabled" and status["targets"] == 0,
                "default knowledge policy is not disabled")
        health = self._health(PROJECT_ID, ROUTE_ID)
        require(health["state"] == "disabled" and health["assessment"] is None and health["selection_aligned"] is False,
                "disabled knowledge health exposed an assessment")
        for method, path, payload, role, expected in (
            ("GET", "/api/v1/knowledge-maintenance", None, "reader", 403),
            ("POST", "/api/v1/knowledge-maintenance/refresh", {}, "reader", 403),
            ("POST", "/api/v1/knowledge-maintenance/refresh", {}, "admin", 409),
            ("GET", f"/api/v1/projects/{PROJECT_ID}/routes/{ROUTE_ID}/knowledge-health", None, "reader", 200),
        ):
            headers = self.suite.auth_headers(role)
            body = None
            if payload is not None:
                headers = {**headers, "Content-Type": "application/json"}
                body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
            actual, response = self.suite.request(method, path, headers=headers, body=body)
            require(actual == expected, "knowledge guard returned an unexpected HTTP status")
            self._safe_result(response) if actual < 400 else no_private_values(response, self._forbidden())
        status_code, response = self.suite.request(
            "POST", "/api/v1/knowledge-maintenance/refresh",
            headers={**self.suite.auth_headers("admin"), "Content-Type": "application/json"},
            body=b'{"unexpected":true}',
        )
        require(status_code == 400 and response.get("error", {}).get("code") == "invalid-request",
                "refresh did not reject a nonempty request object")

    def check_multi_project_bootstrap_and_cache(self) -> None:
        # First enable only the inherited active primary route.  The other project is
        # authorized but intentionally unactivated for required/optional isolation.
        self._policy([ROUTE_ID], prepare_on_drift=False)
        primary_health = self._wait_healthy(PROJECT_ID, ROUTE_ID)
        cache = self.suite.expect_status(200, "GET", "/api/v1/cache", headers=self.suite.auth_headers("admin")).get("result")
        require(isinstance(cache, dict) and type(cache.get("resident_generations")) is int and
                type(cache.get("loads")) is int and cache["resident_generations"] >= 1 and cache["loads"] >= 1,
                "background assessment did not warm the resident generation before first user retrieval")
        before_get = self._status()
        for _ in range(3):
            health = self._health(PROJECT_ID, ROUTE_ID)
            require(health["assessment"] == primary_health["assessment"],
                    "GET knowledge health performed a fresh assessment")
            self._status()
        after_get = self._status()
        require(after_get["checks_completed"] == before_get["checks_completed"] and
                after_get["refresh_requests"] == before_get["refresh_requests"],
                "GET knowledge health or status triggered maintenance")
        refresh_before = self._status()
        for _ in range(2):
            actual, response = self.suite.request(
                "POST", "/api/v1/knowledge-maintenance/refresh",
                headers={**self.suite.auth_headers("admin"), "Content-Type": "application/json"}, body=b"{}",
            )
            require(actual == 202 and self._safe_result(response) == {
                "requested": True, "targets": 1, "completed_synchronously": False,
            }, "refresh did not return its exact asynchronous request receipt")
        def refresh_accounted() -> dict[str, Any] | None:
            status = self._status()
            return status if status["refresh_requests"] >= refresh_before["refresh_requests"] + 2 else None
        refresh_after = self._wait("coalesced maintenance refresh accounting", refresh_accounted)
        require(refresh_after["checks_completed"] >= refresh_before["checks_completed"] and
                refresh_after["coalesced_requests"] >= refresh_before["coalesced_requests"],
                "refresh did not remain a bounded coalescible request")

        primary = self._member(PROJECT_ID, "route", ROUTE_ID,
                               "AlphaWorkspaceEvidence immutable publication review", required=True)
        missing_other = self._member(OTHER_PROJECT, "route", OTHER_ROUTE,
                                     "OtherProjectOnly isolated evidence", required=False)
        optional, _ = self._bootstrap("knowledge-optional-missing", [missing_other, primary])
        require(optional["ready"] is True and optional["context_ready"] is True and optional["health_ready"] is True,
                "missing optional member blocked a ready required member")
        require([(member["project_id"], member["selector"], member["required"]) for member in optional["members"]] == [
            (PROJECT_ID, {"kind": "route", "id": ROUTE_ID}, True),
            (OTHER_PROJECT, {"kind": "route", "id": OTHER_ROUTE}, False),
        ], "bootstrap did not process required members before optional members")
        require(optional["members"][0]["state"] == "ready" and optional["members"][1]["state"] == "unavailable",
                "optional missing generation did not remain a visible per-member result")

        required_missing, _ = self._bootstrap("knowledge-required-missing", [
            {**missing_other, "required": True}, {**primary, "required": False}
        ])
        require(required_missing["ready"] is False and required_missing["context_ready"] is False and
                required_missing["members"][0]["state"] == "unavailable",
                "missing required generation did not prevent bootstrap readiness")

        before_invalid = self.suite.durable_state()
        for members, expected_status in (
            ([primary, {**primary, "required": False}], 400),
            ([{**primary, "project_id": "not-registered"}], 404),
        ):
            actual, response = self.suite.request(
                "POST", "/api/v1/session-bootstrap",
                headers={**self.suite.auth_headers("admin"), "Content-Type": "application/json"},
                body=self._bootstrap_payload("knowledge-reject", members, 65536),
            )
            require(actual == expected_status, "duplicate or untrusted bootstrap member was admitted")
            no_private_values(response, self._forbidden())
        self.suite.unchanged(before_invalid, "rejected bootstrap envelope")

        self._seed_other_route()
        self._policy([ROUTE_ID, OTHER_ROUTE], prepare_on_drift=False)
        self._wait_healthy(PROJECT_ID, ROUTE_ID)
        self._wait_healthy(OTHER_PROJECT, OTHER_ROUTE)
        start_ready_ms = (time.perf_counter_ns() - self.server_started_ns) / 1_000_000
        two_projects = [primary, {**missing_other, "required": True}]
        cold, cold_ms = self._bootstrap("knowledge-cold-two-project", two_projects)
        require(cold["ready"] is True and cold["context_ready"] is True and cold["health_ready"] is True and
                all(member["state"] == "ready" for member in cold["members"]),
                "two-project bootstrap did not return ready compact evidence")
        for member in cold["members"]:
            require(member["context"] is not None and member["health"]["exact_current_at_read"] is False and
                    member["health_ready"] is True, "bootstrap did not retain cached health with context")
        retained = sum(len(self.codec.encode(member["context"]))
                       for member in cold["members"] if member["context"] is not None)
        require(cold["evidence_bytes"] == retained and retained <= cold["max_evidence_bytes"],
                "bootstrap did not account for whole retained context envelopes within the shared budget")
        bounded, _ = self._bootstrap("knowledge-bounded", two_projects, 1024)
        require(bounded["ready"] is False and bounded["context_ready"] is False and
                any(member["state"] == "budget-exceeded" and member["context"] is None for member in bounded["members"]),
                "small bootstrap budget silently returned partial canonical context")

        warm_samples: list[float] = []
        for index in range(5):
            warm, elapsed_ms = self._bootstrap("knowledge-warm-two-project-" + str(index), two_projects)
            require(warm["ready"] is True and warm["evidence_bytes"] <= warm["max_evidence_bytes"],
                    "warmed bootstrap lost readiness or exceeded its whole-envelope budget")
            warm_samples.append(elapsed_ms)
        self.knowledge_measurement = {
            "status": "completed", "server_start_to_routes_healthy_ms": round(start_ready_ms, 3),
            "first_agent_bootstrap_ms": round(cold_ms, 3),
            "warm_bootstrap": {"samples": len(warm_samples), "p50_ms": self.suite.percentile_ms(warm_samples, 50),
                               "p95_ms": self.suite.percentile_ms(warm_samples, 95)},
            "scope": "Sequential loopback HTTP bootstrap timings after background assessments warmed two neutral local "
                     "project routes. They include request decoding and exact-current Brief work; not production capacity, "
                     "concurrent throughput, remote I/O, or an SLO.",
        }
        started = time.perf_counter_ns()
        with ThreadPoolExecutor(max_workers=4) as executor:
            overlap = list(executor.map(lambda index: self._bootstrap("knowledge-overlap-" + str(index), two_projects), range(32)))
        overlap_ms = (time.perf_counter_ns() - started) / 1_000_000
        require(all(result["context_ready"] is True and result["evidence_bytes"] <= result["max_evidence_bytes"]
                    for result, _ in overlap), "concurrent bootstrap lost exact-current context under maintenance")
        samples = [elapsed for _, elapsed in overlap]
        self.knowledge_measurement["concurrent_bootstrap"] = {
            "requests": len(overlap), "concurrency": 4, "elapsed_ms": round(overlap_ms, 3),
            "p50_ms": self.suite.percentile_ms(samples, 50), "p95_ms": self.suite.percentile_ms(samples, 95),
            "health_ready_responses": sum(result["health_ready"] is True for result, _ in overlap),
            "boundary": "Bounded neutral-fixture overlap, not a production capacity or fairness SLO. Cached health may expire; exact-current context must survive independently.",
        }

    def check_publication_bootstrap_v2(self) -> None:
        """Exercise selector identity and source-generation health through real publication state."""
        collision = "bootstrap-collision"
        empty = "bootstrap-empty"
        withdrawn = "bootstrap-withdrawn"
        self._install_route_aliases([collision, empty, withdrawn])
        self._policy([ROUTE_ID, collision], prepare_on_drift=False)

        source_before = self.suite.activation(PROJECT_ID, ROUTE_ID)
        advance_job = self.suite.prepare_candidate(ROUTE_ID)
        self.suite.activate_seed(PROJECT_ID, ROUTE_ID, advance_job["id"], source_before["revision"])
        source_base = self.suite.activation(PROJECT_ID, ROUTE_ID)
        source_job = self.suite.prepare_candidate(ROUTE_ID)
        current = self._publish(collision, source_job)
        withdrawn_proposal = self.suite.submit(source_job, 0, "bootstrap-withdrawn-seed", withdrawn)
        self.suite.resolution(withdrawn_proposal["proposal"]["id"], "accept", 0, withdrawn)
        self.suite.activate_seed(PROJECT_ID, ROUTE_ID, source_job["id"], source_base["revision"])
        for route in (PEER_ROUTE, collision, empty, withdrawn):
            self._seed_alias_route(route)
        source_health = self._wait_healthy(PROJECT_ID, ROUTE_ID)
        self._wait_healthy(PROJECT_ID, collision)

        assessment = source_health["assessment"]
        require(assessment is not None and assessment["generation"]["preparation_id"] == source_job["id"],
                "publication bootstrap did not start from a cached source assessment")
        require(current["source_route_id"] == ROUTE_ID and current["source_route_id"] != collision and
                current["revision"] != assessment["generation"]["revision"],
                "publication fixture did not separate source and publication selection revisions")

        route_member = self._member(PROJECT_ID, "route", collision, "AlphaWorkspaceEvidence", required=True)
        publication_member = self._member(PROJECT_ID, "publication", collision, "AlphaWorkspaceEvidence", required=True)
        mixed, _ = self._bootstrap("knowledge-mixed-route-publication", [route_member, publication_member])
        require(mixed["ready"] is True and mixed["context_ready"] is True and mixed["health_ready"] is True,
                "mixed route/publication bootstrap did not retain ready members")
        route_result, publication_result = mixed["members"]
        require(route_result["selector"] == route_member["selector"] and
                publication_result["selector"] == publication_member["selector"] and
                route_result["context"]["route_id"] == collision and "publication_id" not in route_result["context"] and
                publication_result["context"]["publication_id"] == collision and
                publication_result["context"]["route_id"] == ROUTE_ID,
                "route and publication selectors with the same name crossed identities")
        publication_health = publication_result["health"]
        require(isinstance(publication_health, dict) and
                set(publication_health) == _HEALTH_FIELDS | {"assessment_scope", "selection_revision_scope"} and
                publication_health["assessment_scope"] == "source-generation" and
                publication_health["selection_revision_scope"] == "publication" and
                set(publication_health["current_generation"]) == _GENERATION_FIELDS | {"publication_id"} and
                publication_health["current_generation"]["publication_id"] == collision and
                publication_health["current_generation"]["revision"] == current["revision"] and
                set(publication_health["assessment"]["generation"]) == _GENERATION_FIELDS and
                publication_health["assessment"]["generation"]["revision"] == assessment["generation"]["revision"] and
                publication_result["health_ready"] is True,
                "publication health did not reuse matching source health across independent revisions")

        from retrieval_observability_checks import RetrievalObservabilityChecks
        telemetry = RetrievalObservabilityChecks(self.suite)
        self.suite.expect_status(200, "GET", f"/api/v1/projects/{OTHER_PROJECT}",
                                 headers=self.suite.auth_headers("admin"))
        before_unauthorized = telemetry.metrics()
        denied, response = self.suite.request(
            "POST", "/api/v1/session-bootstrap",
            headers={**self.suite.auth_headers("reader"), "Content-Type": "application/json"},
            body=self._bootstrap_payload("knowledge-unauthorized-optional", [
                route_member,
                self._member(OTHER_PROJECT, "route", OTHER_ROUTE, "OtherProjectOnly", required=False),
            ], 65536),
        )
        require(denied == 404 and response.get("error", {}).get("code") == "project-not-found",
                "unauthorized optional project did not preserve existence-concealing refusal")
        no_private_values(response, self._forbidden())
        require(telemetry.metrics() == before_unauthorized,
                "unauthorized optional project started retrieval telemetry before authorization completed")

        empty_result, _ = self._bootstrap("knowledge-empty-publication", [
            self._member(PROJECT_ID, "route", empty, "AlphaWorkspaceEvidence", required=True),
            self._member(PROJECT_ID, "publication", empty, "AlphaWorkspaceEvidence", required=False),
        ])
        require(empty_result["context_ready"] is True and empty_result["members"][0]["state"] == "ready" and
                empty_result["members"][1]["context"] is None and
                empty_result["members"][1]["reason_code"] == "active-generation-missing" and
                empty_result["members"][1]["health_reason_code"] == "active-generation-missing",
                "empty publication fell back to the identically named route")

        from publication_withdrawal_checks import withdraw
        withdraw(self.suite, withdrawn, 1, withdrawn_proposal["proposal"]["id"], source_job["id"])
        withdrawn_result, _ = self._bootstrap("knowledge-withdrawn-publication", [
            self._member(PROJECT_ID, "route", withdrawn, "AlphaWorkspaceEvidence", required=True),
            self._member(PROJECT_ID, "publication", withdrawn, "AlphaWorkspaceEvidence", required=False),
        ])
        require(withdrawn_result["context_ready"] is True and withdrawn_result["members"][0]["state"] == "ready" and
                withdrawn_result["members"][1]["context"] is None and
                withdrawn_result["members"][1]["reason_code"] == "publication-withdrawn" and
                withdrawn_result["members"][1]["health_reason_code"] == "publication-withdrawn",
                "withdrawn publication fell back to the identically named route")

        self._policy([PEER_ROUTE, collision], prepare_on_drift=False)
        self._wait_healthy(PROJECT_ID, PEER_ROUTE)
        peer_job = self.suite.prepare_candidate(PEER_ROUTE)
        changed = self._publish(collision, peer_job)
        changed_result, _ = self._bootstrap("knowledge-publication-selection-changed", [publication_member])
        changed_member = changed_result["members"][0]
        require(changed["source_route_id"] == PEER_ROUTE and changed_member["context"]["preparation_id"] == peer_job["id"] and
                changed_member["health"]["state"] == "generation-changed" and
                changed_member["health"]["selection_aligned"] is False and changed_member["health_ready"] is False,
                "publication selection change promoted a nonmatching cached source assessment")
        restore_job = self.suite.prepare_candidate(ROUTE_ID)
        self._publish(collision, restore_job)

        self._policy([collision], prepare_on_drift=False)
        unmonitored, _ = self._bootstrap("knowledge-publication-unmonitored", [publication_member])
        require(unmonitored["context_ready"] is True and unmonitored["members"][0]["context"] is not None and
                unmonitored["members"][0]["health_ready"] is False and
                unmonitored["members"][0]["health"]["assessment"] is None,
                "unmonitored publication source was promoted to healthy")

        before_reads = self._durable_and_tree_state()
        for index in range(3):
            observed, _ = self._bootstrap("knowledge-publication-read-only-" + str(index), [publication_member])
            require(observed["members"][0]["context"] is not None and observed["read_only"] is True,
                    "publication bootstrap lost exact-current context during read-only checks")
        require(self._durable_and_tree_state() == before_reads,
                "publication bootstrap reads or scheduler assessment mutated durable state or source artifacts")
        # Restore the inherited cadence expected by the following drift checks.
        self._policy([ROUTE_ID, OTHER_ROUTE], prepare_on_drift=False)
        self._wait_healthy(PROJECT_ID, ROUTE_ID)
        self.knowledge_measurement["publication_bootstrap_v2"] = {
            "status": "completed",
            "comparison": "No route/publication latency comparison was recorded; this suite only asserts selector and health semantics.",
            "boundary": "Real loopback HTTP and PostgreSQL fixture state under the parent StandGuard; no production throughput claim. Expiry timing remains covered by native scheduler/bootstrap tests.",
        }

    def check_drift_read_only_recovery(self) -> None:
        before_durable, before_trees = self._durable_and_tree_state()
        self._drift_source()
        stale = self._wait("cached stale knowledge assessment", lambda: self._stale_health())
        assessment = stale["assessment"]
        require(stale["state"] == "unhealthy" and assessment["index_current"] is False and
                assessment["healthy"] is False and "knowledge-index-not-current" in assessment["reason_codes"] and
                assessment["mutations_performed"] is False and assessment["maintenance_action"] is None,
                "read-only drift assessment markers differ")
        status, response = self.suite.post_retrieval(PROJECT_ID, "brief", {"query": "AlphaWorkspaceEvidence"})
        require(status == 409 and response.get("error", {}).get("code") == "stale-index",
                "stale source did not fail exact-current retrieval before bootstrap")
        failed, _ = self._bootstrap("knowledge-stale-bootstrap", [
            self._member(PROJECT_ID, "route", ROUTE_ID, "AlphaWorkspaceEvidence", required=True),
        ])
        require(failed["ready"] is False and failed["context_ready"] is False and
                failed["members"][0]["state"] == "unavailable" and failed["members"][0]["reason_code"] == "stale-index" and
                failed["members"][0]["health"] is not None and failed["members"][0]["health"]["state"] == "unhealthy",
                "stale bootstrap did not retain the exact-current failure")
        self._restore_source()
        self._wait_healthy(PROJECT_ID, ROUTE_ID)
        after_durable, after_trees = self._durable_and_tree_state()
        require(after_durable == before_durable and after_trees == before_trees,
                "read-only knowledge checks changed durable state, source, index, or preparation artifacts")

    def _stale_health(self) -> dict[str, Any] | None:
        health = self._health(PROJECT_ID, ROUTE_ID)
        assessment = health["assessment"]
        if assessment is not None and assessment.get("index_current") is False:
            return health
        return None

    def check_corrupt_artifact_recovery(self) -> None:
        before = self._durable_and_tree_state()
        active = self.suite.activation(PROJECT_ID, ROUTE_ID)
        identifier = active["preparation_id"]
        key = self.suite.scalar("SELECT artifact_key FROM shin_server.preparations WHERE id=" + str(identifier))
        artifact = self.suite.preparation_root / f"job-{identifier}-{key}" / "docs/kb/generated/sections.jsonl"
        original = artifact.read_text(encoding="utf-8")
        try:
            self.suite.write_fixture_file(artifact, original + "{invalid-artifact\n")
            def detected():
                value = self._health(PROJECT_ID, ROUTE_ID)
                return value if value["state"] in {"unhealthy", "unavailable"} else None
            observed = self._wait("background corrupt artifact detection", detected)
            require(observed["assessment"]["healthy"] is False and observed["assessment"]["reason_codes"],
                    "corrupt artifact retained healthy cached evidence")
            result, _ = self._bootstrap("knowledge-corrupt", [
                self._member(PROJECT_ID, "route", ROUTE_ID, "AlphaWorkspaceEvidence", required=True),
            ])
            require(result["ready"] is False and result["context_ready"] is False and result["members"][0]["health"] is not None,
                    "corrupt artifact bootstrap lost health or admitted exact-current context")
        finally:
            self.suite.write_fixture_file(artifact, original)
        self._wait_healthy(PROJECT_ID, ROUTE_ID)
        require(self._durable_and_tree_state() == before, "artifact health checks mutated durable state or other content")

    def check_drift_preparation_deduplication(self) -> None:
        self._policy([ROUTE_ID, OTHER_ROUTE], prepare_on_drift=True)
        self._wait_healthy(PROJECT_ID, ROUTE_ID)
        active_before = self.suite.activation(PROJECT_ID, ROUTE_ID)
        heads_before = self.suite.durable_state()["publication_heads"]
        source_before = self.suite.tree_digest(self.suite.fixture_root)
        output_before = self.suite.tree_digest(self.suite.out_dir)
        preparation_before = self.suite.table_count("preparations")
        self._drift_source()

        def candidate_requested() -> dict[str, Any] | None:
            health = self._stale_health()
            if health is None:
                return None
            action = health["assessment"]["maintenance_action"]
            if isinstance(action, dict) and action.get("state") == "candidate-requested":
                return health
            return None

        observed = self._wait("one drift preparation request", candidate_requested)
        action = observed["assessment"]["maintenance_action"]
        require(observed["assessment"]["mutations_performed"] is True and type(action.get("job_id")) is int and
                action["job_id"] > 0 and type(action.get("replayed")) is bool,
                "drift preparation receipt is incomplete")
        job_id = action["job_id"]
        job = self.suite.assert_public_job(self.suite.get_job(job_id), project_id=PROJECT_ID, route_id=ROUTE_ID)
        expected = self.suite.sql_json("SELECT jsonb_build_object('expected_snapshot_id', expected_snapshot_id, "
                                       "'expected_route_revision', expected_route_revision) "
                                       "FROM shin_server.preparations WHERE id=" + str(job_id))
        require(expected == {"expected_snapshot_id": active_before["snapshot_id"],
                             "expected_route_revision": active_before["revision"]},
                "drift preparation did not bind the active expected identity")
        self._wait("periodic replay of drift assessment", lambda: self._maintenance_reobserved(job_id))
        require(self.suite.table_count("preparations") == preparation_before + 1,
                "same drift identity enqueued more than one ordinary candidate")
        require(self.suite.activation(PROJECT_ID, ROUTE_ID) == active_before and
                self.suite.durable_state()["publication_heads"] == heads_before,
                "drift preparation activated a generation or changed publication state")
        require(self.suite.tree_digest(self.suite.fixture_root) != source_before and
                self.suite.tree_digest(self.suite.out_dir) == output_before,
                "drift preparation rewrote canonical source or index output")
        self._restore_source()
        self._wait_healthy(PROJECT_ID, ROUTE_ID)

    def _maintenance_reobserved(self, job_id: int) -> bool:
        health = self._stale_health()
        if health is None:
            return False
        action = health["assessment"]["maintenance_action"]
        return isinstance(action, dict) and action.get("state") == "candidate-requested" and action.get("job_id") == job_id and action.get("replayed") is True
