#pragma once
#include "local_project.h"

namespace shin::observer {
Json serve_local_projects(const LocalProjectRequest& request, const std::vector<fs::path>& roots,
                          const fs::path& executable, const std::function<bool()>& stopped, bool registered = false, bool supervised = false,
                          const std::function<void()>& started = {}, const Json& usage_sources = nullptr);
std::unique_ptr<StateStore> lock_local_service_control(const fs::path& home);
Json start_local_service(const LocalProjectRequest& request, const fs::path& executable);
Json initialize_local_service(const LocalProjectRequest& request, const fs::path& executable);
Json detach_local_service(const LocalProjectRequest& request);
Json request_local_service(const LocalProjectRequest& request, const std::function<bool()>& cancelled = {});
// Restricted update channel: only status or stop, same Core, exact pinned Observer.
Json control_local_service_for_update(const fs::path& home, const std::string& observer_version, bool stop);
Json qualify_local_project(const LocalProjectRequest& request, const fs::path& executable,
                           const std::function<bool()>& cancelled = {});
int local_service_worker();
int local_refresh_worker();
int local_resident_worker();
} // namespace shin::observer
