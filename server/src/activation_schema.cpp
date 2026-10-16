#include "shin/server/activation_schema.h"
#include "shin/server/model.h"

#include <pqxx/pqxx>

#include <set>
#include <string>
#include <tuple>

namespace shin::server {
void append_activation_schema_checks(SchemaChecks& checks) {
    checks.add(
        "SELECT c.relname,a.attname,format_type(a.atttypid,a.atttypmod),a.attnotnull "
        "FROM pg_attribute a JOIN pg_class c ON c.oid=a.attrelid JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='shin_server' AND c.relname IN ('active_routes','activation_receipts') AND c.relkind='r' AND a.attnum>0 AND NOT a.attisdropped",
        [](const pqxx::result& rows) {
            using Column = std::tuple<std::string, std::string, std::string, bool>;
            static const std::set<Column> expected{
                {"active_routes","project_id","text",true}, {"active_routes","route_id","text",true},
                {"active_routes","revision","bigint",true}, {"active_routes","preparation_id","bigint",false},
                {"active_routes","snapshot_id","text",false}, {"active_routes","route_fingerprint","text",false},
                {"active_routes","scope_workspace_id","text",false},
                {"activation_receipts","project_id","text",true}, {"activation_receipts","route_id","text",true},
                {"activation_receipts","previous_revision","bigint",true}, {"activation_receipts","revision","bigint",true},
                {"activation_receipts","preparation_id","bigint",false}, {"activation_receipts","snapshot_id","text",false},
                {"activation_receipts","scope_workspace_id","text",false},
                {"activation_receipts","principal_id","text",true}, {"activation_receipts","created_at","timestamp with time zone",true}};
            std::set<Column> actual;
            for (const auto& row : rows)
                actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(), row[3].as<bool>());
            if (actual != expected) throw ServiceError(503, "schema-unsupported", "Activation schema is invalid.");
        });
    checks.add(
        "SELECT (SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.active_routes'::regclass AND contype='c')=3 "
        "AND (SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.activation_receipts'::regclass AND contype='c')=5 "
        "AND EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.active_routes'::regclass AND contype='p' AND conkey=ARRAY[1,2]::smallint[]) "
        "AND EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.activation_receipts'::regclass AND contype='p' AND conkey=ARRAY[1,2,3]::smallint[]) "
        "AND EXISTS(SELECT 1 FROM pg_index WHERE indexrelid='shin_server.active_routes_preparation_idx'::regclass "
        "AND indrelid='shin_server.active_routes'::regclass AND indkey='4'::int2vector AND indpred IS NULL AND indisvalid AND indisready)",
        [](const pqxx::result& rows) {
            if (rows.size() != 1 || !rows[0][0].as<bool>())
                throw ServiceError(503, "schema-unsupported", "Activation constraints are invalid.");
        });
    checks.add(
        "SELECT EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.audit_events'::regclass "
        "AND contype='f' AND conkey=ARRAY[4,7,8]::smallint[] AND confrelid='shin_server.activation_receipts'::regclass "
        "AND confkey=ARRAY[1,2,3]::smallint[] AND confdeltype='r' AND convalidated)",
        [](const pqxx::result& rows) {
            if (rows.size() != 1 || !rows[0][0].as<bool>())
                throw ServiceError(503, "schema-unsupported", "Activation audit reference is invalid.");
        });
    checks.add(
        "SELECT EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.active_routes'::regclass AND contype='f' "
        "AND conkey=ARRAY[1]::smallint[] AND confrelid='shin_server.projects'::regclass "
        "AND confkey=ARRAY[1]::smallint[] AND confdeltype='r' AND convalidated) "
        "AND EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.active_routes'::regclass AND contype='f' "
        "AND conkey=ARRAY[1,2,4]::smallint[] AND confrelid='shin_server.preparations'::regclass "
        "AND confkey=ARRAY[2,3,1]::smallint[] AND confdeltype='r' AND convalidated)",
        [](const pqxx::result& rows) {
            if (rows.size() != 1 || !rows[0][0].as<bool>())
                throw ServiceError(503, "schema-unsupported", "Activation ownership constraints are invalid.");
        });
    checks.add(
        "SELECT EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.activation_receipts'::regclass AND contype='f' "
        "AND conkey=ARRAY[1]::smallint[] AND confrelid='shin_server.projects'::regclass "
        "AND confkey=ARRAY[1]::smallint[] AND confdeltype='r' AND convalidated) "
        "AND EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.activation_receipts'::regclass AND contype='f' "
        "AND conkey=ARRAY[1,2,5]::smallint[] AND confrelid='shin_server.preparations'::regclass "
        "AND confkey=ARRAY[2,3,1]::smallint[] AND confdeltype='r' AND convalidated)",
        [](const pqxx::result& rows) {
            if (rows.size() != 1 || !rows[0][0].as<bool>())
                throw ServiceError(503, "schema-unsupported", "Activation ownership constraints are invalid.");
        });
}
} // namespace shin::server
