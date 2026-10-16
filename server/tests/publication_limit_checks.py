"""Quota-boundary probes; large rows are SQL-seeded metadata, not published artifacts."""

from __future__ import annotations

from typing import Any

from publication_test_support import require
from publication_withdrawal_checks import withdraw


COLUMNS = ("project_id,publication_id,preparation_id,base_revision,base_preparation_id,submitted_by,"
           "idempotency_key,request_hash,source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint")


def assert_last_slot(suite: Any, job: dict[str, Any], publication: str, key: str, global_limit: bool) -> None:
    proposal = suite.submit(job, 0, key, publication)
    count_sql = "SELECT count(*) FROM shin_server.publication_proposals"
    if not global_limit:
        count_sql += " WHERE project_id='visible'"
    require(suite.scalar(count_sql) == ("8192" if global_limit else "1024"), "last proposal capacity slot was not admitted exactly")
    before = suite.durable_state()
    status, response = suite.post_json(suite.path(publication, "/proposals"),
                                      {"preparation_id": job["id"], "base_revision": 0}, key=key + "-overflow")
    require(status == 409, "proposal history exceeded its configured bound")
    suite.assert_error_code(response, "publication-proposal-capacity")
    require(suite.submit(job, 0, key, publication, replayed=True) == proposal, "full history did not preserve exact submission replay")
    suite.unchanged(before, "capacity refusal and replay")
    proposal_id = proposal["proposal"]["id"]
    suite.resolution(proposal_id, "accept", 0, publication)
    before = suite.durable_state()
    receipt = withdraw(suite, publication, 1, proposal_id, job["id"])
    after = suite.durable_state()
    require(after["publication_proposals"] == before["publication_proposals"], "full-capacity withdrawal admitted a proposal")
    require(len(after["publication_withdrawals"]) == len(before["publication_withdrawals"]) + 1 and
            len(after["audit_events"]) == len(before["audit_events"]) + 1, "full-capacity withdrawal did not commit once")
    require(suite.scalar(count_sql) == ("8192" if global_limit else "1024"), "withdrawal changed proposal capacity")
    require(suite.get_head(publication)["state"] == "withdrawn", "full quota blocked withdrawal")
    require(withdraw(suite, publication, 1, proposal_id, job["id"], replayed=True) == receipt, "full quota blocked receipt replay")
    suite.unchanged(after, "full-capacity withdrawal replay")


def check_publication_limits(suite: Any) -> None:
    job = suite.jobs[suite.get_head()["preparation_id"]]
    template = suite.submit(job, 0, "capacity-template", "capacity")
    template_id = template["proposal"]["id"]
    suite.resolution(template_id, "reject", 0, "capacity")
    count = int(suite.scalar("SELECT count(*) FROM shin_server.publication_proposals WHERE project_id='visible'"))
    require(0 < count < 1023, "project capacity fixture has insufficient room")
    try:
        suite.psql(
            f"INSERT INTO shin_server.publication_proposals({COLUMNS}) "
            "SELECT p.project_id,'capacity',p.preparation_id,0,NULL,p.submitted_by,"
            "'p3e11-cap-project-'||n,p.request_hash,p.source_route_id,p.workspace_id,p.source_host_id,p.profile_id,p.route_fingerprint "
            f"FROM shin_server.publication_proposals p CROSS JOIN generate_series(1,{1023-count}) n WHERE p.id={template_id}"
        )
        assert_last_slot(suite, job, "capacity", "capacity-last-project", False)
    finally:
        suite.psql("DELETE FROM shin_server.publication_proposals WHERE idempotency_key LIKE 'p3e11-cap-project-%'")

    count = int(suite.scalar("SELECT count(*) FROM shin_server.publication_proposals"))
    needed = 8191 - count
    require(0 < needed <= 8192, "global capacity fixture has insufficient room")
    require(suite.scalar("SELECT count(*) FROM shin_server.projects WHERE id LIKE 'p3e11-cap-global-%'") == "0",
            "global capacity fixture namespace is already occupied")
    try:
        suite.psql(
            "INSERT INTO shin_server.projects(id,display_name) SELECT 'p3e11-cap-global-'||n,'Capacity fixture' FROM generate_series(0,7) n;"
            "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,root,root_identity) "
            "SELECT id,'primary','local-dev','Capacity source','/fixture/'||id,'fixture-'||id "
            "FROM shin_server.projects WHERE id LIKE 'p3e11-cap-global-%';"
            "INSERT INTO shin_server.preparations(project_id,route_id,principal_id,idempotency_key,request_hash,route_fingerprint,"
            "expected_snapshot_id,state,worker_id,artifact_snapshot_id,artifact_bytes,reserved_artifact_bytes,artifact_namespace,artifact_key) "
            "SELECT q.id,p.route_id,p.principal_id,q.id,p.request_hash,p.route_fingerprint,'','prepared',p.worker_id,"
            "p.artifact_snapshot_id,p.artifact_bytes,p.reserved_artifact_bytes,p.artifact_namespace,md5(q.id)||md5(q.id||'-artifact') "
            f"FROM shin_server.projects q CROSS JOIN shin_server.preparations p WHERE q.id LIKE 'p3e11-cap-global-%' AND p.id={job['id']};"
            f"INSERT INTO shin_server.publication_proposals({COLUMNS}) "
            "SELECT p.project_id,'capacity',p.id,0,NULL,p.principal_id,'p3e11-cap-global-row-'||n,p.request_hash,"
            "p.route_id,'primary','local-dev',t.profile_id,p.route_fingerprint "
            f"FROM generate_series(1,{needed}) n JOIN shin_server.preparations p "
            "ON p.project_id='p3e11-cap-global-'||((n-1)/1024)::text "
            f"CROSS JOIN shin_server.publication_proposals t WHERE t.id={template_id}"
        )
        require(suite.scalar(
            "SELECT COALESCE(max(n),0)<=1024 FROM (SELECT count(*) n FROM shin_server.publication_proposals GROUP BY project_id) counts"
        ) == "t", "global quota fixture violates the per-project proposal limit")
        assert_last_slot(suite, job, "global-capacity", "capacity-last-global", True)
    finally:
        suite.psql(
            "DELETE FROM shin_server.publication_proposals WHERE project_id LIKE 'p3e11-cap-global-%';"
            "DELETE FROM shin_server.preparations WHERE project_id LIKE 'p3e11-cap-global-%';"
            "DELETE FROM shin_server.workspaces WHERE project_id LIKE 'p3e11-cap-global-%';"
            "DELETE FROM shin_server.projects WHERE id LIKE 'p3e11-cap-global-%'"
        )
