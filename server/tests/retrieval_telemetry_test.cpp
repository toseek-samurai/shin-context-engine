#include "shin/server/retrieval_telemetry.h"

#include <array>
#include <exception>
#include <iostream>
#include <latch>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace shin::server;
using Telemetry = RetrievalTelemetry;
using Operation = RetrievalOperation;
using Selector = RetrievalSelector;
using Phase = RetrievalPhase;

static_assert(!std::is_copy_constructible_v<Telemetry::Attempt>);
static_assert(!std::is_copy_assignable_v<Telemetry::Attempt>);
static_assert(noexcept(std::declval<Telemetry&>().begin(Operation::search, Selector::route)));
static_assert(noexcept(std::declval<Telemetry::Attempt&>().mark(Phase::execution)));
static_assert(noexcept(std::declval<Telemetry&>().finish(
    std::declval<Telemetry::Attempt&>(), std::declval<std::string_view>(), std::declval<std::string_view>(),
    std::declval<const Json&>(), nullptr, 500, std::declval<std::string_view>())));

constexpr std::array<const char*, 3> kOperations{"search", "brief", "pack"};
constexpr std::array<const char*, 2> kSelectors{"route", "publication"};
constexpr std::array<const char*, 5> kPhases{"validation", "selection", "admission", "execution", "finalization"};
constexpr std::size_t kPayloadLimit = 2U * 1024U * 1024U;
constexpr std::size_t kRecordLimit = 12U * 1024U;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void expect_error(Function&& action, int status, std::string_view code) {
    try { action(); }
    catch (const ServiceError& error) {
        check(error.status == status && error.code == code, "unexpected telemetry error status/code");
        return;
    }
    throw std::runtime_error("expected telemetry ServiceError");
}

Telemetry::Clock::time_point at(std::uint64_t ns) {
    return Telemetry::Clock::time_point{} + std::chrono::nanoseconds(ns);
}

Json result_fixture(Json evidence = Json::object()) {
    return {{"route_id", "actual-route"}, {"workspace_id", "actual-workspace"},
            {"source_host_id", "actual-source-host"}, {"snapshot_id", "actual-snapshot"},
            {"preparation_id", "actual-preparation"}, {"revision", 7}, {"evidence", std::move(evidence)}};
}

Json null_quality() {
    return {{"retrieval_mode", nullptr}, {"returned_records", nullptr}, {"candidate_rows", nullptr},
            {"covered_groups", nullptr}, {"total_groups", nullptr}, {"pack_complete", nullptr},
            {"pack_coverage_complete", nullptr}, {"required_requirements", nullptr},
            {"satisfied_required_requirements", nullptr}};
}

const Json& cell(const Json& metrics, std::size_t operation = 0, std::size_t selector = 0) {
    for (const auto& value : metrics.at("cells"))
        if (value.at("operation") == kOperations.at(operation) &&
            value.at("selector_kind") == kSelectors.at(selector)) return value;
    throw std::runtime_error("missing operation/selector cell");
}

void finish_one(Telemetry& telemetry, const Json& request = Json::object(),
                const Json* result = nullptr, bool validated = true,
                std::string_view project = "project", int status = 200,
                std::uint64_t duration = 15) {
    auto attempt = telemetry.begin(Operation::search, Selector::route, at(100));
    attempt.request_validated = validated;
    telemetry.finish(attempt, project, "requested-route", request, result, status,
                     status == 200 ? "" : "retrieval-failed", at(100 + duration));
}

Json history(const Telemetry& telemetry) {
    Json items = Json::array();
    std::uint64_t after = 0;
    const auto instance = telemetry.metrics().at("instance_id").get<std::string>();
    for (;;) {
        const auto page = telemetry.requests(after, 32, instance);
        for (const auto& item : page.at("items")) items.push_back(item);
        if (page.at("next_after").is_null()) return items;
        const auto next = page.at("next_after").get<std::uint64_t>();
        check(next > after, "diagnostic cursor did not advance");
        after = next;
    }
}

void check_payload(const Telemetry& telemetry) {
    const auto items = history(telemetry);
    std::size_t bytes = 0;
    for (const auto& item : items) {
        const auto size = item.dump().size();
        check(size <= kRecordLimit, "encoded diagnostic record exceeds 12KiB");
        bytes += size;
    }
    const auto retention = telemetry.metrics().at("retention");
    check(bytes == retention.at("retained_payload_bytes"), "retained encoded byte accounting is not exact");
    check(items.size() == retention.at("retained_records") && items.size() <= 256 &&
              bytes <= kPayloadLimit, "diagnostic history escaped global bounds");
}

