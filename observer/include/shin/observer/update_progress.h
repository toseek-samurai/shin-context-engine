#pragma once
#include "io.h"
#include <array>
#include <functional>
#include <optional>

namespace shin::observer {
// The first eight values are retained for installed read-only viewers.
enum class ReleaseUpdateStep {
    preparing, waiting_for_monitor, downloading, verifying_archive, extracting,
    verifying_kit, installing, confirming, checking_release, qualifying_projects,
    staging, stopping_service, switching_kit, starting_service, checking_projects,
    restoring_service_state, rolling_back, completed, failed
};
struct ReleaseUpdateProgress {
    ReleaseUpdateStep step = ReleaseUpdateStep::preparing;
    // Bytes for download/staging; completed projects for qualification.
    std::uint64_t received = 0, total = 0;
};
using ReleaseUpdateProgressSink = std::function<void(ReleaseUpdateProgress)>;
using ReleaseUpdateOperation = std::function<Json(const ReleaseUpdateProgressSink&)>;
inline void report_update(const ReleaseUpdateProgressSink& sink, ReleaseUpdateProgress value) noexcept {
    try { if (sink) sink(value); } catch (...) { /* Observation never changes installation authority. */ }
}
inline std::optional<double> release_download_percent(ReleaseUpdateProgress value) {
    if (value.step != ReleaseUpdateStep::downloading || !value.total) return {};
    return static_cast<double>(std::min(100.0L, 100.0L * value.received / value.total));
}
inline constexpr std::array update_steps{
    ReleaseUpdateStep::preparing, ReleaseUpdateStep::downloading, ReleaseUpdateStep::verifying_archive,
    ReleaseUpdateStep::extracting, ReleaseUpdateStep::verifying_kit, ReleaseUpdateStep::qualifying_projects,
    ReleaseUpdateStep::staging, ReleaseUpdateStep::stopping_service, ReleaseUpdateStep::switching_kit,
    ReleaseUpdateStep::starting_service, ReleaseUpdateStep::checking_projects, ReleaseUpdateStep::confirming};
inline std::size_t update_step_index(ReleaseUpdateStep step) {
    if (step == ReleaseUpdateStep::checking_release || step == ReleaseUpdateStep::waiting_for_monitor) return 0;
    if (step == ReleaseUpdateStep::installing) return 5;
    if (step == ReleaseUpdateStep::restoring_service_state) return 10;
    for (std::size_t i = 0; i < update_steps.size(); ++i) if (update_steps[i] == step) return i;
    return update_steps.size(); // Recovery and terminal outcomes are not forward steps.
}
inline Json update_progress_frame(ReleaseUpdateProgress value) {
    return {{"step", static_cast<unsigned>(value.step)}, {"received", value.received}, {"total", value.total}};
}
inline std::optional<ReleaseUpdateProgress> parse_update_progress(const Json& row) {
    if (!row.is_object() || row.size() != 3 || !row.contains("step") || !row["step"].is_number_unsigned() ||
        !row.contains("received") || !row["received"].is_number_unsigned() ||
        !row.contains("total") || !row["total"].is_number_unsigned()) return {};
    const auto step = row["step"].get<std::uint64_t>();
    if (step > static_cast<unsigned>(ReleaseUpdateStep::failed)) return {};
    return ReleaseUpdateProgress{static_cast<ReleaseUpdateStep>(step), row["received"], row["total"]};
}
struct UpdateTimeline {
    // Only observed steps can be marked complete. Skipped steps stay pending.
    std::array<bool, update_steps.size()> seen{}, done{};
    ReleaseUpdateProgress current;
    std::size_t active = update_steps.size();
    void observe(ReleaseUpdateProgress value) {
        const auto previous = update_step_index(current.step), next = update_step_index(value.step);
        if (next != previous && value.step != ReleaseUpdateStep::failed && value.step != ReleaseUpdateStep::rolling_back &&
            previous < done.size() && seen[previous]) done[previous] = true;
        current = value;
        if (next < seen.size()) { active = next; seen[next] = true; done[next] = false; }
    }
};
} // namespace shin::observer
