#pragma once
#include "shin/observer/local_project.h"
namespace shin::context { struct RuntimeOptions; }
namespace shin::observer {
// Explicit session operations may audit the worktree. Ordinary reads never do.
Json local_session_operation(const shin::context::RuntimeOptions& options, const fs::path& workspace,
    const LocalProjectRequest& request, const std::function<void()>& guard);
void local_session_fields(const Json& payload, LocalProjectRequest& request);
Json local_session_payload(const LocalProjectRequest& request);
void local_session_actions(Json& assessment);
}
