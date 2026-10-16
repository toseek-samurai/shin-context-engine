#include "shin/server/retrieval_service.h"

#include "agent_context_pack.h"
#include "context_internal.h"
#include "shin/server/candidate_artifact.h"
#include "shin/server/active_snapshot.h"
#include "shin/server/route_files.h"
#include "shin/server/version.h"

namespace shin::server {
namespace {
namespace core = shin::context;
constexpr std::size_t kResponseBytes = 512U * 1024U;

[[noreturn]] void invalid_request() {
    throw ServiceError(400, "invalid-request", "Retrieval request fields or bounds are invalid.");
}

bool request_current(const Json& request, bool pack) {
    if (!request.is_object()) invalid_request();
    for (const auto& [key, value] : request.items()) {
        (void)value;
        if (key != "require_current" && key != (pack ? "task_request" : "query") &&
            (pack || key != "limit")) invalid_request();
    }
    if (request.contains("require_current") && !request["require_current"].is_boolean()) invalid_request();
    return request.value("require_current", true);
}

core::QueryRequest query_request(const Json& request, const core::Profile& profile, bool brief, bool current) {
    core::QueryRequest result;
    result.query = core::trim(require_text(request, "query", 4096));
    if (result.query.empty()) invalid_request();
    result.limit = brief ? 6 : 10;
    if (request.contains("limit")) {
        if (!request["limit"].is_number_integer() || request["limit"] < 1 || request["limit"] > 32)
            invalid_request();
        result.limit = request["limit"].get<int>();
    }
    result.per_doc_limit = brief ? 2 : 0;
    result.require_current = current;
    result.terms = core::parse_terms(result.query);
    core::apply_profile_query_expansion(profile, result);
    if (result.term_groups.empty() || result.term_groups.size() > core::kMaxQueryGroups) invalid_request();
    std::size_t expanded = 0;
    for (const auto& group : result.term_groups) expanded += group.size();
    if (expanded > core::kMaxExpandedQueryTerms) invalid_request();
    return result;
}

core::TaskRequest task_request(const Json& request, const core::Profile& profile) {
    if (!request.contains("task_request")) invalid_request();
    auto result = core::parse_task_request(request.at("task_request"), profile);
    if (result.budget.max_evidence_bytes > 65536 || result.budget.max_records > 32 ||
        result.budget.per_query_records > 16 || result.requirements.size() > 16) invalid_request();
    return result;
}

ServiceError retrieval_error(const core::CliError& error) {
    const int status = error.category() == "usage" || error.category() == "request" ? 400 :
                       error.category() == "resource" || error.code() == "index-unavailable" ? 503 : 409;
    // Core diagnostics may embed local paths or input snippets. Transport only their stable code.
    return ServiceError(status, error.code(), "Retrieval could not be admitted or completed; inspect the configured route with the CLI.");
}
} // namespace

RetrievalService::RetrievalService(const Config& config, const RouteCatalog& catalog)
    : config_(config), state_identity_(route_directory_identity(config.preparations.state_dir)),
      catalog_(catalog),
      cache_(config.retrieval), telemetry_(config.observability) {}

Json RetrievalService::retrieve(Store& store, const Principal& principal, const std::string& project_id,
                                 const std::string& route_id, std::string_view operation, const Json& request) {
    return retrieve_selected(store, principal, project_id, route_id, operation, request, false, [&] {
        auto selected = store.active_route(principal, project_id, route_id);
        selected["source_route_id"] = route_id;
        return selected;
    });
}

Json RetrievalService::retrieve_publication(Store& store, const Principal& principal, const std::string& project_id,
                                             const std::string& publication, std::string_view operation, const Json& request) {
    return retrieve_selected(store, principal, project_id, publication, operation, request, true,
        [&] { return store.publication_head(principal, project_id, publication); });
}

Json RetrievalService::retrieve_selected(Store& store, const Principal& principal, const std::string& project_id,
                                          const std::string& name, std::string_view operation, const Json& request,
                                          bool publication_selection, const std::function<Json()>& selection,
                                          const std::optional<WorkspaceReadRequest>& scope) {
    // Authorize against the durable registry before inspecting any route or filesystem input.
    if (scope) {
        (void)store.scoped_workspace(principal, project_id, *scope);
        try {
            if (!publication_selection && catalog_.find(project_id, name).route.workspace_id != scope->workspace_id)
                throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
        } catch (const ServiceError& error) {
            if (error.status == 404) throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
            throw;
        }
    } else (void)store.get_project(principal, project_id);
    std::optional<RetrievalTelemetry::Attribution> attribution;
    if (scope && !principal.admin)
        attribution = RetrievalTelemetry::Attribution{principal.id, scope->tenant_id, scope->workspace_id};
    const ForegroundActivity activity(foreground_);
    if (operation != "search" && operation != "brief" && operation != "pack") invalid_request();
    const auto kind = operation == "search" ? RetrievalOperation::search :
                      operation == "brief" ? RetrievalOperation::brief : RetrievalOperation::pack;
    auto attempt = telemetry_.begin(kind, publication_selection ? RetrievalSelector::publication : RetrievalSelector::route);
    attempt.attribution = std::move(attribution);
    try {
        auto result = execute_selected(store, principal, project_id, name, operation, request, publication_selection, selection, attempt, scope);
        telemetry_.finish(attempt, project_id, name, request, &result, 200, "");
        return result;
    } catch (const ServiceError& error) {
        telemetry_.finish(attempt, project_id, name, request, nullptr, error.status, error.code);
        throw;
    } catch (...) {
        telemetry_.finish(attempt, project_id, name, request, nullptr, 500, "internal-error");
        throw;
    }
}

Json RetrievalService::execute_selected(Store& store, const Principal& principal, const std::string& project_id,
                                         const std::string& name, std::string_view operation, const Json& request,
                                         bool publication_selection, const std::function<Json()>& selection,
                                         RetrievalTelemetry::Attempt& attempt, const std::optional<WorkspaceReadRequest>& scope) {
    try {
        if (!publication_selection) (void)catalog_.find(project_id, name);
        const bool pack = operation == "pack";
        const bool current = request_current(request, pack);
        attempt.mark(RetrievalPhase::selection);
        const auto active = selection();
        if (publication_selection && active.at("state") == "withdrawn")
            throw ServiceError(410, "publication-withdrawn", "The shared publication was explicitly withdrawn; inspect its head before proposing a replacement.");
        if (publication_selection && active.at("preparation_id").is_null())
            throw ServiceError(503, "active-generation-missing", "No explicitly accepted generation is selected.");
        attempt.mark(RetrievalPhase::admission);
        const auto source_route = active.at("source_route_id").get<std::string>();
        const auto& binding = catalog_.find(project_id, source_route);
        std::optional<core::QueryRequest> query;
        std::optional<core::TaskRequest> task;
        if (pack) task = task_request(request, binding.options.profile);
        else query = query_request(request, binding.options.profile, operation == "brief", current);
        attempt.request_validated = true;
        const ActiveSnapshot access(config_, catalog_, cache_, store, principal, project_id, name,
                                    publication_selection, active, state_identity_, selection, scope);
        const auto& snapshot = access.snapshot();
        attempt.mark(RetrievalPhase::execution);
        core::RetrievalSession session(snapshot, current);
        Json evidence;
        if (pack) {
            const auto result = core::execute_agent_context_pack(session.options(), *task, session, 1);
            evidence = core::render_agent_context_pack(session.options(),
                {.projection = core::PackProjection::compact, .require_current = current, .strict = false},
                *task, session, result);
        } else if (operation == "brief") evidence = session.brief(*query, core::BriefProjection::compact);
        else evidence = session.search(*query);
        attempt.mark(RetrievalPhase::finalization);
        // CLI routing coordinates are host-private; canonical records and receipt hashes stay intact.
        evidence.erase("root");
        evidence.erase("out_root");
        access.verify();
        Json result{{"project_id", project_id}, {"workspace_id", binding.route.workspace_id},
                    {"source_host_id", binding.route.source_host_id}, {"route_id", source_route}, {"snapshot_id", snapshot->id()},
                    {"preparation_id", active.at("preparation_id")}, {"revision", active.at("revision")},
                    {"admission", current ? "exact-current" : "snapshot-only"}, {"evidence", std::move(evidence)}};
        if (publication_selection) result["publication_id"] = name;
        if (scope) result["tenant_id"] = scope->tenant_id;
        attempt.result_bytes = result.dump().size();
        if (*attempt.result_bytes > kResponseBytes)
            throw ServiceError(413, "retrieval-response-too-large", "Narrow the query or evidence budget.");
        return result;
    } catch (const core::CliError& error) {
        throw retrieval_error(error);
    }
}

Json RetrievalService::retrieve_workspace(Store& store, const Principal& principal, const std::string& project,
                                          const std::string& route, const WorkspaceReadRequest& scope,
                                          std::string_view operation, const Json& request) {
    return retrieve_selected(store, principal, project, route, operation, request, false, [&] {
        auto selected = store.scoped_active_route(principal, catalog_.find(project, route).route, scope);
        selected["source_route_id"] = route;
        return selected;
    }, scope);
}

Json RetrievalService::cache_statistics(const Principal& principal) const {
    if (!principal.admin) throw ServiceError(403, "forbidden", "This operation requires administrative permission.");
    return cache_.statistics();
}

Json RetrievalService::retrieve_workspace_publication(Store& store, const Principal& principal, const std::string& project,
                                                       const std::string& publication, const WorkspaceReadRequest& scope,
                                                       std::string_view operation, const Json& request) {
    return retrieve_selected(store, principal, project, publication, operation, request, true,
        [&] { return store.scoped_publication_head(principal, project, publication, scope); }, scope);
}

Json RetrievalService::telemetry_metrics(const Principal& principal) const {
    if (!principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
    return telemetry_.metrics();
}

Json RetrievalService::capture_journal(Store& store, const Principal& principal) const {
    if (!principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
    return capture_journal_batch(store, principal, journal_snapshot());
}

Json RetrievalService::journal_snapshot(std::uint64_t after) const {
    auto batch = telemetry_.journal_snapshot(after);
    batch["source_host_id"] = config_.database.source_host_id;
    batch["server_version"] = kServerVersion;
    batch["core_version"] = SHIN_VERSION;
    for (auto& record : batch.at("records")) {
        record["context"] = nullptr;
        if (!record.at("selection").is_null()) {
            const auto& binding = catalog_.find(record.at("project_id").get<std::string>(),
                record.at("selection").at("route_id").get<std::string>());
            record["context"] = {{"profile_id", binding.options.profile.id},
                {"profile_fingerprint", binding.profile_bytes_hash}, {"route_fingerprint", binding.fingerprint}};
        }
    }
    return batch;
}

Json RetrievalService::capture_journal_batch(Store& store, const Principal& principal, const Json& snapshot) const {
    if (!principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
    auto batch = snapshot;
    bool newly_admitted = false;
    {
        std::scoped_lock lock(journal_registration_mutex_);
        if (journal_epoch_ == 0) {
            const auto registration = store.register_journal_producer(principal,
                Json{{"instance_id", batch.at("instance_id")}, {"source_host_id", batch.at("source_host_id")},
                     {"server_version", batch.at("server_version")}, {"core_version", batch.at("core_version")}});
            journal_epoch_ = registration.at("producer_epoch").get<std::int64_t>();
            newly_admitted = !registration.at("replayed").get<bool>();
        }
        // Keep the lifetime binding even after reclamation; capture must never reenroll old observations.
        batch["producer_epoch"] = journal_epoch_;
    }
    auto captured = store.capture_retrieval_journal(principal, batch);
    if (captured.at("schema_version") != "shin.retrieval-journal-capture.v1" ||
        captured.at("instance_id") != batch.at("instance_id") || captured.at("producer_epoch") != batch.at("producer_epoch"))
        throw ServiceError(500, "journal-writer-invalid-receipt", "Journal capture acknowledgement changed its binding.");
    if (newly_admitted) captured["replayed"] = false;
    return captured;
}

Json RetrievalService::telemetry_requests(const Principal& principal, std::uint64_t after, std::size_t limit,
                                         std::string_view instance) const {
    if (!principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
    return telemetry_.requests(after, limit, instance);
}

} // namespace shin::server
