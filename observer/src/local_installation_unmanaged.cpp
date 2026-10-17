#include "shin/observer/local_installation.h"

namespace shin::observer {
namespace {
void require_uninstalled(const fs::path& executable) {
    const auto prefix = fs::canonical(executable).parent_path().parent_path();
    if (fs::exists(fs::symlink_status(prefix / ".shin-installation.json")))
        throw Error("installation-platform-unsupported", "This build cannot manage an installed kit receipt; it will not bypass it.");
}
}
std::optional<fs::path> installed_local_home(const fs::path& executable, bool) {
    require_uninstalled(executable); return {};
}
void check_local_installation_home(const fs::path& home, const fs::path& executable, bool) {
    require_uninstalled(executable);
    if (fs::exists(fs::symlink_status(home / "installation")))
        throw Error("installation-home-conflict", "This data home belongs to a managed installation; it was preserved.");
}
LocalInstallationGuard::~LocalInstallationGuard() = default;
std::unique_ptr<LocalInstallationGuard> lock_local_installation(const fs::path& executable, bool) {
    require_uninstalled(executable); return {};
}
Json local_installation(int, char**, const fs::path&) {
    throw Error("installation-platform-unsupported", "Managed desktop installation is not available in this build.");
}
}
