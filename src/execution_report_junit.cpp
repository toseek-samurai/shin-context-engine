#include "execution_report_internal.h"

#include <pugixml/pugixml.hpp>
#include <simdjson/simdjson.h>

#include <charconv>
#include <optional>
#include <unordered_set>

namespace shin::context::execution_report_internal {
namespace {

bool is_ascii_space(char value) {
    return value == ' ' || value == '\n' || value == '\r' || value == '\t';
}

void validate_junit_input_bytes(std::string_view bytes) {
    if (!simdjson::validate_utf8(bytes)) reject_execution_report();
    for (const unsigned char value : bytes) {
        if (value < 0x20 && value != '\t' && value != '\n' && value != '\r') {
            reject_execution_report();
        }
    }
}

std::size_t find_xml_tag_end(std::string_view bytes, std::size_t position) {
    char quote = 0;
    for (std::size_t index = position; index < bytes.size(); ++index) {
        const char character = bytes[index];
        if (quote != 0) {
            if (character == quote) quote = 0;
        } else if (character == '\'' || character == '"') {
            quote = character;
        } else if (character == '>') {
            return index;
        }
    }
    reject_execution_report();
}

void preflight_junit_xml(std::string_view bytes) {
    std::size_t depth = 0;
    std::size_t nodes = 0;
    for (std::size_t index = 0; index < bytes.size();) {
        if (bytes[index] == '\0') reject_execution_report();
        if (bytes[index] != '<') {
            ++index;
            continue;
        }

        if (++index == bytes.size()) reject_execution_report();
        if (bytes.substr(index, 3) == "!--") {
            const std::size_t end = bytes.find("-->", index + 3);
            if (end == std::string_view::npos) reject_execution_report();
            index = end + 3;
            continue;
        }
        if (bytes.substr(index, 8) == "![CDATA[") {
            const std::size_t end = bytes.find("]]>", index + 8);
            if (end == std::string_view::npos) reject_execution_report();
            index = end + 3;
            continue;
        }
        if (bytes[index] == '?') {
            const std::size_t end = bytes.find("?>", index + 1);
            if (end == std::string_view::npos) reject_execution_report();
            index = end + 2;
            continue;
        }
        if (bytes.substr(index, 8) == "!DOCTYPE" || bytes.substr(index, 7) == "!ENTITY") {
            reject_execution_report();
        }

        const std::size_t end = find_xml_tag_end(bytes, index);
        if (bytes[index] == '/') {
            if (depth == 0) reject_execution_report();
            --depth;
            index = end + 1;
            continue;
        }
        if (bytes[index] == '!') {
            index = end + 1;
            continue;
        }

        std::size_t last = end;
        while (last > index && is_ascii_space(bytes[last - 1])) --last;
        if (last == index) reject_execution_report();
        if (++nodes > kMaxExecutionReportNodes) reject_execution_report();
        if (bytes[last - 1] != '/' && ++depth > kMaxExecutionReportDepth) reject_execution_report();
        index = end + 1;
    }
}

void inspect_junit_xml_node(const pugi::xml_node& node, std::size_t depth, std::size_t& nodes) {
    if (++nodes > kMaxExecutionReportNodes) reject_execution_report();
    if (node.type() == pugi::node_doctype) reject_execution_report();

    std::size_t child_depth = depth;
    if (node.type() == pugi::node_element && ++child_depth > kMaxExecutionReportDepth) {
        reject_execution_report();
    }
    if (node.type() == pugi::node_element) {
        std::unordered_set<std::string> attributes;
        for (const pugi::xml_attribute attribute : node.attributes()) {
            if (!attributes.emplace(attribute.name()).second) reject_execution_report();
        }
    }
    for (const pugi::xml_node child : node.children()) {
        inspect_junit_xml_node(child, child_depth, nodes);
    }
}

bool xml_name_is(const pugi::xml_node& node, std::string_view name) {
    return std::string_view(node.name()) == name;
}

std::uint64_t parse_counter_value(const char* value) {
    if (value == nullptr || *value == '\0') reject_execution_report();
    std::uint64_t parsed = 0;
    const char* const end = value + std::char_traits<char>::length(value);
    const auto result = std::from_chars(value, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) reject_execution_report();
    return parsed;
}

struct JunitCounters {
    std::optional<std::uint64_t> tests;
    std::optional<std::uint64_t> failures;
    std::optional<std::uint64_t> errors;
    std::optional<std::uint64_t> skipped;
    std::optional<std::uint64_t> disabled;