void six_cells_and_exactly_once() {
    Telemetry telemetry(ObservabilityConfig{});
    const auto initial = telemetry.metrics();
    check(initial.at("schema_version") == "shin.retrieval-metrics.v1" &&
              initial.at("scope") == "process-lifetime-authorized-service-attempts" &&
              initial.at("cells").size() == 6, "metrics identity or fixed cell count changed");
    std::set<std::pair<std::string, std::string>> dimensions;
    for (std::size_t selector = 0; selector < 2; ++selector) {
        for (std::size_t operation = 0; operation < 3; ++operation) {
            const auto& empty = cell(initial, operation, selector);
            dimensions.emplace(empty.at("operation"), empty.at("selector_kind"));
            for (const auto* key : {"started", "completed", "active", "succeeded", "failed",
                                    "elapsed_total_ns", "elapsed_max_ns"})
                check(empty.at(key) == 0, "new metric cell is not zero");
            auto success = telemetry.begin(static_cast<Operation>(operation), static_cast<Selector>(selector), at(100));
            auto failure = telemetry.begin(static_cast<Operation>(operation), static_cast<Selector>(selector), at(200));
            const auto active = telemetry.metrics();
            check(cell(active, operation, selector).at("started") == 2 &&
                      cell(active, operation, selector).at("active") == 2 &&
                      cell(active, operation, selector).at("completed") == 0,
                  "begin did not immediately count active attempts");
            const auto result = result_fixture();
            telemetry.finish(success, "project", "selector", Json::object(), &result, 200, "", at(110));
            telemetry.finish(failure, "project", "selector", Json::object(), nullptr, 503, "cache-busy", at(220));
            const auto done = telemetry.metrics();
            const auto& value = cell(done, operation, selector);
            check(value.at("started") == 2 && value.at("completed") == 2 && value.at("active") == 0 &&
                      value.at("succeeded") == 1 && value.at("failed") == 1 &&
                      value.at("elapsed_total_ns") == 30 && value.at("elapsed_max_ns") == 20,
                  "completion counters or elapsed times are incorrect");
            const auto records = history(telemetry);
            telemetry.finish(success, "other", "other", Json::object(), nullptr, 500, "changed", at(900));
            telemetry.finish(failure, "other", "other", Json::object(), &result, 200, "", at(900));
            check(telemetry.metrics() == done && history(telemetry) == records,
                  "repeated finish changed counters, duration or diagnostic history");
        }
    }
    check(dimensions.size() == 6 && history(telemetry).size() == 12, "cell identity or completion count mismatch");
    const auto before_reads = telemetry.metrics();
    static_cast<void>(telemetry.requests(0, 1, ""));
    static_cast<void>(telemetry.metrics());
    check(telemetry.metrics() == before_reads, "diagnostic reads changed retrieval measurements");
}

void measured_phase_totals() {
    Telemetry telemetry(ObservabilityConfig{});
    auto attempt = telemetry.begin(Operation::brief, Selector::publication, at(100));
    attempt.mark(Phase::selection, at(103));
    attempt.mark(Phase::admission, at(108));
    attempt.mark(Phase::execution, at(115));
    attempt.mark(Phase::finalization, at(126));
    attempt.result_bytes = 1234;
    const auto result = result_fixture();
    telemetry.finish(attempt, "project", "publication", Json::object(), &result, 200, "", at(139));
    const auto first = history(telemetry).at(0);
    const std::array<std::uint64_t, 5> expected{3, 5, 7, 11, 13};
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        check(first.at("phase_ns").at(kPhases[i]) == expected[i], "phase duration mismatch");
        sum += first.at("phase_ns").at(kPhases[i]).get<std::uint64_t>();
    }
    check(first.at("elapsed_ns") == sum && sum == 39 && first.at("result_bytes") == 1234 &&
              first.at("last_phase") == "finalization", "measured phases do not sum to elapsed");
    auto early = telemetry.begin(Operation::search, Selector::route, at(200));
    early.mark(Phase::selection, at(200));
    telemetry.finish(early, "project", "route", Json::object(), nullptr, 409, "selection-failed", at(207));
    const auto second = history(telemetry).at(1);
    check(second.at("phase_ns") == Json{{"validation", 0}, {"selection", 7}, {"admission", nullptr},
                                            {"execution", nullptr}, {"finalization", nullptr}} &&
              second.at("elapsed_ns") == 7 && second.at("last_phase") == "selection" &&
              second.at("result_bytes").is_null(), "unmeasured phases were fabricated");
}

