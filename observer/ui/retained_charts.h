#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shin::ui {

enum class HistoryMetric { memory, requests, search_time };

struct HistoryPoint {
    double at = 0;
    std::optional<double> value;
    bool connect = false;
};

struct RetainedCharts {
    double begin = 0, end = 0, interval = 0;
    bool available = false, partial = false;
    std::uint64_t requests = 0;
    std::optional<double> mean_search_ms;
    std::array<std::optional<double>, 3> peaks{};
    std::array<std::vector<HistoryPoint>, 3> series;
};

inline std::optional<double> telemetry_measurement(const nlohmann::json& value) {
    if (!value.is_number()) return {};
    const auto measurement = value.get<double>();
    return std::isfinite(measurement) && measurement >= 0 && measurement <= 1e15
        ? std::optional<double>(measurement) : std::nullopt;
}

inline std::optional<double> telemetry_count(const nlohmann::json& value) {
    const auto count = telemetry_measurement(value);
    return count && std::floor(*count) == *count ? count : std::nullopt;
}

// The rendering surfaces select their own period and scale. Bucket identity,
// missing measurements and run continuity belong to the retained history.
inline RetainedCharts retained_charts(const nlohmann::json& history, int hours) {
    RetainedCharts result;
    if (hours <= 0 || !history.is_object() || history.value("status", nlohmann::json()) != "ready") return result;
    const auto captured = telemetry_measurement(history.value("captured_at_ms", nlohmann::json()));
    if (!captured) return result;
    const bool minutes = hours == 1;
    result.end = *captured;
    result.begin = std::max(0.0, result.end - static_cast<double>(hours) * 3600000);
    result.interval = minutes ? 60000.0 : 3600000.0;
    if (minutes) {
        if (const auto oldest = telemetry_measurement(history.value("oldest_at_ms", nlohmann::json())))
            result.begin = std::min(std::max(0.0, result.end - 60000), std::max(result.begin, *oldest));
    }

    const auto rows = history.find(minutes ? "minute_timeline" : "timeline");
    if (rows == history.end() || rows->is_null()) return result;
    if (!rows->is_array() || rows->size() > (minutes ? 61U : 169U)) {
        result.partial = true;
        return result;
    }
    result.available = true;
    for (auto& series : result.series) series.reserve(rows->size());
    std::array<bool, 3> previous_valid{};
    double previous_at = -1, duration_sum = 0, duration_count = 0;
    std::string_view previous_run;
    for (const auto& row : *rows) {
        const auto at = row.is_array() && row.size() == (minutes ? 11U : 10U)
            ? telemetry_measurement(row[0]) : std::nullopt;
        if (!at || *at > result.end || *at <= previous_at) {
            result.partial = true;
            previous_valid.fill(false);
            continue;
        }
        if (*at + result.interval <= result.begin) continue;
        const auto rss_count = telemetry_count(row[8]);
        auto rss = rss_count && *rss_count > 0 ? telemetry_measurement(row[7]) : std::nullopt;
        if (rss) *rss /= 1048576;
        const auto requests = telemetry_count(row[1]);
        const auto sum = telemetry_measurement(row[4]), count = telemetry_count(row[5]);
        const auto mean = sum && count && *count > 0 ? std::optional<double>(*sum / *count) : std::nullopt;
        if (requests) result.requests += static_cast<std::uint64_t>(*requests);
        else result.partial = true;
        if (mean) { duration_sum += *sum; duration_count += *count; }
        const std::array values{rss, requests, mean};
        const std::string_view run = minutes && row[10].is_string() ?
            std::string_view(row[10].get_ref<const std::string&>()) : std::string_view{};
        const bool adjacent = *at - previous_at <= result.interval * 1.5;
        for (std::size_t metric = 0; metric < values.size(); ++metric) {
            const auto& value = values[metric];
            const bool continuous = metric != 0 || (!run.empty() && run == previous_run);
            result.series[metric].push_back({*at, value, previous_valid[metric] && value && adjacent && continuous});
            if (value) result.peaks[metric] = std::max(result.peaks[metric].value_or(0), *value);
            previous_valid[metric] = value.has_value();
        }
        previous_at = *at;
        previous_run = run;
    }
    if (duration_count > 0) result.mean_search_ms = duration_sum / duration_count;
    return result;
}
} // namespace shin::ui
