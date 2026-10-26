#pragma once
#include "io.h"
namespace shin::observer {
inline constexpr auto updater_archive_name = "shin-updater-v1-macos-arm64.zip";
inline constexpr auto updater_bundle_name = "shin-updater-v1";
inline constexpr std::size_t updater_archive_limit = 64U * 1024 * 1024;
// Stable bootstrap ABI. These checks know only the updater envelope, never a kit,
// Core generator, installation journal or project/index format.
Json select_updater_release(const Json& releases, const std::string& current);
void verify_updater_archive(const std::string& bytes);
Json verify_updater_bundle(const fs::path& root, bool inside_kit = false);
} // namespace shin::observer
