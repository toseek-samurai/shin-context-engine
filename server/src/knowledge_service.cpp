#include "shin/server/knowledge_service.h"
#include "shin/server/knowledge_health.h"
#include "shin/server/session_bootstrap.h"
#include "context_internal.h"

namespace shin::server {
namespace {
Principal configured_actor(const Config& config) {
    if (!config.knowledge.enabled) return {"knowledge-disabled", false, {}};
    for (const auto& credential : config.credentials)
        if (credential.principal.id == config.knowledge.principal_id && credential.principal.admin) return credential.principal;
    throw ServiceError(400, "invalid-knowledge-policy", "Knowledge principal is not configured.");
}
std::vector<KnowledgeTarget> targets(const Config& config) {
    std::vector<KnowledgeTarget> result;
    for (const auto& id : config.knowledge.route_ids)
        for (const auto& route : config.retrieval.routes)
            if (route.id == id) result.push_back({route.project_id, route.id});
    return result;
}
void admin(const Principal& principal) {
    if (!principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
}
}

KnowledgeService::KnowledgeService(const Config& config, const RouteCatalog& catalog, Store& store,
                                   RetrievalService& retrieval, PreparationService& preparations)
    : config_(config), catalog_(catalog), store_(store), retrieval_(retrieval), preparations_(preparations),
      actor_(configured_actor(config)), scheduler_(config.knowledge, targets(config),
                                                [this](const KnowledgeTarget& target) { return inspect(target); }) {}

Json KnowledgeService::status(const Principal& principal) const {
    admin(principal);
    return scheduler_.status();
}

Json KnowledgeService::routes(const Principal& principal, const std::string& project) {
    (void)store_.get_project(principal, project);
    Json items = Json::array();
    for (const auto& route : config_.retrieval.routes) {
        if (route.project_id != project) continue;
        const bool monitored = config_.knowledge.enabled &&
            std::find(config_.knowledge.route_ids.begin(), config_.knowledge.route_ids.end(), route.id) != config_.knowledge.route_ids.end();
        items.push_back({{"route_id", route.id}, {"workspace_id", route.workspace_id},
            {"source_host_id", route.source_host_id}, {"profile_id", route.profile}, {"monitored", monitored}});
    }
    return {{"schema_version", "shin.knowledge-routes.v1"}, {"project_id", project},
        {"routes", items}, {"read_only", true}, {"scope", "configured-server-local-routes"}, {"health_assessed", false}};
}

Json KnowledgeService::refresh(const Principal& principal, const Json& request) {
    admin(principal);
    require_fields(request, {});
    return scheduler_.refresh();
}

Json KnowledgeService::inspect(const KnowledgeTarget& target) {
    Json report;
    try { report = retrieval_.inspect_route_health(store_, actor_, target.project_id, target.route_id); }
    catch (const context::CliError& error) {
        throw ServiceError(409, error.code(), "Knowledge assessment could not complete.");
    }
    report["maintenance_action"] = nullptr;
    if (config_.knowledge.prepare_on_drift && report.at("index_current") == false &&
        report.at("vacuum").at("broken_local_links") == 0 &&
        report.at("expected_snapshot_id") != report.at("generation").at("snapshot_id")) {
        const auto key = "knowledge-" + digest_hex(sha256(Json{{"generation", report.at("generation")},
            {"expected", report.at("expected_snapshot_id")}}.dump()));
        report["mutations_performed"] = nullptr;
        try {
            const auto prepared = preparations_.enqueue(actor_, target.project_id, target.route_id,
                Json{{"expected_snapshot_id", report.at("generation").at("snapshot_id")}}, key);
            report["maintenance_action"] = {{"state", "candidate-requested"}, {"job_id", prepared.at("job").at("id")},
                {"job_state", prepared.at("job").at("state")}, {"replayed", prepared.at("replayed")},
                {"next_action", prepared.at("job").at("state") == "prepared" ? "review-preflight-and-activate" :
                    prepared.at("job").at("state") == "failed" || prepared.at("job").at("state") == "cancelled" ?
                    "review-and-explicitly-reprepare" : "wait-for-candidate"}, {"automatic_retry_epoch", false}};
            report["mutations_performed"] = prepared.at("replayed") != true;
        } catch (const ServiceError& error) {
            report["maintenance_action"] = {{"state", "preparation-unavailable"}, {"reason_code", error.code},
                                             {"completion_uncertain", true}};
        }
    }
    report["source_mutated"] = false;
    report["publication_mutated"] = false;
    return report;
}

Json KnowledgeService::health(const Principal& principal, const std::string& project, const std::string& route) {
    (void)store_.get_project(principal, project);
    return selected_health(principal, project, route, store_.active_route(principal, project, route), false);
}

Json KnowledgeService::publication_health(const Principal& principal, const std::string& project, const std::string& publication) {
    (void)store_.get_project(principal, project);
    const auto active = store_.publication_head(principal, project, publication);
    if (active.at("state") == "withdrawn")
        throw ServiceError(410, "publication-withdrawn", "The shared publication was explicitly withdrawn.");
    if (active.at("preparation_id").is_null())
        throw ServiceError(503, "active-generation-missing", "No explicitly accepted generation is selected.");
    auto result = selected_health(principal, project, active.at("source_route_id").get<std::string>(), active, true);
    result["current_generation"]["publication_id"] = publication;
    result["selection_revision_scope"] = "publication";
    result["assessment_scope"] = "source-generation";
    return result;
}

Json KnowledgeService::selected_health(const Principal& principal, const std::string& project,
                                       const std::string& route, const Json& active, bool publication) {
    const auto& binding = catalog_.find(project, route);
    if (publication && (active.at("workspace_id") != binding.route.workspace_id ||
        active.at("source_host_id") != binding.route.source_host_id || active.at("profile_id") != binding.options.profile.id))
        throw ServiceError(409, "active-generation-route-changed", "Selected provenance differs from the configured source route.");
    const auto workspace = store_.get_workspace(principal, project, binding.route.workspace_id);
    catalog_.verify(binding, workspace);
    auto result = scheduler_.snapshot(route);
    result["project_id"] = project;
    result["current_generation"] = knowledge_generation(binding, active);
    result["selection_aligned"] = false;
    if (!result.at("assessment").is_null() && !result.at("assessment").at("generation").is_null()) {
        result["selection_aligned"] = publication ?
            knowledge_same_source_generation(result.at("assessment").at("generation"), result.at("current_generation")) :
            result.at("assessment").at("generation") == result.at("current_generation");
        if (result.at("selection_aligned") != true) result["state"] = "generation-changed";
    }
    return result;
}

Json KnowledgeService::bootstrap(const Principal& principal, const Json& request) {
    return session_bootstrap(request, BootstrapAccess{
        [&](const std::string& project) { (void)store_.get_project(principal, project); },
        [&](const std::string& project, const BootstrapSelector& selector, const std::string& query) {
            const Json brief{{"query", query}, {"limit", 4}, {"require_current", true}};
            return selector.kind == BootstrapSelectorKind::publication ?
                retrieval_.retrieve_publication(store_, principal, project, selector.id, "brief", brief) :
                retrieval_.retrieve(store_, principal, project, selector.id, "brief", brief);
        },
        [&](const std::string& project, const BootstrapSelector& selector) {
            return selector.kind == BootstrapSelectorKind::publication ? publication_health(principal, project, selector.id) :
                health(principal, project, selector.id);
        }});
}

} // namespace shin::server
