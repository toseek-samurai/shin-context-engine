#pragma once
#include "view.h"
#include "../ui/retained_charts.h"

namespace shin::observer::tui {
struct Chart {
    std::string title, unit;
    std::vector<ui::HistoryPoint> points;
    double begin = 0, end = 0, maximum = 1;
    bool retained = false, partial = false;
    std::size_t omitted = 0;
};
// Existing telemetry only. Missing samples, time gaps and minute buckets
// spanning service runs never become interpolated zeroes.
Chart chart(const Snapshot& snapshot, int metric, int range, int traffic = 0);
} // namespace shin::observer::tui
