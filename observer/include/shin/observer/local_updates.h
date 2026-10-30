#pragma once
#include "io.h"
#include "update_progress.h"
namespace shin::observer {
// Public release metadata is untrusted. Selection never changes local state.
// Match the existing HTTP feed budget without expanding local state limits.
inline constexpr std::size_t kMaximumReleaseFeedBytes = 1024U * 1024U;
Json parse_release_feed(const std::string& text);
Json select_local_release(const Json& releases, const std::string& current);
// Windows offers a reviewed MSI download; this never installs or launches it.
Json select_windows_release(const Json& releases, const std::string& current);
void verify_release_listing(const std::string& listing, const std::string& version);
#ifdef __APPLE__
Json local_updates(int argc, char** argv, const fs::path& user_home,
                   const ReleaseUpdateProgressSink& observer = {});
Json install_local_release(int argc, char** argv, const fs::path& user_home);
Json run_updater_request(int argc, char** argv, const fs::path& user_home,
                         const ReleaseUpdateProgressSink& observer = {});
#endif
} // namespace shin::observer
