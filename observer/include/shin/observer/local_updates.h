#pragma once
#include "io.h"
#include "update_progress.h"
namespace shin::observer {
// Public release metadata is untrusted. Selection never changes local state.
Json select_local_release(const Json& releases, const std::string& current);
void verify_release_listing(const std::string& listing, const std::string& version);
#ifdef __APPLE__
Json local_updates(int argc, char** argv, const fs::path& user_home,
                   const ReleaseUpdateProgressSink& observer = {});
Json install_local_release(int argc, char** argv, const fs::path& user_home);
Json run_updater_request(int argc, char** argv, const fs::path& user_home,
                         const ReleaseUpdateProgressSink& observer = {});
#endif
} // namespace shin::observer
