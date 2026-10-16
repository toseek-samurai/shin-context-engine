#pragma once
#include "local_project.h"

namespace shin::observer {
// Local provenance only. These IDs are neither credentials nor Server grants.
Json local_identity(const LocalProjectRequest& request, bool enroll);
// Reject changed identity metadata before init can touch existing project data.
void check_local_identity(const fs::path& home, const fs::path& root, bool installation_only = false);
} // namespace shin::observer
