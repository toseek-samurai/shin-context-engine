#pragma once

#include "shin/server/schema_checks.h"

#include <cstdint>

namespace shin::server {
void append_publication_schema_checks(SchemaChecks& checks);

// Retention must hold the preparation row lock before testing these references.
bool publication_references_preparation(pqxx::transaction_base& transaction, std::uint64_t id);
} // namespace shin::server
