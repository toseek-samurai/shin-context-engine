#pragma once
#include "view.h"
#include <optional>

namespace shin::observer::tui {
struct ChartPoint {
    double at = 0;
    std::optional<double> value;
    bool connect = false;
};
struct Chart {
    std::string title, unit;
    std::vector<ChartPoint> points;
    double begin = 0, end = 0, maximum = 1;
    bool retained = false, partial = false;
    std::size_t omitted = 0;
};
// Existing telemetry only. Missing samples, time gaps and minute buckets
// spanning service runs never become interpolated zeroes.
Chart chart(const Snapshot& snapshot, int metric, int range, int traffic = 0);
} // namespace shin::observer::tui
