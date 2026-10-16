#pragma once

#include "context_types.h"
#include "session_contract.h"

namespace shin::context {

struct ImpactEvaluationOptions {
    int limit = 24;
    int depth = 1;
    std::size_t evidence_bytes = 32768;
    bool require_current = false;
    bool strict = false;
    std::optional<SessionEvaluationOptions> receipt_options;
};

Json evaluate_impact(const RuntimeOptions& options, const ImpactEvaluationOptions& command,
                     std::optional<std::string_view> receipt_bundle = std::nullopt);
int run_impact(const RuntimeOptions& options, const std::vector<std::string>& args);

}  // namespace shin::context
