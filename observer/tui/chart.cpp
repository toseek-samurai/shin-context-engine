#include "chart.h"
#include "../ui/language.h"
#include <algorithm>
#include <cmath>

namespace shin::observer::tui {
namespace {
const Json& field(const Json& value, const char* key) {
    static const Json absent;
    const auto at = value.is_object() ? value.find(key) : value.end();
    return at == value.end() ? absent : *at;
}
std::optional<double> measured(const Json& value) {
    if (!value.is_number()) return {};
    const auto n = value.get<double>();
    return std::isfinite(n) && n >= 0 && n <= 1e15 ? std::optional<double>(n) : std::nullopt;
}
std::optional<double> ratio(const Json& sum, const Json& count) {
    const auto a = measured(sum), b = measured(count);
    return a && b && *b > 0 && std::floor(*b) == *b ? std::optional<double>(*a / *b) : std::nullopt;
}
}
Chart chart(const Snapshot& snapshot, int metric, int range, int traffic) {
    metric = std::clamp(metric, 0, 2); range = std::clamp(range, 0, 2);
    Chart result;
    result.retained = range != 0;
    result.title = metric == 0 ? ui::tr("Memory (service + workers)") :
        metric == 1 ? (range == 0 ? ui::tr("Active queries") : ui::tr("Requests per interval")) : ui::tr("Search time");
    result.unit = metric == 0 ? "MiB" : metric == 2 ? "ms" : range == 1 ? ui::tr("/ min") : range == 2 ? ui::tr("/ h") : ui::tr("queries");
    const auto& telemetry = field(snapshot.service, "telemetry");
    const auto* path = range == 1 ? "/history/minute_timeline" : range == 2 ? "/history/timeline" :
        metric == 2 ? "/request_metrics/recent" : "/samples";
    const auto omitted = measured(field(field(field(telemetry, "projection"), "omitted_rows"), path));
    if (omitted && std::floor(*omitted) == *omitted) {
        result.omitted = static_cast<std::size_t>(*omitted);
        result.partial = result.omitted != 0;
    }
    const auto& owner = range ? field(telemetry, "history") : telemetry;
    if (range && field(owner, "status") != "ready") return result;
    const auto end = measured(field(owner, range ? "captured_at_ms" : "uptime_ms"));
    if (!end) return result;
    result.end = *end;
    const double span = range == 0 ? 60000.0 : range == 1 ? 3600000.0 : 86400000.0;
    result.begin = std::max(0.0, result.end - span);
    if (range == 1) if (const auto oldest = measured(field(owner, "oldest_at_ms")))
        result.begin = std::min(std::max(0.0, result.end - 60000), std::max(result.begin, *oldest));
    const auto& rows = range ? field(owner, range == 1 ? "minute_timeline" : "timeline") :
        metric == 2 ? field(field(telemetry, "request_metrics"), "recent") : field(telemetry, "samples");
    const std::size_t limit = range == 1 ? 61 : range == 2 ? 169 : metric == 2 ? 64 : 120;
    if (!rows.is_array() || rows.size() > limit) { result.partial = result.partial || !rows.is_null(); return result; }
    const double interval = range == 1 ? 60000 : 3600000;
    double previous_at = -1, peak = metric == 0 ? 16 : metric == 1 ? 2 : 1;
    bool previous_valid = false;
    std::string previous_run;
    for (const auto& row : rows) {
        std::optional<double> at, value;
        std::string run;
        bool join = true;
        if (range) {
            if (!row.is_array() || row.size() != (range == 1 ? 11 : 10)) {
                result.partial = true; previous_valid = false; continue;
            }
            at = measured(row[0]);
            if (metric == 0) {
                const auto count = measured(row[8]);
                if (count && *count > 0 && std::floor(*count) == *count) value = measured(row[7]);
                if (value) *value /= 1048576;
                // Hourly memory buckets lack run identity; never connect
                // observations across an unknown service restart.
                if (range == 1 && row[10].is_string()) run = row[10].get<std::string>();
                join = !run.empty() && run == previous_run;
            } else if (metric == 1) value = measured(row[1]);
            else value = ratio(row[4], row[5]);
        } else {
            if (!row.is_object()) { result.partial = true; previous_valid = false; continue; }
            if (metric == 2) {
                if (field(row, "method") != "query") continue;
                const bool diagnostic = field(row, "purpose") == "diagnostic";
                if ((traffic == 1 && diagnostic) || (traffic == 2 && !diagnostic)) continue;
            }
            at = measured(field(row, "elapsed_ms"));
            value = measured(field(row, metric == 0 ? "rss_bytes" : metric == 1 ? "workers" : "duration_ms"));
            if (value && metric == 0) *value /= 1048576;
        }
        if (value && metric == 1 && std::floor(*value) != *value) value.reset();
        if (!at || *at > result.end || *at < previous_at) {
            result.partial = true; previous_valid = false; continue;
        }
        if ((range ? *at + interval : *at) < result.begin) { previous_valid = false; continue; }
        const double gap = *at - previous_at;
        const bool connect = previous_valid && value && join && gap > 0 &&
            gap <= (range ? interval * 1.5 : 3500);
        result.points.push_back({std::max(*at, result.begin), value, connect});
        if (value) peak = std::max(peak, *value);
        previous_valid = value.has_value(); previous_at = *at; previous_run = std::move(run);
    }
    const double padded = peak * 1.12;
    const double magnitude = std::pow(10.0, std::floor(std::log10(padded)));
    const double normalized = padded / magnitude;
    result.maximum = (normalized <= 2 ? 2 : normalized <= 5 ? 5 : 10) * magnitude;
    return result;
}
} // namespace shin::observer::tui
