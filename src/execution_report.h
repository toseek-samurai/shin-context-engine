#pragma once

#include "context_types.h"

namespace shin::context {

inline constexpr std::size_t kMaxExecutionReportBytes = 4 * 1024 * 1024;

struct ExecutionReportSummary {
    std::string format;
    std::string result;
    std::uint64_t total = 0;
    std::uint64_t passed = 0;
    std::uint64_t failed = 0;
    std::uint64_t skipped = 0;
    std::uint64_t flaky = 0;
    std::uint64_t interrupted = 0;
    std::uint64_t expected_failures = 0;
    std::uint64_t global_errors = 0;
};

ExecutionReportSummary parse_execution_report(std::string_view format, std::string_view bytes);
std::string execution_report_outcome(const ExecutionReportSummary& summary);
Json execution_report_summary_json(const ExecutionReportSummary& summary);

}  // namespace shin::context
