"""P3D2 managed active-generation checks mixed into the preparation integration suite."""

from __future__ import annotations

import json
import fcntl
import os
import socket
import threading
import time
from typing import Any

from integration_test import HOST, PORT, IntegrationFailure
from retrieval_integration_test import PROJECT_ID, ROUTE_ID


_ACTIVATION_FIELDS = {"project_id", "route_id", "revision", "preparation_id", "snapshot_id"}
_RECEIPT_FIELDS = {
    "project_id",
    "route_id",
    "previous_revision",
    "revision",
    "preparation_id",
    "snapshot_id",
    "created_at",
}


class ActivationChecks:
    """Exercise explicit activation without treating a CLI output directory as server authority."""

    PROJECT_ID = PROJECT_ID
    ROUTE_ID = ROUTE_ID

    def _activation_path(self, project_id: str, route_id: str) -> str:
        return f"/api/v1/projects/{project_id}/routes/{route_id}/activation"

    def activation(self, project_id: str, route_id: str, *, role: str = "admin") -> dict[str, Any]:
        envelope = self.expect_status(200, "GET", self._activation_path(project_id, route_id), headers=self.auth_headers(role))
        result = envelope.get("result")
        if not isinstance(result, dict) or set(result) != _ACTIVATION_FIELDS:
            raise IntegrationFailure("activation lookup does not match the exact public contract")
        if result["project_id"] != project_id or result["route_id"] != route_id:
            raise IntegrationFailure("activation lookup crossed its configured route boundary")
        if type(result["revision"]) is not int or result["revision"] < 0:
            raise IntegrationFailure("activation lookup has an invalid revision")
        if result["preparation_id"] is not None and (type(result["preparation_id"]) is not int or result["preparation_id"] <= 0):
            raise IntegrationFailure("activation lookup has an invalid preparation identity")
        if result["snapshot_id"] is not None and not isinstance(result["snapshot_id"], str):
            raise IntegrationFailure("activation lookup has an invalid snapshot identity")
        if (result["preparation_id"] is None) != (result["snapshot_id"] is None):
            raise IntegrationFailure("activation lookup returned a partial active generation")
        return result

    def _post_activation(self, project_id: str, route_id: str, preparation_id: int | None,
                         expected_revision: int, *, role: str = "admin") -> tuple[int, dict[str, Any]]:
        return self.request(
            "POST",
            self._activation_path(project_id, route_id),
            headers={**self.auth_headers(role), "Content-Type": "application/json"},
            body=json.dumps(
                {"preparation_id": preparation_id, "expected_revision": expected_revision},
                separators=(",", ":"),
            ).encode("utf-8"),
        )

    def _receipt(self, response: dict[str, Any], *, replayed: bool, project_id: str, route_id: str,
                 previous_revision: int, preparation_id: int | None, snapshot_id: str | None) -> dict[str, Any]:
        result = response.get("result")
        if not isinstance(result, dict) or set(result) != {"replayed", "receipt"} or result["replayed"] is not replayed:
            raise IntegrationFailure("activation response does not match the exact public contract")
        receipt = result["receipt"]
        if not isinstance(receipt, dict) or set(receipt) != _RECEIPT_FIELDS:
            raise IntegrationFailure("activation receipt does not match the exact public contract")
        if (receipt["project_id"] != project_id or receipt["route_id"] != route_id or
                receipt["previous_revision"] != previous_revision or receipt["revision"] != previous_revision + 1 or
                receipt["preparation_id"] != preparation_id or receipt["snapshot_id"] != snapshot_id):
            raise IntegrationFailure("activation receipt does not describe the requested transition")
        if not isinstance(receipt["created_at"], str) or not receipt["created_at"].endswith("Z"):
            raise IntegrationFailure("activation receipt has no safe UTC creation timestamp")
        return receipt

    def _prepare_activation_candidate(self, expected_snapshot_id: str | None, expected_revision: int,
                                      key: str) -> dict[str, Any]:
        status, response = self.post_preparation(self.PROJECT_ID, self.ROUTE_ID, expected_snapshot_id, key)
        if status != 202:
            raise IntegrationFailure("activation candidate preparation was not accepted")
        result = response.get("result")
        if not isinstance(result, dict) or result.get("replayed") is not False:
            raise IntegrationFailure("activation candidate preparation has an invalid response")
        queued = self.assert_public_job(result.get("job"), project_id=self.PROJECT_ID, route_id=self.ROUTE_ID)
        recorded_revision = self.scalar(
            "SELECT expected_route_revision FROM shin_server.preparations WHERE id = " + str(queued["id"])
        )
        if recorded_revision != str(expected_revision):
            raise IntegrationFailure("preparation did not durably capture its active-route revision")
        return self.wait_for_terminal(queued["id"], "prepared")

    def _assert_activation_unchanged(self, expected: dict[str, Any]) -> None:
        actual = self.activation(self.PROJECT_ID, self.ROUTE_ID)
        if actual != expected:
            raise IntegrationFailure("rejected activation changed the durable active generation")

    def _strict_retrieval(self, preparation_id: int, revision: int) -> dict[str, Any]:
        status, response = self.post_retrieval(
            self.PROJECT_ID,
            "brief",
            {"query": "snapshot admission evidence"},
        )
        if status != 200:
            raise IntegrationFailure("strict managed retrieval did not succeed")
        result = response.get("result")
        if (not isinstance(result, dict) or result.get("preparation_id") != preparation_id or
                result.get("revision") != revision or not isinstance(result.get("snapshot_id"), str) or
                not isinstance(result.get("evidence"), dict)):
            raise IntegrationFailure("strict managed retrieval did not retain its admitted generation metadata")
        return result

    def _activation_audit_count(self, preparation_id: int) -> int:
        return int(self.scalar(
            "SELECT count(*) FROM shin_server.audit_events WHERE action='generation.activated' AND preparation_id=" +
            str(preparation_id)
        ))

    def _post_activation_without_response(self, preparation_id: int, expected_revision: int) -> None:
        payload = json.dumps(
            {"preparation_id": preparation_id, "expected_revision": expected_revision},
            separators=(",", ":"),
        ).encode("utf-8")
        request = (
            f"POST {self._activation_path(self.PROJECT_ID, self.ROUTE_ID)} HTTP/1.1\r\n"
            f"Host: {HOST}:{PORT}\r\n"
            f"Authorization: Bearer {self.admin_token}\r\n"
            f"X-Shin-Server-ID: {self.server_id}\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(payload)}\r\n"
            "Connection: close\r\n\r\n"
        ).encode("ascii") + payload
        try:
            connection = socket.create_connection((HOST, PORT), timeout=4.0)
            try:
                connection.sendall(request)
                connection.shutdown(socket.SHUT_WR)
            finally:
                connection.close()
        except OSError as error:
            raise IntegrationFailure("lost-response activation fixture could not send its request") from error

    def check_activation_contract_and_transitions(self) -> None:
        initial = self.activation(self.PROJECT_ID, self.ROUTE_ID)
        if initial != {
            "project_id": self.PROJECT_ID,
            "route_id": self.ROUTE_ID,
            "revision": 0,
            "preparation_id": None,
            "snapshot_id": None,
        }:
            raise IntegrationFailure("new route did not begin as an unpublished revision-zero generation")

        self.expect_status(401, "GET", self._activation_path(self.PROJECT_ID, self.ROUTE_ID))
        self.expect_status(403, "POST", self._activation_path(self.PROJECT_ID, self.ROUTE_ID),
                           headers={**self.auth_headers("reader"), "Content-Type": "application/json"},
                           body=b'{"preparation_id":null,"expected_revision":0}')
        for body in (b"", b"[]", b'{"preparation_id":0,"expected_revision":0}',
                     b'{"preparation_id":null,"expected_revision":-1}',
                     b'{"preparation_id":null,"expected_revision":0,"unexpected":true}'):
            self.expect_status(400, "POST", self._activation_path(self.PROJECT_ID, self.ROUTE_ID),
                               headers={**self.auth_headers("admin"), "Content-Type": "application/json"}, body=body)

        status, response = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, None, 0)
        if status != 200:
            raise IntegrationFailure("explicit initial deactivation was not accepted")
        empty_receipt = self._receipt(
            response,
            replayed=False,
            project_id=self.PROJECT_ID,
            route_id=self.ROUTE_ID,
            previous_revision=0,
            preparation_id=None,
            snapshot_id=None,
        )
        if empty_receipt["revision"] != 1:
            raise IntegrationFailure("empty deactivation did not advance the route revision")
        after_empty = self.activation(self.PROJECT_ID, self.ROUTE_ID)
        if after_empty["revision"] != 1 or after_empty["preparation_id"] is not None:
            raise IntegrationFailure("empty deactivation did not persist an unpublished generation")

        first = self._prepare_activation_candidate(None, 1, "activation-first")
        status, response = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, first["id"], 1)
        if status != 200:
            raise IntegrationFailure("first explicit activation was not accepted")
        first_receipt = self._receipt(
            response,
            replayed=False,
            project_id=self.PROJECT_ID,
            route_id=self.ROUTE_ID,
            previous_revision=1,
            preparation_id=first["id"],
            snapshot_id=first["artifact_snapshot_id"],
        )
        first_active = self.activation(self.PROJECT_ID, self.ROUTE_ID)
        if first_active["revision"] != 2 or first_active["preparation_id"] != first["id"]:
            raise IntegrationFailure("first activation was not persisted as the active generation")
        self._strict_retrieval(first["id"], first_active["revision"])

        second = self._prepare_activation_candidate(first_active["snapshot_id"], 2, "activation-second")
        if first["artifact_snapshot_id"] != second["artifact_snapshot_id"]:
            raise IntegrationFailure("unchanged sources produced different activation candidate snapshot identities")

        publication = os.open(
            self._inspection_artifact(first["id"]) / "docs" / "kb" / ".shin-context.lock",
            os.O_RDONLY | os.O_NOFOLLOW,
        )
        lifetime = os.open(self._inspection_lock(first["id"]), os.O_RDONLY | os.O_NOFOLLOW)
        reader_results: list[tuple[int, dict[str, Any]]] = []
        reader_failures: list[BaseException] = []

        def read_first_generation() -> None:
            try:
                reader_results.append(self.post_retrieval(
                    self.PROJECT_ID,
                    "brief",
                    {"query": "snapshot admission evidence"},
                ))
            except BaseException as error:
                reader_failures.append(error)

        reader = threading.Thread(target=read_first_generation)
        try:
            fcntl.flock(publication, fcntl.LOCK_EX)
            reader.start()
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
                raise IntegrationFailure("old-generation reader did not acquire its lifetime descriptor before publication blocking")

            status, response = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, second["id"], 2)
            if status != 200:
                raise IntegrationFailure("second explicit activation was not accepted while an old reader was admitted")
            second_receipt = self._receipt(
                response,
                replayed=False,
                project_id=self.PROJECT_ID,
                route_id=self.ROUTE_ID,
                previous_revision=2,
                preparation_id=second["id"],
                snapshot_id=second["artifact_snapshot_id"],
            )
            second_active = self.activation(self.PROJECT_ID, self.ROUTE_ID)
            if second_active["revision"] != 3 or second_active["preparation_id"] != second["id"]:
                raise IntegrationFailure("second activation did not replace the active generation")
            status, response = self._retention_post(first["id"], "0" * 64)
            if status != 409 or response.get("error", {}).get("code") != "candidate-artifact-busy":
                raise IntegrationFailure("old-generation retention was not fenced by its admitted reader lifetime")
        finally:
            fcntl.flock(publication, fcntl.LOCK_UN)
            os.close(publication)
            os.close(lifetime)
            if reader.ident is not None:
                reader.join(timeout=10.0)
        if reader.is_alive() or reader_failures or len(reader_results) != 1:
            raise IntegrationFailure("old-generation reader did not complete after its publication lock was released")
        reader_status, reader_response = reader_results[0]
        reader_result = reader_response.get("result")
        if (reader_status != 200 or not isinstance(reader_result, dict) or reader_result.get("preparation_id") != first["id"] or
                reader_result.get("revision") != first_active["revision"]):
            raise IntegrationFailure("admitted old-generation reader did not retain its original generation metadata")

        first_plan = self._retention_get(first["id"])
        if first_plan["can_apply"] is not True or first_plan["plan_id"] is None:
            raise IntegrationFailure("superseded candidate was not eligible for retention")
        status, response = self._retention_post(first["id"], first_plan["plan_id"])
        if status != 200 or response.get("result", {}).get("state") != "released":
            raise IntegrationFailure("superseded candidate could not be released before historical replay")
        cache_before = self.expect_status(200, "GET", "/api/v1/cache", headers=self.auth_headers("admin")).get("result")
        if not isinstance(cache_before, dict) or type(cache_before.get("loads")) is not int:
            raise IntegrationFailure("cache diagnostics did not expose a valid generation load counter")
        self._strict_retrieval(second["id"], second_active["revision"])
        cache_after = self.expect_status(200, "GET", "/api/v1/cache", headers=self.auth_headers("admin")).get("result")
        if not isinstance(cache_after, dict) or cache_after.get("loads") != cache_before["loads"] + 1:
            raise IntegrationFailure("same-content successor did not acquire a separately owned resident generation")

        self.stop_server(expect_clean=True)
        self.start_server()
        if self.activation(self.PROJECT_ID, self.ROUTE_ID) != second_active:
            raise IntegrationFailure("active generation did not survive a clean server restart")

        stale = self._prepare_activation_candidate(second_active["snapshot_id"], 3, "activation-stale")
        status, response = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, stale["id"], 2)
        if status != 409 or response.get("error", {}).get("code") != "activation-conflict":
            raise IntegrationFailure("stale activation compare-and-swap did not fail with activation-conflict")
        self._assert_activation_unchanged(second_active)

        status, response = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, first["id"], 1)
        if status != 200:
            raise IntegrationFailure("historical activation receipt was not replayed after retention")
        replay = self._receipt(
            response,
            replayed=True,
            project_id=self.PROJECT_ID,
            route_id=self.ROUTE_ID,
            previous_revision=1,
            preparation_id=first["id"],
            snapshot_id=first["artifact_snapshot_id"],
        )
        if replay != first_receipt or self.activation(self.PROJECT_ID, self.ROUTE_ID) != second_active:
            raise IntegrationFailure("historical activation replay reactivated or rewrote a retired candidate")

        protected = self._retention_get(second["id"])
        if protected["can_apply"] is not False or protected["reason_codes"] != ["candidate-active"]:
            raise IntegrationFailure("active candidate retention did not fail closed")
        status, response = self._retention_post(second["id"], "0" * 64)
        if status != 409 or response.get("error", {}).get("code") != "candidate-active":
            raise IntegrationFailure("active candidate retention apply did not fail with candidate-active")
        if self.activation(self.PROJECT_ID, self.ROUTE_ID) != second_active:
            raise IntegrationFailure("active-candidate retention attempt changed the active generation")

        status, response = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, None, 3)
        if status != 200:
            raise IntegrationFailure("explicit deactivation after replacement was not accepted")
        deactivation = self._receipt(
            response,
            replayed=False,
            project_id=self.PROJECT_ID,
            route_id=self.ROUTE_ID,
            previous_revision=3,
            preparation_id=None,
            snapshot_id=None,
        )
        if deactivation["revision"] != 4:
            raise IntegrationFailure("deactivation after replacement did not advance the route revision")
        inactive = self.activation(self.PROJECT_ID, self.ROUTE_ID)
        if inactive["revision"] != 4 or inactive["preparation_id"] is not None or inactive["snapshot_id"] is not None:
            raise IntegrationFailure("explicit deactivation did not remove the active target")

        self.activation_followup_job = self._prepare_activation_candidate(None, 4, "activation-followup")
        if second_receipt["revision"] != 3:
            raise IntegrationFailure("second activation receipt changed unexpectedly")

    def check_activation_audit_rollback_and_lost_response(self) -> None:
        before = self.activation(self.PROJECT_ID, self.ROUTE_ID)
        candidate = self.activation_followup_job
        receipts_before = int(self.scalar(
            "SELECT count(*) FROM shin_server.activation_receipts WHERE project_id='visible' AND route_id='visible-retrieval'"
        ))
        trigger_name = "activation_audit_reject_trigger"
        function_name = "activation_audit_reject_function"
        self.psql(
            "CREATE OR REPLACE FUNCTION shin_server." + function_name + "() RETURNS trigger LANGUAGE plpgsql AS "
            "$$ BEGIN IF NEW.action = 'generation.activated' THEN RAISE EXCEPTION 'activation audit rejected'; END IF; "
            "RETURN NEW; END $$"
        )
        self.psql(
            "CREATE TRIGGER " + trigger_name + " BEFORE INSERT ON shin_server.audit_events "
            "FOR EACH ROW EXECUTE FUNCTION shin_server." + function_name + "()"
        )
        try:
            status, _ = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, candidate["id"], before["revision"])
            if status < 500:
                raise IntegrationFailure("activation audit rejection did not fail the activation transaction")
        finally:
            self.psql("DROP TRIGGER IF EXISTS " + trigger_name + " ON shin_server.audit_events")
            self.psql("DROP FUNCTION IF EXISTS shin_server." + function_name + "()")
        if self.activation(self.PROJECT_ID, self.ROUTE_ID) != before:
            raise IntegrationFailure("failed activation audit changed the active-generation head")
        receipts_after_failure = int(self.scalar(
            "SELECT count(*) FROM shin_server.activation_receipts WHERE project_id='visible' AND route_id='visible-retrieval'"
        ))
        if receipts_after_failure != receipts_before or self._activation_audit_count(candidate["id"]) != 0:
            raise IntegrationFailure("failed activation audit retained a receipt or journal event")

        status, response = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, candidate["id"], before["revision"])
        if status != 200:
            raise IntegrationFailure("activation did not retry after audit rollback")
        receipt = self._receipt(
            response,
            replayed=False,
            project_id=self.PROJECT_ID,
            route_id=self.ROUTE_ID,
            previous_revision=before["revision"],
            preparation_id=candidate["id"],
            snapshot_id=candidate["artifact_snapshot_id"],
        )
        active = self.activation(self.PROJECT_ID, self.ROUTE_ID)
        if active["revision"] != receipt["revision"] or active["preparation_id"] != candidate["id"]:
            raise IntegrationFailure("successful activation retry did not persist its head")
        audit_reference = self.scalar(
            "SELECT activation_route_id || ':' || activation_previous_revision FROM shin_server.audit_events "
            "WHERE action='generation.activated' AND preparation_id=" + str(candidate["id"])
        )
        if audit_reference != self.ROUTE_ID + ":" + str(before["revision"]):
            raise IntegrationFailure("activation audit did not retain its route and previous revision receipt key")

        lost = self._prepare_activation_candidate(active["snapshot_id"], active["revision"], "activation-lost-response")
        self._post_activation_without_response(lost["id"], active["revision"])
        deadline = time.monotonic() + 10.0
        committed: dict[str, Any] | None = None
        while time.monotonic() < deadline:
            observed = self.activation(self.PROJECT_ID, self.ROUTE_ID)
            if observed["revision"] == active["revision"] + 1 and observed["preparation_id"] == lost["id"]:
                committed = observed
                break
            time.sleep(0.05)
        if committed is None:
            raise IntegrationFailure("lost-response activation did not durably advance the active-generation head")
        status, response = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, lost["id"], active["revision"])
        if status != 200:
            raise IntegrationFailure("lost-response activation did not replay its durable receipt")
        replay = self._receipt(
            response,
            replayed=True,
            project_id=self.PROJECT_ID,
            route_id=self.ROUTE_ID,
            previous_revision=active["revision"],
            preparation_id=lost["id"],
            snapshot_id=lost["artifact_snapshot_id"],
        )
        if replay["revision"] != committed["revision"] or self._activation_audit_count(lost["id"]) != 1:
            raise IntegrationFailure("lost-response activation replay duplicated or lost its activation audit")
        status, _ = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, None, committed["revision"])
        if status != 200:
            raise IntegrationFailure("lost-response activation fixture could not be explicitly deactivated")

    def check_activation_rejects_preflight_failures(self) -> None:
        before = self.activation(self.PROJECT_ID, self.ROUTE_ID)
        if before["preparation_id"] is not None:
            raise IntegrationFailure("quality-rejection fixture requires an explicitly inactive route")
        for profile_id in ("preflight-eval-failing", "preflight-pack-failing", "preflight-not-configured"):
            self.stop_server(expect_clean=True)
            failed = self._prepare_preflight_candidate(profile_id)
            status, response = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, failed["id"], before["revision"])
            if status != 409 or response.get("error", {}).get("code") != "activation-preflight-failed":
                raise IntegrationFailure("activation did not reject " + profile_id + " strict retrieval quality")
            self._assert_activation_unchanged(before)
        self.stop_server(expect_clean=True)
        self._configure_preflight_route("preflight-passing")
        self.start_server()

    def run_activation_checks(self) -> None:
        if self.server is not None:
            raise IntegrationFailure("activation checks require the owned server to be stopped")
        self._configure_preflight_route("preflight-passing")
        self.start_server()
        self.record("activation-contract-and-transitions", self.check_activation_contract_and_transitions)
        self.record("activation-audit-rollback-and-lost-response", self.check_activation_audit_rollback_and_lost_response)
        self.record("activation-strict-quality-rejection", self.check_activation_rejects_preflight_failures)
        self.record("activation-small-fixture-baseline", self.check_activation_baseline)

    def check_activation_baseline(self) -> None:
        samples: list[float] = []
        deactivation_samples: list[float] = []
        for index in range(5):
            base = self.activation(self.PROJECT_ID, self.ROUTE_ID)
            candidate = self._prepare_activation_candidate(base["snapshot_id"], base["revision"], f"activation-bench-{index}")
            started = time.perf_counter_ns()
            status, response = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, candidate["id"], base["revision"])
            samples.append((time.perf_counter_ns() - started) / 1_000_000)
            if status != 200:
                raise IntegrationFailure("measured activation failed")
            self._receipt(response, replayed=False, project_id=self.PROJECT_ID, route_id=self.ROUTE_ID,
                          previous_revision=base["revision"], preparation_id=candidate["id"], snapshot_id=candidate["artifact_snapshot_id"])
            self._strict_retrieval(candidate["id"], base["revision"] + 1)
            started = time.perf_counter_ns()
            status, response = self._post_activation(self.PROJECT_ID, self.ROUTE_ID, None, base["revision"] + 1)
            deactivation_samples.append((time.perf_counter_ns() - started) / 1_000_000)
            if status != 200:
                raise IntegrationFailure("measured deactivation failed")
            self._release_candidate(candidate)
        self.activation_measurement = {
            "samples": len(samples), "activation_p50_ms": self.percentile_ms(samples, 50),
            "activation_p95_ms": self.percentile_ms(samples, 95),
            "deactivation_p50_ms": self.percentile_ms(deactivation_samples, 50),
            "deactivation_p95_ms": self.percentile_ms(deactivation_samples, 95),
            "scope": "Five sequential small-fixture HTTP activations with full preflight; preparation and cleanup excluded. Not throughput, soak or a production SLO.",
        }
