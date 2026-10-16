#include "shin/server/identity.h"
#include "shin/server/model.h"

#include <pqxx/pqxx>
#include <set>
#include <tuple>

namespace shin::server {
std::string read_server_identity(pqxx::transaction_base& tx) {
    const auto rows = tx.exec("SELECT server_id FROM shin_server.server_identity WHERE singleton");
    if (rows.size() != 1) throw ServiceError(503, "server-identity-unavailable", "Server identity is unavailable.");
    return rows[0][0].as<std::string>();
}

void append_server_identity_schema_checks(SchemaChecks& checks) {
    checks.add(
        "SELECT attname,format_type(atttypid,atttypmod),attnotnull FROM pg_attribute "
        "WHERE attrelid='shin_server.server_identity'::regclass AND attnum>0 AND NOT attisdropped",
        [](const pqxx::result& rows) {
            using Column = std::tuple<std::string, std::string, bool>;
            static const std::set<Column> expected{{"singleton", "boolean", true}, {"server_id", "text", true}};
            std::set<Column> actual;
            for (const auto& row : rows)
                actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<bool>());
            if (actual != expected) throw ServiceError(503, "schema-unsupported", "Server identity schema is not supported.");
        });
    checks.add(
        "SELECT "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.server_identity'::regclass AND contype='p' AND conkey=ARRAY[1]::smallint[]),"
        "(SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.server_identity'::regclass AND contype='c')=2,"
        "(SELECT count(*) FROM shin_server.server_identity WHERE singleton)=1",
        [](const pqxx::result& rows) {
            if (rows.size() != 1) throw ServiceError(503, "schema-unsupported", "Server identity constraints are not supported.");
            for (const auto& value : rows[0]) if (!value.as<bool>())
                throw ServiceError(503, "schema-unsupported", "Server identity constraints are not supported.");
        });
}
} // namespace shin::server
