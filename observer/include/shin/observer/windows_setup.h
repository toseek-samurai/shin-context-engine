#pragma once
#include "local_project.h"

namespace shin::observer {
// Explicit post-install/user recovery. No project discovery or policy approval;
// uses ordinary init for the exact current user's registered routes.
Json prepare_windows_projects(const fs::path& home, const fs::path& executable, bool check_only = false);
}
