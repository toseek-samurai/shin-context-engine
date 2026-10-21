#pragma once
#include "io.h"
namespace shin::observer {
// Public release metadata is untrusted. Selection never changes local state.
Json select_local_release(const Json& releases, const std::string& current);
void verify_release_listing(const std::string& listing, const std::string& version);
#ifdef __APPLE__
Json local_updates(int argc, char** argv, const fs::path& user_home);
#endif
} // namespace shin::observer
