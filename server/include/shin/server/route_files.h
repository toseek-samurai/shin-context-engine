#pragma once

#include "model.h"
#include <filesystem>
#include <string_view>

namespace shin::server {

std::string read_route_file(const std::filesystem::path& path, std::uintmax_t maximum_bytes);
std::string route_directory_identity(const std::filesystem::path& path);

} // namespace shin::server
