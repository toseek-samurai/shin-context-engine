#include "execution_report_internal.h"

#include <unordered_set>

namespace shin::context::execution_report_internal {
namespace {

class BoundedJsonSax final : public nlohmann::json_sax<Json> {
public:
    bool null() override { return scalar(); }
    bool boolean(bool) override { return scalar(); }
    bool number_integer(number_integer_t) override { return scalar(); }
    bool number_unsigned(number_unsigned_t) override { return scalar(); }
    bool number_float(number_float_t, const string_t&) override { return scalar(); }
    bool string(string_t&) override { return scalar(); }
    bool binary(binary_t&) override { return scalar(); }

    bool start_object(std::size_t) override { return start_container(ContainerKind::object); }

    bool key(string_t& value) override {
        if (!bump_node() || stack_.empty() || stack_.back().kind != ContainerKind::object) return false;
        return stack_.back().keys.emplace(value).second;
    }

    bool end_object() override { return end_container(ContainerKind::object); }
    bool start_array(std::size_t) override { return start_container(ContainerKind::array); }
    bool end_array() override { return end_container(ContainerKind::array); }

    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

private:
    enum class ContainerKind { object, array };

    struct Frame {
        ContainerKind kind;
        std::unordered_set<std::string> keys;
    };

    bool scalar() { return bump_node(); }

    bool start_container(ContainerKind kind) {
        if (!bump_node() || stack_.size() >= kMaxExecutionReportDepth) return false;
        stack_.push_back(Frame{kind, {}});
        return true;
    }

    bool end_container(ContainerKind kind) {
        if (stack_.empty() || stack_.back().kind != kind) return false;
        stack_.pop_back();
        return true;
    }

    bool bump_node() {
        return ++nodes_ <= kMaxExecutionReportNodes;
    }

