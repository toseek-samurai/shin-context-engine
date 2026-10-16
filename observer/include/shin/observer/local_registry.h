#pragma once
#include "local_project.h"

namespace shin::observer {
inline constexpr std::size_t kLocalServiceProjectLimit = 16;
inline constexpr std::size_t kLocalResidentProjectLimit = 5;
// Empty service startup creates no project enrollment or publication grant.
void ensure_local_registry(const fs::path& home);
void enroll_local_project(const LocalProjectRequest& request);
std::vector<LocalProjectRequest> registered_local_projects(const fs::path& home);
// Lifecycle writes require the project's binding writer lock.
struct LocalProjectLifecycle {
    std::string phase = "active";
    std::uint64_t revision = 0;
    bool operator==(const LocalProjectLifecycle&) const = default;
};
LocalProjectLifecycle local_project_lifecycle(const fs::path& workspace);
LocalProjectLifecycle require_local_project_active(const fs::path& workspace);
void set_local_project_lifecycle(const fs::path& workspace, const Json& binding, const std::string& phase);
void unenroll_local_project(const fs::path& home, const Json& binding);
} // namespace shin::observer
