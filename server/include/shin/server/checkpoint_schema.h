#pragma once

#include "model.h"
#include "schema_checks.h"

#include <string_view>

namespace shin::server {

void append_checkpoint_schema_checks(SchemaChecks& checks);
bool checkpoint_provenance_valid(const Json& provenance, std::string_view project);
void verify_checkpoint_runtime_grants(pqxx::transaction_base& transaction, std::string_view runtime_role);

} // namespace shin::server
