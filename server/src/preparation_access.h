#pragma once

#include "shin/server/model.h"
#include "shin/server/schema_checks.h"
#include <pqxx/pqxx>
#include <optional>

namespace shin::server {
void legacy_preparation_scope(pqxx::transaction_base& tx, const Principal& actor);
void preparation_worker_scope(pqxx::transaction_base& tx);
pqxx::result preparation_capacity(pqxx::transaction_base& tx);
// Call only after authorizing this route/workspace. These grant SELECT, never mutation.
void preparation_route_source(pqxx::transaction_base& tx, const std::string& project, const std::string& route);
void preparation_publication_sources(pqxx::transaction_base& tx, const std::string& project, const std::string& publication);
void lock_preparation_inputs(pqxx::transaction_base& tx, const std::string& project,
                             std::uint64_t candidate, std::optional<std::int64_t> base);
void append_preparation_access_checks(SchemaChecks& checks);
}
