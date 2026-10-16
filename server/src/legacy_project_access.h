#pragma once
#include "shin/server/config.h"
#include <algorithm>

namespace shin::server::legacy_access {
// Configured project grants only. Enrolled-account admission remains fenced;
// these helpers are not a tenant/workspace authorization context.
inline void validate_principal(const Principal& principal) {
    if (!valid_identifier(principal.id) || principal.project_ids.size() > 128 ||
        !std::all_of(principal.project_ids.begin(), principal.project_ids.end(), valid_identifier))
        throw ServiceError(403, "principal-scope-invalid", "Principal scope is invalid.");
}
inline void require_admin(const Principal& principal) {
    if (!principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
}
// Callers validate the principal and resource syntax before testing visibility.
inline bool principal_can_access(const Principal& principal, std::string_view project) {
    return principal.admin || std::find(principal.project_ids.begin(), principal.project_ids.end(), project) != principal.project_ids.end();
}
inline void require_project_grant(const Principal& principal, std::string_view project) {
    if (!principal_can_access(principal, project))
        throw ServiceError(404, "project-not-found", "Project was not found.");
}
} // namespace shin::server::legacy_access
