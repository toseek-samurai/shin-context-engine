#pragma once

#include "shin/server/model.h"
#include <pqxx/pqxx>
#include <optional>

namespace shin::server::observer_store_detail {
enum class Lock { none, shared, exclusive };
bool hex(const Json& value, std::size_t size);
void authorize(const Principal& principal, bool admin = false);
void admit(pqxx::transaction_base& tx, const Principal& principal, const std::optional<ObserverScope>& scope);
pqxx::result stream_rows(pqxx::transaction_base& tx, const Principal& principal, const std::string& stream, Lock lock);
pqxx::result find_stream_rows(pqxx::transaction_base& tx, const Principal& principal, const std::string& stream, Lock lock);
pqxx::result tombstone_rows(pqxx::transaction_base& tx, const Principal& principal, const std::string& stream);
Json stream_json(const pqxx::row_ref& row);
void audit(pqxx::transaction_base& tx, const Principal& principal, const pqxx::row_ref& stream,
           const std::string& action, std::optional<std::int64_t> sequence = {});
} // namespace shin::server::observer_store_detail
