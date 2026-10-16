#pragma once

#include "model.h"
#include <filesystem>

namespace shin::server {

Json inspect_retention_tree(const std::filesystem::path& root, std::uint64_t maximum_bytes);
void remove_retention_tree(const std::filesystem::path& state_root, const std::string& state_identity, const std::string& name,
                           const Json& inventory, std::uint64_t maximum_bytes);

} // namespace shin::server
