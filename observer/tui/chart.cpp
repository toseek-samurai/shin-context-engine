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
void live_points(Chart& result, const Json& telemetry, int metric, int traffic) {
    using ui::telemetry_measurement;
    const auto end = telemetry_measurement(field(telemetry, "uptime_ms"));
    if (!end) return;
    result.end = *end;
    result.begin = std::max(0.0, result.end - 60000);
    const auto& rows = metric == 2 ? field(field(telemetry, "request_metrics"), "recent") : field(telemetry, "samples");
    if (!rows.is_array() || rows.size() > (metric == 2 ? 64U : 120U)) {
        result.partial = result.partial || !rows.is_null();
        return;
    }
    double previous_at = -1;
    bool previous_valid = false;
    for (const auto& row : rows) {
        if (!row.is_object()) { result.partial = true; previous_valid = false; continue; }
        if (metric == 2) {
            if (field(row, "method") != "query") continue;
            const bool diagnostic = field(row, "purpose") == "diagnostic";
            if ((traffic == 1 && diagnostic) || (traffic == 2 && !diagnostic)) continue;
        }
        const auto at = telemetry_measurement(field(row, "elapsed_ms"));
        if (!at || *at > result.end || *at < previous_at) {
            result.partial = true; previous_valid = false; continue;
        }
        if (*at < result.begin) { previous_valid = false; continue; }
        auto value = metric == 1 ? ui::telemetry_count(field(row, "workers")) :
            telemetry_measurement(field(row, metric == 0 ? "rss_bytes" : "duration_ms"));
        if (value && metric == 0) *value /= 1048576;
        const auto gap = *at - previous_at;
        result.points.push_back({*at, value, previous_valid && value && gap > 0 && gap <= 3500});
        previous_valid = value.has_value();
        previous_at = *at;
    }
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
    const auto omitted = ui::telemetry_count(field(field(field(telemetry, "projection"), "omitted_rows"), path));
    if (omitted) {
        result.omitted = static_cast<std::size_t>(*omitted);
        result.partial = result.omitted != 0;
    }
    if (range) {
        auto retained = ui::retained_charts(field(telemetry, "history"), range == 1 ? 1 : 24);
        result.partial = result.partial || retained.partial;
        if (!retained.available) return result;
        result.begin = retained.begin;
        result.end = retained.end;
        result.points = std::move(retained.series[static_cast<std::size_t>(metric)]);
    } else live_points(result, telemetry, metric, traffic);
    double peak = metric == 0 ? 16 : metric == 1 ? 2 : 1;
    for (const auto& point : result.points)
        if (point.value) peak = std::max(peak, *point.value);
    const double padded = peak * 1.12;
    const double magnitude = std::pow(10.0, std::floor(std::log10(padded)));
    const double normalized = padded / magnitude;
    result.maximum = (normalized <= 2 ? 2 : normalized <= 5 ? 5 : 10) * magnitude;
    return result;
}
} // namespace shin::observer::tui
