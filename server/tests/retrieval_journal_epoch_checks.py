"""P3E19 producer-epoch reclamation checks on the existing PostgreSQL stand.

The caller owns stand setup and ordering. Reclamation operates on the global
epoch prefix, so the sidecar first retires and reclaims the inherited test
producer through the existing administrative contracts, then finishes by
restarting the server to establish an explicit fresh process binding.
"""

from __future__ import annotations

from contextlib import contextmanager
import json
import secrets
import statistics
import time
from typing import Any, Iterator
from urllib.parse import urlencode

from publication_test_support import exact_object, integer, require, timestamp
from retrieval_journal_checks import JOURNAL, SHA256, RetrievalJournalChecks
from retrieval_journal_retention_checks import RetrievalJournalRetentionChecks
from retrieval_integration_test import PROJECT_ID


RECLAMATION = JOURNAL + "/epochs/reclamation"
PRODUCER_FIELDS = {
    "instance_id", "producer_epoch", "source_host_id", "server_version", "core_version",
    "observed_through", "captured_count", "missing_count",
}
PLAN_FIELDS = {
    "schema_version", "server_id", "scope", "reclaimed_through", "through", "producer_count",
    "captured_count", "missing_count", "producers", "prefix_sha256", "previous_receipt_sha256", "plan_id",
}
RECEIPT_FIELDS = {
    "schema_version", "server_id", "plan_id", "previous_reclaimed_through", "reclaimed_through",
    "producer_count", "captured_count", "missing_count", "previous_receipt_sha256", "principal_id",
    "recorded_at", "discard_future_confirmed", "external_archive_verified", "source_mutated",
    "publication_mutated",
}


