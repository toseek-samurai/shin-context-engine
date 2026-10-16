#pragma once

#include "execution_report.h"

#include <limits>

namespace shin::context::execution_report_internal {

inline constexpr std::size_t kMaxExecutionReportNodes = 200000;
inline constexpr std::size_t kMaxExecutionReportDepth = 64;
inline constexpr std::uint64_t kMaxExecutionReportCases = 100000;

[[noreturn]] inline void reject_execution_report() {
    throw_usage_error("invalid-execution-report",
                      "Execution report is invalid or exceeds a safety limit.");
}

[[noreturn]] inline void reject_execution_report_size() {
    throw_usage_error("execution-report-too-large",
                      "Execution report exceeds the 4 MiB safety limit.");
}

[[noreturn]] inline void reject_execution_report_format() {
    throw_usage_error("unsupported-execution-report-format",
                      "Execution report format must be junit or playwright.");
}

inline void checked_increment(std::uint64_t& value) {
    if (value == (std::numeric_limits<std::uint64_t>::max)()) reject_execution_report();
    ++value;
}

inline void checked_add(std::uint64_t& target, std::uint64_t value) {
    if (value > (std::numeric_limits<std::uint64_t>::max)() - target) reject_execution_report();
    target += value;
}

struct ReportTally {
    std::uint64_t total = 0;
    std::uint64_t passed = 0;
    std::uint64_t failed = 0;
    std::uint64_t skipped = 0;
    std::uint64_t flaky = 0;
    std::uint64_t interrupted = 0;
    std::uint64_t expected_failures = 0;
    std::uint64_t global_errors = 0;
};

inline void checked_add_tally(ReportTally& target, const ReportTally& source) {
    checked_add(target.total, source.total);
    checked_add(target.passed, source.passed);
    checked_add(target.failed, source.failed);
    checked_add(target.skipped, source.skipped);
    checked_add(target.flaky, source.flaky);
    checked_add(target.interrupted, source.interrupted);
    checked_add(target.expected_failures, source.expected_failures);
    checked_add(target.global_errors, source.global_errors);
    if (target.total > kMaxExecutionReportCases) reject_execution_report();
}

ReportTally parse_junit_report_tally(std::string_view bytes);
ReportTally parse_playwright_report_tally(std::string_view bytes);

}  // namespace shin::context::execution_report_internal
