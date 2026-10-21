#pragma once
#include "io.h"

namespace shin::observer {
// Only complete, extracted kits are installable. No downloads or client edits.
Json local_installation(int argc, char** argv, const fs::path& user_home);
// Verify a staged complete kit before any downloaded executable is invoked.
Json verify_local_kit(const fs::path& path);
// Only the service bootstrap may enter an installed payload during activation.
std::optional<fs::path> installed_local_home(const fs::path& executable, bool startup = false);
void check_local_installation_home(const fs::path& home, const fs::path& executable, bool startup = false);
class LocalInstallationGuard {
public:
    explicit LocalInstallationGuard(int descriptor) : descriptor_(descriptor) {}
    ~LocalInstallationGuard();
    LocalInstallationGuard(const LocalInstallationGuard&) = delete;
    LocalInstallationGuard& operator=(const LocalInstallationGuard&) = delete;
private:
    int descriptor_;
};
std::unique_ptr<LocalInstallationGuard> lock_local_installation(const fs::path& executable, bool startup = false);
} // namespace shin::observer
