"""P3E20 opt-in retrieval-journal writer checks on the shared integration stand.

The parent publication suite owns stand setup and teardown.  This sidecar only
changes its private runtime configuration through the suite helper, captures
facts through the existing administrative surface, and uses short-lived SQL
triggers to make asynchronous failure paths deterministic.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from copy import deepcopy
import json
import statistics
import time
from typing import Any, Iterator

from publication_test_support import exact_object, integer, require
from retrieval_integration_test import PROJECT_ID
from retrieval_journal_checks import RetrievalJournalChecks
from retrieval_journal_epoch_checks import RetrievalJournalEpochChecks
from retrieval_observability_checks import RetrievalObservabilityChecks


WRITER = "/api/v1/telemetry/retrieval/journal/writer"
STATUS_FIELDS = {
    "schema_version", "enabled", "state", "scope", "read_only", "automatic_retention",
    "pending_batches", "pending_records", "pending_bytes", "pending_through",
    "maximum_batch_records", "maximum_batch_bytes", "sampled_through", "acknowledged_through",
    "unacknowledged_sequences_at_last_poll", "source_progress_is_live", "acknowledgement_includes_missing",
    "source_sample_age_ms", "acknowledged_producer_epoch", "snapshot_polls", "export_attempts", "export_successes",
    "failures", "consecutive_failures", "inserted_records", "new_missing_sequences",
    "recovered_missing_sequences", "next_attempt_in_ms", "last_failure", "shutdown_prefix_acknowledged",
    "all_traffic_complete",
}
DEFAULT_POLICY = {
    "enabled": False,
    "principal_id": "",
    "interval_ms": 1000,
    "max_retry_ms": 30000,
    "flush_on_shutdown": True,
}


class JournalWriterChecks:
    """Real HTTP/SQL checks for the bounded automatic capture sidecar."""

    def __init__(self, suite: Any) -> None:
        self.s = suite
        self.journal = RetrievalJournalChecks(suite)
        self.observability = RetrievalObservabilityChecks(suite)
        self.measurement: dict[str, Any] = {"status": "not-run"}
        self.s.journal_writer_measurement = self.measurement

    def run(self) -> None:
        for name, check in (
            ("journal-writer-disabled-and-status-guards", self.check_disabled_and_status_guards),
            ("journal-writer-enabled-metadata-and-manual-overlap", self.check_enabled_metadata_and_manual_overlap),
            ("journal-writer-retry-gap-and-reclaim-refusal", self.check_retry_gap_and_reclaim_refusal),
            ("journal-writer-shutdown-flush-boundaries", self.check_shutdown_flush_boundaries),
        ):
            self.s.record(name, check)
        if self.s.load_enabled:
            self.s.record("journal-writer-optional-bounded-comparison", self.check_optional_bounded_comparison)
        self.s.journal_writer_measurement = self.measurement

    def status(self, *, expected: int = 200, role: str = "admin", suffix: str = "") -> dict[str, Any]:
        response = self.s.expect_status(expected, "GET", WRITER + suffix, headers=self.s.auth_headers(role))
        if expected != 200:
            return response
        status = exact_object(response["result"], STATUS_FIELDS, "journal writer status")
        require(status["schema_version"] == "shin.journal-writer-status.v1" and
                status["scope"] == "process-lifetime" and status["read_only"] is True and
                status["automatic_retention"] is False and status["source_progress_is_live"] is False and
                status["acknowledgement_includes_missing"] is True and status["all_traffic_complete"] is False,
                "journal writer status scope differs")
        require(type(status["enabled"]) is bool and status["state"] in {
            "disabled", "not-started", "waiting", "exporting", "retry-wait", "blocked", "stopped",
        }, "journal writer state differs")
        for key in (
            "pending_batches", "pending_records", "pending_bytes", "pending_through", "maximum_batch_records",
            "maximum_batch_bytes", "sampled_through", "acknowledged_through",
            "unacknowledged_sequences_at_last_poll", "snapshot_polls", "export_attempts", "export_successes",
            "failures", "consecutive_failures", "inserted_records", "new_missing_sequences",
            "recovered_missing_sequences",
        ):
            integer(status[key], 0, "journal writer " + key)
        require(status["maximum_batch_records"] == 256 and status["maximum_batch_bytes"] == 4 * 1024 * 1024 and
                status["pending_batches"] in (0, 1) and
                (status["pending_batches"] == 0) == (status["pending_records"] == status["pending_bytes"] == status["pending_through"] == 0) and
                status["unacknowledged_sequences_at_last_poll"] == max(0, status["sampled_through"] - status["acknowledged_through"]) and
                status["export_successes"] <= status["export_attempts"] and
                status["consecutive_failures"] <= status["failures"], "journal writer progress bounds differ")
        for key, minimum in (("source_sample_age_ms", 0), ("acknowledged_producer_epoch", 1), ("next_attempt_in_ms", 0)):
            if status[key] is not None:
                integer(status[key], minimum, "journal writer " + key)
        require(type(status["shutdown_prefix_acknowledged"]) is bool, "journal shutdown acknowledgement is not boolean")
        if status["last_failure"] is None:
            require(status["failures"] == 0 or status["consecutive_failures"] == 0,
                    "journal writer cleared a live failure incorrectly")
        else:
            failure = exact_object(status["last_failure"], {"code", "stage"}, "journal writer failure")
            require(isinstance(failure["code"], str) and failure["code"] and failure["stage"] in {"snapshot", "export", "receipt"},
                    "journal writer failure metadata differs")
        return status

    def _await(self, predicate: Any, message: str, *, timeout: float = 12.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        latest: dict[str, Any] | None = None
        while time.monotonic() < deadline:
            latest = self.status()
            if predicate(latest):
                return latest
            time.sleep(0.025)
        raise RuntimeError(message + ("; last status=" + json.dumps(latest, sort_keys=True) if latest else ""))

    @contextmanager
    def configured(self, *, enabled: bool, retained: int, interval_ms: int, max_retry_ms: int,
                   flush_on_shutdown: bool) -> Iterator[None]:
        path = self.s.runtime_config_path
        original_bytes = path.read_bytes()
        original = json.loads(original_bytes)
        require(original["journal_writer"] == DEFAULT_POLICY,
                "journal writer sidecar requires the exact disabled fixture policy")
        changed = deepcopy(original)
        changed["observability"] = {
            "max_retained_requests": retained,
            "max_query_bytes": original["observability"]["max_query_bytes"],
            "capture_query_projects": [],
        }
        changed["journal_writer"] = {
            "enabled": enabled,
            "principal_id": "dev-admin" if enabled else "",
            "interval_ms": interval_ms,
            "max_retry_ms": max_retry_ms,
            "flush_on_shutdown": flush_on_shutdown,
        }
        identity = self.s.server_id
        self.s.stop_server(expect_clean=True)
        try:
            self.s.replace_private_json(path, changed)
            self.s.start_server()
            require(self.s.server_id == identity, "journal writer config restart changed Server identity")
            yield
        finally:
            self.s.stop_server(expect_clean=False)
            self.s.replace_private_json(path, original)
            require(path.read_bytes() == original_bytes, "journal writer config restore changed fixture bytes")
            self.s.start_server()
            require(self.s.server_id == identity, "journal writer config restore changed Server identity")

    def _current_rows(self, instance: str) -> dict[str, Any]:
        return self.s.sql_json(
            "SELECT jsonb_build_object("
            "'producer', (SELECT to_jsonb(p) FROM shin_server.retrieval_journal_producers p WHERE p.instance_id=" +
            self.journal._sql_text(instance) + "),"
            "'sequences', (SELECT COALESCE(jsonb_agg(a.sequence ORDER BY a.sequence), '[]'::jsonb) "
            "FROM shin_server.retrieval_journal_attempts a WHERE a.instance_id=" + self.journal._sql_text(instance) + "),"
            "'duplicates', (SELECT COALESCE(jsonb_agg(sequence ORDER BY sequence), '[]'::jsonb) FROM "
            "(SELECT sequence FROM shin_server.retrieval_journal_attempts WHERE instance_id=" + self.journal._sql_text(instance) +
            " GROUP BY sequence HAVING count(*) > 1) d))")

    def _post_search(self) -> dict[str, Any]:
        return self.observability.post("search", self.observability.payload("search"))

    def _writer_log_status(self, path: Any) -> dict[str, Any]:
        require(path is not None and path.is_file(), "journal writer shutdown log is unavailable")
        events = []
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if value.get("event") == "journal-writer-stopped":
                events.append(value)
        require(len(events) == 1, "journal writer shutdown did not emit exactly one status event")
        return self._status_value(events[0].get("status"), "journal writer shutdown status")

    def _status_value(self, value: Any, label: str) -> dict[str, Any]:
        # Reuse the same exact schema validation for an HTTP result without another GET.
        status = exact_object(value, STATUS_FIELDS, label)
        require(status["schema_version"] == "shin.journal-writer-status.v1" and status["state"] == "stopped",
                "journal writer shutdown status differs")
        return status

    def _await_empty_poll(self) -> dict[str, Any]:
        return self._await(lambda value: value["snapshot_polls"] >= 1 and value["sampled_through"] == 0 and
                           value["export_attempts"] == 0 and value["acknowledged_producer_epoch"] is None,
                           "journal writer did not finish its initial empty poll")

    def check_disabled_and_status_guards(self) -> None:
        with self.journal.protected(), self.configured(enabled=False, retained=128, interval_ms=1000,
                                                      max_retry_ms=30000, flush_on_shutdown=True):
            before = self.s.durable_state()
            self.s.expect_status(401, "GET", WRITER)
            denied = self.status(expected=403, role="reader")
            self.s.assert_error_code(denied, "forbidden")
            invalid = self.status(expected=400, suffix="?unexpected=1")
            self.s.assert_error_code(invalid, "invalid-query")
            current = self.observability.metrics()["instance_id"]
            status = self.status()
            require(status["enabled"] is False and status["state"] == "disabled" and status["snapshot_polls"] == 0 and
                    status["export_attempts"] == 0 and status["acknowledged_producer_epoch"] is None,
                    "disabled journal writer began background work")
            self._post_search()
            self.s.expect_status(200, "GET", "/api/v1/telemetry/retrieval", headers=self.s.auth_headers("admin"))
            time.sleep(0.15)
            after = self.status()
            require(after["snapshot_polls"] == after["export_attempts"] == after["export_successes"] == 0 and
                    self._current_rows(current)["producer"] is None and self._current_rows(current)["sequences"] == [],
                    "disabled journal writer enrolled or captured normal reads")
            require(self.s.durable_state() == before,
                    "disabled journal writer status/read checks changed durable state")

    def check_enabled_metadata_and_manual_overlap(self) -> None:
        with self.journal.protected(), self.configured(enabled=True, retained=128, interval_ms=100,
                                                      max_retry_ms=400, flush_on_shutdown=True):
            self._await_empty_poll()
            before = self.observability.metrics()
            instance = before["instance_id"]
            responses = [self._post_search() for _ in range(2)]
            with ThreadPoolExecutor(max_workers=3) as executor:
                manual = executor.submit(self.journal.capture)
                more = list(executor.map(lambda _: self._post_search(), range(2)))
            receipt = manual.result()
            responses.extend(more)
            require(all(response["result"]["project_id"] == PROJECT_ID for response in responses),
                    "writer overlap changed foreground retrieval responses")
            latest = self.observability.metrics()["retention"]["latest_sequence"]
            status = self._await(lambda value: value["acknowledged_through"] >= latest and
                                 value["pending_batches"] == 0 and value["export_successes"] >= 1,
                                 "enabled journal writer did not acknowledge the current metrics prefix")
            rows = self._current_rows(instance)
            producer = rows["producer"]
            expected = list(range(before["retention"]["latest_sequence"] + 1, latest + 1))
            require(rows["sequences"] == expected and rows["duplicates"] == [] and producer is not None and
                    producer["observed_through"] == latest and producer["captured_count"] == len(expected) and
                    producer["missing_count"] == 0 and
                    producer["producer_epoch"] == status["acknowledged_producer_epoch"] and
                    status["inserted_records"] + receipt["inserted_count"] == len(expected) and receipt["observed_through"] <= latest,
                    "enabled writer/manual capture overlap duplicated or lost current-instance facts")
            metadata = self.s.sql_json("SELECT COALESCE(jsonb_agg(jsonb_build_object('sequence',sequence,'query_capture',query_capture,"
                                       "'request',request,'request_fingerprint',request_fingerprint) ORDER BY sequence), '[]'::jsonb) "
                                       "FROM shin_server.retrieval_journal_attempts WHERE instance_id=" + self.journal._sql_text(instance))
            require(all(item["query_capture"] == "disabled" and item["request"] is None and
                        item["request_fingerprint"] is None for item in metadata),
                    "metadata-only journal writer persisted a request payload")

    def check_retry_gap_and_reclaim_refusal(self) -> None:
        with self.journal.protected(), self.configured(enabled=True, retained=3, interval_ms=100,
                                                      max_retry_ms=400, flush_on_shutdown=True):
            self._await_empty_poll()
            instance = self.observability.metrics()["instance_id"]
            with self.journal.failing_attempt_insert():
                first = self._post_search()
                require(first["result"]["project_id"] == PROJECT_ID, "writer failure changed the first foreground response")
                failed = self._await(lambda value: value["state"] == "retry-wait" and value["pending_batches"] == 1 and
                                     value["pending_records"] == 1 and value["pending_through"] == 1 and
                                     value["failures"] >= 1,
                                     "journal writer did not retain the failed first batch")
                later = [self._post_search() for _ in range(8)]
                require(all(response["result"]["project_id"] == PROJECT_ID for response in later),
                        "journal writer failure changed a foreground response")
                frozen = self._await(lambda value: value["sampled_through"] == 1 and value["pending_batches"] == 1 and
                                     value["pending_records"] == 1 and value["pending_through"] == 1,
                                     "journal writer replaced the immutable failed batch")
                pending_producer = self._current_rows(instance)["producer"]
                require(frozen["acknowledged_producer_epoch"] is None and
                        failed["acknowledged_producer_epoch"] is None and pending_producer is not None and
                        pending_producer["producer_epoch"] > 0 and
                        (pending_producer["observed_through"], pending_producer["captured_count"],
                         pending_producer["missing_count"]) == (0, 0, 0),
                        "failed writer did not retain only the independently committed producer admission")
            recovered = self._await(lambda value: value["acknowledged_through"] == 9 and value["pending_batches"] == 0 and
                                    value["new_missing_sequences"] == 5 and value["export_successes"] >= 1,
                                    "journal writer did not recover the retained tail and its gap")
            rows = self._current_rows(instance)
            producer = rows["producer"]
            require(rows["sequences"] == [1, 7, 8, 9] and rows["duplicates"] == [] and producer is not None and
                    (producer["observed_through"], producer["captured_count"], producer["missing_count"]) == (9, 4, 5) and
                    producer["producer_epoch"] == recovered["acknowledged_producer_epoch"],
                    "writer recovery did not persist the exact retained prefix and known gap")
            reclamation = RetrievalJournalEpochChecks(self.s)
            reclamation.timings["reclamation-plan"] = []
            reclamation.timings["reclamation-apply"] = []
            reclamation._retire_and_reclaim_existing_prefix()
            require(self.journal._journal_counts() == (0, 0), "explicit journal reclamation did not remove the retired prefix")
            foreground = self._post_search()
            require(foreground["result"]["project_id"] == PROJECT_ID, "reclaimed writer blocked foreground retrieval")
            blocked = self._await(lambda value: value["state"] == "blocked" and value["last_failure"] is not None and
                                  value["last_failure"]["code"] == "journal-producer-reclaimed",
                                  "reclaimed writer did not enter terminal refusal")
            attempts = blocked["export_attempts"]
            time.sleep(0.25)
            terminal = self.status()
            require(terminal["export_attempts"] == attempts and self.journal._journal_counts() == (0, 0) and
                    self._current_rows(instance)["producer"] is None,
                    "reclaimed writer retried or reenrolled its producer")

    def check_shutdown_flush_boundaries(self) -> None:
        with self.journal.protected():
            self._check_shutdown_flush_true()
            self._check_shutdown_flush_false()
            self._check_shutdown_flush_failure()

    def _check_shutdown_flush_true(self) -> None:
        with self.configured(enabled=True, retained=128, interval_ms=60000, max_retry_ms=60000, flush_on_shutdown=True):
            self._await_empty_poll()
            instance = self.observability.metrics()["instance_id"]
            self._post_search()
            log = self.s.server_log
            self.s.stop_server(expect_clean=True)
            status = self._writer_log_status(log)
            rows = self._current_rows(instance)
            require(status["shutdown_prefix_acknowledged"] is True and status["acknowledged_through"] == 1 and
                    status["pending_batches"] == 0 and rows["sequences"] == [1] and rows["producer"] is not None and
                    rows["producer"]["observed_through"] == 1,
                    "flush-on-shutdown did not acknowledge and persist the pending prefix")

    def _check_shutdown_flush_false(self) -> None:
        with self.configured(enabled=True, retained=128, interval_ms=60000, max_retry_ms=60000, flush_on_shutdown=False):
            self._await_empty_poll()
            instance = self.observability.metrics()["instance_id"]
            self._post_search()
            log = self.s.server_log
            self.s.stop_server(expect_clean=True)
            status = self._writer_log_status(log)
            rows = self._current_rows(instance)
            require(status["shutdown_prefix_acknowledged"] is False and status["acknowledged_through"] == 0 and
                    status["pending_batches"] == 1 and status["pending_records"] == 1 and status["pending_through"] == 1 and
                    rows["producer"] is None and rows["sequences"] == [],
                    "flush-disabled shutdown exported or falsely acknowledged the pending prefix")

    def _check_shutdown_flush_failure(self) -> None:
        with self.configured(enabled=True, retained=128, interval_ms=60000, max_retry_ms=60000, flush_on_shutdown=True):
            self._await_empty_poll()
            instance = self.observability.metrics()["instance_id"]
            self._post_search()
            log = self.s.server_log
            with self.journal.failing_attempt_insert():
                self.s.stop_server(expect_clean=True)
            status = self._writer_log_status(log)
            rendered = log.read_text(encoding="utf-8")
            rows = self._current_rows(instance)
            require(status["shutdown_prefix_acknowledged"] is False and status["last_failure"] is not None and
                    status["last_failure"]["stage"] == "export" and status["inserted_records"] == 0 and
                    rows["sequences"] == [] and "controlled journal attempt failure" not in rendered,
                    "failed shutdown flush claimed success or leaked SQL error text")

    def check_optional_bounded_comparison(self) -> None:
        samples: dict[str, list[float]] = {}
        for label, enabled in (("disabled", False), ("enabled", True)):
            with self.journal.protected(), self.configured(enabled=enabled, retained=128, interval_ms=100,
                                                          max_retry_ms=400, flush_on_shutdown=True):
                if enabled:
                    self._await_empty_poll()
                before = self.observability.metrics()["retention"]["latest_sequence"]

                def request(_: int) -> float:
                    started = time.perf_counter_ns()
                    self._post_search()
                    return (time.perf_counter_ns() - started) / 1_000_000

                with ThreadPoolExecutor(max_workers=4) as executor:
                    samples[label] = list(executor.map(request, range(64)))
                if enabled:
                    latest = self.observability.metrics()["retention"]["latest_sequence"]
                    self._await(lambda value: value["acknowledged_through"] >= latest and value["pending_batches"] == 0,
                                "enabled bounded sample did not complete background capture", timeout=20.0)
                    require(latest - before == 64, "enabled bounded sample did not retain every foreground observation")
        self.measurement = {
            "status": "passed",
            "requests_per_mode": 64,
            "concurrency": 4,
            "same_request": "publication search with the existing neutral fixture payload",
            "latency_ms": {
                name: {"samples": len(values), "p50": statistics.median(values), "p95": self._p95(values)}
                for name, values in samples.items()
            },
            "scope": "Optional bounded local HTTP timing sample with four client workers; enabled mode waits for journal capture completion. "
                     "It makes no RPS, capacity, production latency, or SLO claim.",
        }

    @staticmethod
    def _p95(values: list[float]) -> float:
        ordered = sorted(values)
        return ordered[round(0.95 * (len(ordered) - 1))]