    bool present() const {
        return tests.has_value() || failures.has_value() || errors.has_value() || skipped.has_value() || disabled.has_value();
    }
};

JunitCounters read_junit_counters(const pugi::xml_node& node) {
    JunitCounters counters;
    for (const pugi::xml_attribute attribute : node.attributes()) {
        const std::string_view name = attribute.name();
        if (name == "status" || name == "result" || name == "outcome") reject_execution_report();
        if (name != "tests" && name != "failures" && name != "errors" && name != "skipped" &&
            name != "disabled") {
            continue;
        }
        const std::uint64_t value = parse_counter_value(attribute.value());
        if (name == "tests") counters.tests = value;
        else if (name == "failures") counters.failures = value;
        else if (name == "errors") counters.errors = value;
        else if (name == "skipped") counters.skipped = value;
        else counters.disabled = value;
    }
    return counters;
}

struct JunitScope {
    ReportTally tally;
    std::uint64_t failure_cases = 0;
    std::uint64_t error_cases = 0;
    std::uint64_t disabled_cases = 0;
};

void validate_text_only_metadata(const pugi::xml_node& node) {
    for (const pugi::xml_node child : node.children()) {
        if (child.type() == pugi::node_element) reject_execution_report();
    }
}

void validate_properties_metadata(const pugi::xml_node& node) {
    for (const pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_element) continue;
        if (!xml_name_is(child, "property")) reject_execution_report();
        validate_text_only_metadata(child);
    }
}

void validate_suite_metadata(const pugi::xml_node& node) {
    if (xml_name_is(node, "properties")) {
        validate_properties_metadata(node);
    } else if (xml_name_is(node, "system-out") || xml_name_is(node, "system-err")) {
        validate_text_only_metadata(node);
    } else {
        reject_execution_report();
    }
}

struct TestcaseAttributes {
    bool skipped = false;
    bool disabled = false;
    bool explicit_pass = false;
    bool explicit_failure = false;
};

bool is_known_testcase_attribute(std::string_view name) {
    return name == "name" || name == "classname" || name == "class" || name == "time" ||
           name == "assertions" || name == "file" || name == "line" || name == "column" ||
           name == "timestamp" || name == "package" || name == "type" || name == "status" ||
           name == "disabled";
}

TestcaseAttributes read_testcase_attributes(const pugi::xml_node& test_case) {
    TestcaseAttributes result;
    bool saw_status = false;
    bool saw_disabled = false;
    for (const pugi::xml_attribute attribute : test_case.attributes()) {
        const std::string_view name = attribute.name();
        const std::string_view value = attribute.value();
        if (!is_known_testcase_attribute(name)) reject_execution_report();
        if (name == "status") {
            if (saw_status || value.empty()) reject_execution_report();
            saw_status = true;
            if (value == "notrun" || value == "skipped" || value == "disabled") {
                result.skipped = true;
                result.disabled = result.disabled || value != "skipped";
            } else if (value == "passed" || value == "success") {
                result.explicit_pass = true;
            } else if (value == "run") {
                continue;
            } else if (value == "fail") {
                result.explicit_failure = true;
            } else {
                reject_execution_report();
            }
        } else if (name == "disabled") {
            if (saw_disabled) reject_execution_report();
            saw_disabled = true;
            if (value == "true" || value == "1") {
                result.skipped = true;
                result.disabled = true;
            } else if (value != "false" && value != "0") {
                reject_execution_report();
            }
        }
    }
    if (result.skipped && (result.explicit_pass || result.explicit_failure)) reject_execution_report();
    return result;
}

void add_junit_case(JunitScope& scope, const pugi::xml_node& test_case) {
    const TestcaseAttributes attributes = read_testcase_attributes(test_case);
    bool has_failure = false;
    bool has_error = false;
    bool has_skipped = false;
    bool has_flaky_or_rerun = false;
    std::size_t terminal_states = 0;

    for (const pugi::xml_node child : test_case.children()) {
        if (child.type() != pugi::node_element) continue;
        if (xml_name_is(child, "failure")) {
            has_failure = true;
            ++terminal_states;
            validate_text_only_metadata(child);
        } else if (xml_name_is(child, "error")) {
            has_error = true;
            ++terminal_states;
            validate_text_only_metadata(child);
        } else if (xml_name_is(child, "skipped")) {
            has_skipped = true;
            ++terminal_states;
            validate_text_only_metadata(child);
        } else if (xml_name_is(child, "flakyFailure") || xml_name_is(child, "flakyError") ||
                   xml_name_is(child, "rerunFailure") || xml_name_is(child, "rerunError")) {
            has_flaky_or_rerun = true;
            validate_text_only_metadata(child);
        } else if (xml_name_is(child, "system-out") || xml_name_is(child, "system-err")) {
            validate_text_only_metadata(child);
        } else if (xml_name_is(child, "properties")) {
            validate_properties_metadata(child);
        } else {
            reject_execution_report();
        }
    }
    if (attributes.skipped) has_skipped = true;
    if (terminal_states > 1 || (has_skipped && has_flaky_or_rerun) ||
        (attributes.explicit_failure && !has_failure && !has_error) ||
        (attributes.skipped && (has_failure || has_error || has_flaky_or_rerun)) ||
        (attributes.explicit_pass && (has_failure || has_error || has_skipped || has_flaky_or_rerun))) {
        reject_execution_report();
    }

    if (++scope.tally.total > kMaxExecutionReportCases) reject_execution_report();
    if (has_failure) {
        checked_increment(scope.tally.failed);
        checked_increment(scope.failure_cases);
    } else if (has_error) {
        checked_increment(scope.tally.failed);
        checked_increment(scope.error_cases);
    } else if (has_skipped) {
        checked_increment(scope.tally.skipped);
        if (attributes.disabled) checked_increment(scope.disabled_cases);
    } else if (has_flaky_or_rerun) {
        checked_increment(scope.tally.flaky);
    } else {
        checked_increment(scope.tally.passed);
    }
}

void validate_junit_counters(const JunitCounters& counters, const JunitScope& scope) {
    if (scope.tally.total == 0) return;
    if (counters.tests && *counters.tests != scope.tally.total) reject_execution_report();
    if (counters.failures && *counters.failures != scope.failure_cases) reject_execution_report();
    if (counters.errors && *counters.errors != scope.error_cases) reject_execution_report();
    if (counters.disabled && *counters.disabled != scope.disabled_cases) reject_execution_report();
    const std::uint64_t skipped = scope.tally.skipped - (counters.disabled ? scope.disabled_cases : 0);
    if (counters.skipped && *counters.skipped != skipped) reject_execution_report();
}

void reject_nonempty_summary_only(const JunitCounters& counters) {
    if (!counters.present()) return;
    if (!counters.tests) {
        if (counters.failures || counters.errors || counters.skipped || counters.disabled) reject_execution_report();
        return;
    }
    if (*counters.tests > kMaxExecutionReportCases) reject_execution_report();
    const std::uint64_t failures = counters.failures.value_or(0);
    const std::uint64_t errors = counters.errors.value_or(0);
    const std::uint64_t skipped = counters.skipped.value_or(0);
    if (counters.disabled.value_or(0) != 0) reject_execution_report();
    if (failures > *counters.tests || errors > *counters.tests || skipped > *counters.tests ||
        failures > *counters.tests - errors || failures + errors > *counters.tests - skipped) {
        reject_execution_report();
    }
    if (*counters.tests != 0) reject_execution_report();
}

JunitScope walk_junit_suite(const pugi::xml_node& suite) {
    JunitScope scope;
    for (const pugi::xml_node child : suite.children()) {
        if (child.type() != pugi::node_element) continue;
        if (xml_name_is(child, "testcase")) {
            add_junit_case(scope, child);
        } else if (xml_name_is(child, "testsuite")) {
            const JunitScope nested = walk_junit_suite(child);
            checked_add_tally(scope.tally, nested.tally);
            checked_add(scope.failure_cases, nested.failure_cases);
            checked_add(scope.error_cases, nested.error_cases);
            checked_add(scope.disabled_cases, nested.disabled_cases);
        } else {
            validate_suite_metadata(child);
        }
    }
    const JunitCounters counters = read_junit_counters(suite);
    if (scope.tally.total == 0) reject_nonempty_summary_only(counters);
    validate_junit_counters(counters, scope);
    return scope;
}

JunitScope walk_junit_container(const pugi::xml_node& container) {
    JunitScope scope;
    for (const pugi::xml_node child : container.children()) {
        if (child.type() != pugi::node_element) continue;
        if (xml_name_is(child, "testsuite")) {
            const JunitScope nested = walk_junit_suite(child);
            checked_add_tally(scope.tally, nested.tally);
            checked_add(scope.failure_cases, nested.failure_cases);
            checked_add(scope.error_cases, nested.error_cases);
            checked_add(scope.disabled_cases, nested.disabled_cases);
        } else if (xml_name_is(child, "testsuites")) {
            const JunitScope nested = walk_junit_container(child);
            checked_add_tally(scope.tally, nested.tally);
            checked_add(scope.failure_cases, nested.failure_cases);
            checked_add(scope.error_cases, nested.error_cases);
            checked_add(scope.disabled_cases, nested.disabled_cases);
        } else {
            validate_suite_metadata(child);
        }
    }
    const JunitCounters counters = read_junit_counters(container);
    if (scope.tally.total == 0) reject_nonempty_summary_only(counters);
    validate_junit_counters(counters, scope);
    return scope;
}

}  // namespace

ReportTally parse_junit_report_tally(std::string_view bytes) {
    validate_junit_input_bytes(bytes);
    preflight_junit_xml(bytes);
    pugi::xml_document document;
    const pugi::xml_parse_result parsed =
        document.load_buffer(bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!parsed) reject_execution_report();

    std::size_t nodes = 0;
    for (const pugi::xml_node child : document.children()) {
        inspect_junit_xml_node(child, 0, nodes);
    }

    pugi::xml_node root;
    std::size_t root_elements = 0;
    for (const pugi::xml_node child : document.children()) {
        if (child.type() == pugi::node_element) {
            root = child;
            ++root_elements;
        }
    }
    if (root_elements != 1 || (!xml_name_is(root, "testsuite") && !xml_name_is(root, "testsuites"))) {
        reject_execution_report();
    }
    return xml_name_is(root, "testsuite") ? walk_junit_suite(root).tally
                                              : walk_junit_container(root).tally;
}

}  // namespace shin::context::execution_report_internal
