#include "shin/server/publication_service.h"
#include "shin/server/config.h"
#include "shin/server/route_files.h"
#include "shin/server/candidate_artifact.h"

#include <limits>

namespace shin::server {
namespace {
std::uint64_t number(const Json& request, const char* key, bool positive) {
    const auto& value = request.at(key);
    if (!value.is_number_integer() || value < (positive ? 1 : 0) ||
        value > std::numeric_limits<std::int64_t>::max() ||
        (!positive && value == std::numeric_limits<std::int64_t>::max()))
        throw ServiceError(400, "invalid-request", "Invalid publication identity or revision.");
    return value.get<std::uint64_t>();
}

Json bounded(Json value) {
    if (value.dump().size() > 65536)
        throw ServiceError(413, "publication-response-too-large", "Narrow the publication review page.");
    return value;
}
} // namespace

PublicationService::PublicationService(const Config& config, const RouteCatalog& catalog, Store& store,
                                       PreparationService& preparations)
    : config_(config), catalog_(catalog), store_(store), preparations_(preparations),
      state_identity_(route_directory_identity(config.preparations.state_dir)) {}

Json PublicationService::head(const Principal& principal, const std::string& project, const std::string& publication) {
    const auto selected = store_.publication_head(principal, project, publication, true);
    Json result{{"project_id", project}, {"publication_id", publication}};
    for (const auto* key : {"state", "revision", "preparation_id", "snapshot_id", "source_route_id", "workspace_id", "history_epoch"})
        result[key] = selected.at(key);
    return result;
}

Json PublicationService::submit(const Principal& principal, const std::string& project, const std::string& publication,
                                 const Json& request, const std::string& key, const std::optional<WorkspaceReadRequest>& scope) {
    const auto write = scope ? std::optional(store_.capture_workspace_write(principal, project, *scope)) : std::nullopt;
    if (request.contains("history_epoch")) require_fields(request, {"preparation_id", "base_revision", "history_epoch"});
    else require_fields(request, {"preparation_id", "base_revision"});
    const auto preparation = number(request, "preparation_id", true);
    const auto revision = number(request, "base_revision", false);
    const auto epoch = request.contains("history_epoch") ? number(request, "history_epoch", false) : 0;
    Json identity{{"operation", "publication-proposal"}, {"project", project},
        {"publication", publication}, {"preparation_id", preparation}, {"base_revision", revision}};
    if (epoch != 0) identity["history_epoch"] = epoch;
    if (scope) identity["workspace_scope"] = {{"tenant_id", scope->tenant_id}, {"workspace_id", scope->workspace_id}};
    const auto hash = digest_hex(sha256(identity.dump()));
    const auto replay = store_.publication_submission_replay(principal, project, publication, preparation, revision, key, hash, epoch, write);
    if (!replay.is_null()) return bounded(replay);
    const auto input = store_.preparation_inspection_input(principal, preparation, write);
    if (input.at("job").at("project_id") != project)
        throw ServiceError(404, "preparation-not-found", "Preparation was not found.");
    const auto& binding = catalog_.find(project, input.at("job").at("route_id").get<std::string>());
    if (scope && scope->workspace_id != binding.route.workspace_id)
        throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
    const auto workspace = write ? write->admission.at("workspace") : store_.get_workspace(principal, project, binding.route.workspace_id);
    catalog_.verify(binding, workspace);
    const auto fingerprint = candidate_execution_fingerprint(binding.fingerprint, config_.preparations.state_dir,
        state_identity_, config_.retrieval.max_generation_bytes, config_.retrieval.max_file_bytes);
    if (input.at("route_fingerprint") != fingerprint)
        throw ServiceError(409, "preparation-route-changed", "Candidate source binding changed.");
    const Json source{{"source_route_id", binding.route.id}, {"workspace_id", binding.route.workspace_id},
                      {"source_host_id", binding.route.source_host_id}, {"profile_id", binding.options.profile.id},
                      {"route_fingerprint", fingerprint}};
    return bounded(store_.submit_publication_proposal(principal, project, publication, preparation, revision, key, hash, source, epoch, write));
}

Json PublicationService::detail(const Principal& principal, const std::string& project, const std::string& publication,
                                 std::uint64_t id) {
    return bounded(store_.publication_proposal(principal, project, publication, id));
}

Json PublicationService::list(const Principal& principal, const std::string& project, const std::string& publication,
                               std::uint64_t after, std::size_t limit) {
    return bounded(store_.list_publication_proposals(principal, project, publication, after, limit));
}

Json PublicationService::resolve(const Principal& principal, const std::string& project, const std::string& publication,
                                  std::uint64_t id, const Json& request, const std::optional<WorkspaceReadRequest>& scope) {
    if (!scope && !principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
    const auto write = scope ? std::optional(store_.capture_workspace_write(principal, project, *scope)) : std::nullopt;
    require_fields(request, {"decision", "expected_revision"});
    const auto decision = require_text(request, "decision", 32);
    if (decision != "accept" && decision != "reject" && decision != "request-reprepare")
        throw ServiceError(400, "invalid-request", "Invalid publication decision.");
    const auto revision = number(request, "expected_revision", false);
    const auto replay = store_.publication_resolution_replay(principal, project, publication, id, decision, revision, write);
    if (!replay.is_null()) return replay;
    if (decision != "accept")
        return store_.resolve_publication_proposal(principal, project, publication, id, decision, revision, nullptr, write);
    const auto proposal = scope ? store_.scoped_publication_proposal(principal, project, publication, id, *scope) :
                                 store_.publication_proposal(principal, project, publication, id);
    return preparations_.with_verified_candidate(principal, proposal.at("proposal").at("preparation_id").get<std::uint64_t>(),
        [&](const Json& verified) {
            return store_.resolve_publication_proposal(principal, project, publication, id, decision, revision, verified, write);
        }, write);
}

Json PublicationService::withdraw(const Principal& principal, const std::string& project, const std::string& publication,
                                   const Json& request, const std::optional<WorkspaceReadRequest>& scope) {
    if (!scope && !principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
    const auto write = scope ? std::optional(store_.capture_workspace_write(principal, project, *scope)) : std::nullopt;
    require_fields(request, {"expected_revision"});
    return store_.withdraw_publication(principal, project, publication, number(request, "expected_revision", false), write);
}

Json PublicationService::retire_history(const Principal& principal, const Json& request,
                                       const std::optional<WorkspaceWriteScope>& write) {
    if (!write && !principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
    std::shared_lock admission(review_mutex_, std::try_to_lock);
    if (!admission.owns_lock())
        throw ServiceError(503, "publication-review-busy", "An admitted publication review still needs its history.");
    return store_.apply_publication_retention(principal, request, write);
}
} // namespace shin::server