void histogram_boundaries() {
    Telemetry telemetry(ObservabilityConfig{});
    constexpr std::array<std::uint64_t, 10> bounds{1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000};
    std::array<std::uint64_t, 11> expected{};
    std::uint64_t total = 0;
    finish_one(telemetry, Json::object(), nullptr, true, "project", 200, 0);
    ++expected[0];
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        for (const auto duration : {bounds[i] * 1000000 - 1, bounds[i] * 1000000, bounds[i] * 1000000 + 1}) {
            finish_one(telemetry, Json::object(), nullptr, true, "project", 200, duration);
            total += duration;
        }
        expected[i] += 2;
        ++expected[i + 1];
    }
    const auto metrics = telemetry.metrics();
    const auto& value = cell(metrics);
    const auto& histogram = value.at("histogram");
    check(histogram.size() == 11, "histogram lacks exact fixed boundaries and overflow");
    for (std::size_t i = 0; i < histogram.size(); ++i) {
        check(histogram[i].at("count") == expected[i], "histogram boundary is not upper-inclusive/non-cumulative");
        check(histogram[i].at("upper_bound_ms") == (i < bounds.size() ? Json(bounds[i]) : Json(nullptr)),
              "histogram upper bound mismatch");
    }
    check(value.at("completed") == 31 && value.at("elapsed_total_ns") == total &&
              value.at("elapsed_max_ns") == 5000000001ULL && expected.back() == 1,
          "histogram total/max or overflow mismatch");
}

void quality_and_selection() {
    Telemetry telemetry(ObservabilityConfig{});
    finish_one(telemetry, Json::object(), nullptr, false, "project", 503);
    const auto failure = history(telemetry).at(0);
    check(failure.at("quality") == null_quality() && failure.at("selection").is_null(),
          "failure fabricated quality or selection provenance");
    const auto empty_result = result_fixture();
    finish_one(telemetry, Json::object(), &empty_result);
    check(history(telemetry).at(1).at("quality") == null_quality(), "missing quality is not null");
    auto result = result_fixture({{"retrieval_mode", "strict"}, {"evidence", Json::array({1, 2})},
        {"query_diagnostics", {{"candidate_rows", 17}}},
        {"query_coverage", {{"covered_groups", 2}, {"total_groups", 3}}}});
    const auto original = result.dump();
    finish_one(telemetry, Json::object(), &result);
    const auto search = history(telemetry).at(2);
    auto expected = null_quality();
    expected.update({{"retrieval_mode", "strict"}, {"returned_records", 2}, {"candidate_rows", 17},
                     {"covered_groups", 2}, {"total_groups", 3}});
    auto selection = result;
    selection.erase("evidence");
    check(search.at("quality") == expected && search.at("selection") == selection && result.dump() == original,
          "returned primitive quality/actual selection was not preserved, or result was modified");
    auto brief = result_fixture({{"retrieval_mode", "owner-scoped"}, {"sections", Json::array({1, 2})},
                                {"facts", Json::array({3})}});
    auto brief_attempt = telemetry.begin(Operation::brief, Selector::route, at(0));
    telemetry.finish(brief_attempt, "project", "route", Json::object(), &brief, 200, "", at(1));
    expected = null_quality();
    expected.update({{"retrieval_mode", "owner-scoped"}, {"returned_records", 3}});
    check(history(telemetry).at(3).at("quality") == expected, "Brief section/fact count is incorrect");
    auto pack = result_fixture({{"retrieval_mode", "planned"}, {"evidence", Json::array({1})},
        {"completion", {{"complete", false}, {"coverage_complete", false}, {"required_requirements", 3},
                         {"satisfied_required_requirements", 1}}}});
    auto pack_attempt = telemetry.begin(Operation::pack, Selector::publication, at(0));
    telemetry.finish(pack_attempt, "project", "publication", Json::object(), &pack, 200, "", at(1));
    expected = null_quality();
    expected.update({{"retrieval_mode", "planned"}, {"returned_records", 1}, {"pack_complete", false},
                     {"pack_coverage_complete", false}, {"required_requirements", 3},
                     {"satisfied_required_requirements", 1}});
    const auto incomplete = history(telemetry).at(4);
    const auto metrics = telemetry.metrics();
    check(incomplete.at("quality") == expected && incomplete.at("outcome") == "success" &&
              incomplete.at("reason_code").is_null() && cell(metrics, 2, 1).at("succeeded") == 1 &&
              cell(metrics, 2, 1).at("failed") == 0, "incomplete Pack was counted as a service failure");
    for (const auto& invalid : Json::array({nullptr, -1, 1.5, "sensitive-quality-marker", true, Json::object()})) {
        auto malformed = result_fixture({{"retrieval_mode", invalid}, {"evidence", "source-snippet-marker"},
            {"query_diagnostics", {{"candidate_rows", invalid}}},
            {"query_coverage", {{"covered_groups", invalid}, {"total_groups", invalid}}}});
        finish_one(telemetry, Json::object(), &malformed);
        check(history(telemetry).back().at("quality") == null_quality(), "non-integer/unknown quality leaked through");
    }
    auto wrong_completion = result_fixture({{"completion", {{"complete", "false"}, {"coverage_complete", 1},
        {"required_requirements", -1}, {"satisfied_required_requirements", 0.5}}}});
    finish_one(telemetry, Json::object(), &wrong_completion);
    check(history(telemetry).back().at("quality") == null_quality(), "completion accepted non-primitive/wrong-type values");
    auto zeros = result_fixture({{"retrieval_mode", "relaxed"}, {"evidence", Json::array()},
        {"query_diagnostics", {{"candidate_rows", 0}}}, {"query_coverage", {{"covered_groups", 0}, {"total_groups", 0}}},
        {"completion", {{"complete", true}, {"coverage_complete", true}, {"required_requirements", 0},
                         {"satisfied_required_requirements", 0}}}});
    finish_one(telemetry, Json::object(), &zeros);
    check(history(telemetry).back().at("quality") == Json{{"retrieval_mode", "relaxed"}, {"returned_records", 0},
        {"candidate_rows", 0}, {"covered_groups", 0}, {"total_groups", 0}, {"pack_complete", true},
        {"pack_coverage_complete", true}, {"required_requirements", 0}, {"satisfied_required_requirements", 0}},
        "known zero/true primitives were mistaken for unavailable quality");
    for (const auto* mode : {"directory-scoped", "identifier-scoped"}) {
        const auto scoped = result_fixture({{"retrieval_mode", mode}});
        finish_one(telemetry, Json::object(), &scoped);
        expected = null_quality();
        expected["retrieval_mode"] = mode;
        check(history(telemetry).back().at("quality") == expected, "scoped retrieval mode was lost");
        check(telemetry.journal_snapshot().at("records").back().at("quality") == expected,
              "journal snapshot discarded the observed scoped mode");
    }
}

