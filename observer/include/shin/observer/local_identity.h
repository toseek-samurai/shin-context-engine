#pragma once
#include "local_project.h"

namespace shin::observer {
// Stable enrollment identity; checking it never opens a historical profile or publication.
std::string local_binding_key(const Json& binding, const fs::path& root, const fs::path& workspace);
// Local provenance only. These IDs are neither credentials nor Server grants.
Json local_identity(const LocalProjectRequest& request, bool enroll);
// Reject changed identity metadata before init can touch existing project data.
void check_local_identity(const fs::path& home, const fs::path& root, bool installation_only = false);
} // namespace shin::observer
