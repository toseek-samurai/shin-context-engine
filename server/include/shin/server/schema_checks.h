#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pqxx { class result; class transaction_base; }

namespace shin::server {
// Owns constant catalog queries and pure result validators for one observation.
class SchemaChecks {
public:
    using Validator = void (*)(const pqxx::result&);
    void add(std::string_view query, Validator validate);
    void run(pqxx::transaction_base& transaction) const;

private:
    struct Check { std::string query; Validator validate; };
    std::vector<Check> checks_;
};
}