void metadata_privacy_and_capture() {
    const Json request{{"query", "private-query-marker"}};
    const auto result = result_fixture({{"evidence", Json::array({{{"snippet", "private-snippet-marker"}}})},
        {"query", "private-query-marker"}, {"retrieval_mode", "private-mode-marker"}});
    Telemetry metadata(ObservabilityConfig{});
    finish_one(metadata, request, &result);
    const auto metadata_record = history(metadata).at(0);
    check(metadata_record.at("request").is_null() && metadata_record.at("query_capture") == "disabled",
          "default policy captured input");
    const auto output = metadata.metrics().dump() + history(metadata).dump();
    for (const auto* marker : {"private-query-marker", "private-snippet-marker", "private-mode-marker"})
        check(output.find(marker) == std::string::npos, "metadata diagnostics leaked query/source content");
    const auto metric_text = metadata.metrics().dump();
    for (const auto* marker : {"requested-route", "actual-route", "actual-workspace", "project_id", "query", "snapshot_id"})
        check(metric_text.find(marker) == std::string::npos, "metrics acquired request/project/path dimensions");

    ObservabilityConfig config;
    config.capture_query_projects = {"project"};
    config.max_query_bytes = request.dump().size();
    Telemetry capture(config);
    finish_one(capture, request, &result, true, "project-other");
    finish_one(capture, request, &result, true, "Project");
    finish_one(capture, request, nullptr, false, "project", 400);
    finish_one(capture, request, &result);
    finish_one(capture, request, nullptr, true, "project", 503);
    const auto records = history(capture);
    for (std::size_t i = 0; i < 3; ++i)
        check(records[i].at("request").is_null() &&
                  records[i].at("query_capture") == (i == 2 ? "invalid-request" : "disabled"),
              "capture ignored exact project allowlist or request validation");
    for (const auto i : {3, 4})
        check(records[i].at("request") == request && records[i].at("query_capture") == "captured",
              "validated allowlisted request at the exact byte limit was not retained");
    check(records.dump().find("private-snippet-marker") == std::string::npos,
          "request capture included response source fragments");
    auto oversize = request;
    oversize["query"] = request.at("query").get<std::string>() + "x";
    check(oversize.dump().size() == config.max_query_bytes + 1, "oversize fixture is not boundary+1");
    finish_one(capture, oversize, &result);
    const auto omitted = history(capture).back();
    check(omitted.at("request").is_null() && omitted.at("query_capture") == "omitted-budget" &&
              omitted.at("outcome") == "success", "oversize input was truncated or changed the outcome");
    const Json escaped{{"query", std::string(12, '\n')}};
    check(escaped.at("query").get<std::string>().size() < config.max_query_bytes &&
              escaped.dump().size() > config.max_query_bytes, "escaped fixture does not exercise encoded byte budget");
    finish_one(capture, escaped, &result);
    check(history(capture).back().at("query_capture") == "omitted-budget" &&
              history(capture).back().at("request").is_null(), "capture budget used raw rather than encoded bytes");
}