    std::size_t nodes_ = 0;
    std::vector<Frame> stack_;
};

const Json* json_member(const Json& object, std::string_view name) {
    if (!object.is_object()) reject_execution_report();
    const auto value = object.find(std::string(name));
    return value == object.end() ? nullptr : &*value;
}

const Json& required_json_member(const Json& object, std::string_view name) {
    const Json* value = json_member(object, name);
    if (value == nullptr) reject_execution_report();
    return *value;
}

const std::string& json_string(const Json& value) {
    if (!value.is_string()) reject_execution_report();
    return value.get_ref<const std::string&>();
}

std::uint64_t json_nonnegative_integer(const Json& value) {
    if (value.is_number_unsigned()) return value.get<Json::number_unsigned_t>();
    if (value.is_number_integer()) {
        const Json::number_integer_t parsed = value.get<Json::number_integer_t>();
        if (parsed >= 0) return static_cast<std::uint64_t>(parsed);
    }
    reject_execution_report();
}

void require_json_array(const Json& value) {
    if (!value.is_array()) reject_execution_report();
}

void require_json_object(const Json& value) {
    if (!value.is_object()) reject_execution_report();
}

bool is_playwright_test_status(const std::string& value) {
    return value == "passed" || value == "failed" || value == "timedOut" || value == "skipped" ||
           value == "interrupted";
}

enum class PlaywrightOutcome { expected, unexpected, flaky, skipped };

PlaywrightOutcome parse_playwright_outcome(const std::string& value) {
    if (value == "expected") return PlaywrightOutcome::expected;
    if (value == "unexpected") return PlaywrightOutcome::unexpected;
    if (value == "flaky") return PlaywrightOutcome::flaky;
    if (value == "skipped") return PlaywrightOutcome::skipped;
    reject_execution_report();
}

struct PlaywrightStats {
    std::uint64_t expected = 0;
    std::uint64_t unexpected = 0;
    std::uint64_t flaky = 0;
    std::uint64_t skipped = 0;
};

PlaywrightStats read_playwright_stats(const Json& root) {
    const Json& stats = required_json_member(root, "stats");
    require_json_object(stats);
    PlaywrightStats parsed;
    parsed.expected = json_nonnegative_integer(required_json_member(stats, "expected"));
    parsed.unexpected = json_nonnegative_integer(required_json_member(stats, "unexpected"));
    parsed.flaky = json_nonnegative_integer(required_json_member(stats, "flaky"));
    parsed.skipped = json_nonnegative_integer(required_json_member(stats, "skipped"));
    if (parsed.expected > kMaxExecutionReportCases || parsed.unexpected > kMaxExecutionReportCases ||
        parsed.flaky > kMaxExecutionReportCases || parsed.skipped > kMaxExecutionReportCases) {
        reject_execution_report();
    }
    return parsed;
}

struct PlaywrightObservation {
    ReportTally tally;
    PlaywrightStats stats;
};

PlaywrightOutcome compute_playwright_outcome(const Json& results, const std::string& expected_status,
                                             bool& interrupted) {
    std::uint64_t expected = 0;
    std::uint64_t unexpected = 0;
    std::uint64_t skipped = 0;
    interrupted = false;

    for (std::size_t index = 0; index < results.size(); ++index) {
        const Json& result = results[index];
        require_json_object(result);
        const std::string& actual_status = json_string(required_json_member(result, "status"));
        if (!is_playwright_test_status(actual_status)) reject_execution_report();
        if (json_nonnegative_integer(required_json_member(result, "retry")) != index) {
            reject_execution_report();
        }
        if (actual_status == "interrupted") {
            interrupted = true;
        } else if (actual_status == "skipped" && expected_status == "skipped") {
            checked_increment(skipped);
        } else if (actual_status == "skipped") {
            continue;
        } else if (actual_status == expected_status) {
            checked_increment(expected);
        } else {
            checked_increment(unexpected);
        }
    }

    if (expected == 0 && unexpected == 0) return PlaywrightOutcome::skipped;
    if (unexpected == 0) return PlaywrightOutcome::expected;
    if (expected == 0 && skipped == 0) return PlaywrightOutcome::unexpected;
    return PlaywrightOutcome::flaky;
}

void add_playwright_test(const Json& test, PlaywrightObservation& observation) {
    require_json_object(test);
    const std::string& expected_status = json_string(required_json_member(test, "expectedStatus"));
    if (!is_playwright_test_status(expected_status)) reject_execution_report();
    const Json& results = required_json_member(test, "results");
    require_json_array(results);
    const PlaywrightOutcome declared = parse_playwright_outcome(json_string(required_json_member(test, "status")));
    bool interrupted = false;
    if (compute_playwright_outcome(results, expected_status, interrupted) != declared) {
        reject_execution_report();
    }
    if (++observation.tally.total > kMaxExecutionReportCases) reject_execution_report();

    switch (declared) {
        case PlaywrightOutcome::expected: checked_increment(observation.stats.expected); break;
        case PlaywrightOutcome::unexpected: checked_increment(observation.stats.unexpected); break;
        case PlaywrightOutcome::flaky: checked_increment(observation.stats.flaky); break;
        case PlaywrightOutcome::skipped: checked_increment(observation.stats.skipped); break;
    }
    if (interrupted) {
        checked_increment(observation.tally.interrupted);
        return;
    }

    switch (declared) {
        case PlaywrightOutcome::expected:
            checked_increment(observation.tally.passed);
            if (expected_status != "passed" && expected_status != "skipped") {
                checked_increment(observation.tally.expected_failures);
            }
            break;
        case PlaywrightOutcome::unexpected: checked_increment(observation.tally.failed); break;
        case PlaywrightOutcome::flaky: checked_increment(observation.tally.flaky); break;
        case PlaywrightOutcome::skipped: checked_increment(observation.tally.skipped); break;
    }
}

void walk_playwright_spec(const Json& spec, PlaywrightObservation& observation) {
    require_json_object(spec);
    const Json& tests = required_json_member(spec, "tests");
    require_json_array(tests);
    for (const Json& test : tests) add_playwright_test(test, observation);
}

void walk_playwright_suite(const Json& suite, PlaywrightObservation& observation) {
    require_json_object(suite);
    const Json& specs = required_json_member(suite, "specs");
    require_json_array(specs);
    for (const Json& spec : specs) walk_playwright_spec(spec, observation);
    if (const Json* suites = json_member(suite, "suites")) {
        require_json_array(*suites);
        for (const Json& nested : *suites) walk_playwright_suite(nested, observation);
    }
}

void validate_playwright_stats(const PlaywrightStats& expected, const PlaywrightStats& observed) {
    if (expected.expected != observed.expected || expected.unexpected != observed.unexpected ||
        expected.flaky != observed.flaky || expected.skipped != observed.skipped) {
        reject_execution_report();
    }
}

}  // namespace

ReportTally parse_playwright_report_tally(std::string_view bytes) {
    BoundedJsonSax sax;
    if (!Json::sax_parse(bytes.begin(), bytes.end(), &sax)) reject_execution_report();
    const Json document = Json::parse(bytes.begin(), bytes.end(), nullptr, false, false);
    if (document.is_discarded()) reject_execution_report();
    require_json_object(document);

    const Json& errors = required_json_member(document, "errors");
    require_json_array(errors);
    const PlaywrightStats expected_stats = read_playwright_stats(document);
    const Json& suites = required_json_member(document, "suites");
    require_json_array(suites);

    PlaywrightObservation observation;
    for (const Json& suite : suites) walk_playwright_suite(suite, observation);
    validate_playwright_stats(expected_stats, observation.stats);
    observation.tally.global_errors = static_cast<std::uint64_t>(errors.size());
    return observation.tally;
}

}  // namespace shin::context::execution_report_internal
