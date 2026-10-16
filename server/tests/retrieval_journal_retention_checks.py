"""P3E16 retention checks over the publication suite's existing PostgreSQL stand.

The checks exercise only the administrative retention surface and short-lived
journal fixtures. They never import arbitrary facts through HTTP.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import json
import secrets
import statistics
import time
from typing import Any, Iterator
from urllib.parse import urlencode

from publication_test_support import exact_object, integer, require
from retrieval_journal_checks import JOURNAL, SHA256, RetrievalJournalChecks
from retrieval_integration_test import PROJECT_ID


PRODUCERS = JOURNAL + "/producers"
PLAN_FIELDS = {
    "schema_version", "server_id", "instance_id", "producer_epoch", "scope", "retired_through", "through", "delete_count",
    "retire_missing_count", "affected_projects", "prefix_sha256", "previous_receipt_sha256", "plan_id",
}
PROJECT_COUNT_FIELDS = {"project_id", "delete_count"}
INVENTORY_FIELDS = {"schema_version", "producers", "next_after", "read_only"}
PRODUCER_FIELDS = {
    "instance_id", "producer_epoch", "source_host_id", "server_version", "core_version", "observed_through", "captured_count",
    "missing_count", "retired_through", "retired_count", "retired_missing_count", "retained_count",
    "last_retention_receipt",
}
APPLY_FIELDS = {"receipt", "replayed"}
RECEIPT_FIELDS = {
    "schema_version", "server_id", "instance_id", "plan_id", "previous_retired_through", "retired_through",
    "deleted_count", "retired_missing_count", "previous_receipt_sha256", "principal_id", "recorded_at",
    "discard_confirmed", "external_archive_verified", "source_mutated", "publication_mutated",
}


class RetrievalJournalRetentionChecks(RetrievalJournalChecks):
    """Explicit bounded producer-prefix retirement contract coverage."""

    def __init__(self, suite: Any) -> None:
        super().__init__(suite)
        self.timings["retention-plan"] = []
        self.timings["retention-apply"] = []

    def run(self) -> None:
        for name, check in (
            ("retrieval-journal-retention-migration-inventory-and-guards", self.check_migration_inventory_and_guards),
            ("retrieval-journal-retention-plan-apply-all-projects", self.check_plan_apply_all_projects_and_replay),
            ("retrieval-journal-retention-stale-append-and-backfill", self.check_stale_append_and_prefix_backfill),
            ("retrieval-journal-retention-bounds-missing-and-rollback", self.check_bounds_missing_and_rollback),
            ("retrieval-journal-retention-concurrent-restart-capacity", self.check_concurrent_restart_and_capacity_reuse),
            ("retrieval-journal-retention-capture-fence", self.check_native_capture_fence),
        ):
            self.s.record(name, check)
        self.s.retrieval_journal_retention_measurement = {
            "status": "passed",
            "calls": {name: {"samples": len(samples), "median_ms": statistics.median(samples),
                              "min_ms": min(samples), "max_ms": max(samples)}
                      for name, samples in self.timings.items() if name.startswith("retention-") and samples},
            "scope": "Successful local HTTP calls including decoding across varied regression fixtures. "
                     "Apply includes replay and one concurrent pair. Not a load test or a production SLO.",
        }

    @staticmethod
    def _attempt_columns() -> str:
        return ("instance_id,producer_epoch,sequence,record_hash,project_id,operation,selector_kind,selector_id,observed_at_unix_ms,"
                "status,reason_code,last_phase,elapsed_ns,result_bytes,validation_ns,query_capture")

    def _insert_fact(self, instance: str, sequence: int, project: str) -> None:
        epoch = int(self.s.scalar("SELECT producer_epoch FROM shin_server.retrieval_journal_producers WHERE instance_id=" +
                                  self._sql_text(instance)))
        self.s.psql(
            "INSERT INTO shin_server.retrieval_journal_attempts(" + self._attempt_columns() + ") VALUES (" +
            self._sql_text(instance) + "," + str(epoch) + "," + str(sequence) + ",repeat('a',64)," + self._sql_text(project) +
            ",'search','route','retention-fixture'," + str(sequence) + ",200,NULL,'validation',1,NULL,1,'disabled')"
        )

    @contextmanager
    def retention_fixture(self, *, sequences: tuple[tuple[int, str], ...] =
                          ((1, PROJECT_ID), (2, "other"), (4, PROJECT_ID), (6, "other"), (7, PROJECT_ID))) -> Iterator[str]:
        instance = secrets.token_hex(32)
        observed = max(sequence for sequence, _ in sequences)
        captured = len(sequences)
        missing = observed - captured
        before = self._journal_counts()
        epoch = self._fixture_epochs(1)[0]
        self.s.psql(
            "INSERT INTO shin_server.retrieval_journal_producers("
            "instance_id,producer_epoch,source_host_id,server_version,core_version,observed_through,captured_count,missing_count,"
            "retired_through,retired_count,retired_missing_count) VALUES (" +
            self._sql_text(instance) + "," + str(epoch) + ",'retention-host','retention-server','retention-core'," +
            f"{observed},{captured},{missing},0,0,0)"
        )
        try:
            for sequence, project in sequences:
                self._insert_fact(instance, sequence, project)
            yield instance
        finally:
            self.s.psql("DELETE FROM shin_server.retrieval_journal_attempts WHERE instance_id=" + self._sql_text(instance) + ";"
                        "DELETE FROM shin_server.retrieval_journal_producers WHERE instance_id=" + self._sql_text(instance))
            self._restore_fixture_lifecycle()
            require(self._journal_counts() == before, "retention fixture did not clean up exactly")

    def _receipt(self, value: Any) -> dict[str, Any]:
        receipt = exact_object(value, RECEIPT_FIELDS, "retention receipt")
        require(receipt["schema_version"] == "shin.retrieval-journal-retention-receipt.v1" and
                isinstance(receipt["server_id"], str) and isinstance(receipt["instance_id"], str) and
                SHA256.fullmatch(receipt["plan_id"]) is not None and receipt["principal_id"] == "dev-admin" and
                isinstance(receipt["recorded_at"], str) and receipt["discard_confirmed"] is True and
                receipt["external_archive_verified"] is False and receipt["source_mutated"] is False and
                receipt["publication_mutated"] is False, "retention receipt contract differs")
        for key in ("previous_retired_through", "retired_through", "deleted_count", "retired_missing_count"):
            integer(receipt[key], 0, "retention receipt " + key)
        require(receipt["previous_retired_through"] < receipt["retired_through"],
                "retention receipt did not advance the prefix fence")
        require(receipt["previous_receipt_sha256"] is None or SHA256.fullmatch(receipt["previous_receipt_sha256"]) is not None,
                "retention receipt previous hash differs")
        return receipt

    def _plan(self, instance: str, through: int, *, expected: int = 200) -> dict[str, Any]:
        started = time.perf_counter_ns()
        response = self.s.expect_status(expected, "GET", PRODUCERS + "/" + instance + "/retention?" +
                                        urlencode({"through": through}), headers=self.s.auth_headers("admin"))
        if expected != 200:
            return response
        self.timings["retention-plan"].append((time.perf_counter_ns() - started) / 1_000_000)
        plan = exact_object(response["result"], PLAN_FIELDS, "retention plan")
        require(plan["schema_version"] == "shin.retrieval-journal-retention-plan.v1" and
                plan["instance_id"] == instance and plan["scope"] == "all-projects-of-producer" and
                isinstance(plan["server_id"], str) and SHA256.fullmatch(plan["prefix_sha256"]) is not None and
                SHA256.fullmatch(plan["plan_id"]) is not None, "retention plan identity differs")
        for key in ("retired_through", "through", "delete_count", "retire_missing_count"):
            integer(plan[key], 0, "retention plan " + key)
        integer(plan["producer_epoch"], 1, "retention plan producer epoch")
        require(plan["retired_through"] < plan["through"] <= 2**63 - 1 and
                plan["delete_count"] + plan["retire_missing_count"] == plan["through"] - plan["retired_through"],
                "retention plan prefix accounting differs")
        require(plan["previous_receipt_sha256"] is None or SHA256.fullmatch(plan["previous_receipt_sha256"]) is not None,
                "retention plan previous receipt hash differs")
        require(isinstance(plan["affected_projects"], list), "retention plan affected projects differs")
        counted = 0
        project_ids: list[str] = []
        for project in plan["affected_projects"]:
            item = exact_object(project, PROJECT_COUNT_FIELDS, "retention affected project")
            require(isinstance(item["project_id"], str), "retention affected project ID differs")
            integer(item["delete_count"], 1, "retention affected project count")
            counted += item["delete_count"]
            project_ids.append(item["project_id"])
        require(counted == plan["delete_count"] and len(project_ids) == len(set(project_ids)),
                "retention all-project delete counts differ")
        return plan

    def _apply(self, instance: str, plan: Any, *, role: str = "admin", suffix: str = "",
               expected: int = 200) -> dict[str, Any]:
        started = time.perf_counter_ns()
        response = self.s.expect_status(expected, "POST", PRODUCERS + "/" + instance + "/retention" + suffix,
                                        headers={**self.s.auth_headers(role), "Content-Type": "application/json"},
                                        body=self._json_body({"plan": plan, "discard_confirmed": True}))
        if expected != 200:
            return response
        self.timings["retention-apply"].append((time.perf_counter_ns() - started) / 1_000_000)
        result = exact_object(response["result"], APPLY_FIELDS, "retention apply result")
        require(type(result["replayed"]) is bool, "retention apply replay marker differs")
        self._receipt(result["receipt"])
        return result

    def inventory(self, *, after: str = "", limit: int = 32, expected: int = 200) -> dict[str, Any]:
        response = self.s.expect_status(expected, "GET", PRODUCERS + "?" + urlencode({"after": after, "limit": limit}),
                                        headers=self.s.auth_headers("admin"))
        if expected != 200:
            return response
        result = exact_object(response["result"], INVENTORY_FIELDS, "retention inventory")
        require(result["schema_version"] == "shin.retrieval-journal-producers.v1" and result["read_only"] is True and
                isinstance(result["producers"], list) and len(result["producers"]) <= limit,
                "retention inventory envelope differs")
        ids: list[str] = []
        for value in result["producers"]:
            producer = exact_object(value, PRODUCER_FIELDS, "retention inventory producer")
            require(SHA256.fullmatch(producer["instance_id"]) is not None and isinstance(producer["source_host_id"], str) and
                    isinstance(producer["server_version"], str) and isinstance(producer["core_version"], str),
                    "retention inventory producer identity differs")
            for key in ("producer_epoch", "observed_through", "captured_count", "missing_count", "retired_through", "retired_count",
                        "retired_missing_count", "retained_count"):
                integer(producer[key], 0, "retention inventory " + key)
            require(producer["captured_count"] + producer["missing_count"] == producer["observed_through"] and
                    producer["retired_count"] + producer["retired_missing_count"] == producer["retired_through"] and
                    producer["retired_count"] <= producer["captured_count"] and
                    producer["retained_count"] == producer["captured_count"] - producer["retired_count"],
                    "retention inventory producer accounting differs")
            if producer["last_retention_receipt"] is not None:
                self._receipt(producer["last_retention_receipt"])
            ids.append(producer["instance_id"])
        require(ids == sorted(ids) and all(identifier > after for identifier in ids), "retention inventory cursor order differs")
        if result["next_after"] is not None:
            require(ids and result["next_after"] == ids[-1] and SHA256.fullmatch(result["next_after"]) is not None,
                    "retention inventory next cursor differs")
        return result

    def check_migration_inventory_and_guards(self) -> None:
        with self.protected():
            require(self.s.scalar("SELECT string_agg(version::text, ',' ORDER BY version) FROM shin_server.schema_migrations") ==
                    "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35", "current migrations are not visible")
            before = self.s.durable_state()
            self.inventory(limit=1)
            self.s.expect_status(401, "GET", PRODUCERS)
            self.s.expect_status(403, "GET", PRODUCERS, headers=self.s.auth_headers("reader"))
            for query in ("after=g", "after=00", "limit=0", "limit=33", "limit=1&limit=2", "unknown=1"):
                self.s.expect_status(400, "GET", PRODUCERS + "?" + query, headers=self.s.auth_headers("admin"))
            missing = "0" * 64
            self._plan(missing, 1, expected=404)
            with self.retention_fixture() as instance:
                self.s.expect_status(403, "GET", PRODUCERS + "/" + instance + "/retention?through=1",
                                     headers=self.s.auth_headers("reader"))
                for query in ("", "through=0", "through=8", "through=1&through=2", "through=one", "through=1&unexpected=1"):
                    self.s.expect_status(400 if query in {"", "through=0", "through=1&through=2", "through=one", "through=1&unexpected=1"} else 409,
                                         "GET", PRODUCERS + "/" + instance + "/retention" + ("?" + query if query else ""),
                                         headers=self.s.auth_headers("admin"))
                plan = self._plan(instance, 1)
                self._apply(missing, plan, expected=404)
                self.s.expect_status(403, "POST", PRODUCERS + "/" + instance + "/retention",
                                     headers={**self.s.auth_headers("reader"), "Content-Type": "application/json"},
                                     body=self._json_body({"plan": plan, "discard_confirmed": True}))
                for body in ({}, {"plan": plan}, {"plan": plan, "discard_confirmed": False},
                             {"plan": plan, "discard_confirmed": True, "extra": 1}):
                    self.s.expect_status(400, "POST", PRODUCERS + "/" + instance + "/retention",
                                         headers={**self.s.auth_headers("admin"), "Content-Type": "application/json"},
                                         body=self._json_body(body))
                self._apply(instance, plan, suffix="?unexpected=1", expected=400)
            require(self.s.durable_state() == before, "retention reads or denied requests changed durable state")

    def check_plan_apply_all_projects_and_replay(self) -> None:
        with self.protected(), self.retention_fixture() as instance:
            before_coverage = self.history()["coverage"]
            before_plan = self.s.durable_state()
            plan = self._plan(instance, 5)
            require((plan["retired_through"], plan["through"], plan["delete_count"], plan["retire_missing_count"]) == (0, 5, 3, 2) and
                    {(item["project_id"], item["delete_count"]) for item in plan["affected_projects"]} ==
                    {(PROJECT_ID, 2), ("other", 1)}, "retention plan did not include every producer project")
            require(self.s.durable_state() == before_plan, "read-only retention plan persisted a mutation")
            altered = dict(plan)
            altered["server_id"] = "wrong-server"
            stale = self._apply(instance, altered, expected=409)
            self.s.assert_error_code(stale, "journal-retention-stale")
            altered_prefix = dict(plan)
            altered_prefix["prefix_sha256"] = "0" * 64
            stale = self._apply(instance, altered_prefix, expected=409)
            self.s.assert_error_code(stale, "journal-retention-stale")
            applied = self._apply(instance, plan)
            require(applied["replayed"] is False, "first retention apply was marked replayed")
            receipt = applied["receipt"]
            require((receipt["previous_retired_through"], receipt["retired_through"], receipt["deleted_count"],
                     receipt["retired_missing_count"]) == (0, 5, 3, 2), "retention receipt counts differ")
            require(self.s.scalar("SELECT string_agg(sequence::text, ',' ORDER BY sequence) FROM shin_server.retrieval_journal_attempts "
                                  "WHERE instance_id=" + self._sql_text(instance)) == "6,7",
                    "retention did not delete the exact producer prefix")
            after_coverage = self.history()["coverage"]
            expected_deltas = {"captured_count": -3, "lifetime_captured_count": 0, "retired_fact_count": 3,
                               "known_missing_sequences_global": 0, "retired_missing_sequences_global": 2}
            require(all(after_coverage[key] - before_coverage[key] == delta for key, delta in expected_deltas.items()),
                    "coverage hid retired facts or permanently closed missing observations")
            producer = self.inventory(after="", limit=32)
            row = next(item for item in producer["producers"] if item["instance_id"] == instance)
            require((row["observed_through"], row["captured_count"], row["missing_count"], row["retired_through"],
                     row["retired_count"], row["retired_missing_count"], row["retained_count"]) == (7, 5, 2, 5, 3, 2, 2) and
                    row["last_retention_receipt"] == receipt, "retention inventory lost lifetime or receipt state")
            replay = self._apply(instance, plan)
            require(replay["replayed"] is True and replay["receipt"] == receipt, "exact retention retry did not replay saved receipt")
            old = self._plan(instance, 6)
            self._apply(instance, old)
            stale_after_newer = self._apply(instance, plan, expected=409)
            self.s.assert_error_code(stale_after_newer, "journal-retention-stale")

    def check_stale_append_and_prefix_backfill(self) -> None:
        sequences = ((1, PROJECT_ID), (2, "other"), (4, PROJECT_ID))
        with self.protected(), self.retention_fixture(sequences=sequences) as instance:
            append_plan = self._plan(instance, 2)
            self._insert_fact(instance, 5, "other")
            self.s.psql("UPDATE shin_server.retrieval_journal_producers SET observed_through=5,captured_count=4,missing_count=1 "
                        "WHERE instance_id=" + self._sql_text(instance))
            applied = self._apply(instance, append_plan)
            require(applied["replayed"] is False and applied["receipt"]["deleted_count"] == 2,
                    "append above retirement prefix invalidated a valid plan")
        with self.protected(), self.retention_fixture(sequences=sequences) as instance:
            prefix_plan = self._plan(instance, 3)
            self._insert_fact(instance, 3, "other")
            self.s.psql("UPDATE shin_server.retrieval_journal_producers SET captured_count=4,missing_count=0 "
                        "WHERE instance_id=" + self._sql_text(instance))
            stale = self._apply(instance, prefix_plan, expected=409)
            self.s.assert_error_code(stale, "journal-retention-stale")
            fresh = self._plan(instance, 3)
            require((fresh["delete_count"], fresh["retire_missing_count"]) == (3, 0),
                    "late prefix backfill did not produce a changed plan")
            self._apply(instance, fresh)

    def check_bounds_missing_and_rollback(self) -> None:
        many = tuple((sequence, PROJECT_ID if sequence % 2 else "other") for sequence in range(1, 258))
        with self.protected(), self.retention_fixture(sequences=many) as instance:
            too_large = self._plan(instance, 257, expected=409)
            self.s.assert_error_code(too_large, "journal-retention-too-large")
        with self.protected(), self.retention_fixture(sequences=many[1:]) as instance:
            boundary = self._plan(instance, 257)
            require((boundary["delete_count"], boundary["retire_missing_count"]) == (256, 1),
                    "boundary plan did not count its missing prefix sequence")
            self._insert_fact(instance, 1, PROJECT_ID)
            self.s.psql("UPDATE shin_server.retrieval_journal_producers SET captured_count=257,missing_count=0 "
                        "WHERE instance_id=" + self._sql_text(instance))
            before = self.s.durable_state()
            stale = self._apply(instance, boundary, expected=409)
            self.s.assert_error_code(stale, "journal-retention-stale")
            require(self.s.durable_state() == before, "oversized backfill partially applied a stale plan")
        with self.protected(), self.retention_fixture(sequences=((3, PROJECT_ID),)) as instance:
            missing = self._plan(instance, 2)
            require((missing["delete_count"], missing["retire_missing_count"], missing["affected_projects"]) == (0, 2, []),
                    "known missing prefix was not explicitly retireable")
            receipt = self._apply(instance, missing)["receipt"]
            require((receipt["deleted_count"], receipt["retired_missing_count"]) == (0, 2),
                    "zero-fact retention did not retain missing-prefix accounting")
        with self.protected(), self.retention_fixture() as instance:
            plan = self._plan(instance, 2)
            before = self.s.durable_state()
            trigger = "retrieval_journal_retention_fail_" + secrets.token_hex(6)
            self.s.psql(f"CREATE FUNCTION shin_server.{trigger}() RETURNS trigger LANGUAGE plpgsql AS $$ "
                        "BEGIN RAISE EXCEPTION 'controlled retention delete failure'; END $$; "
                        f"CREATE TRIGGER {trigger} BEFORE UPDATE ON shin_server.retrieval_journal_producers "
                        f"FOR EACH ROW EXECUTE FUNCTION shin_server.{trigger}()")
            try:
                status, _ = self.s.request("POST", PRODUCERS + "/" + instance + "/retention",
                                           headers={**self.s.auth_headers("admin"), "Content-Type": "application/json"},
                                           body=self._json_body({"plan": plan, "discard_confirmed": True}))
                require(status >= 500, "controlled deletion failure did not abort retention")
            finally:
                self.s.psql(f"DROP TRIGGER IF EXISTS {trigger} ON shin_server.retrieval_journal_producers; "
                            f"DROP FUNCTION IF EXISTS shin_server.{trigger}()")
            require(self.s.durable_state() == before, "failed retention partially committed fence or receipt")
            self._apply(instance, plan)

    def check_concurrent_restart_and_capacity_reuse(self) -> None:
        with self.protected(), self.retention_fixture(sequences=((1, PROJECT_ID), (2, "other"), (3, PROJECT_ID))) as instance:
            plan = self._plan(instance, 2)
            with ThreadPoolExecutor(max_workers=2) as executor:
                results = list(executor.map(lambda _: self._apply(instance, plan), range(2)))
            require(sorted(result["replayed"] for result in results) == [False, True] and
                    results[0]["receipt"] == results[1]["receipt"], "concurrent retention apply did not serialize exact replay")
        with self.protected(), self.retention_fixture(sequences=((1, PROJECT_ID), (2, "other"), (3, PROJECT_ID))) as instance:
            plan = self._plan(instance, 2)
            self.s.stop_server(expect_clean=True)
            self.s.start_server()
            result = self._apply(instance, plan)
            require(result["replayed"] is False and result["receipt"]["retired_through"] == 2,
                    "restart lost a still-current retention plan or producer identity binding")
            self.s.stop_server(expect_clean=True)
            self.s.start_server()
            replay = self._apply(instance, plan)
            require(replay["replayed"] is True and replay["receipt"] == result["receipt"],
                    "restart lost the durable latest retention receipt")

        with self.protected():
            self.capture()
            before_facts = self._journal_counts()[1]
            free = 8192 - before_facts
            require(free >= 256, "retention capacity fixture needs at least one bounded prefix")
            instance = secrets.token_hex(32)
            epoch = self._fixture_epochs(1)[0]
            self.s.psql("INSERT INTO shin_server.retrieval_journal_producers("
                        "instance_id,producer_epoch,source_host_id,server_version,core_version,observed_through,captured_count,missing_count,"
                        "retired_through,retired_count,retired_missing_count) VALUES (" + self._sql_text(instance) +
                        f",{epoch},'retention-capacity','retention-server','retention-core',{free},{free},0,0,0,0);")
            self.s.psql("INSERT INTO shin_server.retrieval_journal_attempts(" + self._attempt_columns() + ") "
                        "SELECT " + self._sql_text(instance) + "," + str(epoch) + ",n,repeat('b',64),'visible','search','route','retention-capacity',"
                        "n,200,NULL,'validation',1,NULL,1,'disabled' FROM generate_series(1," + str(free) + ") AS n")
            try:
                self.o.post("search", self.o.payload("search"))
                blocked = self.capture(expected=507)
                self.s.assert_error_code(blocked, "journal-capacity")
                self._apply(instance, self._plan(instance, 256))
                recovered = self.capture()
                require(recovered["inserted_count"] >= 1, "retention did not reuse released journal fact capacity")
            finally:
                self.s.psql("DELETE FROM shin_server.retrieval_journal_attempts WHERE instance_id=" + self._sql_text(instance) + ";"
                            "DELETE FROM shin_server.retrieval_journal_producers WHERE instance_id=" + self._sql_text(instance))
                self._restore_fixture_lifecycle()
            require(self._journal_counts()[1] >= before_facts, "capacity fixture removed unrelated journal facts")

    def check_native_capture_fence(self) -> None:
        probe = self.s.binary.parent / "shin-server-retrieval-journal-store-probe"
        require(probe.is_file(), "native journal Store probe is unavailable")
        with self.protected():
            before = self._journal_counts()
            capture_instance = secrets.token_hex(32)
            try:
                completed = self.s.run_process([str(probe), str(self.s.runtime_config_path), capture_instance, "capture"],
                                               "native retention capture probe")
                require(json.loads(completed.stdout) == {"ok": True, "instance_id": capture_instance, "mode": "capture"},
                        "native retention capture probe did not complete")
            finally:
                self.s.psql("DELETE FROM shin_server.retrieval_journal_attempts WHERE instance_id=" + self._sql_text(capture_instance) + ";"
                            "DELETE FROM shin_server.retrieval_journal_producers WHERE instance_id=" + self._sql_text(capture_instance))
                self._restore_fixture_lifecycle()
            instance = secrets.token_hex(32)
            try:
                completed = self.s.run_process([str(probe), str(self.s.runtime_config_path), instance, "fences"],
                                               "native retention capture-fence probe")
                require(json.loads(completed.stdout) == {"ok": True, "instance_id": instance, "mode": "fences"},
                        "native retention fence probe did not complete")
                require(self.s.scalar("SELECT observed_through||':'||captured_count||':'||missing_count||':'||retired_through||"
                                      "':'||retired_count||':'||retired_missing_count FROM shin_server.retrieval_journal_producers WHERE instance_id=" +
                                      self._sql_text(instance)) == "7:6:1:5:4:1", "capture fence lost lifetime producer accounting")
                require(self.s.scalar("SELECT string_agg(sequence::text, ',' ORDER BY sequence) FROM shin_server.retrieval_journal_attempts "
                                      "WHERE instance_id=" + self._sql_text(instance)) == "6,7",
                        "retired capture input was not discarded while newer facts were retained")
            finally:
                self.s.psql("DELETE FROM shin_server.retrieval_journal_attempts WHERE instance_id=" + self._sql_text(instance) + ";"
                            "DELETE FROM shin_server.retrieval_journal_producers WHERE instance_id=" + self._sql_text(instance))
                self._restore_fixture_lifecycle()
            require(self._journal_counts() == before, "native retention probe did not clean up")
