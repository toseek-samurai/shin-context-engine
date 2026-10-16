"""Schema019/020/021 preservation; migrations never infer legacy ownership."""
import hashlib
import json
import subprocess
from pathlib import Path

from publication_test_support import require
from publication_history_checks import MIGRATION_013_018_SHA256
from publication_withdrawal_migration_checks import MIGRATION_001_012_SHA256, assert_sql_refused


def check_identity_directory_migration(suite, baseline=19):
    paths = sorted((Path(__file__).resolve().parents[1] / "migrations").glob("[0-9][0-9][0-9]_*.sql"))
    checksums = MIGRATION_001_012_SHA256 + MIGRATION_013_018_SHA256 + (
        "3c1aa4da59ef92e9ec1040ad9956608af959c9a3142bc52418c2986818be9ab2",
        "8ba88a01cd8ecc00545dc9098b940a9484f41417ad5f65c399d7dac6515be3a6",
        "dc99f6b49dbcfaa86425992e3205e2970e2fa60dac8ffb20b84e690198b0379f",
        "f2a18ac3fb7c0f0a4a801aa4dc3167dd23856ef24beb19bc200b442e149c72b6",
        "1424b0dc0455daeed2bdb1394e66ec7d1afcd00a4d978123424ff88a69c5712f",
        "d786645e0a8acbd10dc416e6c7087a6efcc732a3f0602f246ec74751c18da8f7",
        "c08e798e41348b252a23368e342c87c5a74430e60939684a0d58bde7dff264ce",
        "8a290622b3aabb8e63e40d12bda7c491fe7f913252f3642d97309d37a2896635",
        "fadb60e18e41f019166c25bb284e86896d44e42463f6436d177a4925a74d514d",
        "da74fd691f0beea1d2a6409a3b7d756a5d38b4469ca1a1bf673671d068d523d2",
        "8745e3f3cd82e831097eabbe4cb1dbce5d4025810731d02f70523f42e6f44102",
        "8b5f6ae137ea6dd0b73e234b9b74938a91c2076074ffbe87dcfe45b98ad2bad0",
        "81ecb5235452923290e7ff81507db9934d72fe156ff0384304555a2b52575bf8",
        "9e61215ab40f89f7906a1b8709008dc6dc3e405692694b3abfc1e95c997d40ba",
        "508606063592a8407259b2be01196a99056941d2e7ebb08d6b445dcabcf435a4",
        "0f8240592cbe5e533c8ddc04008f969fdb518cd7b651744a256b09049922592e")
    require(len(paths) == 35 and baseline in range(19, 35), "identity migration inventory differs")
    suite.reset_schema()
    try:
        for version, (path, digest) in enumerate(zip(paths[:baseline], checksums), 1):
            require(hashlib.sha256(path.read_bytes()).hexdigest() == digest, "published migration changed")
            suite.psql("SET shin_server.migration_source_host_id='upgrade-host';" + path.read_text())
            suite.psql(f"INSERT INTO shin_server.schema_migrations(version,checksum) VALUES({version},'{digest}')")
        suite.psql("INSERT INTO shin_server.projects(id,display_name) VALUES('upgrade','Unassigned');"
                   "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,root,root_identity) "
                   "VALUES('upgrade','primary','upgrade-host','Checkout','/fixture/source','fixture-source');"
                   "INSERT INTO shin_server.workspace_requests(principal_id,key,request_hash,project_id,workspace_id,response) "
                   "SELECT 'dev-admin','old-workspace',repeat('a',64),'upgrade','primary',to_jsonb(w) FROM shin_server.workspaces w")
        suite.psql("INSERT INTO shin_server.preparations(project_id,route_id,principal_id,idempotency_key,request_hash,route_fingerprint,"
                   "expected_snapshot_id,state,reserved_artifact_bytes) "
                   "VALUES('upgrade','source','dev-admin','old-job',repeat('a',64),repeat('b',64),'','queued',1024)")
        tables = ("projects", "workspaces", "workspace_requests", "server_identity", "audit_events", "preparations")
        if baseline >= 20:
            suite.psql("INSERT INTO shin_server.identity_accounts(id,principal_id,display_name) VALUES('old-user','old-principal','Old user');"
                       "INSERT INTO shin_server.identity_tenants VALUES('old-tenant','Old tenant','old-user');"
                       "INSERT INTO shin_server.identity_memberships VALUES('old-tenant','old-user','owner',true,1)")
            tables += ("identity_accounts", "identity_tenants", "identity_memberships", "identity_audit")
        if baseline >= 23:
            suite.psql("INSERT INTO shin_server.identity_accounts(id,principal_id,display_name) VALUES('guest','guest-principal','Guest');"
                       "INSERT INTO shin_server.identity_memberships VALUES('old-tenant','guest','owner',true,1);"
                       "INSERT INTO shin_server.identity_project_owners VALUES('upgrade','old-tenant');"
                       "INSERT INTO shin_server.identity_workspace_owners VALUES('old-tenant','upgrade','primary','old-user','shared');"
                       "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,root,root_identity) "
                       "VALUES('upgrade','inconsistent','upgrade-host','Old binding','/fixture/inconsistent','fixture-inconsistent');"
                       "INSERT INTO shin_server.identity_workspace_owners VALUES('old-tenant','upgrade','inconsistent','guest','shared')")
            tables += ("identity_project_owners", "identity_workspace_owners", "identity_ownership_audit")
        if baseline in (24, 27, 29):
            suite.psql("INSERT INTO shin_server.preparations(project_id,route_id,principal_id,idempotency_key,request_hash,route_fingerprint,"
                "expected_snapshot_id,state,reserved_artifact_bytes,workspace_admission) "
                "SELECT 'upgrade','source','dev-admin','old-'||id,repeat('a',64),repeat('b',64),'','queued',1024,"
                "jsonb_build_object('authorization',jsonb_build_object('principal_id','dev-admin','ownership',jsonb_build_object("
                "'project_id','upgrade','tenant_id','old-tenant','workspace_id',id))) FROM shin_server.workspaces;"
                "INSERT INTO shin_server.workspace_publication_history(project_id,workspace_id) SELECT project_id,id FROM shin_server.workspaces;"
                "INSERT INTO shin_server.publication_proposals(project_id,publication_id,preparation_id,base_revision,submitted_by,"
                "idempotency_key,request_hash,source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint,history_workspace_id) "
                "SELECT project_id,'upgrade-publication',id,0,principal_id,idempotency_key,request_hash,route_id,"
                "COALESCE(workspace_admission #>> '{authorization,ownership,workspace_id}','primary'),"
                "'upgrade-host','generic-docs',route_fingerprint,workspace_admission #>> '{authorization,ownership,workspace_id}' "
                "FROM shin_server.preparations")
            tables += ("publication_proposals", "workspace_publication_history")
        if baseline in (26, 28, 29):
            suite.psql("INSERT INTO shin_server.checkpoint_slots(project_id,owner_id,slot_id,state,revision,task_id,thread_id,"
                       "provenance,last_operation,last_request_id,last_request_hash,last_expected_revision,last_request_deadline,retry_until,updated_at) "
                       "VALUES('upgrade','old-principal','same-slot','cleared',nextval('shin_server.checkpoint_revisions'),"
                       "'old-task','old-thread','{\"project_id\":\"upgrade\",\"workspace_id\":\"primary\"}',"
                       "'clear',repeat('a',32),repeat('b',64),0,100,100,100)")
            suite.psql("INSERT INTO shin_server.checkpoint_slots SELECT (jsonb_populate_record(NULL::shin_server.checkpoint_slots,"
                       "to_jsonb(s)||jsonb_build_object('slot_id','active-slot','state','active','revision',"
                       "nextval('shin_server.checkpoint_revisions'),'checkpoint_text','historical payload',"
                       "'checkpoint_sha256',repeat('c',64),'checkpoint_id','fnv1a64:0123456789abcdef',"
                       "'captured_at',100,'ttl_seconds',3600,'snapshot_id','fnv1a64:0123456789abcdef',"
                       "'last_operation','receipt-import','receipt_origins',jsonb_build_array(repeat('d',64))))).* "
                       "FROM shin_server.checkpoint_slots s WHERE slot_id='same-slot'")
            tables += ("checkpoint_slots", "checkpoint_lifecycle")
            if baseline >= 28:
                suite.psql("UPDATE shin_server.checkpoint_slots SET scope_workspace_id='primary' WHERE slot_id='active-slot'")
        if baseline == 29:
            suite.psql("INSERT INTO shin_server.preparation_retention(preparation_id,state,plan_id,inventory,requested_by,completed_by,released_at) "
                       "SELECT id,CASE WHEN workspace_admission IS NULL THEN 'intent' ELSE 'released' END,repeat('a',64),"
                       "'{\"entries\":[]}'::jsonb,'dev-admin',CASE WHEN workspace_admission IS NULL THEN NULL ELSE 'dev-admin' END,"
                       "CASE WHEN workspace_admission IS NULL THEN NULL ELSE clock_timestamp() END FROM shin_server.preparations")
            tables += ("preparation_retention",)
        if baseline in (30, 31):
            # Dropped columns keep physical attribute slots; migration verification must use live names.
            suite.psql("ALTER TABLE shin_server.active_routes ADD COLUMN removed_probe boolean;"
                       "ALTER TABLE shin_server.active_routes DROP COLUMN removed_probe;")
            suite.psql("INSERT INTO shin_server.active_routes(project_id,route_id,revision) VALUES('upgrade','source',1);"
                       "INSERT INTO shin_server.activation_receipts(project_id,route_id,previous_revision,revision,principal_id) "
                       "VALUES('upgrade','source',0,1,'dev-admin');"
                       "INSERT INTO shin_server.audit_events(principal_id,action,project_id,activation_route_id,activation_previous_revision) "
                       "VALUES('dev-admin','generation.deactivated','upgrade','source',0);"
                       "INSERT INTO shin_server.retrieval_journal_producers(instance_id,source_host_id,server_version,core_version,"
                       "producer_epoch,observed_through,captured_count) VALUES(repeat('e',64),'upgrade-host','test','test',1,1,1);"
                       "UPDATE shin_server.retrieval_journal_lifecycle SET allocated_through=1;"
                       "INSERT INTO shin_server.retrieval_journal_attempts(instance_id,sequence,record_hash,project_id,operation,"
                       "selector_kind,selector_id,observed_at_unix_ms,status,reason_code,last_phase,elapsed_ns,validation_ns,"
                       "query_capture,request_fingerprint,request,producer_epoch) VALUES(repeat('e',64),1,repeat('a',64),'upgrade',"
                       "'brief','route','source',1,400,'invalid-request','validation',1,1,'captured',repeat('c',64),"
                       "'{\"query\":\"old private request\"}',1);")
            tables += ("active_routes", "activation_receipts", "audit_history", "retrieval_journal_producers",
                       "retrieval_journal_attempts", "retrieval_journal_lifecycle")
        if baseline == 32:
            suite.psql("INSERT INTO shin_server.observer_streams(stream_id,project_id,workspace_id,principal_id,observer_id,"
                       "source_host_id,profile,binding_digest,enrollment_hash,accepted_sequence,confirmed_sequence,observed_sequence,retained_bytes) "
                       "VALUES(repeat('a',32),'upgrade','primary','old-principal','old-observer','upgrade-host','old-profile',repeat('b',64),repeat('c',64),1,1,1,32);"
                       "INSERT INTO shin_server.observer_submissions VALUES(repeat('a',32),1,repeat('d',64),repeat('e',64),repeat('f',64),"
                       "'{\"original\":1}','{\"receipt\":1}',32,now());"
                       "INSERT INTO shin_server.observer_tombstones(stream_id,project_id,workspace_id,principal_id,enrollment_hash,retirement) "
                       "VALUES(repeat('b',32),'upgrade','primary','old-principal',repeat('c',64),'{\"retired\":true}');"
                       "UPDATE shin_server.observer_usage SET streams=1,submissions=1,retained_bytes=32,tombstones=1")
            tables += ("observer_streams", "observer_submissions", "observer_tombstones", "observer_usage")
        if baseline == 34:
            suite.psql("INSERT INTO shin_server.identity_external_bindings(account_id,issuer,subject,enabled,revision) "
                       "VALUES('old-user','https://identity.example.test','preserved-subject',false,7)")
            tables += ("identity_external_bindings",)
        def snapshot():
            value = "to_jsonb(t)-'scope_tenant_id'" if baseline >= 22 else "to_jsonb(t)-'workspace_admission'-'scope_tenant_id'"
            projection = "to_jsonb(t)" if baseline >= 28 else value + "-'scope_workspace_id'"
            result = {}
            for table in tables:
                fields = projection
                if baseline == 32 and table in ("observer_streams", "observer_tombstones"):
                    fields += "-'scope_tenant_id'"
                if baseline == 30 and table in ("active_routes", "activation_receipts"):
                    fields += "-'scope_workspace_id'"
                if baseline in (30, 31) and table == "retrieval_journal_attempts":
                    fields += "-'scope_principal_id'-'scope_tenant_id'-'scope_workspace_id'"
                result[table] = suite.scalar(f"SELECT COALESCE(jsonb_agg({fields} ORDER BY to_jsonb(t)::text),'[]'::jsonb)::text "
                                            f"FROM shin_server.{table} t")
            return result
        before = snapshot()
        if baseline == 27:
            suite.psql("INSERT INTO shin_server.preparations(project_id,route_id,principal_id,idempotency_key,request_hash,"
                       "route_fingerprint,expected_snapshot_id,state,reserved_artifact_bytes,workspace_admission) "
                       "VALUES('upgrade','source','dev-admin','malformed-scope',repeat('e',64),repeat('b',64),'','queued',1024,'{}')")
            result = subprocess.run([str(suite.binary), "migrate", "--config", str(suite.state_root / "config/migrator.json")],
                                    capture_output=True, text=True, timeout=45)
            require(result.returncode != 0 and json.loads(result.stdout or result.stderr).get("error", {}).get("code") == "migration-failed",
                    "malformed captured scope was accepted or failure was unrelated")
            require(suite.scalar("SELECT max(version) FROM shin_server.schema_migrations") == "27" and
                    suite.scalar("SELECT count(*) FROM pg_attribute WHERE attrelid='shin_server.preparations'::regclass "
                                 "AND attname='scope_tenant_id' AND NOT attisdropped") == "0",
                    "failed scope migration left a partial schema")
            require(suite.scalar("SELECT workspace_admission::text FROM shin_server.preparations WHERE idempotency_key='malformed-scope'") == "{}",
                    "failed migration rewrote malformed historical data")
            suite.psql("DELETE FROM shin_server.preparations WHERE idempotency_key='malformed-scope'")
        if baseline == 25:
            suite.psql("INSERT INTO shin_server.projects(id,display_name) VALUES('other-project','Same display name');"
                       "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,source_kind) "
                       "VALUES('other-project','primary','upgrade-host','Same checkout','observer')")
            insert_other = ("INSERT INTO shin_server.workspace_requests(principal_id,key,request_hash,project_id,workspace_id,response) "
                            "SELECT 'dev-admin','old-workspace',repeat('b',64),'other-project','primary',to_jsonb(w) "
                            "FROM shin_server.workspaces w WHERE project_id='other-project'")
            assert_sql_refused(suite, insert_other, "unique_violation")
            before = snapshot()
        for _ in range(2):
            suite.migrate()
            require(snapshot() == before, "identity migration altered legacy data or receipt")
            require(suite.scalar("SELECT count(*) FROM shin_server.preparations WHERE workspace_admission IS NOT NULL") == ("2" if baseline in (24, 27, 29) else "0"),
                    "migration invented job workspace authority")
            require(suite.scalar("SELECT count(*) FROM shin_server.workspace_publication_history") == ("2" if baseline in (24, 27, 29) else "0"),
                    "migration invented workspace history")
            assert_sql_refused(suite, "UPDATE shin_server.preparations SET workspace_admission='[]'::jsonb", "check_violation")
            assert_sql_refused(suite, "UPDATE shin_server.preparations SET workspace_admission=jsonb_build_object('large',repeat('x',33000))", "check_violation")
            if baseline == 19:
                require(suite.scalar("SELECT (SELECT count(*) FROM shin_server.identity_accounts) + "
                                     "(SELECT count(*) FROM shin_server.identity_tenants) + "
                                     "(SELECT count(*) FROM shin_server.identity_memberships) + "
                                     "(SELECT count(*) FROM shin_server.identity_audit)") == "0",
                        "identity migration inferred owners or generated directory history")
            require(suite.scalar("SELECT (SELECT count(*) FROM shin_server.identity_project_owners) + "
                                 "(SELECT count(*) FROM shin_server.identity_workspace_owners) + "
                                 "(SELECT count(*) FROM shin_server.identity_ownership_audit)") == ("3" if baseline >= 23 else "0"),
                    "ownership migration inferred legacy owners")
            if baseline >= 23:
                require(suite.scalar("BEGIN; SET LOCAL ROLE shin_runtime; "
                    "SET LOCAL shin.identity_account='guest'; SET LOCAL shin.identity_principal='guest-principal'; "
                    "SET LOCAL shin.identity_tenant='old-tenant'; "
                    "SELECT (SELECT count(*) FROM shin_server.identity_tenants) + "
                    "(SELECT count(*) FROM shin_server.identity_workspace_owners); COMMIT") == "0",
                    "historical owner membership or shared visibility bypassed personal RLS")
                require(suite.scalar("BEGIN; SET LOCAL ROLE shin_runtime; "
                    "SET LOCAL shin.identity_account='old-user'; SET LOCAL shin.identity_principal='old-principal'; "
                    "SET LOCAL shin.identity_tenant='old-tenant'; "
                    "SELECT (SELECT count(*) FROM shin_server.identity_tenants) + "
                    "(SELECT count(*) FROM shin_server.identity_workspace_owners); COMMIT") == "2",
                    "personal RLS lost the actual owner's history")
            if baseline in (24, 27, 29):
                # Existing scoped and legacy receipts retain byte-equivalent JSON.
                # Exercise database uniqueness directly, bypassing application replay checks.
                for key in ("old-job", "old-primary", "old-inconsistent"):
                    for table, columns in (
                        ("preparations", "project_id,route_id,principal_id,idempotency_key,request_hash,route_fingerprint,expected_snapshot_id,state,reserved_artifact_bytes,workspace_admission"),
                        ("publication_proposals", "project_id,publication_id,preparation_id,base_revision,submitted_by,idempotency_key,request_hash,source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint,history_workspace_id")):
                        assert_sql_refused(suite, f"INSERT INTO shin_server.{table}({columns}) SELECT {columns} "
                                           f"FROM shin_server.{table} WHERE idempotency_key='{key}'", "unique_violation")
                suite.psql("BEGIN; UPDATE shin_server.preparations SET idempotency_key='old-job'; "
                           "UPDATE shin_server.publication_proposals SET idempotency_key='old-job'; ROLLBACK")
        require(suite.scalar("SELECT max(version) FROM shin_server.schema_migrations") == "35", "identity migration missing")
        require(suite.scalar("SELECT count(*) FROM shin_server.identity_external_bindings") == ("1" if baseline == 34 else "0"),
                "migration inferred an external identity binding")
        require(suite.scalar("SELECT count(*) FROM shin_server.identity_devices") == "0", "migration invented device admission")
        if baseline == 32:
            for table in ("observer_streams", "observer_tombstones"):
                require(suite.scalar(f"SELECT count(*) FROM shin_server.{table} WHERE scope_tenant_id IS NOT NULL") == "0",
                        "migration silently adopted old Observer history")
        if baseline == 31:
            require(suite.scalar("SELECT count(*) FROM shin_server.retrieval_journal_attempts WHERE scope_principal_id IS NOT NULL "
                                 "OR scope_tenant_id IS NOT NULL OR scope_workspace_id IS NOT NULL") == "0", "migration attributed old observations")
        if baseline == 30:
            for table in ("active_routes", "activation_receipts"):
                require(suite.scalar(f"SELECT count(*) FROM shin_server.{table} WHERE scope_workspace_id IS NOT NULL") == "0",
                        "route migration inferred legacy ownership")
        if baseline == 27:
            require(suite.scalar("SELECT count(*) FROM shin_server.preparations WHERE workspace_admission IS NULL "
                                 "AND scope_tenant_id IS NULL AND scope_workspace_id IS NULL") == "1",
                    "legacy job gained a generated scope")
            require(suite.scalar("SELECT count(*) FROM shin_server.preparations WHERE scope_tenant_id='old-tenant' "
                                 "AND scope_workspace_id IN ('primary','inconsistent')") == "2",
                    "explicit captured scope was not projected")
            where = " WHERE idempotency_key='old-primary'"
            for change in ("principal_id='someone-else'", "workspace_admission='{}'::jsonb",
                           "workspace_admission=workspace_admission #- '{authorization,ownership,project_id}'",
                           "workspace_admission=workspace_admission #- '{authorization,ownership,workspace_id}'"):
                assert_sql_refused(suite, "UPDATE shin_server.preparations SET " + change + where, "check_violation")
            for field in ('tenant_id', 'workspace_id'):
                assert_sql_refused(suite, "UPDATE shin_server.preparations SET workspace_admission=jsonb_set(workspace_admission,"
                                   f"'{{authorization,ownership,{field}}}','\"foreign\"')" + where, "foreign_key_violation")
            suite.psql("BEGIN; DO $check$ BEGIN UPDATE shin_server.preparations SET scope_tenant_id='foreign' "
                       "WHERE idempotency_key='old-primary'; RAISE EXCEPTION 'generated field accepted write'; "
                       "EXCEPTION WHEN generated_always THEN NULL; END $check$; ROLLBACK")
            # Isolate this FK from the retained publication-history pins.
            suite.psql("BEGIN; SET LOCAL shin.identity_ops='1'; "
                       "INSERT INTO shin_server.workspaces(project_id,id,source_host_id,display_name,root,root_identity) "
                       "VALUES('upgrade','guard-only','upgrade-host','Guard','/fixture/guard','guard-only'); "
                       "INSERT INTO shin_server.identity_workspace_owners VALUES('old-tenant','upgrade','guard-only','old-user','private'); "
                       "INSERT INTO shin_server.preparations(project_id,route_id,principal_id,idempotency_key,request_hash,route_fingerprint,"
                       "expected_snapshot_id,state,reserved_artifact_bytes,workspace_admission) SELECT project_id,route_id,principal_id,"
                       "'guard-only',request_hash,route_fingerprint,expected_snapshot_id,state,reserved_artifact_bytes,"
                       "jsonb_set(workspace_admission,'{authorization,ownership,workspace_id}','\"guard-only\"') "
                       "FROM shin_server.preparations WHERE idempotency_key='old-primary'; "
                       "DO $check$ DECLARE blocked_by text; BEGIN "
                       "DELETE FROM shin_server.identity_workspace_owners WHERE workspace_id='guard-only'; "
                       "RAISE EXCEPTION 'preparation ownership was orphaned'; "
                       "EXCEPTION WHEN foreign_key_violation OR restrict_violation THEN "
                       "GET STACKED DIAGNOSTICS blocked_by=CONSTRAINT_NAME; "
                       "IF blocked_by<>'preparation_scope_workspace_fk' THEN RAISE; END IF; END $check$; ROLLBACK")
            plan = suite.psql("BEGIN; SET LOCAL enable_seqscan=off; EXPLAIN(COSTS OFF) SELECT id FROM shin_server.preparations "
                              "WHERE principal_id='dev-admin' AND project_id='upgrade' AND scope_tenant_id='old-tenant' "
                              "AND scope_workspace_id='primary' AND id>0 ORDER BY id LIMIT 2; ROLLBACK")
            require('preparations_workspace_cursor_idx' in plan, "scoped keyset cursor lacks an eligible composite index")
        if baseline == 26:
            require(suite.scalar("SELECT count(*) FROM shin_server.checkpoint_slots WHERE scope_workspace_id IS NOT NULL") == "0",
                    "migration inferred a scoped namespace from historical provenance")
            clone = ("INSERT INTO shin_server.checkpoint_slots SELECT (jsonb_populate_record(NULL::shin_server.checkpoint_slots,"
                     "to_jsonb(s) || jsonb_build_object('revision',nextval('shin_server.checkpoint_revisions'),"
                     "'scope_workspace_id','primary'))).* FROM shin_server.checkpoint_slots s WHERE scope_workspace_id IS NULL AND slot_id='same-slot'")
            suite.psql(clone)
            assert_sql_refused(suite, clone, "unique_violation")
            suite.psql(clone.replace("'scope_workspace_id','primary'", "'scope_workspace_id','inconsistent',"
                                    "'provenance',jsonb_build_object('project_id','upgrade','workspace_id','inconsistent')"))
            require(suite.scalar("SELECT count(*) FROM shin_server.checkpoint_slots WHERE slot_id='same-slot'") == "3",
                    "checkpoint namespaces collided")
            assert_sql_refused(suite, "UPDATE shin_server.checkpoint_slots SET provenance='{}'::jsonb WHERE scope_workspace_id='primary'",
                               "check_violation")
            assert_sql_refused(suite, "UPDATE shin_server.checkpoint_slots SET scope_workspace_id='missing',"
                               "provenance='{\"project_id\":\"upgrade\",\"workspace_id\":\"missing\"}' WHERE scope_workspace_id='primary'",
                               "foreign_key_violation")
        if baseline == 25:
            suite.psql(insert_other)
            require(suite.scalar("SELECT count(*) FROM shin_server.workspace_requests WHERE key='old-workspace'") == "2",
                    "migration did not separate same-principal project keys")
            assert_sql_refused(suite, insert_other, "unique_violation")
            suite.psql(insert_other.replace("'dev-admin'", "'other-admin'"))
            require(suite.scalar("SELECT count(*) FROM shin_server.workspace_requests WHERE key='old-workspace'") == "3",
                    "migration merged different principals")
    finally:
        suite.reset_schema()
