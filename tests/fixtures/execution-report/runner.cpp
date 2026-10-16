#include "execution_report.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

using shin::context::CliError;
using shin::context::ExecutionReportSummary;
using shin::context::execution_report_summary_json;
using shin::context::kMaxExecutionReportBytes;
using shin::context::parse_execution_report;

struct ExpectedSummary {
    const char* format;
    const char* result;
    std::uint64_t total;
    std::uint64_t passed;
    std::uint64_t failed;
    std::uint64_t skipped;
    std::uint64_t flaky;
    std::uint64_t interrupted;
    std::uint64_t expected_failures;
    std::uint64_t global_errors;
};

std::string read_fixture(const fs::path& root, const char* name) {
    std::ifstream input(root / name, std::ios::binary);
    if (!input) throw std::runtime_error("Fixture cannot be opened.");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void assert_summary(const fs::path& root, const char* name, const ExpectedSummary& expected) {
    const ExecutionReportSummary summary = parse_execution_report(expected.format, read_fixture(root, name));
    require(summary.format == expected.format && summary.result == expected.result, "Unexpected report result.");
    require(summary.total == expected.total && summary.passed == expected.passed &&
                summary.failed == expected.failed && summary.skipped == expected.skipped &&
                summary.flaky == expected.flaky && summary.interrupted == expected.interrupted &&
                summary.expected_failures == expected.expected_failures && summary.global_errors == expected.global_errors,
            "Unexpected report counters.");
    require(summary.total == summary.passed + summary.failed + summary.skipped + summary.flaky + summary.interrupted,
            "Summary total is not the aggregate of testcase outcomes.");
    require(summary.expected_failures <= summary.passed,
            "Expected failures must be a subset of passed testcase outcomes.");
}

void assert_rejected(std::string_view format, std::string_view input) {
    try {
        static_cast<void>(parse_execution_report(format, input));
    } catch (const CliError& error) {
        require(error.category() == "usage", "Report rejection must be a usage error.");
        require(std::string_view(error.what()).find("SECRET_EXECUTION_PAYLOAD") == std::string_view::npos,
                "Report rejection exposed input content.");
        return;
    }
    throw std::runtime_error("Unsafe report was accepted.");
}

void assert_rejected_fixture(const fs::path& root, std::string_view format, const char* name) {
    assert_rejected(format, read_fixture(root, name));
}

void assert_empty_report() {
    const ExecutionReportSummary summary = parse_execution_report("junit", "");
    require(summary.result == "unknown" && summary.total == 0 && summary.passed == 0 && summary.failed == 0 &&
                summary.skipped == 0 && summary.flaky == 0 && summary.interrupted == 0 &&
                summary.expected_failures == 0 && summary.global_errors == 0,
            "Empty report must remain unknown without testcase outcomes.");
}

void assert_synthetic_limits() {
    assert_rejected("playwright", std::string(kMaxExecutionReportBytes + 1, ' '));

    std::string deeply_nested_json(kMaxExecutionReportBytes > 0 ? 65 : 0, '[');
    assert_rejected("playwright", deeply_nested_json);

    std::string deeply_nested_xml;
    for (int index = 0; index <= 64; ++index) {
        deeply_nested_xml += "<testsuite>";
    }
    assert_rejected("junit", deeply_nested_xml);

    std::string excessive_nodes;
    excessive_nodes.reserve(200001 * 7);
    for (int index = 0; index <= 200000; ++index) {
        excessive_nodes += "<node/>";
    }
    assert_rejected("junit", excessive_nodes);

    std::string excessive_cases = "<testsuite>";
    excessive_cases.reserve(excessive_cases.size() + 100001 * 11 + 12);
    for (int index = 0; index <= 100000; ++index) {
        excessive_cases += "<testcase/>";
    }
    excessive_cases += "</testsuite>";
    assert_rejected("junit", excessive_cases);
}

void assert_junit_byte_validation() {
    std::string invalid_utf8 = "<testsuite name=\"";
    invalid_utf8.push_back(static_cast<char>(0xC3));
    invalid_utf8 += "\"/>";
    assert_rejected("junit", invalid_utf8);

    std::string invalid_control = "<testsuite>";
    invalid_control.push_back('\x01');
    invalid_control += "</testsuite>";
    assert_rejected("junit", invalid_control);
}

void assert_summary_json_is_sanitized(const fs::path& root) {
    const ExecutionReportSummary summary =
        parse_execution_report("playwright", read_fixture(root, "playwright-global-error.json"));
    const auto json = execution_report_summary_json(summary);
    const std::string serialized = json.dump();
    require(serialized.find("SECRET_EXECUTION_PAYLOAD") == std::string::npos,
            "Summary JSON exposed a report payload.");
    require(json.size() == 8 && json.contains("total") && json.contains("passed") && json.contains("failed") &&
                json.contains("skipped") && json.contains("flaky") && json.contains("interrupted") &&
                json.contains("expected_failures") && json.contains("global_errors"),
            "Summary JSON must contain only the frozen aggregate fields.");
    require(!json.contains("format") && !json.contains("result") && !json.contains("errors") &&
                !json.contains("stdout") && !json.contains("attachments"),
            "Summary JSON exposed a report detail field.");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: execution-report-runner <fixture-root>\n";
        return EXIT_FAILURE;
    }

    try {
        const fs::path root = argv[1];
        assert_empty_report();
        assert_summary(root, "junit-nested-pass.xml", {"junit", "pass", 2, 2, 0, 0, 0, 0, 0, 0});
        assert_summary(root, "junit-all-skipped.xml", {"junit", "skipped", 2, 0, 0, 2, 0, 0, 0, 0});
        assert_summary(root, "junit-flaky.xml", {"junit", "unknown", 1, 0, 0, 0, 1, 0, 0, 0});
        assert_summary(root, "junit-skipped-flaky.xml", {"junit", "unknown", 2, 0, 0, 1, 1, 0, 0, 0});
        assert_summary(root, "junit-passed-skipped.xml", {"junit", "unknown", 2, 1, 0, 1, 0, 0, 0, 0});
        assert_summary(root, "junit-attribute-skipped.xml", {"junit", "skipped", 2, 0, 0, 2, 0, 0, 0, 0});
        assert_summary(root, "junit-run-failure.xml", {"junit", "fail", 1, 0, 1, 0, 0, 0, 0, 0});
        assert_summary(root, "junit-ctest-properties.xml", {"junit", "pass", 1, 1, 0, 0, 0, 0, 0, 0});
        assert_summary(root, "junit-ctest-failed-disabled.xml", {"junit", "fail", 3, 1, 1, 1, 0, 0, 0, 0});
        assert_rejected("junit", "<testsuite tests=\"1\" failures=\"0\"><testcase status=\"fail\"/></testsuite>");
        const auto disabled_order = parse_execution_report("junit",
            "<testsuite tests=\"2\" disabled=\"2\" skipped=\"0\">"
            "<testcase disabled=\"true\" status=\"skipped\"/>"
            "<testcase status=\"skipped\" disabled=\"true\"/></testsuite>");
        require(disabled_order.skipped == 2 && disabled_order.result == "skipped", "Attribute order changed disabled outcomes.");
        assert_summary(root, "junit-summary-only.xml", {"junit", "unknown", 0, 0, 0, 0, 0, 0, 0, 0});
        assert_rejected_fixture(root, "junit", "junit-summary-only-nonzero.xml");
        assert_rejected_fixture(root, "junit", "junit-summary-only-failure.xml");
        assert_rejected_fixture(root, "junit", "junit-summary-only-merged.xml");
        assert_rejected_fixture(root, "junit", "junit-counter-conflict.xml");
        assert_rejected_fixture(root, "junit", "junit-doctype-entity.xml");
        assert_rejected_fixture(root, "junit", "junit-nested-testcase.xml");
        assert_rejected_fixture(root, "junit", "junit-suite-failure.xml");
        assert_rejected_fixture(root, "junit", "junit-unknown-status.xml");
        assert_rejected_fixture(root, "junit", "junit-result-notrun.xml");
        assert_rejected_fixture(root, "junit", "junit-notrun-failure.xml");
        assert_rejected_fixture(root, "junit", "junit-duplicate-attribute.xml");

        assert_summary(root, "playwright-pass.json", {"playwright", "pass", 1, 1, 0, 0, 0, 0, 0, 0});
        assert_summary(root, "playwright-expected-failure.json", {"playwright", "pass", 1, 1, 0, 0, 0, 0, 1, 0});
        assert_summary(root, "playwright-flaky.json", {"playwright", "unknown", 1, 0, 0, 0, 1, 0, 0, 0});
        assert_summary(root, "playwright-global-error.json", {"playwright", "fail", 0, 0, 0, 0, 0, 0, 0, 2});
        assert_summary(root, "playwright-interrupted.json", {"playwright", "fail", 1, 0, 0, 0, 0, 1, 0, 0});
        assert_rejected_fixture(root, "playwright", "playwright-duplicate-field.json");
        assert_rejected_fixture(root, "playwright", "playwright-stats-conflict.json");
        assert_synthetic_limits();
        assert_junit_byte_validation();
        assert_summary_json_is_sanitized(root);
    } catch (const std::exception& error) {
        std::cerr << "execution-report fixture failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Execution report fixtures passed.\n";
    return EXIT_SUCCESS;
}
