#include "shin/server/observer_ingestion.h"
#include <pqxx/pqxx>
#include <set>
#include <tuple>

namespace shin::server {
void append_observer_schema_checks(SchemaChecks& checks) {
    checks.add(
        "SELECT c.relname,a.attname,pg_catalog.format_type(a.atttypid,a.atttypmod) AS type,a.attnotnull "
        "FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_class c ON c.oid=a.attrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='shin_server' AND c.relkind='r' "
        "AND c.relname IN ('observer_streams','observer_submissions','observer_usage','observer_tombstones') AND a.attnum>0 AND NOT a.attisdropped",
        [](const pqxx::result& rows) {
            using Column = std::tuple<std::string, std::string, std::string, bool>;
            static const std::set<Column> expected = [] {
                std::set<Column> value;
                for (const auto* name : {"stream_id", "project_id", "workspace_id", "principal_id", "observer_id", "source_host_id", "profile", "binding_digest", "enrollment_hash"})
                    value.emplace("observer_streams", name, "text", true);
                value.emplace("observer_streams", "revoked", "boolean", true);
                value.emplace("observer_streams", "scope_tenant_id", "text", false);
                value.emplace("observer_tombstones", "scope_tenant_id", "text", false);
                for (const auto* name : {"accepted_sequence", "confirmed_sequence", "retired_sequence", "observed_sequence", "retained_bytes"})
                    value.emplace("observer_streams", name, "bigint", true);
                value.emplace("observer_streams", "last_retention", "jsonb", false);
                value.emplace("observer_streams", "enrolled_at", "timestamp with time zone", true);
                for (const auto* name : {"stream_id", "submission_id", "submission_hash", "payload_hash"})
                    value.emplace("observer_submissions", name, "text", true);
                value.emplace("observer_submissions", "sequence", "bigint", true);
                for (const auto* name : {"submission", "receipt"}) value.emplace("observer_submissions", name, "jsonb", true);
                value.emplace("observer_submissions", "retained_bytes", "integer", true);
                value.emplace("observer_submissions", "received_at", "timestamp with time zone", true);
                value.emplace("observer_usage", "singleton", "boolean", true);
                for (const auto* name : {"streams", "submissions", "retained_bytes", "tombstones"}) value.emplace("observer_usage", name, "bigint", true);
                for (const auto* name : {"stream_id", "project_id", "workspace_id", "principal_id", "enrollment_hash"})
                    value.emplace("observer_tombstones", name, "text", true);
                value.emplace("observer_tombstones", "retirement", "jsonb", true);
                value.emplace("observer_tombstones", "retired_at", "timestamp with time zone", true);
                return value;
            }();
            std::set<Column> actual;
            for (const auto& row : rows)
                actual.emplace(row["relname"].as<std::string>(), row["attname"].as<std::string>(), row["type"].as<std::string>(), row["attnotnull"].as<bool>());
            if (actual != expected) throw ServiceError(503, "schema-unsupported", "Observer schema is not supported.");
        });
    checks.add(
        "SELECT "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.observer_streams'::regclass AND contype='p' AND conkey=ARRAY[1]::smallint[]) AS stream_pk, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.observer_streams'::regclass AND contype='u' AND conkey=ARRAY[2,3,1]::smallint[]) AS stream_scope, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.observer_streams'::regclass AND contype='f' AND conkey=ARRAY[2,3]::smallint[] AND confrelid='shin_server.workspaces'::regclass AND confkey=ARRAY[1,2]::smallint[] AND confdeltype='r') AS workspace_fk, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.observer_submissions'::regclass AND contype='p' AND conkey=ARRAY[1,2]::smallint[]) AS submission_pk, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.observer_submissions'::regclass AND contype='u' AND conkey=ARRAY[3]::smallint[]) AS submission_unique, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.observer_submissions'::regclass AND contype='f' AND conkey=ARRAY[1]::smallint[] AND confrelid='shin_server.observer_streams'::regclass AND confkey=ARRAY[1]::smallint[] AND confdeltype='r') AS stream_fk, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.observer_usage'::regclass AND contype='p' AND conkey=ARRAY[1]::smallint[]) AS usage_pk, "
        "NOT EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.audit_events'::regclass AND contype='f' AND confrelid='shin_server.observer_streams'::regclass) AS audit_stream_fk_absent, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.observer_tombstones'::regclass AND contype='p' AND conkey=ARRAY[1]::smallint[]) AS tombstone_pk, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.observer_tombstones'::regclass AND contype='f' AND conkey=ARRAY[2,3]::smallint[] AND confrelid='shin_server.workspaces'::regclass AND confkey=ARRAY[1,2]::smallint[] AND confdeltype='r') AS tombstone_workspace_fk, "
        "(SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.observer_tombstones'::regclass AND contype='c')=4 AS tombstone_checks, "
        "EXISTS(SELECT 1 FROM pg_index WHERE indexrelid='shin_server.observer_tombstone_project'::regclass AND indrelid='shin_server.observer_tombstones'::regclass AND indkey='2 1'::int2vector) AS tombstone_project_index, "
        "NOT EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.audit_events'::regclass AND contype='f' AND confrelid='shin_server.observer_submissions'::regclass) AS audit_submission_fk_absent, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.audit_events'::regclass AND conname='audit_observer_shape' AND contype='c') AS audit_shape_check, "
        "(SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.observer_streams'::regclass AND contype='c')=12 AS stream_checks, "
        "(SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.observer_submissions'::regclass AND contype='c')=7 AS submission_checks, "
        "(SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.observer_usage'::regclass AND contype='c')=5 AS usage_checks, "
        "EXISTS(SELECT 1 FROM pg_index WHERE indexrelid='shin_server.observer_active_identity'::regclass AND indrelid='shin_server.observer_streams'::regclass AND indisunique AND indkey='2 3 5'::int2vector AND pg_get_expr(indpred,indrelid)='(NOT revoked)') AS active_unique, "
        "EXISTS(SELECT 1 FROM pg_index WHERE indexrelid='shin_server.observer_owner_cursor'::regclass AND indrelid='shin_server.observer_streams'::regclass AND indkey='4 1'::int2vector) AS owner_index, "
        "(SELECT count(*) FROM shin_server.observer_usage WHERE singleton)=1 AS usage_row, "
        "(SELECT count(*)=2 FROM pg_constraint WHERE conname IN ('observer_streams_owner_fk','observer_tombstones_owner_fk') "
        "AND conrelid IN ('shin_server.observer_streams'::regclass,'shin_server.observer_tombstones'::regclass) "
        "AND contype='f' AND confrelid='shin_server.identity_project_owners'::regclass AND confdeltype='r' AND convalidated "
        "AND pg_get_constraintdef(oid)='FOREIGN KEY (scope_tenant_id, project_id) REFERENCES shin_server.identity_project_owners(tenant_id, project_id) ON DELETE RESTRICT') AS ownership_fks",
        [](const pqxx::result& rows) {
            if (rows.size() != 1) throw ServiceError(503, "schema-unsupported", "Observer schema constraints are not supported.");
            for (const auto& value : rows[0])
                if (!value.as<bool>()) throw ServiceError(503, "schema-unsupported", "Observer schema constraints are not supported.");
        });
}
} // namespace shin::server
