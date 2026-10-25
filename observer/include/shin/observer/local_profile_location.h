#pragma once
#include "local_project.h"

namespace shin::observer {
// Explicit same-policy registry relocation. The service must be stopped;
// a durable intent resumes interrupted metadata writes without index mutation.
Json relocate_local_profile(const LocalProjectRequest& request);
}
