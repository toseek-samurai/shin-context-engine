#pragma once
#include "io.h"

namespace shin::observer {
enum class ProjectState { offline, checking, pending, recovering, blocked, current, standby, stopped, unknown };

// Classify an observed project row for display. Service admission is separate.
bool active_issue(const Json& project);
ProjectState project_state(const Json& project, bool connected);
const char* project_state_name(ProjectState state);
bool quality_warning(const Json& project);
bool partial_coverage(const Json& project);
bool project_connection_problem(const Json& status);

// Read-only presentation of the existing status contract; never probes projects.
std::string local_status_text(const Json& status);
}
