#include "execution_report_internal.h"

namespace shin::context {
namespace {

using execution_report_internal::ReportTally;
using execution_report_internal::checked_add;
using execution_report_internal::reject_execution_report;

bool is_blank(std::string_view value) {
    for (const char character : value) {
        if (character != ' ' && character != '\n' && character != '\r' && character != '\t') return false;
    }
    return true;
}

ExecutionReportSummary make_summary(std::string_view format, const ReportTally& tally) {
    std::uint64_t classified_total = 0;
    checked_add(classified_total, tally.passed);
    checked_add(classified_total, tally.failed);
    checked_add(classified_total, tally.skipped);
    checked_add(classified_total, tally.flaky);
    checked_add(classified_total, tally.interrupted);
    if (classified_total != tally.total || tally.expected_failures > tally.passed) {
        reject_execution_report();
    }

    ExecutionReportSummary summary;
    summary.format = format;
    summary.total = tally.total;
    summary.passed = tally.passed;
    summary.failed = tally.failed;
    summary.skipped = tally.skipped;
    summary.flaky = tally.flaky;
    summary.interrupted = tally.interrupted;
    summary.expected_failures = tally.expected_failures;
    summary.global_errors = tally.global_errors;

    summary.result = execution_report_outcome(summary);
    return summary;
}

}  // namespace

std::string execution_report_outcome(const ExecutionReportSummary& summary) {
    if (summary.failed != 0 || summary.interrupted != 0 || summary.global_errors != 0) return "fail";
    if (summary.total == 0 || summary.flaky != 0 || (summary.skipped != 0 && summary.skipped != summary.total)) return "unknown";
    return summary.skipped == summary.total ? "skipped" : "pass";
}

ExecutionReportSummary parse_execution_report(std::string_view format, std::string_view bytes) {
    using namespace execution_report_internal;
    if (format != "junit" && format != "playwright") reject_execution_report_format();
    if (bytes.size() > kMaxExecutionReportBytes) reject_execution_report_size();
    if (is_blank(bytes)) return make_summary(format, {});

    try {
        const ReportTally tally = format == "junit" ? parse_junit_report_tally(bytes)
                                                      : parse_playwright_report_tally(bytes);
        return make_summary(format, tally);
    } catch (const CliError&) {
        throw;
    } catch (const std::exception&) {
        reject_execution_report();
    }
}

Json execution_report_summary_json(const ExecutionReportSummary& summary) {
    return Json{{"total", summary.total},
                {"passed", summary.passed},
                {"failed", summary.failed},
                {"skipped", summary.skipped},
                {"flaky", summary.flaky},
                {"interrupted", summary.interrupted},
                {"expected_failures", summary.expected_failures},
                {"global_errors", summary.global_errors}};
}

}  // namespace shin::context
