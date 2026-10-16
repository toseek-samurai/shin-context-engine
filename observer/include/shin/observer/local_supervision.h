#pragma once
#include "local_project.h"

namespace shin::observer {
const char* local_service_manager();
#ifdef _WIN32
int windows_scm_dispatch();
int windows_scm_user(const std::string& stop_event);
enum class WindowsScmAction { install, stop, remove };
Json windows_scm_setup(WindowsScmAction action, const fs::path& executable);
#endif
std::string native_launchd_plist(const Json& unit);
// Read-only definition for the OS manager; installation is a separate operation.
Json local_service_definition(const LocalProjectRequest& request, const fs::path& executable, bool installed_entry = false);
Json configure_local_autostart(const LocalProjectRequest& request, const fs::path& executable);
Json update_local_autostart(const LocalProjectRequest& request, const fs::path& executable);
Json local_autostart_status(const LocalProjectRequest& request, const fs::path& executable);
// Must be called before taking the shared startup lock; managed children may
// need that lock to bootstrap. Null means no installed manager for this home.
std::optional<Json> start_installed_local_service(const LocalProjectRequest& request, const fs::path& executable);
void check_local_service_supervision(const LocalProjectRequest& request, const fs::path& executable, bool supervised);
}