void collection_failures_preserve_outcome() {
    ObservabilityConfig config;
    config.capture_query_projects = {"project"};
    Telemetry telemetry(config);
    const Json invalid_utf8{{"query", std::string(1, static_cast<char>(0xff))}};
    const auto result = result_fixture();
    finish_one(telemetry, invalid_utf8, &result);
    const auto failed_capture = history(telemetry).at(0);
    check(failed_capture.at("query_capture") == "capture-failed" && failed_capture.at("request").is_null() &&
              failed_capture.at("outcome") == "success", "capture serialization failure escaped outcome isolation");
    const auto malformed_result = Json::object();
    finish_one(telemetry, Json::object(), &malformed_result);
    auto oversized_result = result;
    oversized_result["route_id"] = std::string(kRecordLimit, 'r');
    finish_one(telemetry, Json::object(), &oversized_result);
    auto failure = telemetry.begin(Operation::search, Selector::route, at(0));
    telemetry.finish(failure, "project", "route", Json::object(), nullptr, 503,
                     "Exception contains private-query-marker / credential", at(1));
    const auto metrics = telemetry.metrics();
    check(cell(metrics).at("completed") == 4 && cell(metrics).at("succeeded") == 3 &&
              cell(metrics).at("failed") == 1 && cell(metrics).at("active") == 0,
          "dropped diagnostic changed service counters");
    check(metrics.at("collection_errors") == 1 && metrics.at("retention").at("dropped") == 2 &&
              metrics.at("retention").at("latest_sequence") == 4 && history(telemetry).size() == 2,
          "diagnostic error/drop accounting is not exact");
    check(history(telemetry).back().at("reason_code") == "unclassified-error" &&
              history(telemetry).dump().find("private-query-marker") == std::string::npos,
          "failure diagnostics retained exception text");
    check_payload(telemetry);
}

void count_and_payload_eviction() {
    ObservabilityConfig config;
    config.max_retained_requests = 256;
    Telemetry count_limited(config);
    for (std::size_t i = 0; i < 260; ++i) finish_one(count_limited);
    const auto items = history(count_limited);
    const auto retention = count_limited.metrics().at("retention");
    check(items.size() == 256 && items.front().at("sequence") == 5 && items.back().at("sequence") == 260 &&
              retention.at("evicted") == 4 && retention.at("dropped") == 0 &&
              retention.at("maximum_records") == 256 && retention.at("maximum_payload_bytes") == kPayloadLimit,
          "global count retention did not evict the oldest records exactly");
    check_payload(count_limited);

    config.max_query_bytes = 8192;
    config.capture_query_projects = {"project"};
    Telemetry byte_limited(config);
    Json request{{"query", ""}};
    request["query"] = std::string(config.max_query_bytes - request.dump().size(), 'x');
    check(request.dump().size() == 8192, "payload fixture is not the maximum encoded request");
    const auto result = result_fixture();
    const auto instance = byte_limited.metrics().at("instance_id").get<std::string>();
    std::vector<std::size_t> sizes;
    for (std::size_t i = 0; i < 256; ++i) {
        finish_one(byte_limited, request, &result);
        const auto page = byte_limited.requests(i, 1, instance);
        check(page.at("items").size() == 1 && page.at("items")[0].at("query_capture") == "captured",
              "large but valid request was dropped instead of retained");
        sizes.push_back(page.at("items")[0].dump().size());
    }
    // Independently compute the maximal suffix that fits the encoded payload budget.
    std::size_t expected_bytes = 0;
    std::size_t expected_count = 0;
    for (auto it = sizes.rbegin(); it != sizes.rend() && expected_bytes + *it <= kPayloadLimit; ++it) {
        expected_bytes += *it;
        ++expected_count;
    }
    const auto bytes = byte_limited.metrics().at("retention");
    const auto retained = history(byte_limited);
    check(expected_count > 0 && expected_count < 256 && retained.size() == expected_count &&
              bytes.at("retained_payload_bytes") == expected_bytes && bytes.at("evicted") == 256 - expected_count &&
              bytes.at("dropped") == 0 && retained.front().at("sequence") == 257 - expected_count &&
              retained.back().at("sequence") == 256, "2MiB retention did not preserve exactly the newest fitting suffix");
    check_payload(byte_limited);
}

