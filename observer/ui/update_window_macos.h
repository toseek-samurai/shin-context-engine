#pragma once
#include "shin/observer/local_updates.h"
#include "language.h"
#include <functional>
#include <optional>

namespace shin::observer {
enum class ReleaseUpdateStep {
    preparing, waiting_for_monitor, downloading, verifying_archive, extracting,
    verifying_kit, installing, confirming
};
struct ReleaseUpdateProgress {
    ReleaseUpdateStep step = ReleaseUpdateStep::preparing;
    std::uint64_t received = 0, total = 0;
};
using ReleaseUpdateProgressSink = std::function<void(ReleaseUpdateProgress)>;
using ReleaseUpdateOperation = std::function<Json(const ReleaseUpdateProgressSink&)>;
inline std::optional<double> release_download_percent(ReleaseUpdateProgress value) {
    if (value.step != ReleaseUpdateStep::downloading || !value.total) return {};
    return static_cast<double>(std::min(100.0L, 100.0L * value.received / value.total));
}
// The helper owns this window; the installed Monitor may exit and be replaced.
// Observation only: UI failure must not retry, cancel or certify installation.
Json run_release_update_window(const std::string& version, shin::ui::Language language,
                              const ReleaseUpdateOperation& operation);
// Private stdin progress channel. This viewer has no installation authority.
int release_update_progress_viewer(const std::string& version, shin::ui::Language language);
} // namespace shin::observer
