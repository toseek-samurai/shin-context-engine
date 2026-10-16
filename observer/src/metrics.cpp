#include "shin/observer/metrics.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace shin::observer {
namespace {
[[noreturn]] void invalid() { throw Error("invalid-metrics", "Stored Observer metrics are inconsistent."); }

void fields(const Json& value, const std::set<std::string>& keys) {
    if (!value.is_object() || value.size() != keys.size() ||
        !std::all_of(keys.begin(), keys.end(), [&](const auto& key) { return value.contains(key); })) invalid();
}

std::uint64_t counter(const Json& value) {
    if (!value.is_number_integer() || value < 0 || value > std::numeric_limits<std::int64_t>::max() - 1) invalid();
    return value.get<std::uint64_t>();
}

double duration(const Json& value) {
    if (!value.is_number()) invalid();
    const auto result = value.get<double>();
    if (!std::isfinite(result) || result < 0) invalid();
    return result;
}
} // namespace

Json new_metrics() {
    return {{"schema_version", "shin.observer-metrics.v1"}, {"scope", "retained-committed-samples"},
        {"sample_count", 0}, {"outcomes", {{"current", 0}, {"stale", 0}, {"unpublished", 0}, {"incomplete", 0}, {"error", 0}}},
        {"scan_duration_ms", {{"last", 0.0}, {"max", 0.0}, {"total", 0.0}}}};
}

void record_metrics(Json& metrics, const Json& observation) {
    auto& outcome = metrics.at("outcomes").at(observation.at("status").get<std::string>());
    outcome = counter(outcome) + 1;
    metrics["sample_count"] = counter(metrics.at("sample_count")) + 1;
    const auto elapsed = duration(observation.at("duration_ms"));
    auto& scan = metrics.at("scan_duration_ms");
    scan["last"] = elapsed;
    scan["max"] = std::max(duration(scan.at("max")), elapsed);
    scan["total"] = duration(scan.at("total")) + elapsed;
}

void verify_metrics(const Json& state) {
    const auto& metrics = state.at("metrics");
    fields(metrics, {"schema_version", "scope", "sample_count", "outcomes", "scan_duration_ms"});
    if (metrics.at("schema_version") != "shin.observer-metrics.v1" || metrics.at("scope") != "retained-committed-samples") invalid();
    const auto samples = counter(metrics.at("sample_count"));
    if (samples != counter(state.at("sequence"))) invalid();
    const auto& outcomes = metrics.at("outcomes");
    fields(outcomes, {"current", "stale", "unpublished", "incomplete", "error"});
    std::uint64_t remaining = samples;
    for (const auto& value : outcomes) {
        const auto count = counter(value);
        if (count > remaining) invalid();
        remaining -= count;
    }
    if (remaining != 0) invalid();
    const auto& scan = metrics.at("scan_duration_ms");
    fields(scan, {"last", "max", "total"});
    const auto last = duration(scan.at("last")), maximum = duration(scan.at("max")), total = duration(scan.at("total"));
    if (last > maximum || maximum > total || (samples == 0 && total != 0)) invalid();
    if (samples != 0 && (state.at("observation").is_null() || last != duration(state.at("observation").at("duration_ms")) ||
        outcomes.at(state.at("observation").at("status").get<std::string>()) == 0)) invalid();
}
} // namespace shin::observer
