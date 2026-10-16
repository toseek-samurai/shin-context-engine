"""P3E14 HTTP checks inside the publication suite's existing StandGuard.

No independent database setup, candidate publication, or source writes. Private
configuration restarts preserve the deployment identity and all unrelated fields.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from copy import deepcopy
import json
from typing import Any, Iterator
from urllib.parse import urlencode

from publication_test_support import exact_object, integer, no_private_values, require
from retrieval_integration_test import PROJECT_ID, ROUTE_ID


METRICS = "/api/v1/telemetry/retrieval"
REQUESTS = METRICS + "/requests"
OPERATIONS = ("search", "brief", "pack")
PHASES = ("validation", "selection", "admission", "execution", "finalization")
BOUNDS = (1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000, None)
SELECTION_FIELDS = {"route_id", "workspace_id", "source_host_id", "snapshot_id", "preparation_id", "revision"}
QUALITY_FIELDS = {"retrieval_mode", "returned_records", "candidate_rows", "covered_groups", "total_groups",
                  "pack_complete", "pack_coverage_complete", "required_requirements", "satisfied_required_requirements"}
RECORD_FIELDS = {"sequence", "operation", "selector_kind", "project_id", "selector_id", "observed_at_unix_ms",
                 "outcome", "status", "reason_code", "last_phase", "elapsed_ns", "result_bytes", "phase_ns",
                 "selection", "quality", "query_capture", "request"}
HEADER_MARKER = "p3e14-header-must-not-be-retained"
QUERY_MARKER = "P3E14UnmatchedDiagnosticNeedle"


class RetrievalObservabilityChecks:
    def __init__(self, suite: Any) -> None:
        self.s = suite
        self.baseline_results: dict[str, Any] = {}

    def run(self) -> None:
        for name, check in (
            ("default-metadata-and-exact-counters", self.check_default),
            ("concurrent-exact-counters", self.check_concurrent),
            ("authorization-and-get-guards", self.check_access),
            ("project-opt-in-validated-capture", self.check_capture),
            ("eviction-pagination-and-restart", self.check_retention),
            ("zero-retention-keeps-counters", self.check_disabled),
        ):
            self.s.record("retrieval-observability-" + name, check)

    def metrics(self) -> dict[str, Any]:
        result = self.s.expect_status(200, "GET", METRICS, headers=self.s.auth_headers("admin"))["result"]
        exact_object(result, {"schema_version", "instance_id", "scope", "counter_policy", "histogram_encoding",
                              "cells", "retention", "collection_errors"}, "retrieval metrics")
        require(result["schema_version"] == "shin.retrieval-metrics.v1" and
                result["scope"] == "process-lifetime-authorized-service-attempts", "metrics scope/schema differs")
        require(isinstance(result["instance_id"], str) and len(result["instance_id"]) == 64 and
                all(c in "0123456789abcdef" for c in result["instance_id"]), "invalid telemetry instance")
        require(result["counter_policy"] == "saturating-uint64" and result["histogram_encoding"] ==
                "non-cumulative-upper-inclusive; null upper bound is +Inf", "counter/histogram policy differs")
        require(len(result["cells"]) == 6 and set(self.cells(result)) ==
                {(operation, selector) for operation in OPERATIONS for selector in ("route", "publication")},
                "metrics must have exactly six fixed cells")
        for cell in result["cells"]:
            exact_object(cell, {"operation", "selector_kind", "started", "completed", "active", "succeeded", "failed",
                                "elapsed_total_ns", "elapsed_max_ns", "histogram"}, "metric cell")
            for key in ("started", "completed", "active", "succeeded", "failed", "elapsed_total_ns", "elapsed_max_ns"):
                integer(cell[key], 0, "metric " + key)
            require(cell["active"] == 0 and cell["started"] == cell["completed"] == cell["succeeded"] + cell["failed"],
                    "settled metric counters do not balance")
            require(cell["elapsed_max_ns"] <= cell["elapsed_total_ns"], "maximum elapsed exceeds total")
            require(len(cell["histogram"]) == len(BOUNDS), "histogram bucket count differs")
            for bucket, bound in zip(cell["histogram"], BOUNDS):
                exact_object(bucket, {"upper_bound_ms", "count"}, "histogram bucket")
                require(bucket["upper_bound_ms"] == bound, "histogram boundaries differ")
                integer(bucket["count"], 0, "histogram count")
            require(sum(bucket["count"] for bucket in cell["histogram"]) == cell["completed"],
                    "histogram does not count every completion exactly once")
        retention = exact_object(result["retention"], {"maximum_records", "maximum_payload_bytes", "retained_records",
            "retained_payload_bytes", "evicted", "dropped", "disabled", "latest_sequence"}, "retention")
        for key, value in retention.items():
            integer(value, 0, "retention " + key)
        require(retention["retained_records"] <= retention["maximum_records"] and
                retention["retained_payload_bytes"] <= retention["maximum_payload_bytes"] == 2 * 1024 * 1024,
                "diagnostic retention exceeded bounds")
        require(result["collection_errors"] == retention["dropped"] == 0, "telemetry collection lost records")
        return result

    @staticmethod
    def cells(metrics: dict[str, Any]) -> dict[tuple[str, str], Any]:
        return {(cell["operation"], cell["selector_kind"]): cell for cell in metrics["cells"]}

    def page(self, after: int = 0, limit: int = 32, instance: str | None = None) -> dict[str, Any]:
        params: dict[str, Any] = {"after": after, "limit": limit}
        if instance is not None:
            params["instance_id"] = instance
        # Only cursor metadata goes in GET URLs, never raw query/task text.
        result = self.s.expect_status(200, "GET", REQUESTS + "?" + urlencode(params),
                                      headers=self.s.auth_headers("admin"))["result"]
        exact_object(result, {"schema_version", "instance_id", "scope", "oldest_available_sequence", "latest_sequence",
                              "gap", "next_after", "items"}, "diagnostic page")
        require(result["schema_version"] == "shin.retrieval-request-page.v1" and
                result["scope"] == "bounded-process-local-diagnostics", "diagnostic page scope/schema differs")
        require(type(result["gap"]) is bool and isinstance(result["items"], list) and len(result["items"]) <= limit,
                "invalid diagnostic page bounds")
        sequences = [item["sequence"] for item in result["items"]]
        require(sequences == sorted(set(sequences)) and all(after < n <= result["latest_sequence"] for n in sequences),
                "diagnostic page skipped its cursor or repeated/reordered records")
        if instance is not None:
            require(result["instance_id"] == instance, "diagnostic page changed instance")
        return result

    def post(self, operation: str, payload: Any, *, selector: str = "publication", project: str = PROJECT_ID,
             name: str | None = None, status: int = 200, role: str = "reader") -> dict[str, Any]:
        name = name or ("shared" if selector == "publication" else ROUTE_ID)
        path = f"/api/v1/projects/{project}/{selector}s/{name}/{operation}"
        body = payload if isinstance(payload, bytes) else json.dumps(payload, separators=(",", ":")).encode("utf-8")
        actual, response = self.s.request("POST", path, body=body, headers={**self.s.auth_headers(role),
            "Content-Type": "application/json", "X-P3E14-Diagnostic": HEADER_MARKER})
        require(actual == status, "telemetry fixture " + operation + f" returned HTTP {actual}, expected {status}")
        if status == 200:
            fields = {"project_id", "route_id", "workspace_id", "source_host_id", "snapshot_id", "preparation_id",
                      "revision", "admission", "evidence"} | ({"publication_id"} if selector == "publication" else set())
            exact_object(response["result"], fields, "unchanged retrieval result")
        return response

    def payload(self, operation: str) -> dict[str, Any]:
        if operation != "pack":
            return {"query": "  AlphaWorkspaceEvidence  ", "limit": 3, "require_current": True}
        task = self.s.task_request()
        task["task"]["objective"] = "P3E14 inspect selected publication evidence."
        task["task"]["scope"] = []
        task["task"]["must"] = [{"id": "selected", "query": "AlphaWorkspaceEvidence"}]
        return {"task_request": task, "require_current": True}

    def record(self, item: dict[str, Any], response: dict[str, Any], *, operation: str, selector: str = "publication",
               status: int = 200, phase: str = "finalization", capture: str = "disabled", request: Any = None,
               project: str = PROJECT_ID, name: str | None = None) -> None:
        exact_object(item, RECORD_FIELDS, "retrieval diagnostic")
        require((item["operation"], item["selector_kind"], item["project_id"], item["selector_id"]) ==
                (operation, selector, project, name or ("shared" if selector == "publication" else ROUTE_ID)),
                "diagnostic request identity differs")
        require(item["status"] == status and item["outcome"] == ("success" if status == 200 else "error") and
                item["last_phase"] == phase, "diagnostic outcome or failed phase differs")
        require(item["query_capture"] == capture and item["request"] == request, "diagnostic capture policy/input differs")
        for key in ("sequence", "observed_at_unix_ms", "elapsed_ns"):
            integer(item[key], 1 if key != "elapsed_ns" else 0, "diagnostic " + key)
        exact_object(item["phase_ns"], set(PHASES), "diagnostic phases")
        for index, name in enumerate(PHASES):
            value = item["phase_ns"][name]
            if index <= PHASES.index(phase):
                integer(value, 0, "measured phase")
            else:
                require(value is None, "unmeasured phase fabricated a duration")
        require(sum(value for value in item["phase_ns"].values() if value is not None) == item["elapsed_ns"],
                "phase durations do not partition elapsed time")
        expected_quality = dict.fromkeys(QUALITY_FIELDS)
        if status != 200:
            require(item["reason_code"] == response["error"]["code"] and item["selection"] is None and
                    item["result_bytes"] is None, "failed request fabricated successful provenance")
        else:
            result = response["result"]
            require(item["reason_code"] is None and item["selection"] == {key: result[key] for key in SELECTION_FIELDS},
                    "diagnostic selection differs from actual retrieval")
            integer(item["result_bytes"], 1, "result bytes")
            evidence = result["evidence"]
            if evidence.get("retrieval_mode") in ("strict", "relaxed", "owner-scoped", "directory-scoped", "planned"):
                expected_quality["retrieval_mode"] = evidence["retrieval_mode"]
            if isinstance(evidence.get("evidence"), list):
                expected_quality["returned_records"] = len(evidence["evidence"])
            if isinstance(evidence.get("sections"), list) and isinstance(evidence.get("facts"), list):
                expected_quality["returned_records"] = len(evidence["sections"]) + len(evidence["facts"])
            for source, target, field in (
                ("query_diagnostics", "candidate_rows", "candidate_rows"),
                ("query_coverage", "covered_groups", "covered_groups"),
                ("query_coverage", "total_groups", "total_groups"),
                ("completion", "pack_complete", "complete"),
                ("completion", "pack_coverage_complete", "coverage_complete"),
                ("completion", "required_requirements", "required_requirements"),
                ("completion", "satisfied_required_requirements", "satisfied_required_requirements"),
            ):
                expected_quality[target] = evidence.get(source, {}).get(field)
        require(item["quality"] == expected_quality, "diagnostic quality differs from existing Core envelope")

    def delta(self, before: dict[str, Any], expected: list[tuple[str, str, int]],
              records: list[dict[str, Any]] | None = None) -> dict[str, Any]:
        after = self.metrics()
        require(after["instance_id"] == before["instance_id"], "counter window crossed a restart")
        require(after["retention"]["latest_sequence"] - before["retention"]["latest_sequence"] == len(expected),
                "authorized attempts did not advance sequence exactly once")
        for key, old in self.cells(before).items():
            new = self.cells(after)[key]
            statuses = [status for op, selector, status in expected if (op, selector) == key]
            for field, count in (("started", len(statuses)), ("completed", len(statuses)),
                                 ("succeeded", statuses.count(200)), ("failed", len(statuses) - statuses.count(200))):
                require(new[field] - old[field] == count, "exact service counter delta differs: " + field)
            if not statuses:
                require(new == old, "unrelated metric cell changed")
            elif records is not None:
                durations = [r["elapsed_ns"] for r in records if (r["operation"], r["selector_kind"]) == key]
                require(len(durations) == len(statuses) and new["elapsed_total_ns"] - old["elapsed_total_ns"] == sum(durations)
                        and new["elapsed_max_ns"] == max([old["elapsed_max_ns"], *durations]),
                        "elapsed counters do not equal recorded attempts")
                buckets = [0] * len(BOUNDS)
                for duration in durations:
                    bucket = next(i for i, bound in enumerate(BOUNDS) if bound is None or duration <= bound * 1_000_000)
                    buckets[bucket] += 1
                require([n["count"] - o["count"] for n, o in zip(new["histogram"], old["histogram"])] == buckets,
                        "histogram delta does not match measured request durations")
        return after

    @contextmanager
    def unchanged(self) -> Iterator[None]:
        roots = (self.s.fixture_root, self.s.peer_root, self.s.other_root, self.s.out_dir,
                 self.s.other_out_dir, self.s.preparation_root)
        trees = [self.s.tree_digest(root) for root in roots]
        durable = self.s.durable_state()
        yield
        self.s.unchanged(durable, "retrieval observability")
        require(trees == [self.s.tree_digest(root) for root in roots], "telemetry changed source/index/artifact content")

    @contextmanager
    def configured(self, retained: int, query_bytes: int, projects: list[str]) -> Iterator[None]:
        original = json.loads(self.s.runtime_config_path.read_text(encoding="utf-8"))
        changed = deepcopy(original)
        changed["observability"] = {"max_retained_requests": retained, "max_query_bytes": query_bytes,
                                    "capture_query_projects": projects}
        identity = self.s.server_id
        self.s.stop_server(expect_clean=True)
        try:
            self.s.replace_private_json(self.s.runtime_config_path, changed)
            self.s.start_server()
            require(self.s.server_id == identity, "observability restart changed deployment identity")
            yield
        finally:
            self.s.stop_server(expect_clean=False)
            self.s.replace_private_json(self.s.runtime_config_path, original)
            self.s.start_server()
        require(json.loads(self.s.runtime_config_path.read_text(encoding="utf-8")) == original and
                self.s.server_id == identity, "observability restart did not restore config/identity")

    def private_values(self, value: Any, *, include_query: bool = True) -> None:
        forbidden = [self.s.admin_token, self.s.reader_token, HEADER_MARKER, "immutable publication review.",
                     str(self.s.fixture_root), str(self.s.peer_root), str(self.s.other_root), str(self.s.preparation_root)]
        if include_query:
            forbidden += ["AlphaWorkspaceEvidence", QUERY_MARKER, "P3E14 inspect selected publication evidence."]
        no_private_values(value, forbidden)

    def check_default(self) -> None:
        config = json.loads(self.s.runtime_config_path.read_text(encoding="utf-8"))
        require(config["observability"] == {"max_retained_requests": 128, "max_query_bytes": 4096,
                                             "capture_query_projects": []}, "fixture does not use metadata-only defaults")
        with self.unchanged():
            current = self.s.get_head()
            require(current["state"] == "active" and current["source_route_id"] == ROUTE_ID,
                    "telemetry requires the unchanged main shared source candidate")
            before = self.metrics()
            expected, replies = [], []
            for selector in ("route", "publication"):
                for operation in OPERATIONS:
                    payload = self.payload(operation)
                    # Private route's older active candidate is intentionally not republished.
                    payload["require_current"] = selector == "publication"
                    response = self.post(operation, payload, selector=selector)
                    if selector == "publication":
                        self.baseline_results[operation] = response["result"]
                        require(response["result"]["preparation_id"] == current["preparation_id"] and
                                response["result"]["snapshot_id"] == current["snapshot_id"], "shared fixture selection differs")
                    expected.append((operation, selector, 200))
                    replies.append((response, {"operation": operation, "selector": selector}))
            incomplete = self.payload("pack")
            incomplete["task_request"]["task"]["must"] = [{"id": "missing", "query": QUERY_MARKER}]
            response = self.post("pack", incomplete)
            require(response["result"]["evidence"]["completion"]["complete"] is False, "Pack fixture is not incomplete")
            expected.append(("pack", "publication", 200))
            replies.append((response, {"operation": "pack"}))
            for operation, payload, name, status, phase in (
                ("search", {"query": QUERY_MARKER, "require_current": "yes"}, "shared", 400, "validation"),
                ("brief", {"query": [QUERY_MARKER]}, "shared", 400, "admission"),
                ("search", {"query": QUERY_MARKER}, "p3e14-empty", 503, "selection"),
            ):
                response = self.post(operation, payload, name=name, status=status)
                self.s.assert_error_code(response, "active-generation-missing" if status == 503 else "invalid-request")
                expected.append((operation, "publication", status))
                replies.append((response, {"operation": operation, "status": status, "phase": phase, "name": name}))
            page = self.page(before["retention"]["latest_sequence"], instance=before["instance_id"])
            require(len(page["items"]) == len(expected) and page["gap"] is False and page["next_after"] is None,
                    "default telemetry lost a known request")
            for item, (response, arguments) in zip(page["items"], replies):
                self.record(item, response, **arguments)
            require(page["items"][6]["quality"]["pack_complete"] is False and page["items"][6]["outcome"] == "success",
                    "incomplete Pack was classified as a service failure")
            after = self.delta(before, expected, page["items"])
            self.private_values(after)
            self.private_values(page)
            require(self.metrics() == after and self.page(before["retention"]["latest_sequence"],
                    instance=before["instance_id"]) == page, "diagnostic GETs counted themselves or changed records")

    def check_concurrent(self) -> None:
        with self.unchanged():
            before = self.metrics()
            keys = [(operation, selector) for selector in ("route", "publication") for operation in OPERATIONS]

            def retrieve(key: tuple[str, str]) -> dict[str, Any]:
                operation, selector = key
                payload = self.payload(operation)
                payload["require_current"] = selector == "publication"
                return self.post(operation, payload, selector=selector)

            with ThreadPoolExecutor(max_workers=2) as executor:
                replies = dict(zip(keys, executor.map(retrieve, keys)))
            page = self.page(before["retention"]["latest_sequence"], instance=before["instance_id"])
            require(len(page["items"]) == len(keys) and page["gap"] is False and page["next_after"] is None,
                    "concurrent requests lost diagnostic records")
            require({(item["operation"], item["selector_kind"]) for item in page["items"]} == set(keys),
                    "concurrent diagnostics duplicated or misattributed a request")
            for item in page["items"]:
                key = (item["operation"], item["selector_kind"])
                self.record(item, replies[key], operation=key[0], selector=key[1])
                if key[1] == "publication":
                    require(replies[key]["result"] == self.baseline_results[key[0]], "concurrent telemetry changed evidence")
            self.delta(before, [(operation, selector, 200) for operation, selector in keys], page["items"])
            self.private_values(page)

    def check_access(self) -> None:
        with self.unchanged():
            before = self.metrics()
            for path in (METRICS, REQUESTS):
                self.s.expect_status(403, "GET", path, headers=self.s.auth_headers("reader"))
                self.s.expect_status(401, "GET", path)
                self.s.expect_status(401, "GET", path, headers={"Authorization": "Bearer p3e14-invalid-token"})
            for query in ("after=0", "limit=1", "instance_id=unused", "unknown=1"):
                self.s.expect_status(400, "GET", METRICS + "?" + query, headers=self.s.auth_headers("admin"))
            for query in ("limit=0", "limit=33", "limit=-1", "limit=true", "limit=", "limit=1.5", "after=",
                          "after=-1", "after=abc", "after=18446744073709551616", "after=1.5", "unknown=1",
                          "limit=1&limit=2", "after=0&after=1", "instance_id=a&instance_id=b"):
                self.s.expect_status(400, "GET", REQUESTS + "?" + query, headers=self.s.auth_headers("admin"))
            future = before["retention"]["latest_sequence"] + 1
            self.s.expect_status(400, "GET", REQUESTS + "?" + urlencode({"after": future, "instance_id": before["instance_id"]}),
                                 headers=self.s.auth_headers("admin"))
            for operation in OPERATIONS:
                for selector, name in (("route", "other-retrieval"), ("publication", "shared")):
                    self.post(operation, self.payload(operation), project="other", selector=selector, name=name, status=404)
            self.post("search", b"{", status=400)
            require(self.metrics() == before, "denied projects/authentication/transport/GETs entered service telemetry")

    def check_capture(self) -> None:
        with self.unchanged(), self.configured(16, 2048, [PROJECT_ID]):
            before = self.metrics()
            require(before["retention"]["latest_sequence"] == 0, "capture restart retained process history")
            expected = []
            for operation in OPERATIONS:
                payload = self.payload(operation)
                response = self.post(operation, payload)
                require(response["result"] == self.baseline_results[operation], "capture changed retrieval evidence or provenance")
                item = self.page()["items"][-1]
                self.record(item, response, operation=operation, capture="captured", request=payload)
                self.private_values(item, include_query=False)
                expected.append((operation, "publication", 200))
            oversized = {"query": " " * 2100 + "AlphaWorkspaceEvidence", "require_current": True}
            response = self.post("search", oversized)
            self.record(self.page()["items"][-1], response, operation="search", capture="omitted-budget")
            expected.append(("search", "publication", 200))
            response = self.post("brief", {"query": [QUERY_MARKER]}, status=400)
            self.record(self.page()["items"][-1], response, operation="brief", status=400,
                        phase="admission", capture="invalid-request")
            expected.append(("brief", "publication", 400))
            response = self.post("pack", {"task_request": [QUERY_MARKER]}, status=400)
            self.s.assert_error_code(response, "invalid-task-document")
            self.record(self.page()["items"][-1], response, operation="pack", status=400,
                        phase="admission", capture="invalid-request")
            expected.append(("pack", "publication", 400))
            response = self.post("search", {"query": QUERY_MARKER, "require_current": False}, selector="route",
                                 project="other", name="other-retrieval", role="admin", status=503)
            self.s.assert_error_code(response, "active-generation-missing")
            self.record(self.page()["items"][-1], response, operation="search", selector="route", project="other",
                        name="other-retrieval", status=503, phase="admission")
            expected.append(("search", "route", 503))
            page = self.page()
            require(len(page["items"]) == len(expected), "capture fixture lost request records")
            self.delta(before, expected, page["items"])
            before_denied = self.metrics()
            self.post("search", {"query": QUERY_MARKER}, selector="route", project="other", name="other-retrieval", status=404)
            require(self.metrics() == before_denied and self.page() == page, "capture allowlist bypassed project authorization")
            require(self.s.server_log is not None, "capture process log unavailable")
            self.private_values(self.s.server_log.read_text(encoding="utf-8"))

    def check_retention(self) -> None:
        with self.unchanged(), self.configured(3, 4096, []):
            before = self.metrics()
            require(before["retention"]["latest_sequence"] == 0 and self.page()["items"] == [], "restart history was not empty")
            for _ in range(5):
                self.post("search", self.payload("search"))
            after = self.delta(before, [("search", "publication", 200)] * 5)
            require(after["retention"]["retained_records"] == 3 and after["retention"]["evicted"] == 2 and
                    after["retention"]["maximum_records"] == 3, "small retention did not evict exactly the oldest two")
            instance = after["instance_id"]
            first = self.page(limit=1)
            require(first["gap"] is True and first["oldest_available_sequence"] == 3 and first["latest_sequence"] == 5 and
                    [r["sequence"] for r in first["items"]] == [3] and first["next_after"] == 3, "eviction gap/page differs")
            second = self.page(3, 1, instance)
            last = self.page(4, 1, instance)
            require(second["gap"] is False and [r["sequence"] for r in second["items"]] == [4] and second["next_after"] == 4 and
                    last["gap"] is False and [r["sequence"] for r in last["items"]] == [5] and last["next_after"] is None,
                    "same-instance pagination skipped/repeated records")
            end = self.page(5, instance=instance)
            require(end["items"] == [] and end["next_after"] is None and end["gap"] is False, "terminal cursor differs")
            boundary = self.page(2, instance=instance)
            require(boundary["gap"] is False and [r["sequence"] for r in boundary["items"]] == [3, 4, 5],
                    "cursor immediately before retained history falsely reports a gap")
            for params in ({"after": 1}, {"after": 0, "instance_id": "0" * 64}):
                response = self.s.expect_status(409, "GET", REQUESTS + "?" + urlencode(params), headers=self.s.auth_headers("admin"))
                self.s.assert_error_code(response, "telemetry-instance-mismatch")
            require(self.metrics() == after, "pagination and refused cursors counted as retrieval")
            self.s.stop_server(expect_clean=True)
            self.s.start_server()
            restarted = self.metrics()
            require(restarted["instance_id"] != instance and restarted["retention"]["latest_sequence"] == 0 and
                    restarted["retention"]["retained_records"] == restarted["retention"]["evicted"] == 0 and
                    all(cell["started"] == 0 for cell in restarted["cells"]), "restart did not reset process-local telemetry")
            response = self.s.expect_status(409, "GET", REQUESTS + "?" + urlencode({"after": 5, "instance_id": instance}),
                                            headers=self.s.auth_headers("admin"))
            self.s.assert_error_code(response, "telemetry-instance-mismatch")
            empty = self.page()
            require(empty["items"] == [] and empty["oldest_available_sequence"] is None and empty["gap"] is False,
                    "new process fabricated retained history")

    def check_disabled(self) -> None:
        with self.unchanged(), self.configured(0, 4096, []):
            before = self.metrics()
            self.post("search", self.payload("search"))
            self.post("brief", {"query": QUERY_MARKER, "require_current": "invalid"}, status=400)
            after = self.delta(before, [("search", "publication", 200), ("brief", "publication", 400)])
            retention = after["retention"]
            require(retention["maximum_records"] == retention["retained_records"] == retention["retained_payload_bytes"] ==
                    retention["evicted"] == 0 and retention["disabled"] == 2, "zero retention did not disable only diagnostics")
            page = self.page()
            require(page["items"] == [] and page["oldest_available_sequence"] is None and page["latest_sequence"] == 2 and
                    page["gap"] is True and page["next_after"] is None, "disabled history did not expose its diagnostic gap")
            end = self.page(2, instance=after["instance_id"])
            require(end["items"] == [] and end["gap"] is False and self.metrics() == after,
                    "disabled diagnostic reads self-counted or fabricated history")
