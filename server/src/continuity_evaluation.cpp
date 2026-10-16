#include "shin/server/retrieval_service.h"
#include "shin/server/active_snapshot.h"
#include "context_internal.h"
#include "session_contract.h"
#include "impact.h"
#include <algorithm>

namespace shin::server {

Json RetrievalService::evaluate_continuity(Store& store, const Principal& principal, const std::string& project,
                                          const Json& selector, const std::string& operation, const std::string& task,
                                          const std::string& thread, const std::string& input, bool require_current,
                                          const Json& expected_provenance, const Json& receipt_origins,
                                          const std::optional<WorkspaceReadRequest>& scope) {
    if (scope) (void)store.scoped_workspace(principal, project, *scope);
    else (void)store.get_project(principal, project);
    require_fields(selector, {"kind", "id"});
    const auto kind = require_text(selector, "kind", 16);
    const auto name = require_text(selector, "id", 63);
    if ((kind != "route" && kind != "publication") || !valid_identifier(name) ||
        (operation != "checkpoint" && operation != "resume" && operation != "impact")) {
        throw ServiceError(400, "invalid-request", "Continuity selection or operation is invalid.");
    }
    const ForegroundActivity activity(foreground_);
    try {
        const bool publication = kind == "publication";
        auto select = [&]() {
            try {
                if (publication) return scope ? store.scoped_publication_head(principal, project, name, *scope) :
                                                store.publication_head(principal, project, name);
                auto active = scope ? store.scoped_active_route(principal, catalog_.find(project, name).route, *scope) :
                                      store.active_route(principal, project, name);
                active["source_route_id"] = name;
                return active;
            } catch (const ServiceError& error) {
                if (scope && error.status == 404) throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
                throw;
            }
        };
        const Json active = select();
        if (publication && active.at("state") == "withdrawn") {
            throw ServiceError(410, "publication-withdrawn", "The shared publication was explicitly withdrawn.");
        }
        const auto source_route = active.at("source_route_id").get<std::string>();
        const auto& binding = catalog_.find(project, source_route);
        Json provenance{{"project_id", project}, {"workspace_id", binding.route.workspace_id},
            {"source_host_id", binding.route.source_host_id}, {"route_id", source_route},
            {"profile_id", binding.options.profile.id}, {"selector", selector},
            {"snapshot_id", active.at("snapshot_id")}, {"preparation_id", active.at("preparation_id")},
            {"revision", active.at("revision")}};
        if (!expected_provenance.is_null()) {
            for (const auto* field : {"project_id", "workspace_id", "source_host_id", "route_id", "profile_id", "selector"}) {
                if (provenance.at(field) != expected_provenance.at(field)) {
                    throw ServiceError(409, "checkpoint-source-scope-changed", "Checkpoint belongs to a different source scope.");
                }
            }
        }
        const ActiveSnapshot access(config_, catalog_, cache_, store, principal, project, name,
                                    publication, active, state_identity_, select, scope);
        auto options = access.snapshot()->options();
        options.generated_snapshot = access.snapshot();
        context::SessionEvaluationOptions command;
        command.task_id = task;
        command.thread_id = thread;
        command.jobs = 1;
        command.require_current = require_current;
        // Origin binds exact saved records; current authorization can revoke this host policy.
        if (operation != "checkpoint" && principal.receipt_producer && !receipt_origins.empty()) {
            const auto checkpoint = context::parse_session_document(input);
            if (!checkpoint_receipt_origins_valid(checkpoint, principal.id, receipt_origins))
                throw ServiceError(500, "database-corrupt", "Stored receipt origins are invalid.");
            for (const auto& receipt : checkpoint.at("receipts")) {
                const auto canonical = receipt.dump();
                const Json digest = digest_hex(sha256(canonical));
                if (std::find(receipt_origins.begin(), receipt_origins.end(), digest) != receipt_origins.end())
                    command.trusted_receipt_documents.insert(canonical);
            }
        }
        Json value;
        if (operation == "checkpoint") value = context::evaluate_checkpoint(options, command, input);
        else if (operation == "resume") value = context::evaluate_resume(options, command, input);
        else {
            const auto checkpoint = context::parse_session_document(input);
            const auto draft = context::validate_session_checkpoint(checkpoint, options.profile);
            if (checkpoint.at("binding").at("root") != options.root.generic_string() ||
                checkpoint.at("binding").at("profile") != options.profile.id ||
                checkpoint.at("binding").at("task_id") != task || checkpoint.at("binding").at("thread_id") != thread) {
                throw ServiceError(409, "checkpoint-source-scope-changed", "Checkpoint belongs to a different source scope.");
            }
            const auto bundle = Json{{"schema_version", "shin.receipt-bundle.v1"}, {"receipts", draft.receipts}}.dump();
            context::ImpactEvaluationOptions impact;
            impact.require_current = require_current;
            impact.receipt_options = command;
            value = context::evaluate_impact(options, impact, bundle);
        }
        access.verify();
        return {{"provenance", std::move(provenance)}, {"value", std::move(value)}};
    } catch (const context::CliError& error) {
        const int status = error.category() == "usage" || error.category() == "request" ? 400 :
                           error.category() == "resource" ? 413 : 409;
        throw ServiceError(status, error.code(), "Continuity evaluation failed; inspect the authorized local route.");
    }
}

} // namespace shin::server
