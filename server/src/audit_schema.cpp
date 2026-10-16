#include "shin/server/audit.h"
#include "shin/server/model.h"

#include <pqxx/pqxx>

#include <set>
#include <tuple>

namespace shin::server {
void append_audit_schema_checks(SchemaChecks& checks) {
    checks.add(
        "SELECT c.relname,a.attname,pg_catalog.format_type(a.atttypid,a.atttypmod) AS type,a.attnotnull "
        "FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_class c ON c.oid=a.attrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='shin_server' AND c.relkind='r' "
        "AND c.relname='audit_history' AND a.attnum>0 AND NOT a.attisdropped",
        [](const pqxx::result& rows) {
            using Column = std::tuple<std::string, std::string, std::string, bool>;
            static const std::set<Column> expected{
                {"audit_history", "singleton", "boolean", true},
                {"audit_history", "retired_through", "bigint", true},
                {"audit_history", "retired_events", "bigint", true},
                {"audit_history", "last_retention", "jsonb", false},
            };
            std::set<Column> actual;
            for (const auto& row : rows) {
                actual.emplace(row["relname"].as<std::string>(), row["attname"].as<std::string>(),
                               row["type"].as<std::string>(), row["attnotnull"].as<bool>());
            }
            if (actual != expected) throw ServiceError(503, "schema-unsupported", "Audit schema is not supported.");
        });
    checks.add(
        "SELECT "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.audit_history'::regclass AND conname='audit_history_pkey' AND contype='p' AND conkey=ARRAY[1]::smallint[]) AS history_pk, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.audit_history'::regclass AND conname='audit_history_singleton_check' AND contype='c') AS singleton_check, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.audit_history'::regclass AND conname='audit_history_retired_through_check' AND contype='c') AS retired_through_check, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.audit_history'::regclass AND conname='audit_history_retired_events_check' AND contype='c') AS retired_events_check, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.audit_history'::regclass AND conname='audit_history_last_retention_check' AND contype='c') AS last_retention_check, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.audit_history'::regclass AND conname='audit_history_shape_check' AND contype='c') AS shape_check, "
        "(SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.audit_history'::regclass AND contype='c')=5 AS history_checks, "
        "(SELECT count(*) FROM shin_server.audit_history WHERE singleton)=1 AS history_row",
        [](const pqxx::result& rows) {
            if (rows.size() != 1) throw ServiceError(503, "schema-unsupported", "Audit schema is not supported.");
            for (const auto& value : rows[0]) {
                if (!value.as<bool>()) throw ServiceError(503, "schema-unsupported", "Audit schema is not supported.");
            }
        });
}
} // namespace shin::server
