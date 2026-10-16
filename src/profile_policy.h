#pragma once

#include "context_types.h"

namespace shin::context {

// Read-only local profile projection, not an authorization or activation API.
Json inspect_profile_policy(const fs::path& root, const fs::path& profile_dir,
                            const std::string& profile_id,
                            const std::optional<std::string>& expected_hash = std::nullopt);
int run_profile_policy(const RuntimeOptions& options, const std::vector<std::string>& args);

} // namespace shin::context
