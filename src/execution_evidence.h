#pragma once

#include "session_contract.h"

namespace shin::context {

inline constexpr std::size_t kMaxExecutionSelectors = 32;
inline constexpr std::size_t kMaxExecutionFiles = 8192;
inline constexpr std::uintmax_t kMaxExecutionFileBytes = 16U * 1024U * 1024U;
inline constexpr std::uintmax_t kMaxExecutionTotalBytes = 64U * 1024U * 1024U;

Json capture_execution_inputs(const RuntimeOptions& options, const Json& request,
                              const Json& binding, std::int64_t captured_at);
Json validate_execution_inputs(const Json& input);
Json assess_execution_inputs(const RuntimeOptions& options, const Json& input);
Json parse_execution_receipts(const Json& input);
struct ExecutionReceiptAssessment {
    Json receipts;
    std::int64_t observed_at;
};

ExecutionReceiptAssessment assess_execution_receipts(const RuntimeOptions& options, const Json& receipts,
                               const Json& current_scope, const SessionEvaluationOptions& command,
                               bool observation_complete);
Json execution_receipt_actions(const Json& receipts);
Json parse_execution_bundle(const Json& input);
Json read_execution_bundle(const fs::path& path, const fs::path& root);
int run_execution_evidence(const RuntimeOptions& options, const std::vector<std::string>& args,
                           const std::string& command);

}  // namespace shin::context