void cursor_and_disabled_retention() {
    ObservabilityConfig config;
    config.max_retained_requests = 3;
    Telemetry telemetry(config);
    const auto empty = telemetry.requests(0, 1, "");
    check(empty.at("schema_version") == "shin.retrieval-request-page.v1" && empty.at("items").empty() &&
              empty.at("oldest_available_sequence").is_null() && empty.at("latest_sequence") == 0 &&
              empty.at("next_after").is_null() && empty.at("gap") == false, "initial empty cursor is incorrect");
    const auto instance = empty.at("instance_id").get<std::string>();
    for (int i = 0; i < 5; ++i) finish_one(telemetry);
    const auto first = telemetry.requests(0, 1, "");
    check(first.at("gap") == true && first.at("oldest_available_sequence") == 3 &&
              first.at("latest_sequence") == 5 && first.at("next_after") == 3 &&
              first.at("items").size() == 1 && first.at("items")[0].at("sequence") == 3,
          "evicted prefix is not reported as an explicit cursor gap");
    const auto rest = telemetry.requests(3, 32, instance);
    check(rest.at("gap") == false && rest.at("items").size() == 2 && rest.at("items")[0].at("sequence") == 4 &&
              rest.at("items")[1].at("sequence") == 5 && rest.at("next_after").is_null(), "cursor continuation lost/order-duplicated records");
    check(telemetry.requests(2, 32, instance).at("gap") == false, "contiguous oldest record reported as a gap");
    const auto latest = telemetry.requests(5, 32, instance);
    check(latest.at("items").empty() && latest.at("gap") == false && latest.at("next_after").is_null() &&
              latest.at("latest_sequence") == 5, "latest cursor did not return an empty complete page");
    expect_error([&] { telemetry.requests(0, 0, ""); }, 400, "invalid-query");
    expect_error([&] { telemetry.requests(0, 33, ""); }, 400, "invalid-query");
    expect_error([&] { telemetry.requests(6, 1, instance); }, 400, "invalid-query");
    expect_error([&] { telemetry.requests(1, 1, ""); }, 409, "telemetry-instance-mismatch");
    expect_error([&] { telemetry.requests(0, 1, "different-instance"); }, 409, "telemetry-instance-mismatch");
    Telemetry restarted(config);
    check(restarted.metrics().at("instance_id") != instance, "restart reused process-local instance identity");
    expect_error([&] { restarted.requests(3, 1, instance); }, 409, "telemetry-instance-mismatch");

    config.max_retained_requests = 0;
    Telemetry disabled(config);
    finish_one(disabled);
    finish_one(disabled, Json::object(), nullptr, false, "project", 503);
    const auto metrics = disabled.metrics();
    const auto page = disabled.requests(0, 32, "");
    check(cell(metrics).at("completed") == 2 && cell(metrics).at("succeeded") == 1 && cell(metrics).at("failed") == 1 &&
              metrics.at("retention").at("disabled") == 2 && metrics.at("retention").at("evicted") == 0 &&
              metrics.at("retention").at("dropped") == 0 && page.at("items").empty() &&
              page.at("latest_sequence") == 2 && page.at("oldest_available_sequence").is_null() &&
              page.at("gap") == true && page.at("next_after").is_null(), "zero retention lost metrics or concealed missing history");
    check_payload(disabled);
}

