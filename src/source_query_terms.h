#pragma once

#include <string>
#include <vector>

namespace shin::context {
// Input is the bounded unicode61 vocabulary, not SQL or caller-provided FTS syntax.
// These are query hints, never authority, document coverage or evidence claims.
std::vector<std::string> source_query_terms(const std::vector<std::string>& tokens);
// Descriptive queries prioritize file discovery within the existing span budget.
bool source_query_distinct_paths_first(const std::vector<std::string>& tokens);
}
