#include "shin/server/preparation_service.h"

#include <limits>

namespace shin::server {

Json PreparationService::with_verified_candidate(const Principal& principal, std::uint64_t id,
                                                  const std::function<Json(const Json&)>& commit,
                                                  const std::optional<WorkspaceWriteScope>& write) {
    if (!write && !principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
    return observe_candidate(principal, id, Observation::activation, commit, write);
}

Json PreparationService::active_generation(const Principal& principal, const std::string& project, const std::string& route,
                                           const std::optional<WorkspaceReadRequest>& scope) {
    if (!scope) (void)store_.get_project(principal, project);
    const auto& binding = [&]() -> const BoundRoute& {
        try { return catalog_.find(project, route); }
        catch (const ServiceError& error) {
            if (scope && error.status == 404) throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
            throw;
        }
    }();
    const auto active = scope ? store_.scoped_active_route(principal, binding.route, *scope) : store_.active_route(principal, project, route);
    return {{"project_id", project}, {"route_id", route}, {"revision", active.at("revision")},
            {"preparation_id", active.at("preparation_id")}, {"snapshot_id", active.at("snapshot_id")}};
}

Json PreparationService::activate(const Principal& principal, const std::string& project, const std::string& route, const Json& request,
                                  const std::optional<WorkspaceReadRequest>& scope) {
    if (!scope && !principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
    std::optional<WorkspaceWriteScope> write;
    if (scope) write = store_.capture_workspace_write(principal, project, *scope);
    else (void)store_.get_project(principal, project);
    const auto& binding = [&]() -> const BoundRoute& {
        try { return catalog_.find(project, route); }
        catch (const ServiceError& error) {
            if (scope && error.status == 404) throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
            throw;
        }
    }();
    if (write) {
        if (binding.route.workspace_id != write->workspace.workspace_id)
            throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
        catalog_.verify(binding, write->admission.at("workspace"));
        write->route_id = route;
    }
    require_fields(request, {"preparation_id", "expected_revision"});
    const auto& target = request.at("preparation_id");
    const auto& revision = request.at("expected_revision");
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if ((!target.is_null() && (!target.is_number_integer() || target < 1 || target > maximum)) ||
        !revision.is_number_integer() || revision < 0 || revision >= maximum)
        throw ServiceError(400, "invalid-request", "Invalid activation target or expected revision.");
    const auto expected = revision.get<std::uint64_t>();
    const auto existing = store_.activation_replay(principal, project, route, expected, target, write);
    if (!existing.is_null()) return existing;
    if (target.is_null()) return store_.commit_activation(principal, project, route, expected, nullptr, write);
    const auto id = target.get<std::uint64_t>();
    const auto job = write ? store_.scoped_preparation(principal, project, id, write->workspace) : store_.get_preparation(principal, id);
    if (job.at("project_id") != project || job.at("route_id") != route)
        throw ServiceError(409, "activation-target-mismatch", "Candidate belongs to another route.");
    return observe_candidate(principal, id, Observation::activation, [&](const Json& verified) {
        return store_.commit_activation(principal, project, route, expected, verified, write);
    }, write);
}

} // namespace shin::server
