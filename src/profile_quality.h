#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "retrieval_session.h"

namespace shin::context {

struct SuiteEvaluation {
    std::size_t total = 0;
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t query_total = 0;
    std::size_t query_passed = 0;
    std::size_t quality_failed_queries = 0;
    std::size_t resource_failed_queries = 0;
    std::vector<std::string> failed_case_ids;
};

inline Json eval_resource_check_json(const Profile& profile, const SuiteEvaluation& suite) {
    return {{"policy", profile.eval_resource_policy},
            {"ok", suite.query_total > 0 && suite.resource_failed_queries == 0},
            {"failed_queries", suite.resource_failed_queries},
            {"warning_queries", profile.eval_resource_policy == "advisory"
                                    ? suite.resource_failed_queries : 0}};
}

SuiteEvaluation evaluate_profile_cases(RetrievalSession& session);

// Pack evaluation is case-aggregate only; query_total and query_passed remain zero.
SuiteEvaluation evaluate_pack_cases(RetrievalSession& session);

}  // namespace shin::context
