#include "shin/server/active_snapshot.h"
#include "shin/server/route_files.h"

namespace shin::server {

ActiveSnapshot::ActiveSnapshot(const Config& config, const RouteCatalog& catalog, SnapshotCache& cache,
                               Store& store, const Principal& principal, const std::string& project,
                               const std::string& name, bool publication, const Json& active,
                               const std::string& state_identity, const std::function<Json()>& select,
                               const std::optional<WorkspaceReadRequest>& scope)
    : catalog_(catalog), binding_(catalog.find(project, active.at("source_route_id").get<std::string>())),
      store_(store), principal_(principal), scope_(scope),
      publication_(publication && scope ? std::optional(name) : std::nullopt),
      selected_(publication_ ? active : Json(nullptr)),
      admission_(read_workspace()), workspace_(admission_.at("workspace")) {
    if (publication && (active.at("workspace_id") != binding_.route.workspace_id ||
        active.at("source_host_id") != binding_.route.source_host_id || active.at("profile_id") != binding_.options.profile.id))
        throw ServiceError(409, "active-generation-route-changed", "Selected provenance differs from the configured source route.");
    catalog_.verify(binding_, workspace_);
    if (active.at("preparation_id").is_null())
        throw ServiceError(503, "active-generation-missing", "This route has no explicitly activated generation.");
    if (active.at("artifact_namespace") != candidate_namespace(config.preparations.state_dir, state_identity) ||
        active.at("route_fingerprint") != candidate_execution_fingerprint(binding_.fingerprint,
            config.preparations.state_dir, state_identity, config.retrieval.max_generation_bytes, config.retrieval.max_file_bytes))
        throw ServiceError(409, "active-generation-route-changed", "Active generation belongs to another execution route.");
    const auto id = active.at("preparation_id").get<std::uint64_t>();
    const auto key = active.at("artifact_key").get<std::string>();
    lifetime_ = std::make_unique<CandidateArtifactLock>(config.preparations.state_dir, state_identity, id, key,
                                                       CandidateArtifactLock::Mode::inspect_existing);
    if (select() != active)
        throw ServiceError(409, "active-generation-changed", "Retry against the newly activated generation.");
    auto options = binding_.options;
    options.out_root = config.preparations.state_dir / candidate_artifact_name(id, key);
    verify_candidate_layout(options.out_root, config.retrieval.max_file_bytes);
    const auto snapshot_id = active.at("snapshot_id").get<std::string>();
    // Share immutable payloads within one ownership domain, never cached grants.
    // Account/membership revisions are rechecked, not multiplied into residency.
    const auto ownership_key = scope ? "workspace:" + admission_.at("authorization").at("ownership").dump() + ":" : "";
    const auto cache_key = ownership_key + std::string(publication ? "publication:" : "route:") + project + ":" + name + ":" + std::to_string(id) + ":" + key;
    snapshot_ = cache.acquire(cache_key, snapshot_id, [&] {
        const auto loaded = context::GeneratedSnapshot::load(options,
            {config.retrieval.max_generation_bytes, config.retrieval.max_file_bytes});
        if (loaded->id() != snapshot_id || loaded->payload_bytes() != active.at("artifact_bytes"))
            throw ServiceError(409, "active-generation-artifact-mismatch", "Active generation differs from its durable binding.");
        return loaded;
    });
}

Json ActiveSnapshot::read_workspace() const {
    const auto& route = binding_.route;
    if (scope_) {
        if (scope_->workspace_id != route.workspace_id)
            throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
        return store_.scoped_workspace(principal_, route.project_id, *scope_);
    }
    return {{"workspace", store_.get_workspace(principal_, route.project_id, route.workspace_id)}, {"authorization", nullptr}};
}

void ActiveSnapshot::verify() const {
    catalog_.verify(binding_, workspace_);
    lifetime_->verify();
    if (publication_) {
        if (!scope_) throw ServiceError(500, "invalid-read-scope", "Scoped publication has no workspace binding.");
        if (store_.scoped_publication_head(principal_, binding_.route.project_id, *publication_, *scope_) != selected_)
            throw ServiceError(409, "active-generation-changed", "Publication changed during retrieval; retry against the current generation.");
    }
    if (!admission_.at("authorization").is_null() && read_workspace() != admission_)
        throw ServiceError(409, "workspace-read-scope-changed", "Workspace access changed during retrieval; retry with current permissions.");
}

} // namespace shin::server
