"""P3E13 HTTP/SQL checks, invoked only by the guarded publication harness."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import copy
import json
import threading
from typing import Any, Callable

from publication_test_support import IDENTITY_FIELDS, exact_object, require, withdrawal_receipt


def withdraw(suite: Any, publication: str, revision: int, proposal_id: int,
             preparation_id: int, *, replayed: bool = False) -> dict[str, Any]:
    status, response = suite.post_json(suite.path(publication, "/withdrawal"), {"expected_revision": revision})
    require(status == 200, f"withdrawal returned HTTP {status}")
    return assert_withdrawal(suite, response, publication, revision, proposal_id, preparation_id, replayed=replayed)


def assert_withdrawal(suite: Any, response: dict[str, Any], publication: str, revision: int,
                      proposal_id: int, preparation_id: int, *, replayed: bool) -> dict[str, Any]:
    result = exact_object(suite.public_result(response), {"replayed", "receipt"}, "withdrawal result")
    require(result["replayed"] is replayed, "withdrawal replay marker differs")
    receipt = withdrawal_receipt(result["receipt"], "visible", publication, revision, proposal_id, preparation_id)
    row = suite.sql_json("SELECT to_jsonb(w) || jsonb_build_object('created_at', "
                         "to_char(w.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')) "
                         "FROM shin_server.publication_withdrawals w WHERE id=" + str(receipt["id"]))
    for field in receipt.keys() - {"schema_version"}:
        require(row[field] == receipt[field], "withdrawal HTTP/SQL mismatch: " + field)
    require(suite.scalar("SELECT count(*) FROM shin_server.publication_withdrawals WHERE previous_proposal_id=" +
                         str(proposal_id)) == "1", "withdrawal duplicated its accepted proposal")
    suite.assert_publication_audit(proposal_id, preparation_id, "publication.withdrawn", "dev-admin")
    return receipt


class PublicationWithdrawalChecks:
    def __init__(self, suite: Any) -> None:
        self.s = suite

    def run(self) -> None:
        for name, check in (
            ("lifecycle-and-historical-base", self.lifecycle),
            ("fresh-warm-refusal-before-source", self.retrieval_refusal),
            ("denied-invalid-no-side-effects", self.invalid_requests),
            ("concurrent-exact-retries", self.concurrent_retries),
            ("accept-versus-withdraw-cas", self.accept_race),
            ("audit-transaction-rollback", self.audit_rollback),
            ("retention-pins-and-private-route", self.retention),
            ("restart-and-lost-reply", self.lost_reply),
            ("missing-active-pointer-corruption", self.missing_active_pointer),
        ):
            self.s.record("publication-withdrawal-" + name, check)

    def seed(self, publication: str, job: dict[str, Any] | None = None) -> tuple[dict[str, Any], int]:
        s = self.s
        if job is None:
            job = s.jobs[s.get_head()["preparation_id"]]
        proposal = s.submit(job, 0, publication + "-seed", publication)
        proposal_id = proposal["proposal"]["id"]
        s.resolution(proposal_id, "accept", 0, publication)
        require(s.get_head(publication)["state"] == "active", "accepted publication is not active")
        return job, proposal_id

    def withdrawn_head(self, publication: str, revision: int) -> dict[str, Any]:
        s = self.s
        current = s.get_head(publication, role="reader")
        require(current == {**dict.fromkeys(IDENTITY_FIELDS), "project_id": "visible",
                            "publication_id": publication, "revision": revision, "state": "withdrawn", "history_epoch": 0},
                "withdrawn head did not retain its revision with null selection")
        require(s.scalar("SELECT count(*) FROM shin_server.publication_heads WHERE project_id='visible' "
                          "AND publication_id=" + s._sql_text(publication)) == "0",
                "withdrawal retained an active pointer")
        return current

    def transition_delta(self, before: dict[str, Any], *, accepted: bool = False) -> None:
        after = self.s.durable_state()
        changed = {"publication_heads", "audit_events", "publication_resolutions" if accepted else "publication_withdrawals"}
        for table in before.keys() - changed:
            require(after[table] == before[table], "transition unexpectedly mutated " + table)
        for table in changed - {"publication_heads"}:
            require(len(after[table]) == len(before[table]) + 1, "transition did not append exactly one " + table)
            require(all(row in after[table] for row in before[table]), "transition rewrote historical " + table)

    def lifecycle(self) -> None:
        s, publication = self.s, "withdraw-lifecycle"
        job, proposal_id = self.seed(publication)
        before = s.durable_state()
        receipt = withdraw(s, publication, 1, proposal_id, job["id"])
        self.transition_delta(before)
        self.withdrawn_head(publication, 2)
        require(s.get_detail(proposal_id, publication)["current"]["state"] == "withdrawn",
                "proposal detail hid withdrawn current state")
        before = s.durable_state()
        s.resolution(proposal_id, "accept", 0, publication, replayed=True)
        self.withdrawn_head(publication, 2)
        s.unchanged(before, "historical acceptance replay after withdrawal")
        next_proposal = s.submit(job, 2, "withdraw-republish", publication)
        require(next_proposal["base"]["state"] == "withdrawn" and
                next_proposal["proposal"]["base_preparation_id"] is None and
                next_proposal["proposal"]["base_withdrawal_id"] == receipt["id"],
                "proposal did not bind the typed withdrawn base")
        accepted = s.resolution(next_proposal["proposal"]["id"], "accept", 2, publication)
        require(accepted["revision"] == 3 and s.get_head(publication)["state"] == "active", "republish reset logical revision")
        s.assert_retrieval(job, 3, publication)
        before = s.durable_state()
        require(withdraw(s, publication, 1, proposal_id, job["id"], replayed=True) == receipt,
                "old withdrawal replay returned a new receipt at a later head")
        s.unchanged(before, "historical withdrawal replay at active head")
        # Both historical accepted and withdrawn revisions stay legal proposal bases.
        for revision, state, withdrawal_id in ((1, "active", None), (2, "withdrawn", receipt["id"])):
            historical = s.submit(job, revision, "withdraw-historical-" + str(revision), publication)
            require(historical["base"]["state"] == state and
                    historical["proposal"]["base_withdrawal_id"] == withdrawal_id and
                    historical["proposal"]["state"] == "conflicted", "historical base was replaced with current head")
            s.resolution(historical["proposal"]["id"], "reject", 3, publication)
        second = withdraw(s, publication, 3, next_proposal["proposal"]["id"], job["id"])
        require(second["id"] != receipt["id"], "different accepted heads shared a withdrawal identity")
        self.withdrawn_head(publication, 4)
        before = s.durable_state()
        require(withdraw(s, publication, 1, proposal_id, job["id"], replayed=True) == receipt,
                "old withdrawal replay changed at a later withdrawn head")
        s.unchanged(before, "historical withdrawal replay at withdrawn head")
        s.assert_source_bases()

    def refuse_retrieval(self, publication: str) -> None:
        s = self.s
        before = s.durable_state()
        cache = s.expect_status(200, "GET", "/api/v1/cache", headers=s.auth_headers("admin"))["result"]
        for operation in ("search", "brief", "pack"):
            body = {"task_request": s.task_request()} if operation == "pack" else {"query": "AlphaWorkspaceEvidence"}
            for strict in (False, True):
                status, response = s.post_json(s.path(publication, "/" + operation),
                                               {**body, "require_current": strict}, role="reader")
                s._assert_error(status, response, 410, "publication-withdrawn")
        require(s.expect_status(200, "GET", "/api/v1/cache", headers=s.auth_headers("admin"))["result"] == cache,
                "withdrawn retrieval touched generation cache admission")
        s.unchanged(before, "withdrawn retrieval refusals")

    def retrieval_refusal(self) -> None:
        s, publication = self.s, "withdraw-retrieval"
        job, proposal_id = self.seed(publication)
        s.assert_retrieval(job, 1, publication)
        roots = (s.fixture_root, s.peer_root, s.preparation_root)
        trees = [s.tree_digest(root) for root in roots]
        # Source disappearance must not prevent withdrawal itself, even with a warm selection.
        source = s.fixture_root if job["workspace_id"] == "primary" else s.peer_root
        missing = source.with_name(source.name + "-withdrawal-unavailable")
        require(not missing.exists(), "withdrawal source relocation target exists")
        original = json.loads(s.runtime_config_path.read_text(encoding="utf-8"))
        source.rename(missing)
        try:
            receipt = withdraw(s, publication, 1, proposal_id, job["id"])
            self.withdrawn_head(publication, 2)
            self.refuse_retrieval(publication)
            s.stop_server(expect_clean=True)
            changed = copy.deepcopy(original)
            changed["retrieval"]["routes"] = [route for route in changed["retrieval"]["routes"]
                                                if route["root"] != str(source)]
            s.replace_private_json(s.runtime_config_path, changed)
            s.start_server()
            # Cold process, missing source, and absent source-catalog entry must still produce 410.
            self.refuse_retrieval(publication)
            before = s.durable_state()
            require(withdraw(s, publication, 1, proposal_id, job["id"], replayed=True) == receipt,
                    "replay required a configured or present source")
            s.unchanged(before, "source-free withdrawal replay")
        finally:
            s.stop_server(expect_clean=False)
            missing.rename(source)
            s.replace_private_json(s.runtime_config_path, original)
            s.start_server()
        require(trees == [s.tree_digest(root) for root in roots], "withdrawal modified sources or artifacts")
        s.assert_source_bases()

    def invalid_requests(self) -> None:
        s, publication = self.s, "withdraw-invalid"
        job, proposal_id = self.seed(publication)
        path = s.path(publication, "/withdrawal")
        before = s.durable_state()
        s.expect_status(401, "POST", path, headers={"Content-Type": "application/json"}, body=b'{"expected_revision":1}')
        status, _ = s.post_json(path, {"expected_revision": 1}, role="reader")
        require(status == 403, "reader could withdraw publication")
        status, _ = s.post_json(s.path(publication, "/withdrawal", project="missing"), {"expected_revision": 1})
        require(status == 404, "missing project withdrawal was not scoped")
        for value in ({}, None, [], {"expected_revision": True}, {"expected_revision": -1},
                      {"expected_revision": 1.0}, {"expected_revision": "1"},
                      {"expected_revision": 9223372036854775807}, {"expected_revision": 9223372036854775808},
                      {"expected_revision": 1, "reason": "extra"}, {"expected_revision": 1, "preparation_id": job["id"]},
                      b'{"expected_revision":1,"expected_revision":1}', b'{'):
            status, _ = s.post_json(path, value)
            require(status == 400, "invalid withdrawal body was not rejected exactly")
        status, _ = s.post_json(path + "?expected_revision=1", {"expected_revision": 1})
        require(status == 400, "withdrawal accepted query parameters")
        for name, revision in ((publication, 0), (publication, 2), ("withdraw-empty", 0)):
            status, response = s.post_json(s.path(name, "/withdrawal"), {"expected_revision": revision})
            s._assert_error(status, response, 409, "publication-conflict")
        status, response = s.post_json(s.path(publication, "/proposals"),
                                       {"preparation_id": job["id"], "base_revision": 2}, key="withdraw-future-base")
        s._assert_error(status, response, 409, "publication-conflict")
        s.unchanged(before, "denied and invalid withdrawal")
        withdraw(s, publication, 1, proposal_id, job["id"])
        before = s.durable_state()
        for revision in (0, 2, 3):
            status, response = s.post_json(path, {"expected_revision": revision})
            s._assert_error(status, response, 409, "publication-conflict")
        status, _ = s.post_json(path, {"expected_revision": 1}, role="reader")
        require(status == 403, "receipt replay bypassed administrator policy")
        s.unchanged(before, "already-withdrawn and denied replay")

    @staticmethod
    def race(actions: list[Callable[[], tuple[int, dict[str, Any]]]]) -> list[tuple[int, dict[str, Any]]]:
        barrier = threading.Barrier(len(actions))

        def call(action: Callable[[], tuple[int, dict[str, Any]]]) -> tuple[int, dict[str, Any]]:
            barrier.wait(timeout=5.0)
            return action()

        with ThreadPoolExecutor(max_workers=len(actions)) as pool:
            futures = [pool.submit(call, action) for action in actions]
            return [future.result(timeout=25.0) for future in futures]

    def concurrent_retries(self) -> None:
        s, publication = self.s, "withdraw-concurrent"
        job, proposal_id = self.seed(publication)
        before = s.durable_state()
        results = self.race([lambda: s.post_json(s.path(publication, "/withdrawal"), {"expected_revision": 1})] * 4)
        require([status for status, _ in results] == [200] * 4, "exact concurrent withdrawal retries did not all acknowledge")
        markers = [s.public_result(response)["replayed"] for _, response in results]
        require(sum(marker is False for marker in markers) == 1 and sum(marker is True for marker in markers) == 3,
                "concurrent retries did not elect exactly one new withdrawal")
        receipts = [assert_withdrawal(s, response, publication, 1, proposal_id, job["id"], replayed=marker)
                    for (_, response), marker in zip(results, markers)]
        require(all(receipt == receipts[0] for receipt in receipts), "concurrent replies differ")
        self.transition_delta(before)
        self.withdrawn_head(publication, 2)

    def accept_race(self) -> None:
        s, publication = self.s, "withdraw-accept-race"
        job, previous = self.seed(publication)
        next_proposal = s.submit(job, 1, "withdraw-race-next", publication)["proposal"]["id"]
        before = s.durable_state()
        results = self.race([
            lambda: s.resolve_request(next_proposal, "accept", 1, publication, retry_busy=True),
            lambda: s.post_json(s.path(publication, "/withdrawal"), {"expected_revision": 1}),
        ])
        require(sorted(status for status, _ in results) == [200, 409], "accept/withdraw did not have exactly one CAS winner")
        accepted = results[0][0] == 200
        s.assert_error_code(results[1 if accepted else 0][1], "publication-conflict")
        self.transition_delta(before, accepted=accepted)
        if accepted:
            s.assert_receipt(results[0][1], next_proposal, "accept", 1, publication, replayed=False)
            current = s.get_head(publication)
            require(current["state"] == "active" and current["revision"] == 2 and current["preparation_id"] == job["id"],
                    "withdrawal overwrote accepted winner")
            s.assert_head_sql(current)
        else:
            assert_withdrawal(s, results[1][1], publication, 1, previous, job["id"], replayed=False)
            self.withdrawn_head(publication, 2)
            require(s.get_detail(next_proposal, publication)["proposal"]["state"] == "conflicted", "losing accept became terminal")
        before = s.durable_state()
        status, response = (s.post_json(s.path(publication, "/withdrawal"), {"expected_revision": 1}) if accepted else
                            s.resolve_request(next_proposal, "accept", 1, publication, retry_busy=True))
        s._assert_error(status, response, 409, "publication-conflict")
        s.unchanged(before, "stale race loser retry")

    def audit_rollback(self) -> None:
        s, publication = self.s, "withdraw-audit"
        job, proposal_id = self.seed(publication)
        before = s.durable_state()
        with s.fail_publication_audit():
            status, _ = s.post_json(s.path(publication, "/withdrawal"), {"expected_revision": 1})
            require(status >= 500, "audit failure did not abort withdrawal")
        s.unchanged(before, "withdrawal audit rollback")
        require(s.get_head(publication)["state"] == "active", "audit rollback lost active pointer")
        withdraw(s, publication, 1, proposal_id, job["id"])
        self.transition_delta(before)

    def retention(self) -> None:
        s, publication = self.s, "withdraw-retention"
        job, proposal_id = self.seed(publication, s.prepare_candidate())
        artifact = s._inspection_artifact(job["id"])
        trees = [s.tree_digest(root) for root in (s.fixture_root, s.peer_root, s.preparation_root)]
        _, other_id = self.seed("withdraw-other-pin", job)
        pin = s.submit(job, 1, "withdraw-pending-pin", publication)["proposal"]["id"]
        receipt = withdraw(s, publication, 1, proposal_id, job["id"])
        require(trees == [s.tree_digest(root) for root in (s.fixture_root, s.peer_root, s.preparation_root)],
                "withdrawal deleted or rewrote an artifact")
        for stage in ("other-head-and-proposal", "proposal-only"):
            plan = s._retention_get(job["id"])
            require(plan["can_apply"] is False and plan["reason_codes"] == ["candidate-active"], stage + " pin was lost")
            before = s.durable_state()
            status, response = s._retention_post(job["id"], "0" * 64)
            s._assert_error(status, response, 409, "candidate-active")
            s.unchanged(before, stage + " retention refusal")
            if stage == "other-head-and-proposal":
                withdraw(s, "withdraw-other-pin", 1, other_id, job["id"])
        s.resolution(pin, "reject", 2, publication)
        s._release_candidate(job)
        require(not artifact.exists(), "withdrawal ledger retained a released head's artifact pin")
        before = s.durable_state()
        require(withdraw(s, publication, 1, proposal_id, job["id"], replayed=True) == receipt,
                "released artifacts broke withdrawal replay")
        s.unchanged(before, "released artifact withdrawal replay")
        # A private active route independently pins its candidate, and remains usable.
        local_job = s.prepare_candidate("withdraw-private")
        _, local_id = self.seed("withdraw-private-pin", local_job)
        local = s.activation("visible", local_job["route_id"])
        status, response = s._post_activation("visible", local_job["route_id"], local_job["id"], local["revision"])
        require(status == 200, "private pin fixture activation failed")
        s._receipt(response, replayed=False, project_id="visible", route_id=local_job["route_id"],
                   previous_revision=local["revision"], preparation_id=local_job["id"],
                   snapshot_id=local_job["artifact_snapshot_id"])
        s.local_heads[local_job["route_id"]] = s.activation("visible", local_job["route_id"])
        withdraw(s, "withdraw-private-pin", 1, local_id, local_job["id"])
        plan = s._retention_get(local_job["id"])
        require(plan["can_apply"] is False and plan["reason_codes"] == ["candidate-active"], "private active pin was released")
        status, response = s.post_json(f"/api/v1/projects/visible/routes/{local_job['route_id']}/search",
                                       {"query": "AlphaWorkspaceEvidence"}, role="reader")
        require(status == 200 and response["result"]["preparation_id"] == local_job["id"], "withdrawal disabled private source retrieval")
        s.assert_source_bases()

    def lost_reply(self) -> None:
        s, publication = self.s, "withdraw-lost-reply"
        job, proposal_id = self.seed(publication)
        before = s.durable_state()
        s.send_without_reply(s.path(publication, "/withdrawal"), {"expected_revision": 1})
        s.wait_for_sql_count("publication_withdrawals", len(before["publication_withdrawals"]) + 1)
        self.transition_delta(before)
        current = self.withdrawn_head(publication, 2)
        receipt = withdraw(s, publication, 1, proposal_id, job["id"], replayed=True)
        before = s.durable_state()
        s.kill_owned_server()
        s.start_server()
        require(s.get_head(publication) == current, "restart reset withdrawn head")
        require(withdraw(s, publication, 1, proposal_id, job["id"], replayed=True) == receipt, "restart changed lost-reply receipt")
        self.refuse_retrieval(publication)
        s.unchanged(before, "restart and lost withdrawal reply")
        proposal = s.submit(job, 2, "withdraw-restart-republish", publication)["proposal"]["id"]
        s.resolution(proposal, "accept", 2, publication)
        active = s.get_head(publication)
        before = s.durable_state()
        s.kill_owned_server()
        s.start_server()
        require(s.get_head(publication) == active and active["revision"] == 3, "restart lost republished transition")
        require(withdraw(s, publication, 1, proposal_id, job["id"], replayed=True) == receipt, "restart replay cleared newer head")
        s.unchanged(before, "restart after republish")

    def missing_active_pointer(self) -> None:
        s, publication = self.s, "withdraw-corrupt-pointer"
        _, proposal_id = self.seed(publication)
        before = s.durable_state()
        pointer = next(row for row in before["publication_heads"] if row["project_id"] == "visible" and
                       row["publication_id"] == publication)
        s.psql("DELETE FROM shin_server.publication_heads WHERE project_id='visible' AND publication_id=" + s._sql_text(publication))
        try:
            damaged = s.durable_state()
            for suffix in ("", f"/proposals/{proposal_id}"):
                response = s.expect_status(500, "GET", s.path(publication, suffix), headers=s.auth_headers("reader"))
                s.assert_error_code(response, "database-corrupt")
            s.unchanged(damaged, "missing pointer corruption reads")
        finally:
            s.psql("INSERT INTO shin_server.publication_heads SELECT * FROM jsonb_populate_record("
                   "NULL::shin_server.publication_heads," + s._sql_text(json.dumps(pointer)) + "::jsonb)")
        s.unchanged(before, "restored active pointer fixture")
        require(s.get_head(publication)["state"] == "active" and
                s.get_detail(proposal_id, publication)["current"]["state"] == "active", "pointer fixture did not recover")
