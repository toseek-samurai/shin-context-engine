"""R1 private checkpoint HTTP checks for the existing publication integration stand.

The parent suite owns PostgreSQL, migrations, server lifetime and fixture roots.
This sidecar only uses authenticated HTTP plus bounded private draft files needed
to construct a native receipt scope; it restores any intentionally drifted source.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import shutil
import time
from typing import Any
from urllib.parse import urlencode

from integration_test import IntegrationFailure
from publication_integration_test import PEER_ROUTE
from publication_test_support import no_private_values, require
from retrieval_integration_test import PROJECT_ID, ROUTE_ID
from authenticated_receipt_checks import check_authenticated_import, check_origin_migration


CHECKPOINT_ROOT = "/api/v1/projects/visible/task-checkpoints"
REQUEST_ID = re.compile(r"[0-9a-f]{32}\Z")
PRIVATE_RECORD_KEYS = {
    "checkpoint_text", "request_hash", "request_id", "request_deadline", "principal_id",
    "root", "out_root", "artifact_key", "canonical_payload", "receipt_origins",
}


class CheckpointIntegrationChecks:
    """Exercise private checkpoint state without taking ownership of the stand."""

    def __init__(self, suite: Any) -> None:
        self.s = suite
        self.records: dict[tuple[str, str], dict[str, Any]] = {}
        self.drafts = self.s.state_root / "checkpoint-sidecar-drafts"

    def run(self) -> None:
        try:
            for name, check in (
                ("checkpoint-save-replay-update-and-cas", self.check_save_replay_update_and_cas),
                ("checkpoint-owner-task-and-read-isolation", self.check_owner_task_and_read_isolation),
                ("checkpoint-resume-impact-restart-and-private-projection", self.check_resume_impact_restart_and_private_projection),
                ("checkpoint-publication-scope-and-currentness", self.check_publication_scope_and_currentness),
                ("checkpoint-deadline-body-and-receipt-boundaries", self.check_deadline_body_and_receipt_boundaries),
                ("checkpoint-origin-migration-preserves-saved-state", lambda: check_origin_migration(self)),
                ("checkpoint-authenticated-receipt-import", lambda: check_authenticated_import(self)),
                ("checkpoint-clear-prune-recreate-and-capacity", self.check_clear_prune_recreate_and_capacity),
                ("checkpoint-schema-and-grant-tamper-readiness", self.check_schema_and_grant_tamper_readiness),
            ):
                self.s.record(name, check)
        finally:
            self._cleanup()

    def _forbidden(self) -> list[str]:
        return [value for value in (
            str(self.s.fixture_root), str(self.s.peer_root), str(self.s.other_root), str(self.s.out_dir),
            str(self.s.other_out_dir), str(self.s.preparation_root), self.s.admin_token, self.s.reader_token,
        ) if value]

    @staticmethod
    def _json(value: Any) -> bytes:
        return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")

    @staticmethod
    def _request_id(seed: int) -> str:
        value = f"{seed:032x}"
        require(REQUEST_ID.fullmatch(value) is not None, "checkpoint fixture generated an invalid request id")
        return value

    @staticmethod
    def _deadline(seconds: int = 120) -> int:
        return int(time.time()) + seconds

    @staticmethod
    def _slot(name: str) -> str:
        return "checkpoint-" + name

    def _path(self, slot: str = "", suffix: str = "") -> str:
        return CHECKPOINT_ROOT + ("/" + slot if slot else "") + suffix

    def _request(self, method: str, path: str, body: Any | None = None, *, role: str = "admin") -> tuple[int, dict[str, Any]]:
        headers = self.s.auth_headers(role)
        payload = None
        if body is not None:
            headers = {**headers, "Content-Type": "application/json"}
            payload = body if isinstance(body, bytes) else self._json(body)
        status, envelope = self.s.request(method, path, headers=headers, body=payload)
        no_private_values(envelope, self._forbidden())
        return status, envelope

    def _error(self, status: int, envelope: dict[str, Any], expected_status: int, code: str) -> None:
        require(status == expected_status and envelope.get("error", {}).get("code") == code,
                f"checkpoint request returned {status}/{envelope.get('error', {}).get('code')} instead of {expected_status}/{code}")

    @staticmethod
    def _draft(*, marker: str = "checkpoint", receipts: list[dict[str, Any]] | None = None) -> dict[str, Any]:
        return {
            "schema_version": "shin.session-draft.v1",
            "task_request": {
                "schema_version": "shin.task-request.v1",
                "request_id": "checkpoint-neutral-request",
                "task": {
                    "objective": "Preserve neutral checkpoint state without granting external authority.",
                    "must": [{"id": "route-policy", "query": "project identity profile fingerprint"}],
                },
                "budget": {
                    "max_evidence_bytes": 8192,
                    "max_records": 6,
                    "per_query_records": 6,
                    "per_document_records": 2,
                },
            },
            "state": {
                "current_slice": "Checkpoint sidecar " + marker,
                "assertions": [{
                    "id": "authority", "kind": "invariant",
                    "text": "The host retains external authority.", "paths": ["docs/policy.md"],
                    "receipt_ids": [receipt["receipt_id"] for receipt in receipts or []],
                }],
                "changed_paths": ["docs/policy.md"],
                "identifiers": ["CheckpointIntegration"],
                "next_actions": [{"id": "review", "text": "Review the checkpoint state."}],
            },
            "receipts": receipts or [], "vcs": None, "ttl_seconds": 3600,
        }

    def _save_payload(self, *, task: str, thread: str, expected_revision: int, request_id: str,
                      require_current: bool = True, selector: dict[str, str] | None = None,
                      deadline: int | None = None, draft: dict[str, Any] | None = None) -> dict[str, Any]:
        return {
            "schema_version": "shin.task-checkpoint-write.v1",
            "selector": selector or {"kind": "route", "id": ROUTE_ID},
            "task_id": task,
            "thread_id": thread,
            "expected_revision": expected_revision,
            "request_id": request_id,
            "request_deadline": self._deadline() if deadline is None else deadline,
            "require_current": require_current,
            "draft": draft or self._draft(marker=task),
        }

    def _metadata(self, value: Any, *, task: str | None = None, thread: str | None = None,
                  active: bool | None = None) -> dict[str, Any]:
        require(isinstance(value, dict), "checkpoint metadata is not an object")
        require(value.get("schema_version") == "shin.task-checkpoint.v1", "checkpoint metadata schema differs")
        require(type(value.get("revision")) is int and value["revision"] >= 1,
                "checkpoint metadata omitted a positive globally monotonic revision")
        require(type(value.get("retry_until")) is int and value["retry_until"] >= 0,
                "checkpoint metadata omitted its persisted replay deadline fence")
        if task is not None:
            require(value.get("task_id") == task and value.get("thread_id") == thread,
                    "checkpoint metadata crossed task or thread identity")
        if active is not None:
            require((value.get("state") == "active") is active, "checkpoint metadata state differs")
        require(not (PRIVATE_RECORD_KEYS & set(value)), "checkpoint metadata exposed internal store fields")
        no_private_values(value, self._forbidden())
        return value

    def _save(self, slot: str, payload: dict[str, Any], *, role: str = "admin", replayed: bool | None = None) -> dict[str, Any]:
        status, envelope = self._request("POST", self._path(slot), payload, role=role)
        require(status in {200, 201}, f"checkpoint save returned {status}/{envelope.get('error', {}).get('code')}")
        result = envelope.get("result")
        require(isinstance(result, dict) and type(result.get("replayed")) is bool,
                "checkpoint save result omitted replay metadata")
        if replayed is not None:
            require(result["replayed"] is replayed, "checkpoint replay marker differs")
        metadata = self._metadata({key: item for key, item in result.items() if key != "replayed"},
                                  task=payload["task_id"], thread=payload["thread_id"], active=True)
        self.records[(role, slot)] = metadata
        return {"status": status, "metadata": metadata, "result": result}

    def _get(self, slot: str, *, role: str = "admin", expected: int = 200) -> dict[str, Any]:
        status, envelope = self._request("GET", self._path(slot), role=role)
        if status != expected:
            raise IntegrationFailure(f"checkpoint GET returned HTTP {status}, expected {expected}")
        if expected != 200:
            return envelope
        return self._metadata(envelope.get("result"))

    def _resume(self, slot: str, task: str, thread: str, *, impact: bool = False,
                require_current: bool = True, role: str = "admin") -> tuple[int, dict[str, Any]]:
        schema = "shin.task-impact-request.v1" if impact else "shin.task-resume-request.v1"
        suffix = "/impact" if impact else "/resume"
        return self._request("POST", self._path(slot, suffix), {
            "schema_version": schema, "task_id": task, "thread_id": thread, "require_current": require_current,
        }, role=role)

    def _assessment(self, result: Any, *, impact: bool) -> dict[str, Any]:
        require(isinstance(result, dict) and result.get("read_only") is True,
                "continuity result did not mark the operation read-only")
        require(result.get("schema_version") == ("shin.task-impact.v1" if impact else "shin.task-resume.v1"),
                "continuity result schema differs")
        metadata = self._metadata(result.get("checkpoint"))
        assessment = result.get("assessment")
        require(isinstance(assessment, dict) and assessment.get("schema_version") ==
                ("shin.task-impact-assessment.v1" if impact else "shin.task-resume-assessment.v1"),
                "continuity assessment projection schema differs")
        rendered = json.dumps(result, sort_keys=True)
        for private in ("\"root\"", "\"out_root\"", "\"checkpoint_text\"", "\"request_hash\"", "\"argv\""):
            require(private not in rendered, "continuity response exposed a host-private coordinate or command")
        no_private_values(result, self._forbidden())
        require(isinstance(result.get("provenance"), dict), "continuity result omitted safe provenance")
        return {"metadata": metadata, "assessment": assessment}

    def check_save_replay_update_and_cas(self) -> None:
        slot, task, thread = self._slot("cas"), "checkpoint-cas-task", "checkpoint-cas-thread"
        create = self._save_payload(task=task, thread=thread, expected_revision=0, request_id=self._request_id(1))
        first = self._save(slot, create, replayed=False)
        replay = self._save(slot, create, replayed=True)
        require(replay["metadata"] == first["metadata"], "exact checkpoint retry changed public metadata")

        update = self._save_payload(task=task, thread=thread, expected_revision=first["metadata"]["revision"],
                                    request_id=self._request_id(2), draft=self._draft(marker="updated"))
        second = self._save(slot, update, replayed=False)
        require(second["metadata"]["revision"] > first["metadata"]["revision"],
                "checkpoint update did not allocate a newer revision")

        race_revision = second["metadata"]["revision"]
        contenders = [self._save_payload(task=task, thread=thread, expected_revision=race_revision,
                                         request_id=self._request_id(3 + index), draft=self._draft(marker=f"race-{index}"))
                      for index in range(2)]
        with ThreadPoolExecutor(max_workers=2) as executor:
            outcomes = list(executor.map(lambda body: self._request("POST", self._path(slot), body), contenders))
        successes = [(status, response, body) for (status, response), body in zip(outcomes, contenders) if status in {200, 201}]
        conflicts = [(status, response) for status, response in outcomes if status == 409]
        require(len(successes) == 1 and len(conflicts) == 1,
                "checkpoint compare-and-swap race did not produce one winner and one conflict")
        self._error(conflicts[0][0], conflicts[0][1], 409, "checkpoint-revision-conflict")
        winner = self._metadata({key: value for key, value in successes[0][1]["result"].items() if key != "replayed"},
                                task=task, thread=thread, active=True)
        self.records[("admin", slot)] = winner

        changed_same_id = {**successes[0][2], "draft": self._draft(marker="same-id-conflict")}
        status, response = self._request("POST", self._path(slot), changed_same_id)
        self._error(status, response, 409, "checkpoint-request-conflict")
        stale = self._save_payload(task=task, thread=thread, expected_revision=race_revision,
                                   request_id=self._request_id(5), draft=self._draft(marker="stale"))
        status, response = self._request("POST", self._path(slot), stale)
        self._error(status, response, 409, "checkpoint-revision-conflict")

    def check_owner_task_and_read_isolation(self) -> None:
        slot = self._slot("owner")
        admin_task, reader_task, thread = "checkpoint-admin-task", "checkpoint-reader-task", "checkpoint-owner-thread"
        admin = self._save(slot, self._save_payload(task=admin_task, thread=thread, expected_revision=0,
                                                    request_id=self._request_id(10)))
        reader = self._save(slot, self._save_payload(task=reader_task, thread=thread, expected_revision=0,
                                                     request_id=self._request_id(11)), role="reader")
        require(admin["metadata"]["revision"] != reader["metadata"]["revision"],
                "same private slot across principals reused one record")
        self._get(slot, role="admin")
        self._get(slot, role="reader")
        for role, task in (("admin", reader_task), ("reader", admin_task)):
            status, response = self._resume(slot, task, thread, role=role)
            self._error(status, response, 404, "checkpoint-not-found")
        status, response = self._resume(slot, admin_task, "wrong-thread", role="admin")
        self._error(status, response, 404, "checkpoint-not-found")

        before = self._list("admin")
        self._get(slot, role="admin")
        after = self._list("admin")
        require(self._inventory_state(after) == self._inventory_state(before),
                "checkpoint metadata GET changed private inventory")
        require(all(item.get("task_id") != reader_task for item in before["entries"]),
                "administrator inventory exposed another principal's same-named slot")

    @staticmethod
    def _inventory_state(value: dict[str, Any]) -> dict[str, Any]:
        return {**value, "entries": [{key: item for key, item in row.items() if key != "observed_at"}
                                      for row in value["entries"]]}

    def _list(self, role: str, *, after: str = "", limit: int = 32) -> dict[str, Any]:
        query = urlencode({key: value for key, value in {"after": after, "limit": limit}.items() if value != ""})
        status, envelope = self._request("GET", CHECKPOINT_ROOT + ("?" + query if query else ""), role=role)
        require(status == 200, "checkpoint collection lookup failed")
        result = envelope.get("result")
        require(isinstance(result, dict) and result.get("schema_version") == "shin.task-checkpoint-inventory.v1" and
                isinstance(result.get("entries"), list), "checkpoint inventory schema differs")
        for entry in result["entries"]:
            require(isinstance(entry, dict) and "observed_at" in entry, "checkpoint inventory omitted observation metadata")
            self._metadata({key: value for key, value in entry.items() if key != "observed_at"})
        no_private_values(result, self._forbidden())
        return result

    def _sql_image(self) -> Any:
        # Keep the SQL oracle metadata-only: canonical checkpoint bytes and replay hashes are never test output.
        return self.s.sql_json(
            "SELECT COALESCE(jsonb_agg(to_jsonb(c) - ARRAY['checkpoint_text',"
            "'last_request_hash','last_request_id']::text[] ORDER BY c.project_id,c.owner_id,c.slot_id),"
            "'[]'::jsonb) FROM shin_server.checkpoint_slots c")

    def check_resume_impact_restart_and_private_projection(self) -> None:
        slot, task, thread = self._slot("resume"), "checkpoint-resume-task", "checkpoint-resume-thread"
        saved = self._save(slot, self._save_payload(task=task, thread=thread, expected_revision=0,
                                                    request_id=self._request_id(20)))
        status, response = self._resume(slot, task, thread)
        require(status == 200, "checkpoint resume did not return HTTP 200")
        resumed = self._assessment(response.get("result"), impact=False)
        require(resumed["metadata"] == saved["metadata"], "resume returned different checkpoint metadata")
        current = resumed["assessment"]
        require(current.get("ok") is True and current.get("status") == "current" and
                current.get("index_current") is True and current.get("continuation_ready") is True and
                current.get("continuation_pack", {}).get("completion", {}).get("complete") is True,
                "unchanged checkpoint did not restore current complete continuation evidence")
        status, response = self._resume(slot, task, thread, impact=True)
        require(status == 200, "checkpoint impact did not return HTTP 200")
        impacted = self._assessment(response.get("result"), impact=True)
        require(impacted["metadata"] == saved["metadata"], "impact returned different checkpoint metadata")
        require(impacted["assessment"].get("ok") is True and
                impacted["assessment"].get("index_current") is True and impacted["assessment"].get("reread") == [],
                "unchanged checkpoint Impact invented changed evidence")

        before_metadata = self._get(slot)
        observed_roots = (self.s.fixture_root, self.s.out_dir,
                          self.s._inspection_artifact(saved["metadata"]["provenance"]["preparation_id"]))
        before_tree = tuple(self.s.tree_digest(root) for root in observed_roots)
        before_sql = self._sql_image()
        self.s.stop_server(expect_clean=True)
        self.s.start_server()
        require(self._get(slot) == before_metadata, "checkpoint metadata did not survive restart")
        status, response = self._resume(slot, task, thread)
        require(status == 200, "checkpoint resume did not survive restart")
        self._assessment(response.get("result"), impact=False)
        require(tuple(self.s.tree_digest(root) for root in observed_roots) == before_tree,
                "checkpoint metadata/read-only evaluation changed source or generated-tree bytes")
        require(self._sql_image() == before_sql,
                "checkpoint metadata/read-only evaluation changed durable checkpoint rows")

    def check_publication_scope_and_currentness(self) -> None:
        publication = "publication-peer"
        head = self.s.get_head(publication)
        if head["state"] != "active":
            proposal = self.s.submit(self.s.prepare_candidate(PEER_ROUTE), head["revision"], "checkpoint-publication-seed", publication)
            self.s.resolution(proposal["proposal"]["id"], "accept", head["revision"], publication)
            head = self.s.get_head(publication)
        slot, task, thread = self._slot("publication"), "checkpoint-publication-task", "checkpoint-publication-thread"
        self._save(slot, self._save_payload(task=task, thread=thread, expected_revision=0, request_id=self._request_id(30),
                                            selector={"kind": "publication", "id": publication}))
        replacement = self.s.prepare_candidate(ROUTE_ID)
        proposal = self.s.submit(replacement, head["revision"], "checkpoint-publication-change", publication)
        self.s.resolution(proposal["proposal"]["id"], "accept", head["revision"], publication)
        status, response = self._resume(slot, task, thread)
        self._error(status, response, 409, "checkpoint-source-scope-changed")
        from publication_withdrawal_checks import withdraw
        current = self.s.get_head(publication)
        withdraw(self.s, publication, current["revision"], proposal["proposal"]["id"], replacement["id"])
        status, response = self._resume(slot, task, thread)
        self._error(status, response, 410, "publication-withdrawn")

        dirty_slot, dirty_task, dirty_thread = self._slot("dirty"), "checkpoint-dirty-task", "checkpoint-dirty-thread"
        source = self.s.fixture_root / "docs" / "policy.md"
        original = source.read_text(encoding="utf-8")
        try:
            self.s.write_fixture_file(source, original + "\nCheckpoint dirty source observation.\n")
            self._save(dirty_slot, self._save_payload(task=dirty_task, thread=dirty_thread, expected_revision=0,
                                                      request_id=self._request_id(31), require_current=False))
            status, response = self._resume(dirty_slot, dirty_task, dirty_thread, require_current=True)
            require(status == 200, "require-current resume did not retain a readable drift assessment")
            assessment = self._assessment(response.get("result"), impact=False)["assessment"]
            require(assessment.get("ok") is False and assessment.get("index_current") is False,
                    "require-current resume did not report source drift through its assessment")
            require(assessment.get("continuation_ready") is False and assessment.get("continuation_pack") is None,
                    "stale source was admitted as continuation evidence")
            status, response = self._resume(dirty_slot, dirty_task, dirty_thread, impact=True, require_current=False)
            require(status == 200, "dirty checkpoint Impact did not remain inspectable")
            impact = self._assessment(response.get("result"), impact=True)["assessment"]
            require(impact.get("index_current") is False and
                    any(row.get("path") == "docs/policy.md" for row in impact.get("reread", [])),
                    "checkpoint Impact omitted the known changed source")
        finally:
            self.s.write_fixture_file(source, original)

    def _native_receipt(self, task: str, thread: str) -> dict[str, Any]:
        self.drafts.mkdir(mode=0o700, exist_ok=True)
        draft = self._draft(marker="receipt-scope")
        path = self.drafts / "receipt-scope.json"
        self.s.write_fixture_file(path, self._json(draft).decode("utf-8"))
        now = int(time.time())
        output = self.s.run_process([
            str(self.s.core), "--profile", "generic-docs", "--profile-dir", str(self.s.profile_dir),
            "--root", str(self.s.fixture_root), "--out-dir", str(self.s.out_dir), "checkpoint", "--input", str(path),
            "--task-id", task, "--thread-id", thread, "--observed-at", str(now), "--require-current",
        ], "checkpoint receipt scope fixture", timeout=45.0)
        initial = json.loads(output.stdout)
        scope = dict(initial["binding"], snapshot_id=initial["baseline"]["snapshot_id"],
                     worktree_id=initial["baseline"]["worktree_id"])
        return {
            "schema_version": "shin.execution-receipt.v1", "receipt_id": "admin-claimed-receipt",
            "kind": "test", "producer": "dev-admin", "observed_at": now - 2, "ttl_seconds": 1,
            "result": "pass", "scope": scope, "command_fingerprint": "fnv1a64:0000000000000000",
        }

    def check_deadline_body_and_receipt_boundaries(self) -> None:
        slot, task, thread = self._slot("invalid"), "checkpoint-invalid-task", "checkpoint-invalid-thread"
        base = self._save_payload(task=task, thread=thread, expected_revision=0, request_id=self._request_id(40))
        invalids = (
            ({key: value for key, value in base.items() if key != "require_current"}, "missing require_current"),
            ({**base, "require_current": "true"}, "non-boolean require_current"),
            ({**base, "request_id": "A" * 32}, "non-lowercase request id"),
            ({**base, "request_deadline": self._deadline(305)}, "deadline above retry window"),
            ({**base, "request_deadline": self._deadline(-1)}, "expired deadline"),
            ({**base, "draft": {**base["draft"], "state": {**base["draft"]["state"], "current_slice": "sk-" + "Ab9z" * 12}}},
             "secret-like draft"),
        )
        for body, label in invalids:
            status, response = self._request("POST", self._path(slot), body)
            if label == "expired deadline":
                self._error(status, response, 409, "checkpoint-request-expired")
            elif label == "secret-like draft":
                self._error(status, response, 400, "secret-like-session-content")
                require("Ab9z" not in json.dumps(response), "secret-like rejection leaked the synthetic marker")
            else:
                self._error(status, response, 400, "invalid-request")
        oversized = {**base, "draft": {**base["draft"], "state": {
            **base["draft"]["state"], "current_slice": "x" * 32768,
        }}}
        status, _ = self._request("POST", self._path(slot), oversized)
        require(status == 413, "checkpoint HTTP body limit no longer rejects a draft above 32 KiB")

        receipt_slot, receipt_task, receipt_thread = self._slot("receipt"), "checkpoint-receipt-task", "checkpoint-receipt-thread"
        receipt = self._native_receipt(receipt_task, receipt_thread)
        fresh_claim = {**receipt, "receipt_id": "admin-fresh-claimed-receipt", "ttl_seconds": 3600}
        self._save(receipt_slot, self._save_payload(task=receipt_task, thread=receipt_thread, expected_revision=0,
                                                    request_id=self._request_id(41),
                                                    draft=self._draft(marker="receipt", receipts=[receipt, fresh_claim])))
        status, response = self._resume(receipt_slot, receipt_task, receipt_thread, require_current=False)
        require(status == 200, "expired untrusted receipt checkpoint did not remain inspectable")
        assessment = self._assessment(response.get("result"), impact=False)["assessment"]
        receipts = assessment.get("receipts")
        require(isinstance(receipts, list) and len(receipts) == 2 and receipts[0].get("producer") == "dev-admin" and
                receipts[0].get("trust_source") == "server-no-active-receipt-origin" and
                receipts[0].get("producer_trusted") is False and receipts[0].get("age") == "expired",
                "server treated an admin-named expired receipt as authenticated evidence")
        require(receipts[1].get("age") == "current" and receipts[1].get("producer_trusted") is False and
                receipts[1].get("passed_current") is False,
                "fresh admin-named receipt gained execution trust without authenticated import")

    def check_clear_prune_recreate_and_capacity(self) -> None:
        rebind_slot = self._slot("rebind")
        old_task, old_thread = "checkpoint-rebind-old-task", "checkpoint-rebind-old-thread"
        original = self._save(rebind_slot, self._save_payload(task=old_task, thread=old_thread, expected_revision=0,
                                                              request_id=self._request_id(45)))
        original_clear = {
            "schema_version": "shin.task-checkpoint-clear.v1", "task_id": old_task, "thread_id": old_thread,
            "expected_revision": original["metadata"]["revision"], "request_id": self._request_id(46),
            "request_deadline": self._deadline(30),
        }
        status, response = self._request("POST", self._path(rebind_slot, "/clear"), original_clear)
        require(status in {200, 201}, "checkpoint rebind fixture clear did not succeed")
        tombstone = self._metadata({key: value for key, value in response["result"].items() if key != "replayed"},
                                   task=old_task, thread=old_thread, active=False)
        new_task, new_thread = "checkpoint-rebind-new-task", "checkpoint-rebind-new-thread"
        rebound = self._save(rebind_slot, self._save_payload(task=new_task, thread=new_thread,
                                                             expected_revision=tombstone["revision"],
                                                             request_id=self._request_id(47)))
        require(rebound["metadata"]["revision"] > tombstone["revision"],
                "checkpoint save after an observed tombstone did not allocate a newer revision")
        status, response = self._resume(rebind_slot, old_task, old_thread)
        self._error(status, response, 404, "checkpoint-not-found")
        before_repeat = self._get(rebind_slot)
        status, response = self._request("POST", self._path(rebind_slot, "/clear"), original_clear)
        self._error(status, response, 404, "checkpoint-not-found")
        require(self._get(rebind_slot) == before_repeat,
                "a stale clear after checkpoint task rebind changed the new slot record")

        slot, task, thread = self._slot("clear"), "checkpoint-clear-task", "checkpoint-clear-thread"
        write_deadline = self._deadline(5)
        create = self._save_payload(task=task, thread=thread, expected_revision=0, request_id=self._request_id(50),
                                    deadline=write_deadline)
        saved = self._save(slot, create)
        clear_deadline = self._deadline(2)
        clear = {
            "schema_version": "shin.task-checkpoint-clear.v1", "task_id": task, "thread_id": thread,
            "expected_revision": saved["metadata"]["revision"], "request_id": self._request_id(51),
            "request_deadline": clear_deadline,
        }
        status, response = self._request("POST", self._path(slot, "/clear"), clear)
        require(status in {200, 201}, "checkpoint clear did not succeed")
        cleared_result = response.get("result")
        require(isinstance(cleared_result, dict) and cleared_result.get("replayed") is False,
                "checkpoint clear omitted its non-replay marker")
        cleared = self._metadata({key: value for key, value in cleared_result.items() if key != "replayed"},
                                 task=task, thread=thread, active=False)
        require(cleared["revision"] > saved["metadata"]["revision"], "checkpoint clear did not allocate a tombstone revision")
        require(cleared["retry_until"] == max(write_deadline, clear_deadline),
                "checkpoint tombstone did not retain the maximum accepted request deadline")
        status, response = self._resume(slot, task, thread)
        self._error(status, response, 410, "checkpoint-cleared")

        # An old save retry remains a conflict, not a resurrection, while its longer
        # accepted deadline still fences the cleared slot.
        status, response = self._request("POST", self._path(slot), create)
        self._error(status, response, 409, "checkpoint-revision-conflict")
        before_prune = self._list("admin")
        status, response = self._request("POST", CHECKPOINT_ROOT + "/prune", {"schema_version": "shin.checkpoint-prune-request.v1"})
        require(status in {200, 201}, "checkpoint prune did not return a success response")
        prune = response.get("result")
        require(isinstance(prune, dict) and prune.get("schema_version") == "shin.checkpoint-prune.v1" and
                prune.get("removed") == 0 and prune.get("read_only") is False,
                "checkpoint prune removed a tombstone before its maximum replay fence expired")
        after_early_prune = self._list("admin")
        require(self._inventory_state(after_early_prune) == self._inventory_state(before_prune),
                "checkpoint prune changed inventory before the maximum replay fence expired")

        short_slot, short_task, short_thread = self._slot("short-prune"), "checkpoint-short-task", "checkpoint-short-thread"
        short_deadline = self._deadline(5)
        short = self._save(short_slot, self._save_payload(task=short_task, thread=short_thread, expected_revision=0,
                                                          request_id=self._request_id(52), deadline=short_deadline))
        short_clear = {
            "schema_version": "shin.task-checkpoint-clear.v1", "task_id": short_task, "thread_id": short_thread,
            "expected_revision": short["metadata"]["revision"], "request_id": self._request_id(53),
            "request_deadline": short_deadline,
        }
        status, response = self._request("POST", self._path(short_slot, "/clear"), short_clear)
        require(status in {200, 201}, "short-lived checkpoint clear did not succeed")
        short_tombstone = self._metadata({key: value for key, value in response["result"].items() if key != "replayed"},
                                         task=short_task, thread=short_thread, active=False)
        require(short_tombstone["retry_until"] == short_deadline,
                "short-lived checkpoint did not preserve its accepted deadline")
        wait_until = max(write_deadline, short_tombstone["retry_until"])
        wait_limit = time.monotonic() + 15.0
        while int(time.time()) <= wait_until:
            require(time.monotonic() < wait_limit, "checkpoint fixture wall clock did not reach the retry deadline")
            time.sleep(0.05)
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            status, response = self._request("POST", CHECKPOINT_ROOT + "/prune", {"schema_version": "shin.checkpoint-prune-request.v1"})
            require(status in {200, 201}, "checkpoint expiry prune failed")
            status, _ = self._request("GET", self._path(short_slot))
            if status == 404:
                break
            time.sleep(0.05)
        else:
            raise IntegrationFailure("expired checkpoint tombstone was not pruned within the bounded fixture wait")

        lifecycle_floor = int(self.s.scalar("SELECT reject_deadlines_through FROM shin_server.checkpoint_lifecycle WHERE singleton"))
        require(lifecycle_floor >= max(cleared["retry_until"], short_tombstone["retry_until"]),
                "prune did not persist its replay deadline floor")
        recreated = self._save(short_slot, self._save_payload(task=short_task, thread=short_thread, expected_revision=0,
                                                              request_id=self._request_id(54)), replayed=False)
        require(recreated["metadata"]["revision"] > short_tombstone["revision"],
                "checkpoint recreate reused a revision after tombstone pruning")
        prior_floor = lifecycle_floor
        injected_floor = int(time.time()) + 60
        self.s.psql("UPDATE shin_server.checkpoint_lifecycle SET reject_deadlines_through=" + str(injected_floor) +
                    " WHERE singleton")
        try:
            stale = self._save_payload(task="checkpoint-floor-task", thread="checkpoint-floor-thread", expected_revision=0,
                                       request_id=self._request_id(55), deadline=int(time.time()) + 30)
            status, response = self._request("POST", self._path(self._slot("floor")), stale)
            self._error(status, response, 409, "checkpoint-request-expired")
        finally:
            self.s.psql("UPDATE shin_server.checkpoint_lifecycle SET reject_deadlines_through=" + str(prior_floor) +
                        " WHERE singleton")

        # The fixture already has one reader-private row.  Seed only the remaining
        # tombstones, then prove a new slot is refused and remove the seed in-place.
        current = int(self.s.scalar("SELECT count(*) FROM shin_server.checkpoint_slots "
                                    "WHERE project_id='visible' AND owner_id='dev-reader'"))
        require(current <= 128, "checkpoint fixture exceeded the owner quota before its boundary check")
        needed = 128 - current
        now = int(time.time())
        if needed:
            self.s.psql(
                "INSERT INTO shin_server.checkpoint_slots "
                "(project_id,owner_id,slot_id,state,revision,task_id,thread_id,provenance,checkpoint_text,"
                "checkpoint_sha256,checkpoint_id,captured_at,ttl_seconds,snapshot_id,last_operation,last_request_id,"
                "last_request_hash,last_expected_revision,last_request_deadline,retry_until,updated_at) "
                "SELECT 'visible','dev-reader','checkpoint-cap-seed-' || lpad(g::text,3,'0'),'cleared',"
                "nextval('shin_server.checkpoint_revisions'),'checkpoint-cap-task','checkpoint-cap-thread',"
                "(SELECT provenance FROM shin_server.checkpoint_slots WHERE project_id='visible' "
                "AND owner_id='dev-reader' AND slot_id='checkpoint-owner'),"
                "NULL,NULL,NULL,NULL,NULL,NULL,'clear',lpad(g::text,32,'0'),repeat('a',64),0," +
                str(now + 30) + "," + str(now + 30) + "," + str(now) + " FROM generate_series(1," + str(needed) + ") g")
        try:
            overflow = self._save_payload(task="checkpoint-cap-task", thread="checkpoint-cap-thread", expected_revision=0,
                                          request_id=self._request_id(56))
            status, response = self._request("POST", self._path(self._slot("capacity")), overflow, role="reader")
            self._error(status, response, 507, "checkpoint-capacity")
        finally:
            self.s.psql("DELETE FROM shin_server.checkpoint_slots WHERE project_id='visible' AND owner_id='dev-reader' "
                        "AND slot_id LIKE 'checkpoint-cap-seed-%'")

        total = int(self.s.scalar("SELECT count(*) FROM shin_server.checkpoint_slots"))
        require(0 < total < 1024, "checkpoint global capacity fixture has no available boundary")
        self.s.psql(
            "INSERT INTO shin_server.checkpoint_slots "
            "(project_id,owner_id,slot_id,state,revision,task_id,thread_id,provenance,"
            "last_operation,last_request_id,last_request_hash,last_expected_revision,last_request_deadline,retry_until,updated_at) "
            "SELECT 'visible','checkpoint-cap-owner-' || ((g-1)/128)::text,'checkpoint-global-' || g::text,'cleared',"
            "nextval('shin_server.checkpoint_revisions'),'checkpoint-cap-task','checkpoint-cap-thread',"
            "(SELECT provenance FROM shin_server.checkpoint_slots WHERE project_id='visible' "
            "AND owner_id='dev-reader' AND slot_id='checkpoint-owner'),"
            "'clear',lpad(g::text,32,'0'),repeat('a',64),0," +
            str(now + 30) + "," + str(now + 30) + "," + str(now) +
            " FROM generate_series(1," + str(1024 - total) + ") g")
        try:
            global_overflow = self._save_payload(task="checkpoint-global-task", thread="checkpoint-global-thread",
                                                expected_revision=0, request_id=self._request_id(57))
            status, response = self._request("POST", self._path(self._slot("global-capacity")), global_overflow)
            self._error(status, response, 507, "checkpoint-capacity")
            require(self.s.scalar("SELECT count(*) FROM shin_server.checkpoint_slots") == "1024",
                    "global checkpoint overflow changed retained rows")
        finally:
            self.s.psql("DELETE FROM shin_server.checkpoint_slots WHERE owner_id LIKE 'checkpoint-cap-owner-%'")

    def check_schema_and_grant_tamper_readiness(self) -> None:
        ready = "/health/ready"
        revision_constraint = "checkpoint_slots_revision_check"
        tampered = False
        try:
            self.s.psql("ALTER TABLE shin_server.checkpoint_slots DROP CONSTRAINT " + revision_constraint)
            tampered = True
            self.s.psql("ALTER TABLE shin_server.checkpoint_slots ADD CONSTRAINT " + revision_constraint + " CHECK (true)")
            self.s.wait_for_response(ready, 503, headers=self.s.auth_headers("admin"))
        finally:
            if tampered:
                self.s.psql("ALTER TABLE shin_server.checkpoint_slots DROP CONSTRAINT " + revision_constraint)
                self.s.psql("ALTER TABLE shin_server.checkpoint_slots ADD CONSTRAINT " + revision_constraint +
                            " CHECK (revision > 0)")
                self.s.wait_for_response(ready, 200, headers=self.s.auth_headers("admin"))

        granted = False
        try:
            self.s.psql("GRANT UPDATE ON SEQUENCE shin_server.checkpoint_revisions TO shin_runtime")
            granted = True
            self.s.wait_for_response(ready, 503, headers=self.s.auth_headers("admin"))
        finally:
            if granted:
                self.s.psql("REVOKE UPDATE ON SEQUENCE shin_server.checkpoint_revisions FROM shin_runtime")
                self.s.wait_for_response(ready, 200, headers=self.s.auth_headers("admin"))

    def _cleanup(self) -> None:
        if self.s.server is not None:
            for (role, slot), metadata in tuple(self.records.items()):
                if metadata.get("state") != "active":
                    continue
                try:
                    self._request("POST", self._path(slot, "/clear"), {
                        "schema_version": "shin.task-checkpoint-clear.v1", "task_id": metadata["task_id"],
                        "thread_id": metadata["thread_id"], "expected_revision": metadata["revision"],
                        "request_id": self._request_id(0x1000 + len(slot)), "request_deadline": self._deadline(30),
                    }, role=role)
                except (IntegrationFailure, OSError, KeyError):
                    # The parent finally block remains responsible for preserving the original failure.
                    pass
        shutil.rmtree(self.drafts, ignore_errors=True)
