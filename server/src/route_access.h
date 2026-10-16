#pragma once
#include "shin/server/model.h"
#include "shin/server/schema_checks.h"
#include <pqxx/pqxx>

namespace shin::server {
void select_content_route(pqxx::transaction_base& tx, const std::string& route);
bool preparation_has_active_route(pqxx::transaction_base& tx, std::uint64_t id);
std::int64_t activation_history_size(pqxx::transaction_base& tx);
void append_route_history_checks(SchemaChecks& checks);
}