void dropped_record_cursor_gaps() {
    Telemetry telemetry(ObservabilityConfig{});
    const auto valid = result_fixture();
    const auto malformed = Json::object();
    const auto instance = telemetry.metrics().at("instance_id").get<std::string>();
    finish_one(telemetry, Json::object(), &valid);
    finish_one(telemetry, Json::object(), &malformed);
    const auto tail = telemetry.requests(0, 32, "");
    check(tail.at("items").size() == 1 && tail.at("items")[0].at("sequence") == 1 &&
              tail.at("latest_sequence") == 2 && tail.at("gap") == true && tail.at("next_after").is_null(),
          "dropped tail after a normal record was concealed");
    const auto missing_tail = telemetry.requests(1, 1, instance);
    check(missing_tail.at("items").empty() && missing_tail.at("gap") == true &&
              missing_tail.at("latest_sequence") == 2 && missing_tail.at("next_after").is_null(),
          "empty page after the last retained record concealed a dropped tail");
    check(telemetry.requests(2, 1, instance).at("gap") == false,
          "acknowledged dropped tail was reported as a new gap");
    finish_one(telemetry, Json::object(), &valid);
    const auto interior = telemetry.requests(0, 32, "");
    check(interior.at("items").size() == 2 && interior.at("items")[0].at("sequence") == 1 &&
              interior.at("items")[1].at("sequence") == 3 && interior.at("gap") == true &&
              interior.at("oldest_available_sequence") == 1 && interior.at("latest_sequence") == 3 &&
              interior.at("next_after").is_null(), "dropped interior record was concealed by an intact prefix");
    const auto prefix = telemetry.requests(0, 1, "");
    check(prefix.at("gap") == false && prefix.at("next_after") == 1,
          "gap outside the limited page contaminated its contiguous prefix");
    const auto continuation = telemetry.requests(1, 1, instance);
    check(continuation.at("gap") == true && continuation.at("items").size() == 1 &&
              continuation.at("items")[0].at("sequence") == 3 && continuation.at("next_after").is_null(),
          "cursor continuation across a dropped interior record lost the gap");
    const auto metrics = telemetry.metrics();
    check(cell(metrics).at("started") == 3 && cell(metrics).at("completed") == 3 &&
              cell(metrics).at("succeeded") == 3 && cell(metrics).at("failed") == 0 && cell(metrics).at("active") == 0 &&
              metrics.at("retention").at("dropped") == 1 && metrics.at("retention").at("evicted") == 0 &&
              metrics.at("collection_errors") == 1,
          "malformed success projection changed service success accounting");
    Telemetry dropped_first(ObservabilityConfig{});
    finish_one(dropped_first, Json::object(), &malformed);
    const auto only_drop = dropped_first.requests(0, 32, "");
    check(only_drop.at("items").empty() && only_drop.at("gap") == true &&
              only_drop.at("latest_sequence") == 1 && only_drop.at("oldest_available_sequence").is_null(),
          "entirely dropped history was presented as complete");
    finish_one(dropped_first, Json::object(), &valid);
    const auto after_drop = dropped_first.requests(0, 32, "");
    check(after_drop.at("gap") == true && after_drop.at("items").size() == 1 &&
              after_drop.at("items")[0].at("sequence") == 2, "normal record after a dropped prefix concealed the gap");
}

void collector_config_bounds() {
    auto invalid = [](ObservabilityConfig config) {
        expect_error([&] { Telemetry telemetry(config); }, 500, "invalid-config");
    };
    ObservabilityConfig config;
    config.max_retained_requests = 257;
    invalid(config);
    config = ObservabilityConfig{};
    config.max_query_bytes = 0;
    invalid(config);
    config.max_query_bytes = 8193;
    invalid(config);
    config = ObservabilityConfig{};
    config.max_retained_requests = 0;
    config.capture_query_projects = {"project"};
    invalid(config);
    config = ObservabilityConfig{};
    for (int i = 0; i < 33; ++i) config.capture_query_projects.push_back("project-" + std::to_string(i));
    invalid(config);
    config.capture_query_projects.pop_back();
    config.max_query_bytes = 1;
    Telemetry accepted(config);
    finish_one(accepted);
    check(cell(accepted.metrics()).at("completed") == 1, "valid minimum query budget / maximum allowlist rejected");
}

