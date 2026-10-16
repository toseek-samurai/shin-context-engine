"""P3E15 retrieval journal checks within the publication suite's existing StandGuard.

The suite owns no stand setup. It records current process diagnostics only through
the administrative capture endpoint and uses short-lived SQL fixtures only for
deterministic aggregate and capacity checks.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import json
import re
import secrets
import statistics
import time
from typing import Any, Iterator
from urllib.parse import urlencode

from publication_test_support import exact_object, integer, no_private_values, require
from retrieval_integration_test import PROJECT_ID, ROUTE_ID
from retrieval_observability_checks import RetrievalObservabilityChecks


JOURNAL = "/api/v1/telemetry/retrieval/journal"
CAPTURE = JOURNAL + "/capture"
SUMMARY = JOURNAL + "/summary"
QUERIES = JOURNAL + "/queries"
ATTEMPTS = JOURNAL + "/attempts/"
JOURNAL_HEADER_MARKER = "p3e15-journal-header-must-not-persist"
JOURNAL_QUERY_MARKER = "AlphaWorkspaceEvidence"
FNV64 = re.compile(r"fnv1a64:[0-9a-f]{16}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")

COVERAGE_FIELDS = {
    "population", "all_traffic_complete", "producer_count", "captured_count", "known_missing_sequences_global",
    "lifetime_captured_count", "retired_fact_count", "retired_missing_sequences_global",
    "allocated_producer_epochs", "reclaimed_through_epoch", "reclaimed_producer_count",
    "unknown_after_last_capture", "feedback_available",
}
CAPTURE_FIELDS = {
    "schema_version", "instance_id", "producer_epoch", "inserted_count", "already_observed_count", "new_missing_sequences",
    "observed_through", "retired_ignored_count", "retired_through", "replayed", "coverage", "source_mutated",
    "publication_mutated", "recovered_missing_sequences",
}
ATTEMPT_FIELDS = {
    "scope_principal_id", "scope_tenant_id", "scope_workspace_id",
    "id", "instance_id", "producer_epoch", "sequence", "record_hash", "project_id", "operation", "selector_kind", "selector_id",
    "observed_at_unix_ms", "status", "reason_code", "last_phase", "elapsed_ns", "result_bytes", "validation_ns",
    "selection_ns", "admission_ns", "execution_ns", "finalization_ns", "route_id", "workspace_id", "source_host_id",
    "snapshot_id", "preparation_id", "revision", "profile_id", "profile_fingerprint", "route_fingerprint",
    "retrieval_mode", "returned_records", "candidate_rows", "covered_groups", "total_groups", "pack_complete",
    "pack_coverage_complete", "required_requirements", "satisfied_required_requirements", "query_capture",
    "request_fingerprint", "received_at",
}
GROUP_DIMENSIONS = {
    "operation", "selector_kind", "selector_id", "profile_id", "profile_fingerprint", "route_fingerprint",
    "snapshot_id", "server_version", "core_version",
}
GROUP_MEASUREMENTS = {
    "attempts", "example_attempt_id", "successes", "errors", "elapsed_total_ns", "success_p95_ns", "error_p95_ns", "known_pack_count",
    "incomplete_pack_count", "known_result_count", "zero_result_count", "known_coverage_count",
    "captured_request_count", "validation_total_ns", "selection_total_ns", "admission_total_ns", "execution_total_ns",
    "finalization_total_ns",
    "selection_count", "admission_count", "execution_count", "finalization_count",
}
REPORT_FIELDS = {
    "schema_version", "metric_contract", "project_id", "groups", "total_groups", "omitted_groups", "project_attempts",
    "project_captured_requests", "query_identity", "coverage", "read_only",
    "project_successes", "project_errors", "project_known_packs", "project_incomplete_packs", "project_known_results", "project_zero_results",
}


class RetrievalJournalChecks:
    def __init__(self, suite: Any) -> None:
        self.s = suite
        self.o = RetrievalObservabilityChecks(suite)
        self.timings: dict[str, list[float]] = {"capture": [], "summary": [], "queries": []}

    def run(self) -> None:
        for name, check in (
            ("retrieval-journal-migration-and-clean-capture", self.check_migration_and_clean_capture),
            ("retrieval-journal-retry-and-concurrent-capture", self.check_retry_and_concurrent_capture),
            ("retrieval-journal-out-of-order-capture", self.check_out_of_order_capture),
            ("retrieval-journal-controlled-metric-cohorts", self.check_controlled_metric_cohorts),
            ("retrieval-journal-bounded-cohorts-full-totals", self.check_bounded_cohorts_full_totals),
            ("retrieval-journal-opt-in-exact-payloads", self.check_opt_in_exact_payloads),
            ("retrieval-journal-jsonb-request-boundary", self.check_jsonb_request_boundary),
            ("retrieval-journal-eviction-gap-and-retry", self.check_eviction_gap_and_retry),
            ("retrieval-journal-read-bounds-and-admin-guards", self.check_read_bounds_and_admin_guards),
            ("retrieval-journal-transaction-rollback-and-recovery", self.check_transaction_rollback_and_recovery),
            ("retrieval-journal-synthetic-capacity-and-cleanup", self.check_synthetic_capacity_and_cleanup),
        ):
            self.s.record(name, check)
        self.s.retrieval_journal_measurement = {
            "status": "passed",
            "calls": {name: {"samples": len(samples), "median_ms": statistics.median(samples) if samples else None,
                              "min_ms": min(samples, default=None), "max_ms": max(samples, default=None)}
                      for name, samples in self.timings.items()},
            "scope": "Bounded local HTTP timings with response decoding on the existing integration stand. "
                     "They are neither a load test nor a production throughput or SLO claim.",
        }

    @staticmethod
    def _json_body(value: Any) -> bytes:
        return json.dumps(value, separators=(",", ":")).encode("utf-8")

    def _coverage(self, value: Any) -> dict[str, Any]:
        result = exact_object(value, COVERAGE_FIELDS, "retrieval journal coverage")
        require(result["population"] == "persisted-journal-facts-only" and result["all_traffic_complete"] is False and
                result["unknown_after_last_capture"] is True and result["feedback_available"] is False,
                "journal coverage scope differs")
        for key in ("producer_count", "captured_count", "lifetime_captured_count", "retired_fact_count",
                    "known_missing_sequences_global", "retired_missing_sequences_global", "allocated_producer_epochs",
                    "reclaimed_through_epoch", "reclaimed_producer_count"):
            integer(result[key], 0, "journal coverage " + key)
        require(result["captured_count"] + result["retired_fact_count"] == result["lifetime_captured_count"],
                "journal coverage did not distinguish retained from lifetime facts")
        require(result["reclaimed_producer_count"] <= result["reclaimed_through_epoch"] <= result["allocated_producer_epochs"],
                "journal coverage epoch lifecycle accounting differs")
        return result

    def capture(self, *, role: str = "admin", body: Any = None, suffix: str = "",
                header_marker: bool = False, expected: int = 200) -> dict[str, Any]:
        if body is None:
            body = {}
        headers = {**self.s.auth_headers(role), "Content-Type": "application/json"}
        if header_marker:
            headers["X-P3E15-Journal-Private"] = JOURNAL_HEADER_MARKER
        started = time.perf_counter_ns()
        response = self.s.expect_status(expected, "POST", CAPTURE + suffix, headers=headers, body=self._json_body(body))
        if expected == 200:
            self.timings["capture"].append((time.perf_counter_ns() - started) / 1_000_000)
        if expected != 200:
            return response
        result = exact_object(response["result"], CAPTURE_FIELDS, "retrieval journal capture")
        require(result["schema_version"] == "shin.retrieval-journal-capture.v1" and
                isinstance(result["instance_id"], str) and SHA256.fullmatch(result["instance_id"]) is not None and
                type(result["replayed"]) is bool and result["source_mutated"] is False and result["publication_mutated"] is False,
                "journal capture metadata differs")
        for key in ("producer_epoch", "inserted_count", "already_observed_count", "new_missing_sequences", "recovered_missing_sequences",
                    "retired_ignored_count", "retired_through", "observed_through"):
            integer(result[key], 0, "journal capture " + key)
        require(result["producer_epoch"] > 0, "journal capture omitted its registered producer epoch")
        require(result["retired_through"] <= result["observed_through"],
                "journal capture returned a retirement fence beyond observed progress")
        self._coverage(result["coverage"])
        return result

    def history(self, *, project: str | None = None, after: int = 0, limit: int = 32) -> dict[str, Any]:
        params: dict[str, Any] = {"after": after, "limit": limit}
        if project is not None:
            params["project_id"] = project
        result = self.s.expect_status(200, "GET", JOURNAL + "?" + urlencode(params),
                                      headers=self.s.auth_headers("admin"))["result"]
        exact_object(result, {"schema_version", "items", "next_after", "coverage", "read_only"}, "journal history")
        require(result["schema_version"] == "shin.retrieval-journal-page.v1" and result["read_only"] is True and
                isinstance(result["items"], list) and len(result["items"]) <= limit, "journal history envelope differs")
        ids: list[int] = []
        for item in result["items"]:
            self._attempt(item, detail=False)
            ids.append(item["id"])
            if project is not None:
                require(item["project_id"] == project, "history ignored project filter")
        require(ids == sorted(set(ids)) and all(after < value for value in ids), "journal history cursor order differs")
        if result["next_after"] is not None:
            integer(result["next_after"], 1, "journal history next_after")
            require(ids and result["next_after"] == ids[-1], "journal history continuation differs")
        self._coverage(result["coverage"])
        return result

    def detail(self, identifier: int) -> dict[str, Any]:
        result = self.s.expect_status(200, "GET", ATTEMPTS + str(identifier), headers=self.s.auth_headers("admin"))["result"]
        exact_object(result, {"schema_version", "attempt", "read_only"}, "journal detail")
        require(result["schema_version"] == "shin.retrieval-journal-detail.v1" and result["read_only"] is True,
                "journal detail envelope differs")
        self._attempt(result["attempt"], detail=True)
        require(result["attempt"]["id"] == identifier, "journal detail returned a different attempt")
        return result["attempt"]

    def report(self, endpoint: str, project: str) -> dict[str, Any]:
        started = time.perf_counter_ns()
        result = self.s.expect_status(200, "GET", endpoint + "?" + urlencode({"project_id": project}),
                                      headers=self.s.auth_headers("admin"))["result"]
        self.timings["queries" if endpoint == QUERIES else "summary"].append((time.perf_counter_ns() - started) / 1_000_000)
        exact_object(result, REPORT_FIELDS, "journal report")
        expected_schema = "shin.retrieval-journal-queries.v1" if endpoint == QUERIES else "shin.retrieval-journal-summary.v1"
        require(result["schema_version"] == expected_schema and result["metric_contract"] == "shin.retrieval-journal-metrics.v1" and
                result["project_id"] == project and result["query_identity"] == "exact-captured-payload-only" and
                result["read_only"] is True and isinstance(result["groups"], list) and len(result["groups"]) <= 32,
                "journal report envelope differs")
        integer(result["total_groups"], 0, "journal total groups")
        integer(result["omitted_groups"], 0, "journal omitted groups")
        integer(result["project_attempts"], 0, "journal project attempts")
        integer(result["project_captured_requests"], 0, "journal project captured requests")
        for key in ("project_successes", "project_errors", "project_known_packs", "project_incomplete_packs", "project_known_results", "project_zero_results"):
            integer(result[key], 0, key)
        require(result["project_successes"] + result["project_errors"] == result["project_attempts"] and
                result["project_incomplete_packs"] <= result["project_known_packs"] <= result["project_attempts"] and
                result["project_zero_results"] <= result["project_known_results"] <= result["project_attempts"],
                "project totals or known-value denominators are inconsistent")
        require(result["total_groups"] == len(result["groups"]) + result["omitted_groups"], "journal group accounting differs")
        for group in result["groups"]:
            self._group(group, queries=endpoint == QUERIES)
        self._coverage(result["coverage"])
        return result

    @staticmethod
    def _p95(values: list[int]) -> float | None:
        if not values:
            return None
        ordered = sorted(values)
        position = 0.95 * (len(ordered) - 1)
        lower = int(position)
        upper = min(lower + 1, len(ordered) - 1)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)

    def _attempt(self, value: Any, *, detail: bool) -> dict[str, Any]:
        fields = ATTEMPT_FIELDS | ({"request"} if detail else set())
        attempt = exact_object(value, fields, "journal attempt")
        require(isinstance(attempt["id"], int) and attempt["id"] > 0 and SHA256.fullmatch(attempt["instance_id"]) is not None and
                SHA256.fullmatch(attempt["record_hash"]) is not None and attempt["operation"] in {"search", "brief", "pack"} and
                attempt["selector_kind"] in {"route", "publication"} and isinstance(attempt["received_at"], str),
                "journal attempt identity differs")
        for key in ("producer_epoch", "sequence", "observed_at_unix_ms", "elapsed_ns", "validation_ns"):
            integer(attempt[key], 0 if key != "sequence" else 1, "journal attempt " + key)
        require(attempt["producer_epoch"] > 0, "journal attempt omitted producer epoch")
        require(100 <= attempt["status"] <= 599 and (attempt["status"] == 200) == (attempt["reason_code"] is None),
                "journal attempt status/reason differs")
        phases = [attempt["validation_ns"], attempt["selection_ns"], attempt["admission_ns"], attempt["execution_ns"], attempt["finalization_ns"]]
        phase_index = ("validation", "selection", "admission", "execution", "finalization").index(attempt["last_phase"])
        require(all(isinstance(item, int) and item >= 0 for item in phases[:phase_index + 1]) and
                all(item is None for item in phases[phase_index + 1:]) and sum(item for item in phases if item is not None) == attempt["elapsed_ns"],
                "journal attempt phase boundary differs")
        if attempt["status"] != 200:
            require(all(attempt[key] is None for key in ("route_id", "workspace_id", "source_host_id", "snapshot_id", "preparation_id", "revision")),
                    "failed journal attempt fabricated selection provenance")
        if attempt["profile_fingerprint"] is not None:
            require(FNV64.fullmatch(attempt["profile_fingerprint"]) is not None, "journal profile fingerprint format differs")
        if attempt["snapshot_id"] is not None:
            require(FNV64.fullmatch(attempt["snapshot_id"]) is not None, "journal snapshot identity format differs")
        if attempt["route_fingerprint"] is not None:
            require(SHA256.fullmatch(attempt["route_fingerprint"]) is not None, "journal route fingerprint format differs")
        if detail:
            captured = attempt["query_capture"] == "captured"
            has_request = attempt["request"] is not None
            has_fingerprint = attempt["request_fingerprint"] is not None
            require(captured == has_request == has_fingerprint,
                    "journal detail request capture boundary differs")
        else:
            require("request" not in attempt, "journal history leaked captured request JSON")
        return attempt

    def _group(self, value: Any, *, queries: bool) -> dict[str, Any]:
        fields = GROUP_DIMENSIONS | GROUP_MEASUREMENTS | ({"request_fingerprint"} if queries else set())
        group = exact_object(value, fields, "journal query group" if queries else "journal summary cohort")
        require(group["operation"] in {"search", "brief", "pack"} and group["selector_kind"] in {"route", "publication"} and
                isinstance(group["server_version"], str) and isinstance(group["core_version"], str), "journal group dimensions differ")
        if group["profile_fingerprint"] is not None:
            require(FNV64.fullmatch(group["profile_fingerprint"]) is not None, "journal group lost prefixed profile fingerprint")
        if group["snapshot_id"] is not None:
            require(FNV64.fullmatch(group["snapshot_id"]) is not None, "journal group lost snapshot identity")
        if group["route_fingerprint"] is not None:
            require(SHA256.fullmatch(group["route_fingerprint"]) is not None, "journal group route fingerprint differs")
        if queries:
            require(SHA256.fullmatch(group["request_fingerprint"]) is not None, "query group has no exact request fingerprint")
        optional_totals = {phase + "_total_ns" for phase in ("selection", "admission", "execution", "finalization")}
        for key in GROUP_MEASUREMENTS - {"success_p95_ns", "error_p95_ns"} - optional_totals:
            integer(group[key], 0, "journal group " + key)
        for phase in ("selection", "admission", "execution", "finalization"):
            count = group[phase + "_count"]
            require(count <= group["attempts"], "phase denominator exceeds attempt count")
            total = group[phase + "_total_ns"]
            if count == 0:
                require(total is None, "unmeasured phase was represented as zero")
            else:
                integer(total, 0, "measured phase total")
        for key in ("success_p95_ns", "error_p95_ns"):
            require(group[key] is None or (isinstance(group[key], (int, float)) and not isinstance(group[key], bool) and group[key] >= 0),
                    "journal group percentile is invalid")
        require(group["attempts"] == group["successes"] + group["errors"], "journal group outcome count differs")
        return group

    def _journal_counts(self) -> tuple[int, int]:
        return self.s.table_count("retrieval_journal_producers"), self.s.table_count("retrieval_journal_attempts")

    def _protected_state(self) -> dict[str, Any]:
        state = self.s.durable_state()
        return {table: value for table, value in state.items()
                if table not in {"retrieval_journal_producers", "retrieval_journal_attempts", "retrieval_journal_lifecycle"}}

    @contextmanager
    def protected(self) -> Iterator[None]:
        before = self._protected_state()
        roots = (self.s.fixture_root, self.s.peer_root, self.s.other_root, self.s.out_dir,
                 self.s.other_out_dir, self.s.preparation_root)
        trees = [self.s.tree_digest(root) for root in roots]
        yield
        require(self._protected_state() == before, "journal operation mutated source, index, publication, management, or audit state")
        require([self.s.tree_digest(root) for root in roots] == trees, "journal operation mutated source or index content")

    @staticmethod
    def _sql_text(value: str) -> str:
        return "'" + value.replace("'", "''") + "'"

    def _producer(self, instance: str) -> dict[str, Any]:
        return self.s.sql_json("SELECT to_jsonb(p) FROM shin_server.retrieval_journal_producers p WHERE instance_id=" +
                               self._sql_text(instance))

    def _fixture_epochs(self, count: int) -> list[int]:
        require(count >= 0, "fixture epoch count is invalid")
        if count == 0:
            return []
        allocated = int(self.s.scalar(
            "SELECT allocated_through FROM shin_server.retrieval_journal_lifecycle WHERE singleton"))
        epochs = list(range(allocated + 1, allocated + count + 1))
        self.s.psql("UPDATE shin_server.retrieval_journal_lifecycle SET allocated_through=" + str(epochs[-1]) +
                    " WHERE singleton")
        return epochs

    def _restore_fixture_lifecycle(self) -> None:
        self.s.psql(
            "UPDATE shin_server.retrieval_journal_lifecycle SET allocated_through=GREATEST(reclaimed_through, "
            "(SELECT COALESCE(max(producer_epoch),0) FROM shin_server.retrieval_journal_producers)) WHERE singleton")

    def check_migration_and_clean_capture(self) -> None:
        with self.protected():
            require(self.s.scalar("SELECT string_agg(version::text, ',' ORDER BY version) FROM shin_server.schema_migrations") ==
                    "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35", "current migrations are not visible to the journal integration fixture")
            require(self._journal_counts() == (0, 0), "clean journal fixture already contains persisted observations")
            metrics = self.o.metrics()
            require(metrics["retention"]["latest_sequence"] == 0, "clean process has diagnostic history before journal capture")
            first = self.capture(header_marker=True)
            require((first["inserted_count"], first["already_observed_count"], first["new_missing_sequences"],
                     first["observed_through"], first["replayed"]) == (0, 0, 0, 0, False),
                    "empty clean-process capture receipt differs")
            coverage = first["coverage"]
            require((coverage["producer_count"], coverage["captured_count"], coverage["known_missing_sequences_global"]) == (1, 0, 0),
                    "empty clean-process capture did not persist producer metadata")
            require(self.history()["items"] == [], "empty journal capture fabricated fact rows")
            replay = self.capture()
            require((replay["inserted_count"], replay["already_observed_count"], replay["new_missing_sequences"],
                     replay["observed_through"], replay["replayed"]) == (0, 0, 0, 0, True),
                    "empty capture retry differs")
            require(replay["producer_epoch"] == first["producer_epoch"],
                    "empty capture retry changed its registered producer epoch")
            require(self._journal_counts() == (1, 0), "empty capture retry changed fact or producer count")

    def check_retry_and_concurrent_capture(self) -> None:
        with self.protected():
            before_metrics = self.o.metrics()
            before_journal = self._journal_counts()
            success = self.o.post("search", self.o.payload("search"))
            invalid = self.o.post("pack", {"task_request": ["invalid-journal-pack"]}, status=400)
            self.s.assert_error_code(invalid, "invalid-task-document")
            after_metrics = self.o.metrics()
            expected_count = after_metrics["retention"]["latest_sequence"] - before_metrics["retention"]["latest_sequence"]
            require(expected_count == 2, "journal retry fixture did not produce two measured observations")
            require(self._journal_counts() == before_journal,
                    "normal retrieval persisted journal rows without an explicit administrative capture")
            with ThreadPoolExecutor(max_workers=2) as executor:
                receipts = list(executor.map(lambda _: self.capture(), range(2)))
            require(sum(item["inserted_count"] for item in receipts) == expected_count and
                    sum(item["already_observed_count"] for item in receipts) == expected_count and
                    sum(item["new_missing_sequences"] for item in receipts) == 0 and
                    sorted(item["replayed"] for item in receipts) == [False, True],
                    "concurrent capture did not serialize exact insertion and retry accounting")
            instance = receipts[0]["instance_id"]
            require(all(item["instance_id"] == instance for item in receipts), "concurrent capture crossed process instances")
            epoch = receipts[0]["producer_epoch"]
            require(all(item["producer_epoch"] == epoch for item in receipts),
                    "concurrent capture crossed registered producer epochs")
            producer = self._producer(instance)
            require((producer["observed_through"], producer["captured_count"], producer["missing_count"]) ==
                    (after_metrics["retention"]["latest_sequence"], expected_count, 0),
                    "concurrent capture producer progress differs")
            require(producer["producer_epoch"] == epoch, "capture receipt epoch differs from stored producer identity")
            facts = [item for item in self.history()["items"] if item["instance_id"] == instance]
            require(len(facts) == expected_count and len({item["sequence"] for item in facts}) == expected_count and
                    all(item["request_fingerprint"] is None and item["producer_epoch"] == epoch for item in facts),
                    "capture retry duplicated, crossed epochs, or exposed default request facts")
            summary = self.report(SUMMARY, PROJECT_ID)
            invalid_pack = [group for group in summary["groups"] if group["operation"] == "pack" and group["errors"] == 1]
            require(len(invalid_pack) == 1 and invalid_pack[0]["known_pack_count"] == 0 and
                    invalid_pack[0]["incomplete_pack_count"] == 0,
                    "invalid Pack was incorrectly reported as a known incomplete Pack")
            retry = self.capture()
            require((retry["inserted_count"], retry["already_observed_count"], retry["new_missing_sequences"], retry["replayed"]) ==
                    (0, expected_count, 0, True), "post-concurrency capture retry differs")
            require(success["result"]["project_id"] == PROJECT_ID, "journal fixture changed retrieval response")

    @contextmanager
    def controlled_facts(self) -> Iterator[None]:
        instance = "c" * 64
        request_fingerprint = "d" * 64
        before = self._journal_counts()
        epoch = self._fixture_epochs(1)[0]
        producer = ("INSERT INTO shin_server.retrieval_journal_producers(instance_id,producer_epoch,source_host_id,server_version,core_version,"
                    "observed_through,captured_count,missing_count) VALUES "
                    f"('{instance}',{epoch},'fixture-host','server-fixture','core-fixture',5,5,0)")
        rows = (
            "INSERT INTO shin_server.retrieval_journal_attempts("
            "instance_id,producer_epoch,sequence,record_hash,project_id,operation,selector_kind,selector_id,observed_at_unix_ms,status,reason_code,"
            "last_phase,elapsed_ns,result_bytes,validation_ns,selection_ns,admission_ns,execution_ns,finalization_ns,route_id,workspace_id,"
            "source_host_id,snapshot_id,preparation_id,revision,profile_id,profile_fingerprint,route_fingerprint,retrieval_mode,returned_records,"
            "candidate_rows,covered_groups,total_groups,pack_complete,pack_coverage_complete,required_requirements,satisfied_required_requirements,"
            "query_capture,request_fingerprint,request) VALUES "
            f"('{instance}',{epoch},1,repeat('1',64),'other','search','publication','shared',1000,200,NULL,'validation',10,NULL,10,NULL,NULL,NULL,NULL,"
            f"'primary-route','primary-workspace','fixture-host','fnv1a64:0123456789abcdef',9,3,'server-profile','fnv1a64:fedcba9876543210',"
            f"repeat('2',64),'strict',0,4,1,2,NULL,NULL,NULL,NULL,'captured','{request_fingerprint}','{{\"query\":\"fixture-report-query\"}}'::jsonb),"
            f"('{instance}',{epoch},2,repeat('3',64),'other','search','publication','shared',1001,200,NULL,'validation',30,NULL,30,NULL,NULL,NULL,NULL,"
            f"'primary-route','primary-workspace','fixture-host','fnv1a64:0123456789abcdef',9,3,'server-profile','fnv1a64:fedcba9876543210',"
            f"repeat('2',64),'strict',2,4,1,2,NULL,NULL,NULL,NULL,'captured','{request_fingerprint}','{{\"query\":\"fixture-report-query\"}}'::jsonb),"
            f"('{instance}',{epoch},3,repeat('4',64),'other','brief','publication','shared',1002,409,'selection-failed','validation',50,NULL,50,NULL,NULL,NULL,NULL,"
            "NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,'disabled',NULL,NULL),"
            f"('{instance}',{epoch},4,repeat('5',64),'other','pack','publication','shared',1003,200,NULL,'validation',40,NULL,40,NULL,NULL,NULL,NULL,"
            f"'primary-route','primary-workspace','fixture-host','fnv1a64:0123456789abcdef',9,3,'server-profile','fnv1a64:fedcba9876543210',"
            "repeat('2',64),'strict',0,4,1,2,false,true,3,2,'disabled',NULL,NULL),"
            f"('{instance}',{epoch},5,repeat('6',64),'other','pack','publication','shared',1004,200,NULL,'validation',60,NULL,60,NULL,NULL,NULL,NULL,"
            f"'primary-route','primary-workspace','fixture-host','fnv1a64:0123456789abcdef',9,3,'server-profile','fnv1a64:fedcba9876543210',"
            "repeat('2',64),'strict',NULL,4,1,2,NULL,NULL,NULL,NULL,'disabled',NULL,NULL)"
        )
        self.s.psql(producer + ";" + rows)
        try:
            yield
        finally:
            self.s.psql("DELETE FROM shin_server.retrieval_journal_attempts WHERE instance_id='" + instance + "';"
                        "DELETE FROM shin_server.retrieval_journal_producers WHERE instance_id='" + instance + "'")
            self._restore_fixture_lifecycle()
            require(self._journal_counts() == before, "controlled journal fixture did not clean up exactly")

    def check_controlled_metric_cohorts(self) -> None:
        with self.protected(), self.controlled_facts():
            before = self.s.durable_state()
            summary = self.report(SUMMARY, "other")
            groups = {group["operation"]: group for group in summary["groups"]}
            require(set(groups) == {"search", "brief", "pack"} and summary["project_attempts"] == 5 and
                    summary["project_captured_requests"] == 2, "controlled summary cohort inventory differs")
            require(tuple(summary[key] for key in ("project_successes", "project_errors", "project_known_packs",
                    "project_incomplete_packs", "project_known_results", "project_zero_results")) == (4, 1, 1, 1, 3, 2),
                    "project summary totals differ from the controlled population")
            expected = {
                "search": (2, 2, 0, 40, self._p95([10, 30]), None, 0, 0, 2, 1),
                "brief": (1, 0, 1, 50, None, self._p95([50]), 0, 0, 0, 0),
                "pack": (2, 2, 0, 100, self._p95([40, 60]), None, 1, 1, 1, 1),
            }
            for operation, values in expected.items():
                group = groups[operation]
                require((group["attempts"], group["successes"], group["errors"], group["elapsed_total_ns"],
                         group["success_p95_ns"], group["error_p95_ns"], group["known_pack_count"],
                         group["incomplete_pack_count"], group["known_result_count"], group["zero_result_count"]) == values,
                        "controlled metric aggregate differs for " + operation)
                require(group["server_version"] == "server-fixture" and group["core_version"] == "core-fixture", "group lost producer versions")
                require(group["validation_total_ns"] == group["elapsed_total_ns"] and
                        all(group[phase + "_count"] == 0 and group[phase + "_total_ns"] is None
                            for phase in ("selection", "admission", "execution", "finalization")),
                        "controlled unknown phase observations were converted into zero-duration observations")
                if operation != "brief":
                    require(group["profile_fingerprint"] == "fnv1a64:fedcba9876543210" and
                            group["snapshot_id"] == "fnv1a64:0123456789abcdef", "group lost profile/snapshot provenance")
            queries = self.report(QUERIES, "other")
            require(len(queries["groups"]) == 1 and queries["groups"][0]["attempts"] == 2 and
                    queries["groups"][0]["request_fingerprint"] == "d" * 64, "exact captured payload group differs")
            exemplar = self.detail(queries["groups"][0]["example_attempt_id"])
            require(exemplar["request_fingerprint"] == queries["groups"][0]["request_fingerprint"] and
                    exemplar["project_id"] == "other" and exemplar["operation"] == "search",
                    "query report example does not belong to its group")
            detail = self.detail(self.history(project="other")["items"][0]["id"])
            require(detail["request"] == {"query": "fixture-report-query"}, "detail did not retain explicit captured request")
            no_private_values(summary, [JOURNAL_HEADER_MARKER, "fixture-report-query"])
            no_private_values(queries, [JOURNAL_HEADER_MARKER, "fixture-report-query"])
            require(self.s.durable_state() == before, "journal analytical reads mutated controlled facts")

    def check_opt_in_exact_payloads(self) -> None:
        with self.protected(), self.o.configured(16, 8192, [PROJECT_ID]):
            payload = self.o.payload("search")
            self.o.post("search", payload)
            self.o.post("search", payload)
            excluded = self.o.post("search", {"query": JOURNAL_QUERY_MARKER, "require_current": False}, selector="route",
                                   project="other", name="other-retrieval", role="admin", status=503)
            self.s.assert_error_code(excluded, "active-generation-missing")
            receipt = self.capture(header_marker=True)
            require((receipt["inserted_count"], receipt["already_observed_count"], receipt["new_missing_sequences"]) == (3, 0, 0),
                    "opt-in capture did not persist the expected current process population")
            visible = [item for item in self.history(project=PROJECT_ID)["items"] if item["instance_id"] == receipt["instance_id"]]
            captured = [self.detail(item["id"]) for item in visible]
            matching = [item for item in captured if item["request"] == payload]
            require(len(matching) == 2 and len({item["request_fingerprint"] for item in matching}) == 1 and
                    SHA256.fullmatch(matching[0]["request_fingerprint"]) is not None,
                    "repeated exact opt-in payloads lost their common fingerprint")
            metadata = self.history(project=PROJECT_ID)
            summary = self.report(SUMMARY, PROJECT_ID)
            queries = self.report(QUERIES, PROJECT_ID)
            fingerprint = matching[0]["request_fingerprint"]
            query_groups = [group for group in queries["groups"] if group["request_fingerprint"] == fingerprint]
            require(len(query_groups) == 1 and query_groups[0]["attempts"] == 2, "exact payload repetitions did not group")
            no_private_values(metadata, [JOURNAL_HEADER_MARKER, JOURNAL_QUERY_MARKER])
            no_private_values(summary, [JOURNAL_HEADER_MARKER, JOURNAL_QUERY_MARKER])
            no_private_values(queries, [JOURNAL_HEADER_MARKER, JOURNAL_QUERY_MARKER])
            other = [item for item in self.history(project="other")["items"] if item["instance_id"] == receipt["instance_id"]]
            require(len(other) == 1 and self.detail(other[0]["id"])["request"] is None and other[0]["request_fingerprint"] is None,
                    "capture allowlist persisted excluded project request identity")
            excluded_queries = self.report(QUERIES, "other")
            require(excluded_queries["groups"] == [] and excluded_queries["project_attempts"] == 1 and
                    excluded_queries["project_captured_requests"] == 0, "excluded population received synthetic query identity")

    def check_jsonb_request_boundary(self) -> None:
        with self.protected(), self.o.configured(8, 8192, [PROJECT_ID]):
            nul_payload = self.o.payload("pack")
            nul_payload["task_request"]["task"]["objective"] = "neutral\x00objective"
            self.o.post("pack", nul_payload)
            literal_payload = self.o.payload("pack")
            literal_payload["task_request"]["task"]["objective"] = "literal \\u0000 text"
            self.o.post("pack", literal_payload)
            receipt = self.capture()
            require(receipt["inserted_count"] == 2 and receipt["new_missing_sequences"] == 0,
                    "one JSONB-incompatible body prevented metadata capture")
            facts = [self.detail(item["id"]) for item in self.history(project=PROJECT_ID)["items"]
                     if item["instance_id"] == receipt["instance_id"]]
            require(len(facts) == 2 and all(item["status"] == 200 for item in facts), "request storage changed service outcomes")
            require(facts[0]["query_capture"] == "capture-failed" and facts[0]["request"] is None and
                    facts[0]["request_fingerprint"] is None, "unrepresentable body was silently rewritten or hashed")
            require(facts[1]["query_capture"] == "captured" and facts[1]["request"] == literal_payload,
                    "literal escape text was confused with an actual NUL character")
            replay = self.capture()
            require(replay["inserted_count"] == 0 and replay["already_observed_count"] == 2,
                    "JSONB omission changed retry identity")

    def check_bounded_cohorts_full_totals(self) -> None:
        instance = "f" * 64
        with self.protected():
            counts = self._journal_counts()
            epoch = self._fixture_epochs(1)[0]
            self.s.psql("INSERT INTO shin_server.retrieval_journal_producers(instance_id,producer_epoch,source_host_id,server_version,core_version,"
                        "observed_through,captured_count,missing_count) VALUES "
                        f"('{instance}',{epoch},'fixture-host','server-fixture','core-fixture',34,34,0);"
                        "INSERT INTO shin_server.retrieval_journal_attempts(instance_id,producer_epoch,sequence,record_hash,project_id,operation,"
                        "selector_kind,selector_id,observed_at_unix_ms,status,reason_code,last_phase,elapsed_ns,validation_ns,query_capture) "
                        f"SELECT '{instance}',{epoch},n,repeat('f',64),'other','search','route','bounded-'||n,n,400,'fixture-error','validation',n,n,'disabled' "
                        "FROM generate_series(1,34) AS n")
            try:
                summary = self.report(SUMMARY, "other")
                require((len(summary["groups"]), summary["total_groups"], summary["omitted_groups"]) == (32, 34, 2),
                        "bounded report lost omitted cohort accounting")
                require(summary["project_attempts"] == summary["project_errors"] == 34 and
                        sum(group["errors"] for group in summary["groups"]) == 32,
                        "project totals were incorrectly derived from the truncated cohort list")
            finally:
                self.s.psql(f"DELETE FROM shin_server.retrieval_journal_attempts WHERE instance_id='{instance}';"
                            f"DELETE FROM shin_server.retrieval_journal_producers WHERE instance_id='{instance}'")
                self._restore_fixture_lifecycle()
            require(self._journal_counts() == counts, "bounded cohort fixture did not clean up")

    def check_eviction_gap_and_retry(self) -> None:
        with self.protected(), self.o.configured(3, 4096, []):
            for _ in range(5):
                self.o.post("search", self.o.payload("search"))
            metrics = self.o.metrics()
            require((metrics["retention"]["latest_sequence"], metrics["retention"]["retained_records"], metrics["retention"]["evicted"]) ==
                    (5, 3, 2), "eviction journal fixture did not produce the expected bounded native snapshot")
            receipt = self.capture()
            require((receipt["inserted_count"], receipt["already_observed_count"], receipt["new_missing_sequences"],
                     receipt["observed_through"], receipt["replayed"]) == (3, 0, 2, 5, False),
                    "journal capture did not account for native eviction source gap")
            producer = self._producer(receipt["instance_id"])
            require((producer["observed_through"], producer["captured_count"], producer["missing_count"]) == (5, 3, 2),
                    "journal producer did not preserve native snapshot gap accounting")
            facts = [item for item in self.history(project=PROJECT_ID)["items"] if item["instance_id"] == receipt["instance_id"]]
            require([item["sequence"] for item in facts] == [3, 4, 5], "journal persisted an inconsistent evicted source suffix")
            replay = self.capture()
            require((replay["inserted_count"], replay["already_observed_count"], replay["new_missing_sequences"], replay["replayed"]) ==
                    (0, 3, 0, True), "evicted snapshot retry changed progress or duplicated facts")
            durable_ids = [item["id"] for item in facts]
            self.s.stop_server(expect_clean=True)
            self.s.start_server()
            restored = [item for item in self.history(project=PROJECT_ID)["items"] if item["id"] in durable_ids]
            require(restored == facts and self._producer(receipt["instance_id"]) == producer and
                    self.o.metrics()["retention"]["latest_sequence"] == 0,
                    "journal facts or producer progress did not survive process restart")

    def check_read_bounds_and_admin_guards(self) -> None:
        with self.protected():
            before_metrics = self.o.metrics()
            before = self.s.durable_state()
            for endpoint in (JOURNAL, SUMMARY + "?project_id=" + PROJECT_ID, QUERIES + "?project_id=" + PROJECT_ID, ATTEMPTS + "1"):
                self.s.expect_status(403, "GET", endpoint, headers=self.s.auth_headers("reader"))
                self.s.expect_status(401, "GET", endpoint)
            self.capture(role="reader", expected=403)
            self.capture(role="admin", suffix="?unexpected=1", expected=400)
            for body in ([], {"unexpected": True}, b"not-json"):
                headers = {**self.s.auth_headers("admin"), "Content-Type": "application/json"}
                self.s.expect_status(400, "POST", CAPTURE, headers=headers,
                                     body=body if isinstance(body, bytes) else self._json_body(body))
            for query in ("limit=0", "limit=33", "after=-1", "after=9223372036854775808", "after=1&after=2", "unknown=1",
                          "project_id=Invalid"):
                self.s.expect_status(400, "GET", JOURNAL + "?" + query, headers=self.s.auth_headers("admin"))
            for endpoint in (SUMMARY, QUERIES):
                for query in ("", "project_id=Invalid", "project_id=" + PROJECT_ID + "&unexpected=1",
                              "project_id=" + PROJECT_ID + "&project_id=other"):
                    self.s.expect_status(400, "GET", endpoint + ("?" + query if query else ""), headers=self.s.auth_headers("admin"))
            for identifier, expected in (("0", 404), ("9223372036854775808", 400), ("not-an-id", 404)):
                self.s.expect_status(expected, "GET", ATTEMPTS + identifier, headers=self.s.auth_headers("admin"))
            page = self.history(limit=1)
            require(len(page["items"]) <= 1, "journal page ignored maximum one-item bound")
            require(self.s.durable_state() == before and self.o.metrics() == before_metrics,
                    "journal reads or refused requests changed durable state or telemetry")

    @contextmanager
    def failing_attempt_insert(self) -> Iterator[None]:
        name = "retrieval_journal_fail_" + secrets.token_hex(6)
        self.s.psql(f"CREATE FUNCTION shin_server.{name}() RETURNS trigger LANGUAGE plpgsql AS $$ "
                    "BEGIN RAISE EXCEPTION 'controlled journal attempt failure'; END $$; "
                    f"CREATE TRIGGER {name} BEFORE INSERT ON shin_server.retrieval_journal_attempts "
                    f"FOR EACH ROW EXECUTE FUNCTION shin_server.{name}()")
        try:
            yield
        finally:
            self.s.psql(f"DROP TRIGGER IF EXISTS {name} ON shin_server.retrieval_journal_attempts; "
                        f"DROP FUNCTION IF EXISTS shin_server.{name}()")

    def check_transaction_rollback_and_recovery(self) -> None:
        with self.protected(), self.o.configured(8, 4096, []):
            self.o.post("search", self.o.payload("search"))
            instance = self.o.metrics()["instance_id"]
            before = self.s.durable_state()
            with self.failing_attempt_insert():
                status, _ = self.s.request("POST", CAPTURE, headers={**self.s.auth_headers("admin"), "Content-Type": "application/json"},
                                           body=self._json_body({}))
                require(status >= 500, "controlled attempt insert trigger did not abort journal capture")
                admitted = self._producer(instance)
                after = self.s.durable_state()
                lifecycle = before["retrieval_journal_lifecycle"][0]
                require(admitted["producer_epoch"] == lifecycle["allocated_through"] + 1 and
                        all(admitted[key] == 0 for key in ("observed_through", "captured_count", "missing_count",
                            "retired_through", "retired_count", "retired_missing_count")) and admitted["last_retention"] is None,
                        "failed first capture did not preserve only an empty producer admission")
                require(after["retrieval_journal_lifecycle"] ==
                        [{**lifecycle, "allocated_through": admitted["producer_epoch"]}] and
                        [row for row in after["retrieval_journal_producers"] if row["instance_id"] != instance] ==
                        before["retrieval_journal_producers"] and
                        len(after["retrieval_journal_producers"]) == len(before["retrieval_journal_producers"]) + 1,
                        "failed first capture changed unrelated producer or lifecycle state")
                require(all(after[key] == value for key, value in before.items()
                            if key not in ("retrieval_journal_lifecycle", "retrieval_journal_producers")),
                        "failed first capture committed facts or unrelated durable state")
                status, _ = self.s.request("POST", CAPTURE, headers={**self.s.auth_headers("admin"), "Content-Type": "application/json"},
                                           body=self._json_body({}))
                require(status >= 500 and self.s.durable_state() == after,
                        "failed capture retry committed progress or allocated a second epoch")
            recovered = self.capture()
            require((recovered["inserted_count"], recovered["already_observed_count"], recovered["new_missing_sequences"], recovered["replayed"]) ==
                    (1, 0, 0, False), "journal capture did not recover after rolled-back SQL failure")
            require(recovered["producer_epoch"] == admitted["producer_epoch"],
                    "capture recovery changed the acknowledged lifetime epoch")

    def _insert_producers(self, instances: list[str], *, prefix: str) -> None:
        if not instances:
            return
        epochs = self._fixture_epochs(len(instances))
        values = ",".join(f"('{instance}',{epoch},'{prefix}-host','{prefix}-server','{prefix}-core',0,0,0)"
                          for instance, epoch in zip(instances, epochs))
        self.s.psql("INSERT INTO shin_server.retrieval_journal_producers(instance_id,producer_epoch,source_host_id,server_version,core_version,"
                    "observed_through,captured_count,missing_count) VALUES " + values)

    def check_out_of_order_capture(self) -> None:
        instance = secrets.token_hex(32)
        probe = self.s.binary.parent / "shin-server-retrieval-journal-store-probe"
        require(probe.is_file(), "native journal Store probe is unavailable")
        with self.protected():
            counts = self._journal_counts()
            try:
                completed = self.s.run_process([str(probe), str(self.s.runtime_config_path), instance],
                                               "native out-of-order journal probe")
                result = json.loads(completed.stdout)
                require(result == {"ok": True, "instance_id": instance, "synthetic": True}, "native Store probe did not complete")
                require(self.s.scalar("SELECT observed_through||':'||captured_count||':'||missing_count "
                        "FROM shin_server.retrieval_journal_producers WHERE instance_id=" + self._sql_text(instance)) == "6:5:1",
                        "out-of-order batches did not preserve exact missing-prefix accounting")
                require(self.s.scalar("SELECT string_agg(sequence::text,',' ORDER BY sequence) FROM shin_server.retrieval_journal_attempts "
                        "WHERE instance_id=" + self._sql_text(instance)) == "1,2,3,4,6", "late facts were lost or duplicated")
            finally:
                self.s.psql("DELETE FROM shin_server.retrieval_journal_attempts WHERE instance_id=" + self._sql_text(instance) + ";"
                            "DELETE FROM shin_server.retrieval_journal_producers WHERE instance_id=" + self._sql_text(instance))
                self._restore_fixture_lifecycle()
            require(self._journal_counts() == counts, "native probe did not clean up")

    def check_synthetic_capacity_and_cleanup(self) -> None:
        with self.protected():
            producers_before, facts_before = self._journal_counts()
            producer_instances = [secrets.token_hex(32) for _ in range(max(0, 128 - producers_before))]
            self._insert_producers(producer_instances, prefix="producer-capacity")
            try:
                with self.o.configured(8, 4096, []):
                    self.o.post("search", self.o.payload("search"))
                    failed = self.capture(expected=507)
                    self.s.assert_error_code(failed, "journal-capacity")
                    require(self._journal_counts() == (128, facts_before), "producer capacity failure partially persisted capture")
            finally:
                if producer_instances:
                    self.s.psql("DELETE FROM shin_server.retrieval_journal_producers WHERE instance_id IN (" +
                                ",".join(self._sql_text(value) for value in producer_instances) + ")")
                self._restore_fixture_lifecycle()
            require(self._journal_counts() == (producers_before, facts_before), "producer capacity fixture cleanup differs")

            self.capture()
            producers_before, facts_before = self._journal_counts()
            free_facts = 8192 - facts_before
            instance = secrets.token_hex(32)
            self._insert_producers([instance], prefix="fact-capacity")
            try:
                if free_facts:
                    self.s.psql(
                        "INSERT INTO shin_server.retrieval_journal_attempts("
                        "instance_id,producer_epoch,sequence,record_hash,project_id,operation,selector_kind,selector_id,observed_at_unix_ms,status,reason_code,"
                        "last_phase,elapsed_ns,result_bytes,validation_ns,selection_ns,admission_ns,execution_ns,finalization_ns,route_id,workspace_id,"
                        "source_host_id,snapshot_id,preparation_id,revision,profile_id,profile_fingerprint,route_fingerprint,retrieval_mode,returned_records,"
                        "candidate_rows,covered_groups,total_groups,pack_complete,pack_coverage_complete,required_requirements,satisfied_required_requirements,"
                        "query_capture,request_fingerprint,request) "
                        f"SELECT '{instance}',(SELECT producer_epoch FROM shin_server.retrieval_journal_producers WHERE instance_id='{instance}'),n,repeat('e',64),'visible','search','route','capacity-route',n,200,NULL,'validation',1,NULL,1,NULL,NULL,NULL,NULL,"
                        "NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,'disabled',NULL,NULL "
                        f"FROM generate_series(1,{free_facts}) AS n")
                    self.s.psql("UPDATE shin_server.retrieval_journal_producers SET observed_through=" + str(free_facts) +
                                ",captured_count=" + str(free_facts) + " WHERE instance_id=" + self._sql_text(instance))
                self.o.post("search", self.o.payload("search"))
                before = self.s.durable_state()
                failed = self.capture(expected=507)
                self.s.assert_error_code(failed, "journal-capacity")
                require(self._journal_counts()[1] == 8192 and self.s.durable_state() == before,
                        "fact capacity failure partially persisted capture")
            finally:
                self.s.psql("DELETE FROM shin_server.retrieval_journal_attempts WHERE instance_id=" + self._sql_text(instance) + ";"
                            "DELETE FROM shin_server.retrieval_journal_producers WHERE instance_id=" + self._sql_text(instance))
                self._restore_fixture_lifecycle()
            require(self._journal_counts() == (producers_before, facts_before), "fact capacity fixture cleanup differs")
