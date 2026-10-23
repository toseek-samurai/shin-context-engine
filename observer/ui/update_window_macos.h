#pragma once
#include "shin/observer/local_updates.h"
#include "update_progress.h"
#include "language.h"
#include <functional>
#include <optional>

namespace shin::observer {
// The helper owns this window; the installed Monitor may exit and be replaced.
// Observation only: UI failure must not retry, cancel or certify installation.
Json run_release_update_window(const std::string& version, shin::ui::Language language,
                              const ReleaseUpdateOperation& operation);
// Private stdin progress channel. This viewer has no installation authority.
int release_update_progress_viewer(const std::string& version, shin::ui::Language language);
} // namespace shin::observer
