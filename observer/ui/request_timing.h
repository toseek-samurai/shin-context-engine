#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace shin::ui {
struct TimingMean {
    double sum = 0;
    std::uint64_t count = 0;
    void add(const nlohmann::json& value, std::uint64_t samples = 1) {
        if (!value.is_number() || samples == 0 || samples > 20000) return;
        const auto ms = value.get<double>();
        if (!std::isfinite(ms) || ms < 0 || ms > 60000) return;
        sum += ms * static_cast<double>(samples);
        count += samples;
    }
    std::optional<double> value() const {
        return count ? std::optional<double>(sum / static_cast<double>(count)) : std::nullopt;
    }
};
struct TimingGroup {
    std::uint64_t requests = 0;
    TimingMean worker, service;
};
struct RequestTiming {
    bool available = false, truncated = false, invalid = false;
    std::uint64_t requests = 0, errors = 0, diagnostic = 0, unclassified = 0;
    TimingMean worker, service;
    std::array<TimingMean, 5> phases;
    std::array<TimingGroup, 3> readers; // warm, cold, unknown
};
inline std::optional<std::uint64_t> timing_count(const nlohmann::json& value) {
    if (!value.is_number()) return {};
    const auto number = value.get<double>();
    if (!std::isfinite(number) || number < 0 || number > 20000 || std::floor(number) != number) return {};
    return static_cast<std::uint64_t>(number);
}
inline RequestTiming request_timing(const nlohmann::json& telemetry, bool retained, int filter) {
    using Json = nlohmann::json;
    RequestTiming result;
    if (!telemetry.is_object()) return result;
    const auto part = telemetry.value(retained ? "history" : "request_metrics", Json::object());
    if (!part.is_object()) return result;
    const auto rows = part.value(retained ? "request_cohorts" : "recent", Json());
    if (!rows.is_array() || rows.size() > (retained ? 48 : 64) ||
        (retained && part.value("status", Json()) != "ready")) return result;
    result.available = true;
    result.truncated = part.value(retained ? "request_cohorts_truncated" : "recent_truncated", Json(false)) == true;
    constexpr std::array phases{"admission", "documents", "source", "source_collection", "final_verification"};
    for (const auto& row : rows) {
        Json worker, service, phase_values = Json::array();
        std::uint64_t count = 1, errors = 0, service_count = 1, worker_count = 1;
        std::array<std::uint64_t, 5> phase_counts{1, 1, 1, 1, 1};
        Json purpose, reader;
        if (retained) {
            if (!row.is_array() || row.size() != 22 || !row[1].is_string()) { result.invalid = true; continue; }
            if (row[1] != "query") continue;
            bool valid = true;
            std::array<std::uint64_t, 9> counts{};
            std::size_t at = 0;
            for (int index : {4, 5, 6, 16, 17, 18, 19, 20, 21}) {
                const auto value = timing_count(row[index]);
                if (!value) { valid = false; break; }
                counts[at++] = *value;
            }
            if (!valid) { result.invalid = true; continue; }
            count = counts[0]; errors = counts[1];
            service_count = counts[2]; worker_count = counts[3];
            valid = errors <= count && service_count <= count && worker_count <= count;
            for (std::size_t i = 0; i < 5; ++i) {
                phase_counts[i] = counts[4 + i];
                valid &= phase_counts[i] <= count;
                phase_values.push_back(row[11 + i]);
            }
            if (!valid) { result.invalid = true; continue; }
            purpose = row[2]; reader = row[3]; worker = row[9]; service = row[7];
        } else {
            if (!row.is_object()) { result.invalid = true; continue; }
            if (row.value("method", Json()) != "query") continue;
            errors = row.value("outcome", Json()) == "response-error" ? 1 : 0;
            purpose = row.value("purpose", Json()); reader = row.value("reader_state", Json());
            worker = row.value("duration_ms", Json()); service = row.value("service_duration_ms", Json());
            const auto values = row.value("phase_ms", Json::object());
            for (const auto* phase : phases) phase_values.push_back(values.is_object() ? values.value(phase, Json()) : Json());
        }
        const bool diagnostic = purpose == "diagnostic";
        if ((filter == 1 && diagnostic) || (filter == 2 && !diagnostic)) continue;
        result.requests += count; result.errors += errors;
        (diagnostic ? result.diagnostic : result.unclassified) += count;
        auto& group = result.readers[reader == "warm" ? 0 : reader == "cold" ? 1 : 2];
        group.requests += count;
        result.worker.add(worker, worker_count); group.worker.add(worker, worker_count);
        result.service.add(service, service_count); group.service.add(service, service_count);
        for (std::size_t i = 0; i < 5; ++i) result.phases[i].add(phase_values[i], phase_counts[i]);
    }
    return result;
}
} // namespace shin::ui