class RetrievalJournalEpochChecks(RetrievalJournalChecks):
    """Regression coverage for explicit producer-epoch reclamation only."""

    def run(self) -> None:
        self.timings["reclamation-plan"] = []
        self.timings["reclamation-apply"] = []
        for name, check in (
            ("retrieval-journal-epoch-migration-and-guards", self.check_migration_and_guards),
            ("retrieval-journal-epoch-native-admission-recovery", self.check_native_admission_recovery),
            ("retrieval-journal-epoch-plan-replay-and-coverage", self.check_plan_replay_and_coverage),
            ("retrieval-journal-epoch-append-and-prefix-staleness", self.check_append_and_prefix_staleness),
            ("retrieval-journal-epoch-post-delete-rollback", self.check_rollback),
            ("retrieval-journal-epoch-http-reclaim-refusal", self.check_http_reclaim_refusal),
        ):
            self.s.record(name, check)
        self._restore_fresh_process_binding()
        self.s.retrieval_journal_epoch_measurement = {
            "status": "passed",
            "calls": {name: {"samples": len(samples), "median_ms": statistics.median(samples),
                             "min_ms": min(samples), "max_ms": max(samples)}
                      for name, samples in self.timings.items() if name.startswith("reclamation-") and samples},
            "scope": "Bounded local HTTP plan/apply timings on the existing PostgreSQL integration stand. "
                     "They are regression measurements, not a production throughput or SLO claim.",
        }

    def _lifecycle(self) -> dict[str, int]:
        row = self.s.sql_json(
            "SELECT to_jsonb(l) FROM shin_server.retrieval_journal_lifecycle l WHERE singleton")
        for key in ("allocated_through", "reclaimed_through", "reclaimed_producer_count",
                    "reclaimed_captured_count", "reclaimed_missing_count"):
            integer(row[key], 0, "journal lifecycle " + key)
        require(row["reclaimed_through"] <= row["allocated_through"] and
                row["reclaimed_producer_count"] <= row["reclaimed_through"],
                "journal lifecycle prefix accounting differs")
        return {key: row[key] for key in (
            "allocated_through", "reclaimed_through", "reclaimed_producer_count",
            "reclaimed_captured_count", "reclaimed_missing_count",
        )}

    def _require_reclaimed_prefix(self) -> None:
        lifecycle = self._lifecycle()
        require(lifecycle["allocated_through"] == lifecycle["reclaimed_through"] and
                self._journal_counts() == (0, 0),
                "epoch reclamation sidecar must run before a journal producer remains active")

    def _retire_and_reclaim_existing_prefix(self) -> None:
        """Use the existing admin retention contract before global epoch reclamation."""
        retention = RetrievalJournalRetentionChecks(self.s)
        after = ""
        while True:
            page = retention.inventory(after=after, limit=32)
            for producer in page["producers"]:
                floor = producer["retired_through"]
                while floor < producer["observed_through"]:
                    plan = retention._plan(producer["instance_id"], min(producer["observed_through"], floor + 256))
                    retention._apply(producer["instance_id"], plan)
                    floor = plan["through"]
            if page["next_after"] is None:
                break
            after = page["next_after"]
        while True:
            lifecycle = self._lifecycle()
            if lifecycle["reclaimed_through"] == lifecycle["allocated_through"]:
                return
            plan = self._plan(min(lifecycle["allocated_through"], lifecycle["reclaimed_through"] + 32))
            self._apply(plan)

    @staticmethod
    def _body(value: Any) -> bytes:
        return json.dumps(value, separators=(",", ":")).encode("utf-8")

    def _producer(self, value: Any) -> dict[str, Any]:
        producer = exact_object(value, PRODUCER_FIELDS, "reclamation producer")
        require(SHA256.fullmatch(producer["instance_id"]) is not None and
                isinstance(producer["source_host_id"], str) and isinstance(producer["server_version"], str) and
                isinstance(producer["core_version"], str), "reclamation producer provenance differs")
        for key in ("producer_epoch", "observed_through", "captured_count", "missing_count"):
            integer(producer[key], 0, "reclamation producer " + key)
        require(producer["producer_epoch"] > 0 and
                producer["captured_count"] + producer["missing_count"] == producer["observed_through"],
                "reclamation producer progress differs")
        return producer

    def _plan(self, through: int, *, expected: int = 200) -> dict[str, Any]:
        started = time.perf_counter_ns()
        response = self.s.expect_status(
            expected, "GET", RECLAMATION + "?" + urlencode({"through": through}), headers=self.s.auth_headers("admin"))
        if expected != 200:
            return response
        self.timings["reclamation-plan"].append((time.perf_counter_ns() - started) / 1_000_000)
        plan = exact_object(response["result"], PLAN_FIELDS, "journal reclamation plan")
        require(plan["schema_version"] == "shin.retrieval-journal-reclamation-plan.v1" and
                plan["scope"] == "all-producers-in-epoch-prefix" and isinstance(plan["server_id"], str) and
                SHA256.fullmatch(plan["prefix_sha256"]) is not None and SHA256.fullmatch(plan["plan_id"]) is not None,
                "journal reclamation plan identity differs")
        for key in ("reclaimed_through", "through", "producer_count", "captured_count", "missing_count"):
            integer(plan[key], 0, "journal reclamation plan " + key)
        require(plan["reclaimed_through"] < plan["through"] <= 2**63 - 1 and
                plan["producer_count"] == len(plan["producers"]) <= 32,
                "journal reclamation range or producer bound differs")
        producers = [self._producer(value) for value in plan["producers"]]
        require([value["producer_epoch"] for value in producers] ==
                list(range(plan["reclaimed_through"] + 1, plan["through"] + 1)),
                "journal reclamation plan did not attribute every epoch in its prefix")
        require(sum(value["captured_count"] for value in producers) == plan["captured_count"] and
                sum(value["missing_count"] for value in producers) == plan["missing_count"],
                "journal reclamation plan aggregate counts differ")
        require(plan["previous_receipt_sha256"] is None or SHA256.fullmatch(plan["previous_receipt_sha256"]) is not None,
                "journal reclamation plan previous receipt hash differs")
        return plan

    def _receipt(self, value: Any) -> dict[str, Any]:
        receipt = exact_object(value, RECEIPT_FIELDS, "journal reclamation receipt")
        timestamp(receipt["recorded_at"])
        require(receipt["schema_version"] == "shin.retrieval-journal-reclamation-receipt.v1" and
                isinstance(receipt["server_id"], str) and SHA256.fullmatch(receipt["plan_id"]) is not None and
                receipt["principal_id"] == "dev-admin" and isinstance(receipt["recorded_at"], str) and
                receipt["discard_future_confirmed"] is True and receipt["external_archive_verified"] is False and
                receipt["source_mutated"] is False and receipt["publication_mutated"] is False,
                "journal reclamation receipt contract differs")
        for key in ("previous_reclaimed_through", "reclaimed_through", "producer_count", "captured_count", "missing_count"):
            integer(receipt[key], 0, "journal reclamation receipt " + key)
        require(receipt["previous_reclaimed_through"] < receipt["reclaimed_through"],
                "journal reclamation receipt did not advance the global fence")
        require(receipt["previous_receipt_sha256"] is None or SHA256.fullmatch(receipt["previous_receipt_sha256"]) is not None,
                "journal reclamation receipt previous hash differs")
        return receipt

    def _apply(self, plan: Any, *, role: str = "admin", body: Any = None, expected: int = 200) -> dict[str, Any]:
        if body is None:
            body = {"plan": plan, "discard_future_confirmed": True}
        started = time.perf_counter_ns()
        response = self.s.expect_status(expected, "POST", RECLAMATION,
                                        headers={**self.s.auth_headers(role), "Content-Type": "application/json"},
                                        body=self._body(body))
        if expected != 200:
            return response
        self.timings["reclamation-apply"].append((time.perf_counter_ns() - started) / 1_000_000)
        result = exact_object(response["result"], {"receipt", "replayed"}, "journal reclamation apply")
        require(type(result["replayed"]) is bool, "journal reclamation replay marker differs")
        self._receipt(result["receipt"])
        return result

    @contextmanager
    def reclaimed_fixture(self, counts: tuple[tuple[int, int], ...], *, require_clean: bool = True) -> Iterator[list[dict[str, Any]]]:
        if require_clean:
            self._require_reclaimed_prefix()
        require(0 < len(counts) <= 32, "reclamation fixture producer count is invalid")
        epochs = self._fixture_epochs(len(counts))
        producers: list[dict[str, Any]] = []
        values: list[str] = []
        for epoch, (captured, missing) in zip(epochs, counts):
            require(captured >= 0 and missing >= 0 and captured + missing > 0, "reclamation fixture progress is invalid")
            instance = secrets.token_hex(32)
            observed = captured + missing
            producer = {"instance_id": instance, "producer_epoch": epoch, "source_host_id": "epoch-fixture-host",
                        "server_version": "epoch-fixture-server", "core_version": "epoch-fixture-core",
                        "observed_through": observed, "captured_count": captured, "missing_count": missing}
            producers.append(producer)
            values.append("(" + self._sql_text(instance) + f",{epoch},'epoch-fixture-host','epoch-fixture-server',"
                          f"'epoch-fixture-core',{observed},{captured},{missing},{observed},{captured},{missing})")
        self.s.psql(
            "INSERT INTO shin_server.retrieval_journal_producers("
            "instance_id,producer_epoch,source_host_id,server_version,core_version,observed_through,captured_count,missing_count,"
            "retired_through,retired_count,retired_missing_count) VALUES " + ",".join(values))
        try:
            yield producers
        finally:
            ids = ",".join(self._sql_text(value["instance_id"]) for value in producers)
            self.s.psql("DELETE FROM shin_server.retrieval_journal_attempts WHERE instance_id IN (" + ids + ");"
                        "DELETE FROM shin_server.retrieval_journal_producers WHERE instance_id IN (" + ids + ")")
            self._restore_fixture_lifecycle()

    def check_migration_and_guards(self) -> None:
        with self.protected():
            require(self.s.scalar("SELECT string_agg(version::text, ',' ORDER BY version) FROM shin_server.schema_migrations") ==
                    "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35", "current migrations are not visible to epoch checks")
            before = self.s.durable_state()
            lifecycle = self._lifecycle()
            self.s.expect_status(401, "GET", RECLAMATION)
            self.s.expect_status(403, "GET", RECLAMATION + "?through=1", headers=self.s.auth_headers("reader"))
            for query in ("", "through=0", "through=one", "through=1&through=2", "through=1&unexpected=1"):
                self.s.expect_status(400, "GET", RECLAMATION + ("?" + query if query else ""),
                                     headers=self.s.auth_headers("admin"))
            future = self._plan(lifecycle["allocated_through"] + 1, expected=409)
            self.s.assert_error_code(future, "journal-reclamation-range")
            self.s.expect_status(401, "POST", RECLAMATION, headers={"Content-Type": "application/json"}, body=b"{}")
            self.s.expect_status(403, "POST", RECLAMATION,
                                 headers={**self.s.auth_headers("reader"), "Content-Type": "application/json"}, body=b"{}")
            for body in ({}, {"plan": {}}, {"plan": {}, "discard_future_confirmed": False},
                         {"plan": {}, "discard_future_confirmed": True, "extra": 1}):
                self.s.expect_status(400, "POST", RECLAMATION,
                                     headers={**self.s.auth_headers("admin"), "Content-Type": "application/json"},
                                     body=self._body(body))
            require(self.s.durable_state() == before, "reclamation guards or reads changed durable state")

    def check_native_admission_recovery(self) -> None:
        probe = self.s.binary.parent / "shin-server-retrieval-journal-store-probe"
        require(probe.is_file(), "native journal Store probe is unavailable")
        with self.protected():
            self.capture()
            before = self._lifecycle()
            self._retire_and_reclaim_existing_prefix()
            self._require_reclaimed_prefix()
            instance = secrets.token_hex(32)
            completed = self.s.run_process([str(probe), str(self.s.runtime_config_path), instance, "epochs"],
                                           "native journal epoch reclamation probe", timeout=90.0)
            result = exact_object(json.loads(completed.stdout), {"ok", "instance_id", "mode", "allocated_through"},
                                  "native journal epoch reclamation probe result")
            require(result["ok"] is True and result["instance_id"] == instance and result["mode"] == "epochs",
                    "native journal epoch reclamation probe did not complete")
            integer(result["allocated_through"], before["allocated_through"] + 130, "native epoch admissions")
            require(result["allocated_through"] == before["allocated_through"] + 130,
                    "native epoch probe allocated unexpected extra producer epochs")
            lifecycle = self._lifecycle()
            require(lifecycle["allocated_through"] == lifecycle["reclaimed_through"] == result["allocated_through"] and
                    lifecycle["reclaimed_producer_count"] >= before["reclaimed_producer_count"] + 129 and self._journal_counts() == (0, 0),
                    "native epoch probe did not reclaim its complete retired prefix")

    def check_plan_replay_and_coverage(self) -> None:
        with self.protected(), self.reclaimed_fixture(((2, 1),)) as producers:
            producer = producers[0]
            before = self._lifecycle()
            plan = self._plan(producer["producer_epoch"])
            require(plan["producers"] == [producer] and
                    (plan["producer_count"], plan["captured_count"], plan["missing_count"]) == (1, 2, 1),
                    "reclamation plan lost exact producer attribution")
            altered = dict(plan)
            altered["server_id"] = "wrong-server"
            stale = self._apply(altered, expected=409)
            self.s.assert_error_code(stale, "journal-reclamation-stale")
            altered = dict(plan)
            altered["producers"] = []
            stale = self._apply(altered, expected=409)
            self.s.assert_error_code(stale, "journal-reclamation-stale")
            applied = self._apply(plan)
            require(applied["replayed"] is False and
                    (applied["receipt"]["producer_count"], applied["receipt"]["captured_count"], applied["receipt"]["missing_count"]) == (1, 2, 1),
                    "reclamation apply receipt counts differ")
            replay = self._apply(plan)
            require(replay["replayed"] is True and replay["receipt"] == applied["receipt"],
                    "latest exact reclamation retry did not replay its receipt")
            lifecycle = self._lifecycle()
            require((lifecycle["reclaimed_through"], lifecycle["reclaimed_producer_count"],
                     lifecycle["reclaimed_captured_count"] - before["reclaimed_captured_count"],
                     lifecycle["reclaimed_missing_count"] - before["reclaimed_missing_count"]) ==
                    (producer["producer_epoch"], producer["producer_epoch"], 2, 1),
                    "reclamation lifecycle counters differ")
            coverage = self.history()["coverage"]
            require(coverage["allocated_producer_epochs"] == lifecycle["allocated_through"] and
                    coverage["reclaimed_through_epoch"] == lifecycle["reclaimed_through"] and
                    coverage["reclaimed_producer_count"] == lifecycle["reclaimed_producer_count"],
                    "journal coverage omitted reclamation lifecycle state")

    def check_append_and_prefix_staleness(self) -> None:
        with self.protected(), self.reclaimed_fixture(((1, 1),)) as producers:
            first = producers[0]
            plan = self._plan(first["producer_epoch"])
            with self.reclaimed_fixture(((1, 0),), require_clean=False) as later:
                require(later[0]["producer_epoch"] > plan["through"], "fixture did not create a later epoch")
                applied = self._apply(plan)
                require(applied["replayed"] is False, "later epoch append incorrectly invalidated prefix plan")
                self._apply(self._plan(later[0]["producer_epoch"]))
            self._require_reclaimed_prefix()

        with self.protected(), self.reclaimed_fixture(((2, 1),)) as producers:
            producer = producers[0]
            plan = self._plan(producer["producer_epoch"])
            self.s.psql(
                "INSERT INTO shin_server.retrieval_journal_attempts("
                "instance_id,producer_epoch,sequence,record_hash,project_id,operation,selector_kind,selector_id,observed_at_unix_ms,"
                "status,reason_code,last_phase,elapsed_ns,result_bytes,validation_ns,query_capture) VALUES (" +
                self._sql_text(producer["instance_id"]) + f",{producer['producer_epoch']},4,repeat('e',64)," + self._sql_text(PROJECT_ID) + ","
                "'search','route','epoch-fixture',4,200,NULL,'validation',1,NULL,1,'disabled');" +
                "UPDATE shin_server.retrieval_journal_producers SET observed_through=4,captured_count=3,missing_count=1 "
                "WHERE instance_id=" + self._sql_text(producer["instance_id"]))
            stale = self._apply(plan, expected=409)
            self.s.assert_error_code(stale, "journal-reclamation-stale")

    def check_rollback(self) -> None:
        with self.protected(), self.reclaimed_fixture(((1, 1),)) as producers:
            plan = self._plan(producers[0]["producer_epoch"])
            before = self.s.durable_state()
            trigger = "retrieval_journal_reclamation_fail_" + secrets.token_hex(6)
            self.s.psql(f"CREATE FUNCTION shin_server.{trigger}() RETURNS trigger LANGUAGE plpgsql AS $$ "
                        "BEGIN RAISE EXCEPTION 'controlled reclamation lifecycle failure'; END $$; "
                        f"CREATE TRIGGER {trigger} BEFORE UPDATE ON shin_server.retrieval_journal_lifecycle "
                        f"FOR EACH ROW EXECUTE FUNCTION shin_server.{trigger}()")
            try:
                status, _ = self.s.request("POST", RECLAMATION,
                                           headers={**self.s.auth_headers("admin"), "Content-Type": "application/json"},
                                           body=self._body({"plan": plan, "discard_future_confirmed": True}))
                require(status >= 500, "controlled post-delete lifecycle failure did not abort apply")
            finally:
                self.s.psql(f"DROP TRIGGER IF EXISTS {trigger} ON shin_server.retrieval_journal_lifecycle; "
                            f"DROP FUNCTION IF EXISTS shin_server.{trigger}()")
            require(self.s.durable_state() == before, "failed reclamation partially committed lifecycle or receipt")
            self._apply(plan)

    def check_http_reclaim_refusal(self) -> None:
        with self.protected():
            self._require_reclaimed_prefix()
            refused = self.capture(expected=409)
            self.s.assert_error_code(refused, "journal-producer-reclaimed")
            require(self._journal_counts() == (0, 0), "reclaimed HTTP producer was silently reenrolled")

    def _restore_fresh_process_binding(self) -> None:
        self.s.stop_server(expect_clean=True)
        self.s.start_server()
        receipt = self.capture()
        lifecycle = self._lifecycle()
        require(receipt["producer_epoch"] > lifecycle["reclaimed_through"] and receipt["replayed"] is False and
                self._journal_counts() == (1, 0),
                "explicit restart did not establish a fresh post-reclamation process binding")
