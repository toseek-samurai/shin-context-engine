#pragma once

#include "model.h"
#include "schema_checks.h"

namespace shin::server {
enum class IdentityMaintenance;
void append_recovery_schema_checks(SchemaChecks& checks);
Json recovery_operation(pqxx::transaction_base& tx, const std::string& expected_server_id,
                        const Json& input, bool apply, IdentityMaintenance mode);
}
