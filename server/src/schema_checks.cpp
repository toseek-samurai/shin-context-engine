#include "shin/server/schema_checks.h"
#include "shin/server/model.h"

#include <pqxx/pqxx>
#include <stdexcept>

namespace shin::server {
void SchemaChecks::add(std::string_view query, Validator validate) {
    if (query.empty() || query.size() > 32768 || query.find('\0') != std::string_view::npos || validate == nullptr || checks_.size() >= 64)
        throw std::logic_error("Invalid compiled schema verification plan.");
    checks_.push_back({std::string(query), validate});
}

void SchemaChecks::run(pqxx::transaction_base& transaction) const {
    if (checks_.empty()) throw std::logic_error("Schema verification cannot be empty.");
    std::vector<pqxx::result> results;
    results.reserve(checks_.size());
    try {
        pqxx::pipeline pipeline{transaction, "schema-verification"};
        pipeline.retain(static_cast<int>(checks_.size()));
        std::vector<pqxx::pipeline::query_id> queries;
        queries.reserve(checks_.size());
        for (const auto& check : checks_) queries.push_back(pipeline.insert(check.query));
        pipeline.resume();
        for (const auto query : queries) results.push_back(pipeline.retrieve(query));
        pipeline.complete();
    } catch (const pqxx::sql_error& error) {
        const auto state = error.sqlstate();
        if (state == "42P01" || state == "42703" || state == "42704" || state == "3F000")
            throw ServiceError(503, "schema-unsupported", "The database schema is not supported.");
        throw;
    }
    // All results are detached before validation can throw; none are reused by another lease.
    for (std::size_t index = 0; index < checks_.size(); ++index) checks_[index].validate(results[index]);
}
} // namespace shin::server
