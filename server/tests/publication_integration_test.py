#!/usr/bin/env python3
"""P3E11 real PostgreSQL/HTTP publication checks on the exclusive development stand.

The parent harness owns migrations, schema reset and StandGuard. This suite only
uses neutral local documentation fixtures; it never reads consumer repositories.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import threading
import time
from typing import Any, Iterator

from integration_test import BUILD_ROOT, HOST, PORT, IntegrationFailure, IntegrationSuite, under_build
from preparation_integration_test import DEFAULT_LEDGER_PROBE, PreparationIntegrationSuite, optional_probe
from publication_test_support import (
    IDENTITY_FIELDS, RECEIPT_FIELDS, detail, exact_object, head, integer, no_private_values, require, timestamp,
)
from retrieval_integration_test import DEFAULT_CORE, PROJECT_ID, ROUTE_ID, validate_candidate_core


PEER_ROUTE = "publication-peer"
PUBLICATION = "shared"
MAX_LOAD_REQUESTS = 512
DB_TABLES = (
    "publication_heads", "publication_proposals", "publication_resolutions", "publication_withdrawals", "preparations",
    "preparation_retention", "active_routes", "activation_receipts", "audit_events", "retrieval_journal_producers",
    "retrieval_journal_attempts", "retrieval_journal_lifecycle", "publication_history",
)


class PublicationIntegrationSuite(PreparationIntegrationSuite):
    def __init__(self, binary: Path, core: Path, state_root: Path, report_path: Path,
                 ledger_probe: Path | None = None, *, load: bool = False, load_requests: int = 128,
                 publication_only: bool = False, checkpoint_only: bool = False, history_only: bool = False) -> None:
        super().__init__(binary, core, state_root, report_path, ledger_probe)
        self.peer_root = state_root / "fixtures" / "roots" / "visible-secondary"
        self.load_enabled = load
        self.load_requests = load_requests
        self.publication_only = publication_only
        self.checkpoint_only = checkpoint_only
        self.history_only = history_only
        self.publication_measurement: dict[str, Any] = {"status": "not-run"}
        self.busy_retries = 0
        self.last_http_ms = 0.0
        self.jobs: dict[int, dict[str, Any]] = {}
        self.receipts: list[tuple[int, str, int, dict[str, Any]]] = []
        self.retrieval_journal_measurement: dict[str, Any] = {"status": "not-run"}
        self.retrieval_journal_retention_measurement: dict[str, Any] = {"status": "not-run"}
        self.knowledge_measurement: dict[str, Any] = {"status": "not-run"}

    @staticmethod
    def path(publication: str = PUBLICATION, suffix: str = "", project: str = PROJECT_ID) -> str:
        return f"/api/v1/projects/{project}/publications/{publication}" + suffix

    def public_result(self, response: dict[str, Any]) -> Any:
        value = response.get("result")
        no_private_values(value, [str(self.fixture_root), str(self.peer_root), str(self.other_root),
                                  str(self.out_dir), str(self.preparation_root), self.admin_token, self.reader_token])
        return value

    def get_head(self, publication: str = PUBLICATION, *, project: str = PROJECT_ID,
                 role: str = "admin") -> dict[str, Any]:
        response = self.expect_status(200, "GET", self.path(publication, project=project), headers=self.auth_headers(role))
        return head(self.public_result(response), project, publication)

    def get_detail(self, proposal_id: int, publication: str = PUBLICATION,
                   *, role: str = "admin") -> dict[str, Any]:
        started = time.perf_counter_ns()
        response = self.expect_status(200, "GET", self.path(publication, f"/proposals/{proposal_id}"),
                                      headers=self.auth_headers(role))
        self.last_http_ms = (time.perf_counter_ns() - started) / 1_000_000
        result = detail(self.public_result(response), PROJECT_ID, publication)
        require(result["proposal"]["id"] == proposal_id, "detail returned a different proposal")
        self.assert_candidate(result["candidate"])
        self.assert_side_provenance(result["base"])
        self.assert_side_provenance(result["current"])
        return result

    def post_json(self, path: str, payload: Any, *, role: str = "admin",
                  key: str | None = None) -> tuple[int, dict[str, Any]]:
        headers = {**self.auth_headers(role), "Content-Type": "application/json"}
        if key is not None:
            headers["Idempotency-Key"] = key
        body = payload if isinstance(payload, bytes) else json.dumps(payload, separators=(",", ":")).encode("utf-8")
        started = time.perf_counter_ns()
        response = self.request("POST", path, headers=headers, body=body)
        self.last_http_ms = (time.perf_counter_ns() - started) / 1_000_000
        no_private_values(response[1], [str(self.fixture_root), str(self.peer_root), str(self.other_root),
                                       str(self.preparation_root), self.admin_token, self.reader_token])
        return response

    def submit(self, job: dict[str, Any], revision: int, key: str, publication: str = PUBLICATION,
               *, role: str = "admin", replayed: bool = False, epoch: int | None = None) -> dict[str, Any]:
        request = {"preparation_id": job["id"], "base_revision": revision}
        if epoch is not None:
            request["history_epoch"] = epoch
        status, response = self.post_json(self.path(publication, "/proposals"),
                                         request, role=role, key=key)
        require(status == (200 if replayed else 201), f"proposal submission returned HTTP {status}")
        result = exact_object(self.public_result(response), {"replayed", "proposal"}, "proposal submission")
        require(result["replayed"] is replayed, "submission replay marker differs")
        proposal = detail(result["proposal"], PROJECT_ID, publication)
        require(proposal["proposal"]["preparation_id"] == job["id"] and
                proposal["proposal"]["base_revision"] == revision, "submission did not capture its request")
        self.assert_candidate(proposal["candidate"])
        self.assert_side_provenance(proposal["base"])
        self.assert_side_provenance(proposal["current"])
        proposal_id = proposal["proposal"]["id"]
        row = self.sql_json("SELECT to_jsonb(p) || jsonb_build_object('created_at', "
                            "to_char(p.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')) "
                            "FROM shin_server.publication_proposals p WHERE id=" + str(proposal_id))
        for field in ("id", "project_id", "publication_id", "preparation_id", "base_revision", "base_preparation_id", "base_withdrawal_id", "submitted_by", "created_at"):
            require(row[field] == proposal["proposal"][field], "HTTP proposal differs from SQL: " + field)
        for field, expected in (("source_route_id", job["route_id"]), ("workspace_id", job["workspace_id"]),
                                ("source_host_id", "local-dev"), ("profile_id", job["profile_id"]),
                                ("route_fingerprint", job["route_fingerprint"])):
            require(row[field] == expected, "SQL proposal did not capture immutable source provenance: " + field)
        self.assert_publication_audit(proposal_id, job["id"], "publication.proposed", "dev-admin" if role == "admin" else "dev-reader")
        return proposal

    def resolve_request(self, proposal_id: int, decision: str, revision: int,
                        publication: str = PUBLICATION, *, role: str = "admin",
                        retry_busy: bool = False) -> tuple[int, dict[str, Any]]:
        deadline = time.monotonic() + 15.0
        while True:
            status, response = self.post_json(self.path(publication, f"/proposals/{proposal_id}/resolution"),
                                             {"decision": decision, "expected_revision": revision}, role=role)
            if not (retry_busy and status == 503 and response.get("error", {}).get("code") == "preparation-inspection-busy"):
                return status, response
            require(time.monotonic() < deadline, "bounded inspection-busy retries exhausted")
            self.busy_retries += 1
            time.sleep(0.025)

    def resolution(self, proposal_id: int, decision: str, revision: int, publication: str = PUBLICATION,
                   *, replayed: bool = False) -> dict[str, Any]:
        status, response = self.resolve_request(proposal_id, decision, revision, publication, retry_busy=True)
        require(status == 200, f"publication resolution returned HTTP {status}")
        return self.assert_receipt(response, proposal_id, decision, revision, publication, replayed=replayed)

    def assert_receipt(self, response: dict[str, Any], proposal_id: int, decision: str,
                       revision: int, publication: str, *, replayed: bool) -> dict[str, Any]:
        result = exact_object(self.public_result(response), {"replayed", "receipt"}, "resolution result")
        require(result["replayed"] is replayed, "resolution replay marker differs")
        receipt = exact_object(result["receipt"], RECEIPT_FIELDS, "resolution receipt")
        require(receipt["proposal_id"] == proposal_id and receipt["project_id"] == PROJECT_ID and
                receipt["publication_id"] == publication and receipt["decision"] == decision and
                receipt["expected_revision"] == revision and receipt["resolved_by"] == "dev-admin",
                "resolution receipt did not retain the reviewed request")
        integer(receipt["revision"], 0, "receipt revision")
        timestamp(receipt["created_at"])
        require(receipt["revision"] == revision + (decision == "accept"), "resolution changed head revision incorrectly")
        row = self.sql_json("SELECT to_jsonb(r) || jsonb_build_object('created_at', "
                            "to_char(r.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')) "
                            "FROM shin_server.publication_resolutions r WHERE proposal_id=" + str(proposal_id))
        for field in RECEIPT_FIELDS:
            require(row[field] == receipt[field], "HTTP resolution differs from SQL: " + field)
        action = {"accept": "publication.accepted", "reject": "publication.rejected",
                  "request-reprepare": "publication.reprepare-requested"}[decision]
        self.assert_publication_audit(proposal_id, receipt["preparation_id"], action, "dev-admin")
        return receipt

    def assert_publication_audit(self, proposal_id: int, preparation_id: int, action: str, principal: str) -> None:
        rows = self.sql_json("SELECT COALESCE(jsonb_agg(jsonb_build_object('project_id',project_id,"
                             "'preparation_id',preparation_id,'publication_proposal_id',publication_proposal_id,"
                             "'principal_id',principal_id)), '[]'::jsonb) FROM shin_server.audit_events "
                             "WHERE publication_proposal_id=" + str(proposal_id) + " AND action=" + self._sql_text(action))
        require(rows == [{"project_id": PROJECT_ID, "preparation_id": preparation_id,
                          "publication_proposal_id": proposal_id, "principal_id": principal}],
                "publication audit is missing, duplicated or incorrectly correlated")

    def sql_json(self, query: str) -> Any:
        return json.loads(self.scalar(query))

    def durable_state(self) -> dict[str, Any]:
        projections = [f"'{table}', (SELECT COALESCE(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]'::jsonb) "
                       f"FROM shin_server.{table} t)" for table in DB_TABLES]
        return self.sql_json("SELECT jsonb_build_object(" + ",".join(projections) + ")")

    def unchanged(self, before: dict[str, Any], label: str) -> None:
        require(self.durable_state() == before, label + " mutated durable rows")

    def table_count(self, table: str) -> int:
        require(table in DB_TABLES, "SQL count uses an unknown table")
        return int(self.scalar(f"SELECT count(*) FROM shin_server.{table}"))

    def assert_head_sql(self, expected: dict[str, Any]) -> None:
        row = self.sql_json("SELECT to_jsonb(h) FROM shin_server.publication_heads h WHERE project_id='visible' "
                            "AND publication_id=" + self._sql_text(expected["publication_id"]))
        for key in ("project_id", "publication_id", "revision", "preparation_id"):
            require(row[key] == expected[key], "HTTP publication head differs from SQL: " + key)

    def assert_candidate(self, candidate: dict[str, Any]) -> None:
        job = self.jobs.get(candidate["preparation_id"])
        require(job is not None, "public detail returned an unseeded candidate")
        for field, expected in (("snapshot_id", job["artifact_snapshot_id"]), ("source_route_id", job["route_id"]),
                                ("workspace_id", job["workspace_id"]), ("candidate_author", job["author"]),
                                ("state", "prepared")):
            require(candidate[field] == expected, "candidate provenance differs: " + field)
        self.assert_side_provenance(candidate)

    def assert_side_provenance(self, side: dict[str, Any]) -> None:
        preparation = side["preparation_id"]
        if preparation is None:
            require(all(value is None for key, value in side.items() if key not in {"revision", "state"}),
                    "unselected publication has source provenance")
            return
        job = self.jobs.get(preparation)
        require(job is not None, "publication side names an unseeded preparation")
        expected = {"snapshot_id": job["artifact_snapshot_id"], "source_route_id": job["route_id"],
                    "workspace_id": job["workspace_id"], "source_host_id": "local-dev", "profile_id": job["profile_id"]}
        for field, value in expected.items():
            if field in side:
                require(side[field] == value, "publication side lost immutable " + field)

    def prepare_candidate(self, route: str = ROUTE_ID, *, role: str = "admin",
                          project: str = PROJECT_ID) -> dict[str, Any]:
        local = self.activation(project, route)
        status, response = self.post_preparation(project, route, local["snapshot_id"],
                                                 "publication-candidate-" + secrets.token_hex(8), role=role)
        require(status == 202, "publication fixture preparation was not admitted")
        queued = self.assert_public_job(response["result"]["job"], project_id=project, route_id=route)
        job = self.wait_for_terminal(queued["id"], "prepared", project_id=project, route_id=route)
        row = self.sql_json("SELECT to_jsonb(p) FROM shin_server.preparations p WHERE id=" + str(job["id"]))
        require(row["expected_route_revision"] == local["revision"] and
                row["expected_snapshot_id"] == (local["snapshot_id"] or ""), "candidate captured the shared head instead of source base")
        job = {**job, "workspace_id": "secondary" if route == PEER_ROUTE else "primary",
               "author": "dev-admin" if role == "admin" else "dev-reader",
               "profile_id": route if route.startswith("preflight-") else "generic-docs",
               "route_fingerprint": row["route_fingerprint"]}
        require(row["principal_id"] == job["author"], "candidate author differs from SQL")
        self.jobs[job["id"]] = job
        return job

    def configure_publications(self) -> None:
        self.configure_retrieval()
        self.configure_preparations()
        self.write_fixture_file(self.fixture_root / "docs" / "publication-a.md",
                                "# Workspace Alpha\n\nAlphaWorkspaceEvidence immutable publication review.\n")
        self.write_fixture_file(self.peer_root / "README.md", "# Workspace Beta\n\nIndependent local documentation.\n")
        self.write_fixture_file(self.peer_root / "docs" / "policy.md",
                                "# Route Policy\n\nRoutes bind a project identity, profile fingerprint, and generated output.\n")
        self.write_fixture_file(self.peer_root / "docs" / "publication-b.md",
                                "# Workspace Beta\n\nBetaWorkspaceEvidence immutable publication review.\n")
        self._write_preflight_profiles()
        for path in (self.runtime_config_path, self.migrator_config_path):
            config = json.loads(path.read_text(encoding="utf-8"))
            primary = next(route for route in config["retrieval"]["routes"] if route["id"] == ROUTE_ID)
            config["retrieval"]["routes"].append({**primary, "id": PEER_ROUTE, "workspace_id": "secondary",
                                                   "root": str(self.peer_root)})
            config["retrieval"]["routes"].append({**primary, "id": "withdraw-private"})
            for profile in ("preflight-eval-failing", "preflight-pack-failing", "preflight-not-configured"):
                config["retrieval"]["routes"].append({**primary, "id": profile, "profile": profile,
                                                       "profile_dir": str(self._preflight_profiles_dir())})
            self.replace_private_json(path, config)
        require(all(not (root / ".git").exists() for root in (self.fixture_root, self.peer_root, self.other_root)),
                "publication fixtures must not require Git")

    def check_source_bases_and_seed(self) -> None:
        self.local_heads = {}
        for route in (ROUTE_ID, PEER_ROUTE):
            seed = self.prepare_candidate(route)
            status, response = self._post_activation(PROJECT_ID, route, seed["id"], 0)
            require(status == 200, "source route seed was not explicitly activated")
            self._receipt(response, replayed=False, project_id=PROJECT_ID, route_id=route,
                          previous_revision=0, preparation_id=seed["id"], snapshot_id=seed["artifact_snapshot_id"])
            self.local_heads[route] = self.activation(PROJECT_ID, route)
        self.alpha = self.prepare_candidate()
        self.beta = self.prepare_candidate(PEER_ROUTE)
        require(self.alpha["artifact_snapshot_id"] != self.beta["artifact_snapshot_id"], "workspace snapshots are not distinct")
        before = self.durable_state()
        initial = self.get_head()
        require(initial == {"project_id": PROJECT_ID, "publication_id": PUBLICATION,
                            **dict.fromkeys(IDENTITY_FIELDS), "state": "empty", "revision": 0, "history_epoch": 0}, "shared publication did not start empty")
        self.unchanged(before, "empty head lookup")

    def assert_source_bases(self) -> None:
        for route, expected in self.local_heads.items():
            require(self.activation(PROJECT_ID, route) == expected, "shared publication changed a source route active base")

    def assert_cross_workspace_review(self, proposal_id: int, selected: dict[str, Any], candidate: dict[str, Any]) -> None:
        before = self.durable_state()
        expected_detail = self.get_detail(proposal_id)
        roots = (self.fixture_root, self.peer_root, self.preparation_root)
        digests = [self.tree_digest(root) for root in roots]
        response = self.expect_status(200, "GET", self.path(suffix=f"/proposals/{proposal_id}/review"),
                                      headers=self.auth_headers("reader"))
        result = self.public_result(response)
        require(isinstance(result, dict) and set(result) == set(expected_detail) | {"review"}, "review detail fields differ")
        require({key: result[key] for key in expected_detail} == expected_detail, "review changed captured detail provenance")
        review = exact_object(result["review"], {"schema_version", "artifacts", "comparisons", "scope", "source_currentness",
                                                "publication_authorized", "semantic_equivalence"}, "artifact review")
        for field, value in (("schema_version", "shin.publication-review.v1"), ("scope", "artifact-source-fingerprints"),
                             ("source_currentness", "not-assessed"), ("semantic_equivalence", "not-assessed")):
            require(review[field] == value, "artifact review changed its authority boundary: " + field)
        require(review["publication_authorized"] is False, "artifact review authorized publication")
        require(review["artifacts"] == {"base": {"state": "empty"},
                "current": {"state": "artifact-verified", "snapshot_id": selected["snapshot_id"]},
                "candidate": {"state": "artifact-verified", "snapshot_id": candidate["snapshot_id"]}},
                "two-workspace review did not verify the selected artifact identities")
        comparisons = exact_object(review["comparisons"], {"base", "current"}, "review comparisons")
        require(comparisons["base"] == {"state": "not-compared"}, "empty base fabricated source changes")
        comparison = exact_object(comparisons["current"], {"counts", "changes", "omitted_changes", "same_profile", "same_generator"},
                                  "review source comparison")
        manifests = [json.loads((self._inspection_artifact(side["preparation_id"]) / "docs" / "kb" / "generated" / "manifest.json").read_text(encoding="utf-8"))
                     for side in (selected, candidate)]
        expected_changes = set()
        for field, kind in (("sources", "document-source"), ("source_fact_sources", "fact-source")):
            left, right = ({row["path"]: row for row in manifest[field]} for manifest in manifests)
            expected_changes.update((path, kind, "removed") for path in left.keys() - right.keys())
            expected_changes.update((path, kind, "added") for path in right.keys() - left.keys())
            expected_changes.update((path, kind, "changed") for path in left.keys() & right.keys() if left[path] != right[path])
        require(0 < len(expected_changes) <= 32, "neutral review fixture is not a small nonempty source difference")
        require(isinstance(comparison["changes"], list) and len(comparison["changes"]) == len(expected_changes),
                "review source comparison omitted or duplicated small-fixture changes")
        actual_changes = set()
        for change in comparison["changes"]:
            exact_object(change, {"path", "kind", "change"}, "source change")
            actual_changes.add((change["path"], change["kind"], change["change"]))
        require(actual_changes == expected_changes, "review changes differ from independent artifact manifest rows")
        require(comparison["counts"] == {kind: sum(item[2] == kind for item in expected_changes)
                                           for kind in ("added", "removed", "changed")} and
                comparison["omitted_changes"] == 0 and comparison["same_profile"] is True and comparison["same_generator"] is True,
                "review counts, omission or profile/generator metadata differ")
        require(sum(len(json.dumps(change, separators=(",", ":")).encode("utf-8")) for change in comparison["changes"]) <= 32768,
                "review source-change bytes exceeded the global bound")
        self.unchanged(before, "read-only artifact review")
        require(digests == [self.tree_digest(root) for root in roots], "artifact review rewrote source or immutable artifacts")

    def check_competition_and_explicit_current_cas(self) -> None:
        before_jobs = self.table_count("preparations")
        alpha = self.submit(self.alpha, 0, "publication-alpha")
        beta = self.submit(self.beta, 0, "publication-beta")
        require(alpha["base"] == beta["base"] and
                {key: alpha["base"][key] for key in IDENTITY_FIELDS} == {**dict.fromkeys(IDENTITY_FIELDS), "state": "empty", "revision": 0},
                "same-base submissions captured different bases")
        require(self.get_head()["revision"] == 0, "proposal submission activated a shared head")
        before = self.durable_state()
        barrier = threading.Barrier(2)

        def accept(proposal: dict[str, Any]) -> tuple[int, dict[str, Any]]:
            barrier.wait(timeout=5.0)
            return self.resolve_request(proposal["proposal"]["id"], "accept", 0, retry_busy=True)

        with ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(accept, proposal) for proposal in (alpha, beta)]
            responses = [future.result(timeout=25.0) for future in futures]
        require(sorted(status for status, _ in responses) == [200, 409], "two same-revision accepts did not have exactly one winner")
        winner_index = next(index for index, (status, _) in enumerate(responses) if status == 200)
        winner, loser = (alpha, beta)[winner_index], (alpha, beta)[1 - winner_index]
        self.assert_error_code(responses[1 - winner_index][1], "publication-conflict")
        winner_id, loser_id = winner["proposal"]["id"], loser["proposal"]["id"]
        receipt = self.assert_receipt(responses[winner_index][1], winner_id, "accept", 0, PUBLICATION, replayed=False)
        self.receipts.append((winner_id, "accept", 0, receipt))
        current = self.get_head()
        require(current["revision"] == 1 and current["preparation_id"] == winner["candidate"]["preparation_id"],
                "race winner does not match the shared head")
        self.assert_head_sql(current)
        after = self.durable_state()
        require(len(after["publication_resolutions"]) == len(before["publication_resolutions"]) + 1 and
                len(after["audit_events"]) == len(before["audit_events"]) + 1, "accept race wrote duplicate or missing receipt/audit")
        require(self.get_detail(loser_id)["proposal"]["state"] == "conflicted", "losing proposal vanished or became terminal")
        stale = self.submit(self.jobs[loser["candidate"]["preparation_id"]], 0, "publication-stale-base")
        require(stale["proposal"]["state"] == "conflicted", "known stale base was not retained")
        self.assert_cross_workspace_review(stale["proposal"]["id"], winner["candidate"], loser["candidate"])
        reprepare_receipt = self.resolution(stale["proposal"]["id"], "request-reprepare", 1)
        self.receipts.append((stale["proposal"]["id"], "request-reprepare", 1, reprepare_receipt))
        require(self.get_head() == current, "request-reprepare changed the shared head")
        before = self.durable_state()
        status, response = self.resolve_request(loser_id, "accept", 0)
        self._assert_error(status, response, 409, "publication-conflict")
        self.unchanged(before, "old reviewer CAS")
        second_receipt = self.resolution(loser_id, "accept", current["revision"])
        self.receipts.append((loser_id, "accept", 1, second_receipt))
        require(self.get_head()["revision"] == 2 and
                self.get_head()["preparation_id"] == loser["candidate"]["preparation_id"],
                "explicit current-head review could not choose the older candidate")
        self.assert_source_bases()
        require(self.table_count("preparations") == before_jobs, "submit/review/request-reprepare enqueued implicit work")
        self.old_job = self.jobs[winner["candidate"]["preparation_id"]]
        self.selected_job = self.jobs[loser["candidate"]["preparation_id"]]

    def assert_retrieval(self, job: dict[str, Any], revision: int, publication: str = PUBLICATION) -> None:
        token, expected_path, forbidden = (("AlphaWorkspaceEvidence", "docs/publication-a.md", "BetaWorkspaceEvidence")
                                            if job["route_id"] == ROUTE_ID else
                                            ("BetaWorkspaceEvidence", "docs/publication-b.md", "AlphaWorkspaceEvidence"))
        root = self.fixture_root if job["route_id"] == ROUTE_ID else self.peer_root
        task = {"schema_version": "shin.task-request.v1", "request_id": "publication-provenance",
                "task": {"objective": "Inspect selected publication evidence.", "must": [{"id": "selected", "query": token}]},
                "budget": {"max_evidence_bytes": 8192, "max_records": 6, "per_query_records": 6, "per_document_records": 2}}
        self.write_fixture_file(self.task_path, json.dumps(task) + "\n")
        requests = (("search", {"query": token}, ["search", token]),
                    ("brief", {"query": token}, ["brief", token, "--projection", "compact"]),
                    ("pack", {"task_request": task}, ["pack", "--input", str(self.task_path), "--jobs", "1", "--projection", "compact"]))
        for action, body, arguments in requests:
            for strict in (True, False):
                expected = self.run_external_core_json(root, self._inspection_artifact(job["id"]),
                    *arguments, *(["--require-current"] if strict else []), expected_success=True)
                status, response = self.post_json(self.path(publication, "/" + action), {**body, "require_current": strict}, role="reader")
                require(status == 200, "publication " + action + " did not retrieve its admitted candidate")
                result = exact_object(self.public_result(response), {"project_id", "publication_id", "route_id", "workspace_id",
                    "source_host_id", "preparation_id", "snapshot_id", "revision", "admission", "evidence"}, "publication retrieval")
                for key, value in (("project_id", PROJECT_ID), ("publication_id", publication),
                                   ("route_id", job["route_id"]), ("workspace_id", job["workspace_id"]),
                                   ("source_host_id", "local-dev"), ("preparation_id", job["id"]),
                                   ("snapshot_id", job["artifact_snapshot_id"]), ("revision", revision),
                                   ("admission", "exact-current" if strict else "snapshot-only")):
                    require(result.get(key) == value, "publication retrieval provenance differs: " + key)
                evidence = result.get("evidence")
                require(isinstance(evidence, dict), "publication retrieval omitted Core evidence")
                if strict:
                    require(evidence == self.transport_evidence(expected), "publication " + action + " differs from independently executed native Core")
                else:
                    require(self.snapshot_only_parity_document(evidence) == self.snapshot_only_parity_document(expected),
                            "publication " + action + " snapshot-only evidence differs from native Core")
                    freshness = evidence.get("index_freshness", {})
                    require(freshness.get("status") == "not-assessed" and freshness.get("current") is False and
                            freshness.get("fully_verified") is False, "snapshot-only retrieval claimed current source authority")
                rendered = json.dumps(evidence)
                require(expected_path in rendered and forbidden not in rendered, "retrieval mixed workspace evidence")
                if action == "pack":
                    require(evidence.get("completion", {}).get("complete") is True, "selected publication Pack is incomplete")

    def check_exact_provenance_and_read_only_retrieval(self) -> None:
        before = self.durable_state()
        trees = [self.tree_digest(root) for root in (self.fixture_root, self.peer_root, self.preparation_root)]
        current = self.get_head()
        self.assert_retrieval(self.jobs[current["preparation_id"]], current["revision"])
        for proposal_id, _, _, _ in self.receipts:
            self.get_detail(proposal_id, role="reader")
        self.assert_source_bases()
        self.unchanged(before, "publication metadata and retrieval")
        require(trees == [self.tree_digest(root) for root in (self.fixture_root, self.peer_root, self.preparation_root)],
                "publication reads rewrote fixture or artifact files")

    def check_retention_pins_release_and_historical_replay(self) -> None:
        current = self.get_head()
        old_plan = self._retention_plan(self.old_job["id"])
        pin = self.submit(self.old_job, current["revision"], "publication-pin")
        require({key: pin["base"][key] for key in IDENTITY_FIELDS} == {key: current[key] for key in IDENTITY_FIELDS},
                "pin did not capture exact base provenance")
        replacement = self.prepare_candidate()
        selected = self.submit(replacement, current["revision"], "publication-replacement")
        self.resolution(selected["proposal"]["id"], "accept", current["revision"])
        for job in (self.old_job, self.selected_job, replacement):
            plan = self._retention_get(job["id"])
            require(plan["can_apply"] is False and plan["reason_codes"] == ["candidate-active"],
                    "candidate/base/head reference did not pin its artifact")
            before = self.durable_state()
            status, response = self._retention_post(job["id"], old_plan["plan_id"] if job == self.old_job else "0" * 64)
            self._assert_error(status, response, 409, "candidate-active")
            self.unchanged(before, "pinned candidate retention")
        self.resolution(pin["proposal"]["id"], "reject", current["revision"] + 1)
        for job in (self.old_job, self.selected_job):
            artifact = self._inspection_artifact(job["id"])
            self._release_candidate(job)
            require(not artifact.exists(), "released publication artifact remains on disk")
            require(self.scalar("SELECT state FROM shin_server.preparation_retention WHERE preparation_id=" + str(job["id"])) == "released",
                    "publication release was not durable")
        sources = (self.fixture_root / "docs" / "publication-a.md", self.peer_root / "docs" / "publication-b.md")
        originals = {source: source.read_text(encoding="utf-8") for source in sources}
        try:
            for source in sources:
                source.unlink()
            before = self.durable_state()
            for proposal_id, decision, revision, receipt in self.receipts:
                require(self.resolution(proposal_id, decision, revision, replayed=True) == receipt,
                        "historical resolution replay changed after head advance and artifact release")
                status, response = self.resolve_request(proposal_id, "reject", revision)
                self._assert_error(status, response, 409, "publication-conflict")
                status, response = self.resolve_request(proposal_id, decision, revision + 1)
                self._assert_error(status, response, 409, "publication-conflict")
            for job, key in ((self.alpha, "publication-alpha"), (self.beta, "publication-beta")):
                replay = self.submit(job, 0, key, replayed=True)
                require(replay["proposal"]["state"] == "accepted", "submission replay lost its terminal resolution")
            status, response = self.post_json(self.path(suffix="/proposals"),
                                             {"preparation_id": replacement["id"], "base_revision": 2}, key="released-historical-base")
            self._assert_error(status, response, 409, "candidate-retention-pending")
            self.unchanged(before, "terminal replays and differing retries")
        finally:
            for source, original in originals.items():
                self.write_fixture_file(source, original)
        self.assert_source_bases()

    def check_source_drift_and_quality_refusal(self) -> None:
        job = self.prepare_candidate()
        current = self.get_head()
        proposal = self.submit(job, current["revision"], "publication-source-drift")
        path = self.fixture_root / "docs" / "publication-a.md"
        original = path.read_text(encoding="utf-8")
        self.write_fixture_file(path, original + "\nChanged source must not authorize the prepared candidate.\n")
        try:
            before = self.durable_state()
            status, response = self.resolve_request(proposal["proposal"]["id"], "accept", current["revision"], retry_busy=True)
            self._assert_error(status, response, 409, "activation-preflight-failed")
            self.unchanged(before, "source-drift acceptance refusal")
            require(self.get_detail(proposal["proposal"]["id"])["proposal"]["state"] == "pending", "drift refusal closed the proposal")
        finally:
            self.write_fixture_file(path, original)
        self.resolution(proposal["proposal"]["id"], "request-reprepare", current["revision"])
        self._release_candidate(job)
        for route in ("preflight-eval-failing", "preflight-pack-failing", "preflight-not-configured"):
            failed = self.prepare_candidate(route)
            proposal = self.submit(failed, 0, "publication-" + route, publication=route)
            before = self.durable_state()
            status, response = self.resolve_request(proposal["proposal"]["id"], "accept", 0, route, retry_busy=True)
            self._assert_error(status, response, 409, "activation-preflight-failed")
            self.unchanged(before, "strict " + route + " refusal")
            self.resolution(proposal["proposal"]["id"], "reject", 0, route)
            self._release_candidate(failed)

    def check_authorization_and_ownership(self) -> None:
        current_job = self.jobs[self.get_head()["preparation_id"]]
        pending = self.submit(current_job, 0, "publication-auth", "authorization")
        proposal_id = pending["proposal"]["id"]
        hidden = self.prepare_candidate("other-retrieval", project="other")
        before = self.durable_state()
        self.expect_status(401, "GET", self.path("authorization"))
        for suffix in ("", "/proposals", f"/proposals/{proposal_id}", f"/proposals/{proposal_id}/review"):
            self.expect_status(404, "GET", self.path("authorization", suffix, project="other"), headers=self.auth_headers("reader"))
        self.expect_status(404, "GET", self.path("another-publication", f"/proposals/{proposal_id}"), headers=self.auth_headers("admin"))
        status, _ = self.post_json(self.path("authorization", "/proposals", project="other"),
                                  {"preparation_id": hidden["id"], "base_revision": 0}, role="reader", key="wrong-tenant-submit")
        require(status == 404, "proposal submission crossed the tenant boundary")
        status, response = self.post_json(self.path("authorization", "/proposals"),
                                         {"preparation_id": current_job["id"], "base_revision": 0}, role="reader", key="foreign-owner")
        self._assert_error(status, response, 404, "preparation-not-found")
        for decision in ("accept", "reject", "request-reprepare"):
            status, _ = self.resolve_request(proposal_id, decision, 0, "authorization", role="reader")
            require(status == 403, "non-administrator could resolve a proposal")
        status, _ = self.post_json(self.path("authorization", "/proposals"),
                                  {"preparation_id": hidden["id"], "base_revision": 0}, key="wrong-project-candidate")
        require(status == 404, "administrator attached a candidate from another project")
        for action in ("search", "brief", "pack"):
            body = {"task_request": self.task_request()} if action == "pack" else {"query": "private"}
            status, _ = self.post_json(self.path("authorization", "/" + action, project="other"), body, role="reader")
            require(status == 404, "publication retrieval crossed the tenant boundary")
        self.unchanged(before, "authorization denials")
        self.resolution(proposal_id, "reject", 0, "authorization")
        self._release_candidate(hidden)

        # Preparation remains administrator-only. Demote its genuine author after
        # the worker completes to exercise the separate proposal author policy.
        self.stop_server(expect_clean=True)
        self.set_reader_role("admin")
        self.start_server()
        try:
            owned = self.prepare_candidate(role="reader")
        finally:
            self.stop_server(expect_clean=False)
            self.set_reader_role("reader")
            self.start_server()
        owned_proposal = self.submit(owned, 0, "reader-owned-proposal", "author-owned", role="reader")
        require(owned_proposal["proposal"]["submitted_by"] == "dev-reader" and
                owned_proposal["candidate"]["candidate_author"] == "dev-reader", "author submission was relabeled as administrator")
        self.resolution(owned_proposal["proposal"]["id"], "reject", 0, "author-owned")
        self._release_candidate(owned)

    def set_reader_role(self, role: str) -> None:
        require(self.server is None, "principal fixture changed while the server was running")
        config = json.loads(self.runtime_config_path.read_text(encoding="utf-8"))
        principal = next(item for item in config["principals"] if item["id"] == "dev-reader")
        principal["role"] = role
        principal["project_ids"] = [] if role == "admin" else [PROJECT_ID]
        self.replace_private_json(self.runtime_config_path, config)

    @contextmanager
    def fail_publication_audit(self) -> Iterator[None]:
        function = "publication_test_audit_" + secrets.token_hex(6)
        self.psql(f"CREATE FUNCTION shin_server.{function}() RETURNS trigger LANGUAGE plpgsql AS $$ "
                  "BEGIN IF NEW.action LIKE 'publication.%' THEN RAISE EXCEPTION 'controlled publication audit failure'; "
                  "END IF; RETURN NEW; END $$; "
                  f"CREATE TRIGGER {function} BEFORE INSERT ON shin_server.audit_events "
                  f"FOR EACH ROW EXECUTE FUNCTION shin_server.{function}()")
        try:
            yield
        finally:
            self.psql(f"DROP TRIGGER IF EXISTS {function} ON shin_server.audit_events; DROP FUNCTION shin_server.{function}()")

    def check_audit_transaction_rollback(self) -> None:
        job = self.jobs[self.get_head()["preparation_id"]]
        before = self.durable_state()
        with self.fail_publication_audit():
            status, _ = self.post_json(self.path("audit", "/proposals"),
                                      {"preparation_id": job["id"], "base_revision": 0}, key="publication-audit-submit")
            require(status >= 500, "audit trigger did not abort proposal submission")
        self.unchanged(before, "failed proposal audit transaction")
        proposal = self.submit(job, 0, "publication-audit-submit", "audit")
        for decision in ("accept", "reject", "request-reprepare"):
            before = self.durable_state()
            with self.fail_publication_audit():
                status, _ = self.resolve_request(proposal["proposal"]["id"], decision, 0, "audit", retry_busy=True)
                require(status >= 500, "audit trigger did not abort " + decision)
            self.unchanged(before, "failed " + decision + " audit transaction")
        self.resolution(proposal["proposal"]["id"], "reject", 0, "audit")

    def send_without_reply(self, target: str, payload: dict[str, Any], *, key: str | None = None) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        headers = {"Host": f"{HOST}:{PORT}", **self.auth_headers("admin"), "Content-Type": "application/json",
                   "Content-Length": str(len(body)), "Connection": "close"}
        if key is not None:
            headers["Idempotency-Key"] = key
        wire = (f"POST {target} HTTP/1.1\r\n" + "".join(f"{name}: {value}\r\n" for name, value in headers.items()) + "\r\n").encode("ascii") + body
        try:
            with socket.create_connection((HOST, PORT), timeout=4.0) as connection:
                connection.sendall(wire)
                connection.shutdown(socket.SHUT_WR)
        except OSError as error:
            raise IntegrationFailure("lost-reply publication request could not be sent") from error

    def wait_for_sql_count(self, table: str, expected: int) -> None:
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            if self.table_count(table) == expected:
                return
            time.sleep(0.025)
        raise IntegrationFailure("lost-reply publication request did not commit")

    def check_restart_and_lost_reply_replay(self) -> None:
        job = self.jobs[self.get_head()["preparation_id"]]
        before_count = self.table_count("publication_proposals")
        self.send_without_reply(self.path("lost-reply", "/proposals"),
                                {"preparation_id": job["id"], "base_revision": 0}, key="publication-lost-submit")
        self.wait_for_sql_count("publication_proposals", before_count + 1)
        before = self.durable_state()
        self.kill_owned_server()
        self.start_server()
        proposal = self.submit(job, 0, "publication-lost-submit", "lost-reply", replayed=True)
        self.unchanged(before, "lost submission replay after restart")
        proposal_id = proposal["proposal"]["id"]
        before_count = self.table_count("publication_resolutions")
        self.send_without_reply(self.path("lost-reply", f"/proposals/{proposal_id}/resolution"),
                                {"decision": "accept", "expected_revision": 0})
        self.wait_for_sql_count("publication_resolutions", before_count + 1)
        completed = self.get_detail(proposal_id, "lost-reply")["resolution"]
        current = self.get_head("lost-reply")
        before = self.durable_state()
        self.kill_owned_server()
        self.start_server()
        require(self.resolution(proposal_id, "accept", 0, "lost-reply", replayed=True) == completed,
                "lost resolution reply did not replay byte-equivalent receipt after restart")
        require(self.get_head("lost-reply") == current, "restart lost the shared publication head")
        self.unchanged(before, "lost resolution replay after restart")

    def check_history_survives_source_route_reconfiguration(self) -> None:
        job = self.jobs[self.get_head()["preparation_id"]]
        require(job["route_id"] == ROUTE_ID and job["workspace_id"] == "primary", "history fixture must use the original source")
        proposal = self.submit(job, 0, "publication-history", "history")
        proposal_id = proposal["proposal"]["id"]
        receipt = self.resolution(proposal_id, "accept", 0, "history")
        historical_detail = self.get_detail(proposal_id, "history")
        historical_head = self.get_head("history")
        before = self.durable_state()
        original_config = json.loads(self.runtime_config_path.read_text(encoding="utf-8"))
        changed_config = json.loads(json.dumps(original_config))
        route = next(item for item in changed_config["retrieval"]["routes"] if item["id"] == ROUTE_ID)
        route["workspace_id"] = "secondary"
        route["root"] = str(self.peer_root)
        missing = self.fixture_root.with_name("publication-history-source-unavailable")
        require(not missing.exists(), "history source relocation target already exists")
        self.stop_server(expect_clean=True)
        try:
            self.replace_private_json(self.runtime_config_path, changed_config)
            self.start_server()
            self.fixture_root.rename(missing)
            require(self.get_detail(proposal_id, "history") == historical_detail,
                    "history was relabeled using the changed route's workspace")
            require(self.get_head("history") == historical_head, "historical head source changed with configuration")
            replay = self.submit(job, 0, "publication-history", "history", replayed=True)
            require(replay == historical_detail, "submission replay required live source or rewrote stored provenance")
            require(self.resolution(proposal_id, "accept", 0, "history", replayed=True) == receipt,
                    "resolution replay required live source binding after reconfiguration")
            reviewed = self.expect_status(200, "GET", self.path("history", f"/proposals/{proposal_id}/review"),
                                          headers=self.auth_headers("reader"))["result"]
            require({key: reviewed[key] for key in historical_detail} == historical_detail,
                    "artifact review relabeled historical provenance")
            for side in ("current", "candidate"):
                require(reviewed["review"]["artifacts"][side] ==
                        {"state": "artifact-verified", "snapshot_id": job["artifact_snapshot_id"]},
                        "historical artifact review required the current route or live source")
            require(reviewed["review"]["source_currentness"] == "not-assessed" and
                    reviewed["review"]["publication_authorized"] is False,
                    "historical artifact review falsely certified live source or publication")
            status, refused = self.post_json(self.path("history", "/brief"), {"query": "AlphaWorkspaceEvidence"}, role="reader")
            require(status == 409, "historical review permitted retrieval through a changed source route")
            self.assert_error_code(refused, "active-generation-route-changed")
            self.unchanged(before, "historical provenance and replay after route reconfiguration")
        finally:
            self.stop_server(expect_clean=False)
            if missing.exists():
                missing.rename(self.fixture_root)
            self.replace_private_json(self.runtime_config_path, original_config)
            self.start_server()

    def check_review_retention_interleaving(self) -> None:
        epoch = self.get_head("review-race")["history_epoch"]
        first = self.prepare_candidate()
        second = self.prepare_candidate(PEER_ROUTE)
        initial = self.submit(first, 0, "review-race-first", "review-race", epoch=epoch)
        self.resolution(initial["proposal"]["id"], "accept", 0, "review-race")
        historical = self.submit(first, 1, "review-race-historical", "review-race", epoch=epoch)
        proposal_id = historical["proposal"]["id"]
        self.resolution(proposal_id, "reject", 1, "review-race")
        replacement = self.submit(second, 1, "review-race-second", "review-race", epoch=epoch)
        self.resolution(replacement["proposal"]["id"], "accept", 1, "review-race")
        probe = self.binary.with_name("shin-server-publication-review-tests")
        require(under_build(probe) and probe.is_file() and os.access(probe, os.X_OK), "native publication review probe is unavailable")
        before = self.durable_state()
        completed = subprocess.run([str(probe), str(self.runtime_config_path), str(proposal_id)],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, timeout=50, check=False)
        require(completed.returncode == 0, "native review/retention interleaving probe failed")
        require(json.loads(completed.stdout) == {"ok": True, "retention_error": "candidate-artifact-busy", "history_error": "publication-review-busy",
                "all_artifacts_verified": True, "locks_released": True}, "native review/retention result differs")
        self.unchanged(before, "native review/retention interleaving")

    def check_pagination_and_invalid_input(self) -> None:
        job = self.jobs[self.get_head()["preparation_id"]]
        ids = [self.submit(job, 0, f"publication-page-{index}", "pages")["proposal"]["id"] for index in range(65)]
        before = self.durable_state()
        cursor = 0
        seen = []
        for _ in range(67):
            response = self.expect_status(200, "GET", self.path("pages", f"/proposals?after={cursor}&limit=1"),
                                          headers=self.auth_headers("reader"))
            page = exact_object(self.public_result(response), {"items", "next_after", "history_epoch", "history_complete"}, "proposal page")
            require(page["history_epoch"] == 0 and page["history_complete"], "fresh history was reported incomplete")
            require(isinstance(page["items"], list) and len(page["items"]) <= 1, "proposal page exceeded limit")
            for item in page["items"]:
                proposal = detail(item, PROJECT_ID, "pages")
                item_id = proposal["proposal"]["id"]
                require(item_id > cursor, "proposal keyset cursor did not advance")
                seen.append(item_id)
            if page["next_after"] is None:
                break
            require(page["items"] and type(page["next_after"]) is int and page["next_after"] == seen[-1],
                    "proposal cursor does not match last returned id")
            cursor = seen[-1]
        require(seen == ids, "pagination skipped, duplicated or reordered proposals")
        default = self.expect_status(200, "GET", self.path("pages", "/proposals"), headers=self.auth_headers("reader"))["result"]
        require(len(default["items"]) <= 64 and default["next_after"] is not None, "default proposal page is unbounded")
        maximum = self.expect_status(200, "GET", self.path("pages", "/proposals?limit=64"), headers=self.auth_headers("reader"))["result"]
        require([item["proposal"]["id"] for item in maximum["items"]] == ids[:64], "maximum page did not return exactly 64 ordered proposals")
        for query in ("limit=0", "limit=65", "limit=-1", "limit=true", "after=-1", "after=abc",
                      "after=9223372036854775808", "unknown=1", "limit=1&limit=2", "after=0&after=1"):
            self.expect_status(400, "GET", self.path("pages", "/proposals?" + query), headers=self.auth_headers("admin"))
        for payload in (b"", b"[]", b"{}", {"preparation_id": 0, "base_revision": 0},
                        {"preparation_id": True, "base_revision": 0}, {"preparation_id": job["id"], "base_revision": -1},
                        {"preparation_id": job["id"], "base_revision": True},
                        {"preparation_id": 9223372036854775808, "base_revision": 0},
                        {"preparation_id": job["id"], "base_revision": 0, "workspace_id": "forged"}):
            status, _ = self.post_json(self.path("pages", "/proposals"), payload, key="invalid-publication-input")
            require(status == 400, "invalid proposal input was admitted")
        status, _ = self.post_json(self.path("pages", "/proposals"), {"preparation_id": job["id"], "base_revision": 0})
        require(status == 400, "submission without Idempotency-Key was admitted")
        for payload in ({}, {"decision": "merge", "expected_revision": 0}, {"decision": "accept", "expected_revision": -1},
                        {"decision": "accept", "expected_revision": True},
                        {"decision": "accept", "expected_revision": 9223372036854775808},
                        {"decision": "reject", "expected_revision": 0, "preparation_id": job["id"]}):
            status, _ = self.post_json(self.path("pages", f"/proposals/{ids[0]}/resolution"), payload)
            require(status == 400, "invalid resolution input was admitted")
        status, response = self.post_json(self.path("pages", "/proposals"),
                                         {"preparation_id": job["id"], "base_revision": 1}, key="publication-page-0")
        require(status == 409, "changed idempotent submission was not rejected")
        self.unchanged(before, "pagination and invalid inputs")

    def check_bounded_load(self) -> None:
        if not self.load_enabled:
            return
        job = self.jobs[self.get_head()["preparation_id"]]
        before = {table: self.table_count(table) for table in DB_TABLES}
        samples: dict[str, list[float]] = {name: [] for name in ("submit", "resolve", "read", "replay")}
        for index in range(self.load_requests):
            proposal = self.submit(job, 0, f"publication-load-{index}", "load")
            samples["submit"].append(self.last_http_ms)
            proposal_id = proposal["proposal"]["id"]
            receipt = self.resolution(proposal_id, "reject", 0, "load")
            samples["resolve"].append(self.last_http_ms)
            require(self.get_detail(proposal_id, "load")["resolution"] == receipt, "load detail lost a resolution")
            samples["read"].append(self.last_http_ms)
            require(self.resolution(proposal_id, "reject", 0, "load", replayed=True) == receipt, "load replay changed its receipt")
            samples["replay"].append(self.last_http_ms)
        after = {table: self.table_count(table) for table in DB_TABLES}
        delta = {table: after[table] - before[table] for table in DB_TABLES}
        require(delta["publication_proposals"] == self.load_requests and delta["publication_resolutions"] == self.load_requests and
                delta["audit_events"] == 2 * self.load_requests, "bounded load durable counts differ from exact operations")
        require(all(delta[table] == 0 for table in ("preparations", "preparation_retention", "active_routes", "activation_receipts",
                                                  "publication_heads", "publication_withdrawals")),
                "bounded load executed preparation, activation or retention work")
        self.publication_measurement = {
            "status": "passed", "cycles": self.load_requests, "logical_http_requests": 4 * self.load_requests,
            "fixture_workspaces": 2, "fixture_candidates_reused": 1, "durable_row_deltas": delta,
            "latency": {name: {"samples": len(values), "p50_ms": self.percentile_ms(values, 50),
                                "p95_ms": self.percentile_ms(values, 95)} for name, values in samples.items()},
            "scope": "Sequential local HTTP timings with response decoding; independent SQL assertions are outside timing. "
                     "Preparation excluded; rejection only, not acceptance capacity, production RPS or an SLO.",
        }

    def write_report(self, *, ok: bool, failure: str | None = None) -> None:
        super().write_report(ok=ok, failure=failure)
        report = json.loads(self.report_path.read_text(encoding="utf-8"))
        report["publication_measurement"] = self.publication_measurement
        report["publication_scope"] = ("foundation-and-publication-history" if self.history_only else
                                       "foundation-and-checkpoint" if self.checkpoint_only else
                                       "foundation-and-publication" if self.publication_only else "full-inherited-and-publication")
        report["publication_inspection_busy_retries"] = self.busy_retries
        report["publication_history_measurement"] = getattr(self, "publication_history_measurement", {"status": "not-run"})
        report["publication_evidence_boundary"] = "Real PostgreSQL and HTTP only when this harness is executed. No mutation-test score or production capacity claim."
        report["retrieval_journal_measurement"] = self.retrieval_journal_measurement
        report["retrieval_journal_retention_measurement"] = self.retrieval_journal_retention_measurement
        report["retrieval_journal_epoch_measurement"] = getattr(self, "retrieval_journal_epoch_measurement", {"status": "not-run"})
        report["knowledge_measurement"] = self.knowledge_measurement
        report["journal_writer_measurement"] = getattr(self, "journal_writer_measurement", {"status": "not-run"})
        report["retrieval_journal_evidence_boundary"] = "Real PostgreSQL and HTTP only when this harness is executed. " \
                                                      "Bounded local timings are not a production throughput or SLO claim."
        self.replace_private_json(self.report_path, report)

    def reset_publication_fixture(self) -> None:
        self.stop_server(expect_clean=True)
        self.restore_configs()
        self.reset_preparation_rows()
        self.cleanup_private_preparation_root()
        self.prepare_retrieval_fixture()
        self.prepare_private_preparation_root()
        self.configure_publications()
        self.jobs.clear()
        self.receipts.clear()
        self.start_server()

    def run_checkpoint_fixture(self) -> None:
        self.reset_publication_fixture()
        self.record("checkpoint-neutral-source-seed", self.check_source_bases_and_seed)
        from checkpoint_integration_checks import CheckpointIntegrationChecks
        CheckpointIntegrationChecks(self).run()

    def run_history_fixture(self) -> None:
        self.reset_publication_fixture()
        self.record("publication-history-neutral-source-seed", self.check_source_bases_and_seed)
        from publication_history_checks import PublicationHistoryChecks
        PublicationHistoryChecks(self).run()

    def run(self) -> None:
        # Keep all inherited foundation, retrieval, preparation, preflight and
        # activation regressions under this same process-lifetime StandGuard.
        if self.publication_only or self.checkpoint_only or self.history_only:
            IntegrationSuite.run(self)
            self.stop_server(expect_clean=True)
        else:
            super().run()
        try:
            if self.history_only:
                self.run_history_fixture()
                self.stop_server(expect_clean=True)
                return
            if self.checkpoint_only:
                self.run_checkpoint_fixture()
                self.stop_server(expect_clean=True)
                return
            self.record("publication-controlled-reset", self.reset_preparation_rows)
            self.prepare_retrieval_fixture()
            self.prepare_private_preparation_root()
            self.configure_publications()
            self.start_server()
            for name, check in (
                ("publication-source-bases-and-seed", self.check_source_bases_and_seed),
                ("publication-competition-and-current-cas", self.check_competition_and_explicit_current_cas),
                ("publication-exact-provenance-retrieval", self.check_exact_provenance_and_read_only_retrieval),
                ("publication-retention-pins-and-replay", self.check_retention_pins_release_and_historical_replay),
                ("publication-source-and-quality-refusal", self.check_source_drift_and_quality_refusal),
                ("publication-authorization-and-ownership", self.check_authorization_and_ownership),
                ("publication-audit-transaction-rollback", self.check_audit_transaction_rollback),
                ("publication-restart-and-lost-reply", self.check_restart_and_lost_reply_replay),
                ("publication-history-source-reconfiguration", self.check_history_survives_source_route_reconfiguration),
                ("publication-review-retention-interleaving", self.check_review_retention_interleaving),
                ("publication-pagination-and-invalid-input", self.check_pagination_and_invalid_input),
            ):
                self.record(name, check)
            if self.load_enabled:
                self.record("publication-bounded-load", self.check_bounded_load)
            from publication_limit_checks import check_publication_limits
            from publication_withdrawal_checks import PublicationWithdrawalChecks
            PublicationWithdrawalChecks(self).run()
            self.record("publication-history-capacity-boundaries", lambda: check_publication_limits(self))
            from retrieval_observability_checks import RetrievalObservabilityChecks
            RetrievalObservabilityChecks(self).run()
            from retrieval_journal_checks import RetrievalJournalChecks
            RetrievalJournalChecks(self).run()
            from retrieval_journal_retention_checks import RetrievalJournalRetentionChecks
            RetrievalJournalRetentionChecks(self).run()
            from retrieval_journal_epoch_checks import RetrievalJournalEpochChecks
            RetrievalJournalEpochChecks(self).run()
            from knowledge_integration_checks import KnowledgeIntegrationChecks
            knowledge_checks = KnowledgeIntegrationChecks(self)
            knowledge_checks.run()
            self.knowledge_measurement = knowledge_checks.knowledge_measurement
            from journal_writer_checks import JournalWriterChecks
            JournalWriterChecks(self).run()
            self.run_checkpoint_fixture()
            self.run_history_fixture()
            self.stop_server(expect_clean=True)
        finally:
            self.stop_server(expect_clean=False)
            self.restore_configs()
            self.cleanup_private_preparation_root()


def bounded_load_requests(value: str) -> int:
    try:
        count = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("load requests must be an integer") from error
    if not 1 <= count <= MAX_LOAD_REQUESTS:
        raise argparse.ArgumentTypeError(f"load requests must be between 1 and {MAX_LOAD_REQUESTS}")
    return count


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="native candidate shin-server executable")
    parser.add_argument("--core", default=str(DEFAULT_CORE), help="candidate shin-context executable")
    parser.add_argument("--ledger-probe", default=str(DEFAULT_LEDGER_PROBE), help="optional inherited native preparation Store probe")
    parser.add_argument("--state-root", default=str(BUILD_ROOT / "server-dev"))
    parser.add_argument("--report", help="new report path below this worktree build directory")
    parser.add_argument("--load", action="store_true", help="run the optional bounded local workload")
    parser.add_argument("--publication-only", action="store_true", help="focused foundation/publication debugging; never the aggregate release gate")
    parser.add_argument("--checkpoint-only", action="store_true", help="focused foundation/checkpoint debugging; never the aggregate release gate")
    parser.add_argument("--history-only", action="store_true", help="focused foundation/publication-history debugging; never the aggregate release gate")
    parser.add_argument("--load-requests", type=bounded_load_requests, default=128, help="load cycles, four HTTP requests each (default: 128)")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    binary, core = Path(args.binary).resolve(), Path(args.core).resolve()
    state_root = Path(args.state_root).resolve()
    report = Path(args.report).resolve() if args.report else state_root / "reports" / f"publication-integration-{secrets.token_hex(8)}.json"
    if not under_build(binary) or not binary.is_file() or not os.access(binary, os.X_OK):
        print("error: --binary must name this worktree's executable candidate server", file=sys.stderr)
        return 2
    if not under_build(core) or not core.is_file() or not os.access(core, os.X_OK) or not validate_candidate_core(core):
        print("error: --core must name this worktree's current candidate Core", file=sys.stderr)
        return 2
    if not under_build(state_root) or not under_build(report) or report.exists():
        print("error: state/report paths must be below this worktree build directory; report must be new", file=sys.stderr)
        return 2
    suite = None
    try:
        ledger_probe = optional_probe(args.ledger_probe)
        suite = PublicationIntegrationSuite(binary, core, state_root, report, ledger_probe,
                                            load=args.load, load_requests=args.load_requests, publication_only=args.publication_only,
                                            checkpoint_only=args.checkpoint_only, history_only=args.history_only)
        suite.run()
        suite.write_report(ok=True)
    except Exception as error:
        if suite is not None:
            suite.stop_server(expect_clean=False)
            if isinstance(error, IntegrationFailure):
                suite.failure_assertion = str(error)
            try:
                suite.write_report(ok=False, failure=type(error).__name__)
            except OSError:
                pass
        print(f"error: publication integration failed: {type(error).__name__}", file=sys.stderr)
        return 1
    print("publication integration passed; compact report written below isolated build state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
