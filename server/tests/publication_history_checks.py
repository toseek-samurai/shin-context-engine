"""R2 explicit publication history retirement on the locked development stand."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import secrets
import subprocess
import time

from publication_test_support import require
from publication_withdrawal_checks import withdraw
from publication_withdrawal_migration_checks import MIGRATION_001_012_SHA256, assert_sql_refused


HISTORY = "/api/v1/publication-history"
RETENTION = HISTORY + "/retention"
ADDED_COLUMNS = ("history_epoch", "base_source_route_id", "base_workspace_id",
                 "base_source_host_id", "base_profile_id", "base_route_fingerprint")
MIGRATION_013_018_SHA256 = (
    "42d2fe385797cefc213bc205a548e1f7608c8dff763e2a7640f7316efa2c4e9a",
    "4389372a5fbd8cb0740b73a9f981644e98719193a010cd683932953e64fc2811",
    "c8e6e9219da5ad5f164ba80223b0f15f17c472b94f1fca391e2092cc8df76aa3",
    "3ca5919b5c500177c4721773f5c4711db2d02a797657dc7b7bc2e6670c584207",
    "d8d954888595eac80c74a1a88f7fa455c937440cd390ceb31e27d357305ef92c",
    "f8fac41cad0e8d46c2a1ab78263066a7d94968e0f73d6749f1db842286bf3b3a",
)


def check_publication_history_migration(suite):
    suite.reset_schema()
    try:
        migrations = sorted((Path(__file__).resolve().parents[1] / "migrations").glob("[0-9][0-9][0-9]_*.sql"))
        require(len(migrations) == 35, "history migration inventory differs")
        for version, (path, expected) in enumerate(zip(migrations[:18], MIGRATION_001_012_SHA256 + MIGRATION_013_018_SHA256), 1):
            require(hashlib.sha256(path.read_bytes()).hexdigest() == expected, f"published migration {version} changed")
            suite.psql("SET shin_server.migration_source_host_id='upgrade-host';" + path.read_text())
            suite.psql(f"INSERT INTO shin_server.schema_migrations(version,checksum) VALUES ({version},'{expected}')")
        suite.psql(
            "INSERT INTO shin_server.projects(id,display_name) VALUES ('upgrade','Upgrade');"
            "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,root,root_identity) "
            "VALUES ('upgrade','primary','upgrade-host','Source','/fixture/source','fixture-source');"
            "INSERT INTO shin_server.preparations(project_id,route_id,principal_id,idempotency_key,request_hash,route_fingerprint,"
            "expected_snapshot_id,state,worker_id,artifact_snapshot_id,artifact_bytes,reserved_artifact_bytes,artifact_namespace,artifact_key) "
            "VALUES ('upgrade','source','dev-admin','old-job',repeat('a',64),repeat('b',64),'','prepared',"
            "'history-fixture-worker','fnv1a64:0123456789abcdef',512,1024,repeat('c',64),repeat('d',64));"
            "INSERT INTO shin_server.publication_proposals(project_id,publication_id,preparation_id,base_revision,"
            "submitted_by,idempotency_key,request_hash,source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint) "
            "SELECT project_id,'shared',id,0,principal_id,'old-head',repeat('e',64),route_id,'primary',"
            "'upgrade-host','generic-docs',route_fingerprint FROM shin_server.preparations;"
            "INSERT INTO shin_server.publication_resolutions(proposal_id,project_id,publication_id,decision,expected_revision,"
            "revision,preparation_id,resolved_by) SELECT id,project_id,publication_id,'accept',0,1,preparation_id,'dev-admin' "
            "FROM shin_server.publication_proposals;"
            "INSERT INTO shin_server.publication_heads(project_id,publication_id,revision,preparation_id,snapshot_id,"
            "route_fingerprint,proposal_id) SELECT p.project_id,p.publication_id,1,p.preparation_id,j.artifact_snapshot_id,"
            "p.route_fingerprint,p.id FROM shin_server.publication_proposals p JOIN shin_server.preparations j ON j.id=p.preparation_id;"
            "INSERT INTO shin_server.publication_proposals(project_id,publication_id,preparation_id,base_revision,base_preparation_id,"
            "submitted_by,idempotency_key,request_hash,source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint) "
            "SELECT project_id,publication_id,preparation_id,999,preparation_id,submitted_by,'old-pending',request_hash,"
            "source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint FROM shin_server.publication_proposals"
        )
        before = suite.scalar("SELECT jsonb_agg(to_jsonb(p) ORDER BY id)::text FROM shin_server.publication_proposals p")
        rejected = subprocess.run([str(suite.binary), "migrate", "--config", str(suite.state_root / "config/migrator.json")],
                                  capture_output=True, text=True, timeout=45)
        require(rejected.returncode == 1 and json.loads(rejected.stderr)["ok"] is False,
                "missing historical base was silently invented")
        require(suite.scalar("SELECT count(*) FROM shin_server.schema_migrations WHERE version=19") == "0",
                "failed migration committed history")
        require(suite.scalar("SELECT jsonb_agg(to_jsonb(p) ORDER BY id)::text FROM shin_server.publication_proposals p") == before,
                "failed base migration altered old rows")
        suite.psql("UPDATE shin_server.publication_proposals SET base_revision=1 WHERE idempotency_key='old-pending'")
        snapshots = {table: suite.scalar(f"SELECT COALESCE(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text),'[]'::jsonb)::text "
                                         f"FROM shin_server.{table} t")
                     for table in ("publication_proposals", "publication_resolutions", "publication_heads", "audit_events", "server_identity")}
        suite.migrate()
        for table, old in snapshots.items():
            actual = json.loads(suite.scalar(f"SELECT COALESCE(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text),'[]'::jsonb)::text "
                                             f"FROM shin_server.{table} t"))
            if table == "publication_proposals":
                for row in actual:
                    require(row.pop("history_workspace_id") is None, "migration invented history ownership")
                    require(row["history_epoch"] == 0, "migration changed legacy request epoch")
                    for field in ADDED_COLUMNS:
                        row.pop(field)
            require(sorted(actual, key=lambda row: json.dumps(row, sort_keys=True)) ==
                    sorted(json.loads(old), key=lambda row: json.dumps(row, sort_keys=True)),
                    "history migration changed old " + table)
        require(suite.scalar("SELECT base_source_route_id||':'||base_workspace_id||':'||base_source_host_id||':'||base_profile_id "
                             "FROM shin_server.publication_proposals WHERE idempotency_key='old-pending'") ==
                "source:primary:upgrade-host:generic-docs", "historical base provenance was not captured")
        require(suite.scalar("SELECT history_epoch=0 AND retired_proposals=0 AND last_retention IS NULL "
                             "FROM shin_server.publication_history WHERE singleton") == "t", "migration retired history implicitly")
        require(suite.scalar("SELECT has_table_privilege('shin_runtime','shin_server.publication_history','SELECT')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.publication_history','UPDATE')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.publication_history','INSERT')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.publication_history','DELETE')::text") ==
                "true:true:false:false", "history runtime grants are broader than required")
        assert_sql_refused(suite, "UPDATE shin_server.publication_proposals SET base_profile_id='forged'",
                           "insufficient_privilege", runtime=True)
        state = suite.scalar("SELECT to_jsonb(h)::text FROM shin_server.publication_history h")
        require(suite.scalar("SELECT count(*) FROM shin_server.workspace_publication_history") == "0",
                "migration invented scoped history")
        require(suite.scalar("SELECT has_table_privilege('shin_runtime','shin_server.workspace_publication_history','SELECT')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.workspace_publication_history','INSERT')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.workspace_publication_history','UPDATE')::text||':'||"
                             "has_table_privilege('shin_runtime','shin_server.workspace_publication_history','DELETE')::text||':'||"
                             "has_column_privilege('shin_runtime','shin_server.workspace_publication_history','history_epoch','UPDATE')::text") ==
                "true:true:false:false:true", "workspace history grants permit identity rewrite or deletion")
        assert_sql_refused(suite, "UPDATE shin_server.workspace_publication_history SET workspace_id='forged'",
                           "insufficient_privilege", runtime=True)
        suite.migrate()
        require(suite.scalar("SELECT to_jsonb(h)::text FROM shin_server.publication_history h") == state,
                "idempotent migration changed the epoch")
    finally:
        suite.reset_schema()


class PublicationHistoryChecks:
    def __init__(self, suite):
        self.s = suite
        self.epoch = 0
        self.last_receipt = None

    def get(self, path, role="admin"):
        response = self.s.expect_status(200, "GET", path, headers=self.s.auth_headers(role))
        return self.s.public_result(response)

    def plan(self, limit=256):
        return self.get(RETENTION + f"?limit={limit}")

    def apply(self, plan, *, replayed=False):
        status, response = self.s.post_json(RETENTION, {"plan": plan, "discard_confirmed": True})
        require(status == 200, f"history retirement returned HTTP {status}")
        result = self.s.public_result(response)
        require(result["replayed"] is replayed, "retirement replay differs")
        receipt = result["receipt"]
        require(receipt["plan_id"] == plan["plan_id"] and receipt["history_epoch"] == plan["history_epoch"] + 1 and
                receipt["deleted_proposals"] == plan["delete_count"], "retirement receipt lost the reviewed plan")
        require(not receipt["external_archive_verified"] and not receipt["source_mutated"] and
                not receipt["publication_head_mutated"], "retirement overclaimed authority")
        self.epoch = receipt["history_epoch"]
        self.last_receipt = receipt
        return receipt

    def submit(self, key, revision=0, publication="history", job=None):
        return self.s.submit(job or self.s.alpha, revision, key, publication, epoch=self.epoch)

    def archive_audit(self):
        while True:
            export = self.get("/api/v1/audit/export?limit=1000")
            if not export["plan"]["event_count"]:
                break
            archive = export["archive"]
            encoded = json.dumps(archive, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
            require(hashlib.sha256(encoded).hexdigest() == export["plan"]["archive_sha256"], "audit archive hash differs")
            path = self.s.state_root / "reports" / ("history-audit-" + secrets.token_hex(8) + ".json")
            self.s.replace_private_json(path, archive)
            require(json.loads(path.read_text()) == archive, "audit archive did not persist")
            status, response = self.s.post_json("/api/v1/audit/retention", {"plan": export["plan"], "archive_saved": True})
            require(status == 200 and not response["result"]["receipt"]["external_archive_verified"],
                    "explicit audit archive/retention failed or overclaimed durability")

    def controls(self):
        before = self.s.durable_state()
        for path in (HISTORY, RETENTION):
            self.s.expect_status(401, "GET", path)
            self.s.expect_status(403, "GET", path, headers=self.s.auth_headers("reader"))
        for value in ("0", "257", "bad", "1&limit=2"):
            self.s.expect_status(400, "GET", RETENTION + "?limit=" + value, headers=self.s.auth_headers("admin"))
        empty = self.plan()
        require(empty["delete_count"] == 0 and self.get(HISTORY)["history_complete"], "fresh history is not empty")
        status, _ = self.s.post_json(RETENTION, {"plan": empty, "discard_confirmed": True}, role="reader")
        require(status == 403, "reader retired global history")
        for request in ({"plan": empty}, {"plan": empty, "discard_confirmed": False},
                        {"plan": {**empty, "limit": True}, "discard_confirmed": True}):
            status, _ = self.s.post_json(RETENTION, request)
            require(status == 400, "malformed discard was admitted")
        status, response = self.s.post_json(RETENTION, {"plan": empty, "discard_confirmed": True})
        self.s._assert_error(status, response, 409, "publication-history-empty")
        self.s.unchanged(before, "history read-only plans and denials")

    def seed_and_protect(self):
        s = self.s
        self.first = self.submit("history-first")
        s.resolution(self.first["proposal"]["id"], "accept", 0, "history")
        self.pending = self.submit("history-pending", 1, job=s.beta)
        second = self.submit("history-second", 1, job=s.beta)
        s.resolution(second["proposal"]["id"], "accept", 1, "history")
        withdraw(s, "history", 2, second["proposal"]["id"], s.beta["id"])
        self.withdrawn_pending = self.submit("history-withdrawn-pending", 3)
        third = self.submit("history-third", 3)
        s.resolution(third["proposal"]["id"], "accept", 3, "history")
        fourth = self.submit("history-fourth", 4, job=s.beta)
        s.resolution(fourth["proposal"]["id"], "accept", 4, "history")
        self.latest_withdrawal = withdraw(s, "history", 5, fourth["proposal"]["id"], s.beta["id"])
        self.current = self.submit("history-current", 6)
        s.resolution(self.current["proposal"]["id"], "accept", 6, "history")
        rejected = self.submit("history-rejected", 7)
        s.resolution(rejected["proposal"]["id"], "reject", 7, "history")
        protected = self.plan()
        require(protected["delete_count"] == 0, "unarchived audit references were discarded")
        require(protected["counts"] == {"retained": 8, "unresolved": 2, "anchored": 2,
                                        "audit_pinned": 8, "eligible": 0},
                "overlapping head anchors or repeated audit events multiplied inventory rows")
        self.archive_audit()
        self.first_plan = self.plan()
        require(self.first_plan["counts"] == {"retained": 8, "unresolved": 2, "anchored": 2,
                                              "audit_pinned": 0, "eligible": 4},
                "audit retirement changed unrelated protection counts")
        require(self.first_plan["delete_count"] == 4 and self.first_plan["withdrawal_count"] == 1,
                "history plan did not select exactly the obsolete terminal transitions")
        selected = set(self.first_plan["proposal_ids"])
        require(not selected.intersection({self.pending["proposal"]["id"], self.withdrawn_pending["proposal"]["id"],
                                          self.current["proposal"]["id"], fourth["proposal"]["id"]}),
                "history plan selected an unresolved proposal or logical head anchor")
        self.before_head = s.get_head("history")
        self.before_pending = s.get_detail(self.pending["proposal"]["id"], "history")
        self.before_withdrawn = s.get_detail(self.withdrawn_pending["proposal"]["id"], "history")
        self.source_digests = [s.tree_digest(root) for root in (s.fixture_root, s.peer_root, s.preparation_root)]
        before = s.durable_state()
        require(self.plan() == self.first_plan, "unchanged plan is not deterministic")
        s.unchanged(before, "publication history inspection")

    def rollback_and_stale(self):
        s = self.s
        for plan in ({**self.first_plan, "server_id": "0" * 32}, {**self.first_plan, "records_sha256": "0" * 64}):
            before = s.durable_state()
            status, response = s.post_json(RETENTION, {"plan": plan, "discard_confirmed": True})
            s._assert_error(status, response, 409, "publication-history-stale")
            s.unchanged(before, "altered history plan")
        trigger = "history_failure_" + secrets.token_hex(6)
        s.psql(f"CREATE FUNCTION shin_server.{trigger}() RETURNS trigger LANGUAGE plpgsql AS $$ "
               "BEGIN RAISE EXCEPTION 'controlled history failure'; END $$; "
               f"CREATE TRIGGER {trigger} BEFORE UPDATE ON shin_server.publication_history "
               f"FOR EACH ROW EXECUTE FUNCTION shin_server.{trigger}()")
        before = s.durable_state()
        try:
            status, _ = s.post_json(RETENTION, {"plan": self.first_plan, "discard_confirmed": True})
            require(status >= 500, "history failure did not abort the transaction")
            s.unchanged(before, "failed history deletion/epoch transaction")
        finally:
            s.psql(f"DROP TRIGGER {trigger} ON shin_server.publication_history; DROP FUNCTION shin_server.{trigger}()")

    def retire_and_resume(self):
        s = self.s
        self.first_receipt = self.apply(self.first_plan)
        require(self.apply(self.first_plan, replayed=True) == self.first_receipt, "exact history retry changed receipt")
        require(s.get_head("history") == {**self.before_head, "history_epoch": 1}, "retirement changed shared authority")
        require(s.get_detail(self.pending["proposal"]["id"], "history") == self.before_pending and
                s.get_detail(self.withdrawn_pending["proposal"]["id"], "history") == self.before_withdrawn,
                "retirement lost accepted or withdrawn base provenance")
        for proposal_id in self.first_plan["proposal_ids"]:
            s.expect_status(404, "GET", s.path("history", f"/proposals/{proposal_id}"), headers=s.auth_headers("admin"))
        before = s.durable_state()
        status, response = s.post_json(s.path("history", "/proposals"),
                                      {"preparation_id": s.alpha["id"], "base_revision": 0}, key="history-first")
        s._assert_error(status, response, 409, "publication-history-changed")
        status, retained = s.post_json(s.path("history", "/proposals"),
                                      {"preparation_id": s.alpha["id"], "base_revision": 6}, key="history-current")
        require(status == 200 and retained["result"]["replayed"] and
                retained["result"]["proposal"] == s.get_detail(self.current["proposal"]["id"], "history"),
                "retained legacy submission no longer replays after audit archival")
        status, retired = s.resolve_request(self.first["proposal"]["id"], "accept", 0, "history")
        s._assert_error(status, retired, 404, "publication-proposal-not-found")
        status, retained = s.post_json(s.path("history", "/withdrawal"), {"expected_revision": 5})
        require(status == 200 and retained["result"]["replayed"] and retained["result"]["receipt"] == self.latest_withdrawal,
                "latest withdrawal anchor lost its exact retry")
        status, retired = s.post_json(s.path("history", "/withdrawal"), {"expected_revision": 2})
        s._assert_error(status, retired, 409, "publication-conflict")
        s.unchanged(before, "retired versus retained submission retry")
        status, retrieved = s.post_json(s.path("history", "/brief"), {"query": "AlphaWorkspaceEvidence"})
        require(status == 200 and retrieved["result"]["revision"] == 7 and
                retrieved["result"]["snapshot_id"] == s.alpha["artifact_snapshot_id"], "history retirement broke retrieval")
        reviewed = self.get(s.path("history", f"/proposals/{self.pending['proposal']['id']}/review"))
        require(reviewed["review"]["artifacts"]["base"]["state"] == "artifact-verified",
                "retiring a parent proposal broke captured-base artifact review")
        require([s.tree_digest(root) for root in (s.fixture_root, s.peer_root, s.preparation_root)] == self.source_digests,
                "history retirement changed source or artifacts")
        s.stop_server(expect_clean=True)
        s.start_server()
        require(self.apply(self.first_plan, replayed=True) == self.first_receipt, "restart lost history receipt")
        page = self.get(s.path("history", "/proposals"))
        require(page["history_epoch"] == 1 and page["history_complete"] is False, "pagination hid retired history")
        s.resolution(self.pending["proposal"]["id"], "accept", 7, "history")
        require(s.get_head("history")["revision"] == 8, "retained proposal could not advance the monotonic head")
        self.submit("reviewed-new-request", 8, job=s.beta)

    def capacity_cycles(self):
        s = self.s
        for cycle in range(2):
            old_epoch = self.epoch
            template = self.submit(f"capacity-template-{cycle}", publication="history-capacity")
            s.resolution(template["proposal"]["id"], "reject", 0, "history-capacity")
            count = int(s.scalar("SELECT count(*) FROM shin_server.publication_proposals WHERE project_id='visible'"))
            needed = 1024 - count
            require(0 < needed < 1024, "capacity fixture has insufficient room")
            prefix = f"history-capacity-{cycle}-"
            s.psql(
                "INSERT INTO shin_server.publication_proposals(project_id,publication_id,preparation_id,base_revision,"
                "submitted_by,idempotency_key,request_hash,source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint,history_epoch) "
                "SELECT project_id,publication_id,preparation_id,0,submitted_by," + s._sql_text(prefix) + "||n,request_hash,"
                "source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint,history_epoch "
                f"FROM shin_server.publication_proposals CROSS JOIN generate_series(1,{needed}) n WHERE id={template['proposal']['id']};"
                "INSERT INTO shin_server.publication_resolutions(proposal_id,project_id,publication_id,decision,expected_revision,revision,"
                "preparation_id,resolved_by) SELECT id,project_id,publication_id,'reject',0,0,preparation_id,submitted_by "
                "FROM shin_server.publication_proposals WHERE idempotency_key LIKE " + s._sql_text(prefix + "%")
            )
            status, response = s.post_json(s.path("history-capacity", "/proposals"),
                {"preparation_id": s.alpha["id"], "base_revision": 0, "history_epoch": self.epoch}, key=prefix + "overflow")
            s._assert_error(status, response, 409, "publication-proposal-capacity")
            plan = self.plan()
            require(plan["delete_count"] == 256, "bounded plan did not reclaim a full allowed page")
            self.apply(plan)
            require(s.scalar("SELECT count(*) FROM shin_server.publication_proposals WHERE project_id='visible'") == "768",
                    "retirement did not release actual proposal quota")
            status, response = s.post_json(s.path("history-capacity", "/proposals"),
                {"preparation_id": s.alpha["id"], "base_revision": 0, "history_epoch": old_epoch}, key=prefix + "overflow")
            s._assert_error(status, response, 409, "publication-history-changed")
            self.submit(prefix + "reviewed", publication="history-capacity")

    def concurrent_apply_and_stale(self):
        s = self.s
        plan = self.plan(4)
        require(plan["delete_count"] == 4, "concurrency fixture has no retireable rows")
        before = int(s.scalar("SELECT count(*) FROM shin_server.publication_proposals"))
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: s.post_json(RETENTION, {"plan": plan, "discard_confirmed": True}), range(2)))
        require(all(status == 200 for status, _ in results), "concurrent identical retirement failed")
        require(sorted(response["result"]["replayed"] for _, response in results) == [False, True],
                "concurrent retirement applied more than once")
        receipts = [response["result"]["receipt"] for _, response in results]
        require(receipts[0] == receipts[1] and int(s.scalar("SELECT count(*) FROM shin_server.publication_proposals")) == before - 4,
                "concurrent retirement changed receipt or delete count")
        self.epoch = receipts[0]["history_epoch"]
        competing = self.plan(4)
        with ThreadPoolExecutor(max_workers=2) as pool:
            retiring = pool.submit(s.post_json, RETENTION, {"plan": competing, "discard_confirmed": True})
            submitting = pool.submit(s.post_json, s.path("history-concurrent", "/proposals"),
                                     {"preparation_id": s.alpha["id"], "base_revision": 0, "history_epoch": self.epoch},
                                     key="history-epoch-race")
            retired, admitted = retiring.result(), submitting.result()
        require((retired[0], admitted[0]) in {(200, 409), (409, 201)},
                "retirement and old-epoch admission did not serialize to one winner")
        if retired[0] == 200:
            require(admitted[1]["error"]["code"] == "publication-history-changed", "old-epoch race failed for the wrong reason")
            self.epoch = retired[1]["result"]["receipt"]["history_epoch"]
        else:
            require(retired[1]["error"]["code"] == "publication-history-stale", "winning admission did not stale the plan")
        stale = self.plan(4)
        self.submit("append-after-plan", publication="history-concurrent")
        status, response = s.post_json(RETENTION, {"plan": stale, "discard_confirmed": True})
        s._assert_error(status, response, 409, "publication-history-stale")
        current = self.plan(4)
        s.send_without_reply(RETENTION, {"plan": current, "discard_confirmed": True})
        deadline = time.monotonic() + 10
        while int(s.scalar("SELECT history_epoch FROM shin_server.publication_history")) == self.epoch and time.monotonic() < deadline:
            time.sleep(0.025)
        require(int(s.scalar("SELECT history_epoch FROM shin_server.publication_history")) == self.epoch + 1,
                "lost-reply retirement did not commit")
        s.stop_server(expect_clean=True)
        s.start_server()
        self.apply(current, replayed=True)
        status, response = s.post_json(RETENTION, {"plan": plan, "discard_confirmed": True})
        s._assert_error(status, response, 409, "publication-history-stale")

    def global_capacity(self):
        s = self.s
        while (plan := self.plan())["delete_count"]:
            self.apply(plan)
        baseline = self.get(HISTORY)["counts"]
        needed = 8192 - int(s.scalar("SELECT count(*) FROM shin_server.publication_proposals"))
        require(0 < needed <= 8192 and s.scalar("SELECT count(*) FROM shin_server.projects "
                                               "WHERE id LIKE 'history-cap-global-%'") == "0",
                "global history fixture cannot be admitted")
        try:
            s.psql(
                "INSERT INTO shin_server.projects(id,display_name) SELECT 'history-cap-global-'||n,'Quota fixture' "
                "FROM generate_series(0,7) n;"
                "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,root,root_identity) "
                "SELECT id,'primary','local-dev','Source','/fixture/'||id,'fixture-'||id FROM shin_server.projects "
                "WHERE id LIKE 'history-cap-global-%';"
                "INSERT INTO shin_server.preparations(project_id,route_id,principal_id,idempotency_key,request_hash,route_fingerprint,"
                "expected_snapshot_id,state,worker_id,artifact_snapshot_id,artifact_bytes,reserved_artifact_bytes,artifact_namespace,artifact_key) "
                "SELECT q.id,p.route_id,p.principal_id,q.id,p.request_hash,p.route_fingerprint,'','prepared',p.worker_id,"
                "p.artifact_snapshot_id,p.artifact_bytes,p.reserved_artifact_bytes,p.artifact_namespace,md5(q.id)||md5(q.id||'-artifact') "
                f"FROM shin_server.projects q CROSS JOIN shin_server.preparations p WHERE q.id LIKE 'history-cap-global-%' AND p.id={s.alpha['id']};"
                "INSERT INTO shin_server.publication_proposals(project_id,publication_id,preparation_id,base_revision,submitted_by,"
                "idempotency_key,request_hash,source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint,history_epoch) "
                "SELECT p.project_id,'capacity',p.id,0,p.principal_id,'history-global-row-'||n,p.request_hash,"
                f"p.route_id,'primary','local-dev','generic-docs',p.route_fingerprint,{self.epoch} "
                f"FROM generate_series(1,{needed}) n JOIN shin_server.preparations p "
                "ON p.project_id='history-cap-global-'||((n-1)%8)::text ORDER BY n;"
                "INSERT INTO shin_server.publication_resolutions(proposal_id,project_id,publication_id,decision,expected_revision,revision,"
                "preparation_id,resolved_by) SELECT id,project_id,publication_id,'reject',0,0,preparation_id,submitted_by "
                "FROM shin_server.publication_proposals WHERE project_id LIKE 'history-cap-global-%'"
            )
            require(s.scalar("SELECT count(*) FROM shin_server.publication_proposals") == "8192" and
                    s.scalar("SELECT max(n)<=1024 FROM (SELECT count(*) n FROM shin_server.publication_proposals GROUP BY project_id) c") == "t",
                    "global quota fixture exceeded a project bound")
            status, response = s.post_json(s.path("history-global-live", "/proposals"),
                {"preparation_id": s.alpha["id"], "base_revision": 0, "history_epoch": self.epoch}, key="history-global-full")
            s._assert_error(status, response, 409, "publication-proposal-capacity")
            expected_counts = {**baseline, "retained": 8192, "eligible": baseline["eligible"] + needed}
            started = time.monotonic()
            require(self.get(HISTORY)["counts"] == expected_counts,
                    "global capacity inventory changed protection counts")
            inventory_ms = (time.monotonic() - started) * 1000
            started = time.monotonic()
            plan = self.plan()
            plan_ms = (time.monotonic() - started) * 1000
            require(plan["counts"] == expected_counts and self.plan() == plan,
                    "global capacity plan differs from the exact unchanged inventory")
            s.publication_history_measurement = {
                "status": "passed", "retained_proposals": 8192,
                "inventory_http_ms": round(inventory_ms, 3), "plan_http_ms": round(plan_ms, 3),
                "scope": "Bounded seeded capacity fixture; unchanged runtime SQL deadlines, not a production SLO.",
            }
            require(plan["delete_count"] == 256 and len(plan["affected_projects"]) == 8 and plan["scope"] == "legacy-publications",
                    "global plan did not disclose all eight affected projects")
            self.apply(plan)
            require(s.scalar("SELECT count(*) FROM shin_server.publication_proposals") == "7936", "global quota did not recover")
            self.submit("history-global-reused", publication="history-global-live")
        finally:
            # Remove only SQL-seeded fake projects; these do not represent built artifacts.
            s.psql("DELETE FROM shin_server.publication_resolutions WHERE project_id LIKE 'history-cap-global-%';"
                   "DELETE FROM shin_server.publication_proposals WHERE project_id LIKE 'history-cap-global-%';"
                   "DELETE FROM shin_server.preparations WHERE project_id LIKE 'history-cap-global-%';"
                   "DELETE FROM shin_server.workspaces WHERE project_id LIKE 'history-cap-global-%';"
                   "DELETE FROM shin_server.projects WHERE id LIKE 'history-cap-global-%'")

    def run(self):
        for name, operation in (
            ("controls-and-read-only-plans", self.controls),
            ("audit-unresolved-and-head-protection", self.seed_and_protect),
            ("tampered-plan-and-transaction-rollback", self.rollback_and_stale),
            ("retire-replay-provenance-retrieval-and-restart", self.retire_and_resume),
            ("repeated-fill-retire-reuse", self.capacity_cycles),
            ("concurrent-and-lost-reply-apply", self.concurrent_apply_and_stale),
            ("global-capacity-and-explicit-multiproject-scope", self.global_capacity),
            ("admitted-review-lifetime", self.s.check_review_retention_interleaving),
        ):
            self.s.record("publication-history-" + name, operation)
