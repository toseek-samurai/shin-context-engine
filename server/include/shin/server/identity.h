#pragma once

#include <string>

#include "schema_checks.h"

namespace shin::server {
std::string read_server_identity(pqxx::transaction_base& transaction);
void append_server_identity_schema_checks(SchemaChecks& checks);
}