void concurrent_exact_counts() {
    constexpr std::size_t iterations = 192;
    for (const std::size_t thread_count : {1, 4, 8}) {
        ObservabilityConfig config;
        config.max_retained_requests = 37;
        Telemetry telemetry(config);
        const auto result = result_fixture();
        std::latch ready(static_cast<std::ptrdiff_t>(thread_count));
        std::latch release(1);
        std::vector<std::exception_ptr> errors(thread_count);
        std::vector<std::thread> workers;
        workers.reserve(thread_count);
        for (std::size_t worker = 0; worker < thread_count; ++worker) {
            workers.emplace_back([&, worker] {
                try {
                    for (std::size_t i = 0; i < iterations; ++i) {
                        const auto slot = i % 6;
                        auto attempt = telemetry.begin(static_cast<Operation>(slot % 3), static_cast<Selector>(slot / 3), at(100));
                        if (i == 0) { ready.count_down(); release.wait(); }
                        attempt.mark(Phase::selection, at(101));
                        attempt.mark(Phase::admission, at(103));
                        attempt.mark(Phase::execution, at(106));
                        attempt.mark(Phase::finalization, at(110));
                        const bool success = i % 12 < 6;
                        telemetry.finish(attempt, "project", "route", Json::object(), success ? &result : nullptr,
                                         success ? 200 : 503, "cache-busy", at(115));
                        telemetry.finish(attempt, "project", "route", Json::object(), nullptr, 500, "duplicate", at(999));
                        if (i % 16 == 0) {
                            const auto snapshot = telemetry.metrics();
                            for (const auto& value : snapshot.at("cells"))
                                check(value.at("started").get<std::uint64_t>() ==
                                          value.at("completed").get<std::uint64_t>() + value.at("active").get<std::uint64_t>(),
                                      "concurrent read saw inconsistent started/completed/active counters");
                            const auto page = telemetry.requests(0, 32, "");
                            check(page.at("items").size() <= 32, "concurrent diagnostic read exceeded page bound");
                            const auto journal = telemetry.journal_snapshot();
                            const auto& records = journal.at("records");
                            const auto through = journal.at("latest_sequence").get<std::uint64_t>();
                            check(records.size() <= 37 && through >= records.size(), "journal snapshot exceeded its prefix bounds");
                            for (std::size_t record = 0; record < records.size(); ++record)
                                check(records[record].at("sequence") == through - records.size() + record + 1,
                                      "journal snapshot mixed a counter and records from different observations");
                        }
                    }
                } catch (...) { errors[worker] = std::current_exception(); }
            });
        }
        ready.wait();
        const auto in_flight = telemetry.metrics();
        release.count_down();
        for (auto& worker : workers) worker.join();
        for (const auto& error : errors) if (error) std::rethrow_exception(error);
        check(cell(in_flight).at("active") == thread_count && cell(in_flight).at("started") == thread_count &&
                  cell(in_flight).at("completed") == 0, "synchronized concurrent begin count is incorrect");
        const auto metrics = telemetry.metrics();
        const auto per_cell = thread_count * iterations / 6;
        for (const auto& value : metrics.at("cells")) {
            check(value.at("started") == per_cell && value.at("completed") == per_cell && value.at("active") == 0 &&
                      value.at("succeeded") == per_cell / 2 && value.at("failed") == per_cell / 2 &&
                      value.at("elapsed_total_ns") == 15 * per_cell && value.at("elapsed_max_ns") == 15 &&
                      value.at("histogram")[0].at("count") == per_cell, "concurrent completion counts/durations are not exact");
            for (std::size_t i = 1; i < 11; ++i)
                check(value.at("histogram")[i].at("count") == 0, "concurrent histogram incremented an unmeasured bucket");
        }
        const auto completed = thread_count * iterations;
        const auto retained = history(telemetry);
        check(retained.size() == 37 && metrics.at("retention").at("evicted") == completed - 37 &&
                  metrics.at("retention").at("dropped") == 0 && metrics.at("collection_errors") == 0,
              "concurrent bounded retention or diagnostic counts mismatch");
        for (std::size_t i = 0; i < retained.size(); ++i) {
            check(retained[i].at("sequence") == completed - 36 + i, "concurrent sequence is not a unique monotonic suffix");
            std::uint64_t sum = 0;
            for (const auto& value : retained[i].at("phase_ns")) sum += value.get<std::uint64_t>();
            check(sum == 15 && retained[i].at("elapsed_ns") == sum, "concurrent phase sums differ from elapsed");
        }
        check_payload(telemetry);
    }
}
void journal_suffix_snapshot() {
    ObservabilityConfig policy;
    policy.max_retained_requests = 3;
    Telemetry telemetry(policy);
    for (int i = 0; i < 5; ++i) finish_one(telemetry);
    const auto before = telemetry.metrics();
    const auto all = telemetry.journal_snapshot();
    const auto suffix = telemetry.journal_snapshot(3);
    check(all.at("latest_sequence") == 5 && all.at("records").size() == 3,
          "default journal snapshot lost the retained suffix");
    check(suffix.at("latest_sequence") == 5 && suffix.at("records").size() == 2 &&
              suffix.at("records")[0].at("sequence") == 4 && suffix.at("records")[1].at("sequence") == 5,
          "journal cursor did not select only later observations");
    const auto idle = telemetry.journal_snapshot(5);
    check(idle.at("latest_sequence") == 5 && idle.at("records").empty() && idle.at("instance_id") == all.at("instance_id"),
          "idle journal snapshot copied acknowledged data or lost progress identity");
    check(telemetry.metrics() == before, "journal suffix reads changed collector state");
}
} // namespace

int main() {
    try {
        six_cells_and_exactly_once();
        measured_phase_totals();
        histogram_boundaries();
        quality_and_selection();
        metadata_privacy_and_capture();
        collection_failures_preserve_outcome();
        count_and_payload_eviction();
        cursor_and_disabled_retention();
        dropped_record_cursor_gaps();
        collector_config_bounds();
        concurrent_exact_counts();
        journal_suffix_snapshot();
        std::cout << "retrieval telemetry: 12 deterministic native test groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
