#include "shin/server/route_catalog.h"
#include "shin/server/route_files.h"
#include "context_internal.h"
#include "generated_io.h"

namespace shin::server {
namespace core = shin::context;
namespace {
constexpr std::size_t kProfileFileBytes = 1024U * 1024U;
}

RouteCatalog::RouteCatalog(const Config& config) {
    for (const auto& route : config.retrieval.routes) {
        const auto profile_hash = core::fnv1a64(read_route_file(route.profile_dir / (route.profile + ".json"), kProfileFileBytes));
        core::RuntimeOptions options{.root = route.root, .out_root = {},
            .profile_dir = route.profile_dir, .profile = core::make_profile(route.profile, route.profile_dir),
            .force_index = true, .force_source_scan = false, .generated_snapshot = {}};
        if (options.profile.config_hash != profile_hash)
            throw ServiceError(409, "route-profile-changed", "Configured profile changed while opening the route.");
        const auto fingerprint = digest_hex(sha256(Json{
            {"project_id", route.project_id}, {"workspace_id", route.workspace_id},
            {"source_host_id", route.source_host_id}, {"route_id", route.id},
            {"root", route.root}, {"root_identity", route.root_identity}, {"profile", route.profile},
            {"profile_hash", profile_hash}, {"profile_dir", route.profile_dir.string()},
            {"generator", core::kGeneratorVersion}}.dump()));
        bindings_.emplace(route.id, BoundRoute{route, std::move(options), profile_hash, fingerprint});
    }
}

const BoundRoute& RouteCatalog::find(const std::string& project, const std::string& route) const {
    const auto found = bindings_.find(route);
    if (found == bindings_.end() || found->second.route.project_id != project)
        throw ServiceError(404, "route-not-found", "Retrieval route is not available.");
    return found->second;
}

void RouteCatalog::verify(const BoundRoute& binding, const Json& workspace) const {
    const auto& route = binding.route;
    if (workspace.at("project_id") != route.project_id || workspace.at("id") != route.workspace_id ||
        workspace.at("source_kind") != "local" || workspace.at("source_host_id") != route.source_host_id || workspace.at("root") != route.root ||
        workspace.at("root_identity") != route.root_identity)
        throw ServiceError(409, "route-project-mismatch", "Route does not match the registered workspace identity.");
    if (route_directory_identity(route.root) != route.root_identity)
        throw ServiceError(409, "root-identity-changed", "Registered workspace root identity changed.");
    if (core::fnv1a64(read_route_file(route.profile_dir / (route.profile + ".json"), kProfileFileBytes)) != binding.profile_bytes_hash)
        throw ServiceError(409, "route-profile-changed", "Configured profile changed; review the route and restart explicitly.");
}

} // namespace shin::server
