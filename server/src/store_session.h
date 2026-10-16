#pragma once

#include "shin/server/model.h"
#include <pqxx/pqxx>

namespace shin::server {
void validate_store_config(const StoreConfig& config);
pqxx::connection open_store_connection(const StoreConfig& config);
void verify_database_name(pqxx::connection& connection, const StoreConfig& config);
void verify_runtime_connection(pqxx::connection& connection, const StoreConfig& config);
[[noreturn]] void throw_database_unavailable();
}
