#include "shin/server/workspace_schema.h"
#include "shin/server/model.h"

#include <pqxx/pqxx>

#include <set>
#include <string>
#include <tuple>

namespace shin::server {
void append_workspace_schema_checks(SchemaChecks& checks) {
    checks.add(
        "SELECT c.relname,a.attname,pg_catalog.format_type(a.atttypid,a.atttypmod) AS type,a.attnotnull "
        "FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_class c ON c.oid=a.attrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='shin_server' "
        "AND c.relkind='r' AND c.relname IN ('workspaces','workspace_requests') AND a.attnum>0 AND NOT a.attisdropped",
        [](const pqxx::result& rows) {
            using Column = std::tuple<std::string, std::string, std::string, bool>;
            static const std::set<Column> expected{
                {"workspaces", "project_id", "text", true}, {"workspaces", "id", "text", true},
                {"workspaces", "source_host_id", "text", true}, {"workspaces", "display_name", "text", true},
                {"workspaces", "root", "text", false}, {"workspaces", "root_identity", "text", false},
                {"workspaces", "source_kind", "text", true},
                {"workspaces", "created_at", "timestamp with time zone", true}, {"workspaces", "revision", "bigint", true},
                {"workspace_requests", "principal_id", "text", true}, {"workspace_requests", "key", "text", true},
                {"workspace_requests", "request_hash", "text", true}, {"workspace_requests", "project_id", "text", true},
                {"workspace_requests", "workspace_id", "text", true}, {"workspace_requests", "response", "jsonb", true},
                {"workspace_requests", "created_at", "timestamp with time zone", true}};
            std::set<Column> actual;
            for (const auto& row : rows)
                actual.emplace(row["relname"].as<std::string>(), row["attname"].as<std::string>(),
                               row["type"].as<std::string>(), row["attnotnull"].as<bool>());
            if (actual != expected) throw ServiceError(503, "unsupported-schema", "Database schema is not supported.");
        });
    checks.add(
        "SELECT "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.workspaces'::regclass AND contype='p' AND conkey=ARRAY[1,2]::smallint[]) AS workspace_pk, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.workspaces'::regclass AND contype='u' AND conkey=ARRAY[3,5]::smallint[]) AS root_unique, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.workspaces'::regclass AND contype='u' AND conkey=ARRAY[3,6]::smallint[]) AS identity_unique, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.workspaces'::regclass AND contype='f' AND conkey=ARRAY[1]::smallint[] AND confrelid='shin_server.projects'::regclass AND confkey=ARRAY[1]::smallint[] AND confdeltype='r') AS project_fk, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.workspace_requests'::regclass AND contype='p' AND conkey=ARRAY[1,4,2]::smallint[] AND NOT condeferrable) AS requests_pk, "
        "(SELECT count(*)=1 AND bool_and(indisprimary AND indisvalid AND indisready AND indislive AND indimmediate) FROM pg_index WHERE indrelid='shin_server.workspace_requests'::regclass AND indisunique) AS requests_unique, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.workspace_requests'::regclass AND contype='f' AND conkey=ARRAY[4,5]::smallint[] AND confrelid='shin_server.workspaces'::regclass AND confkey=ARRAY[1,2]::smallint[] AND confdeltype='r') AS request_fk, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.audit_events'::regclass AND contype='f' AND conkey=ARRAY[4,9]::smallint[] AND confrelid='shin_server.workspaces'::regclass AND confkey=ARRAY[1,2]::smallint[] AND confdeltype='r') AS audit_fk, "
        "(SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.workspaces'::regclass AND contype='c')=8 AS workspace_checks, "
        "(SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.workspace_requests'::regclass AND contype='c')=4 AS request_checks",
        [](const pqxx::result& rows) {
            if (rows.size() != 1) throw ServiceError(503, "unsupported-schema", "Database schema is not supported.");
            for (const auto& field : rows[0])
                if (!field.as<bool>()) throw ServiceError(503, "unsupported-schema", "Database schema is not supported.");
        });
}
} // namespace shin::server
