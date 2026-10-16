#include "shin/server/continuity_service.h"

namespace shin::server {
namespace {

void schema(const Json& input, const char* expected) {
    if (input.at("schema_version") != expected)
        throw ServiceError(400, "invalid-request", "Continuity request schema is invalid.");
}

CheckpointMutation mutation(const Json& input) {
    const auto& revision = input.at("expected_revision");
    const auto& deadline = input.at("request_deadline");
    if (!revision.is_number_integer() || revision < 0 || revision > INT64_MAX ||
        !deadline.is_number_integer() || deadline < 0 || deadline > 253402300799LL)
        throw ServiceError(400, "invalid-request", "Checkpoint revision or retry deadline is invalid.");
    CheckpointMutation result{require_text(input, "task_id", 128), require_text(input, "thread_id", 128),
        require_text(input, "request_id", 32), digest_hex(sha256(input.dump())),
        revision.get<std::uint64_t>(), deadline.get<std::int64_t>()};
    validate_checkpoint_mutation(result);
    return result;
}

Json result_record(const Json& record) {
    auto result = checkpoint_public_record(record);
    result["replayed"] = record.value("replayed", false);
    return result;
}

Json public_actions(const Json& actions) {
    Json result = Json::array();
    for (const auto& action : actions) {
        Json item = Json::object();
        for (const auto* key : {"code", "id", "receipt_id", "text", "source", "review_required", "automatic_execution"})
            if (action.contains(key)) item[key] = action.at(key);
        item["automatic_execution"] = false;
        result.push_back(std::move(item));
    }
    return result;
}

Json public_assessment(const Json& value, bool impact) {
    Json result;
    if (impact) {
        result = value;
        result.erase("root");
        result.erase("out_root");
        if (result.contains("worktree")) result["worktree"].erase("root");
        if (result.contains("changes")) {
            result["changes"].erase("root");
            if (result["changes"].contains("worktree")) result["changes"]["worktree"].erase("root");
        }
        result["schema_version"] = "shin.task-impact-assessment.v1";
    } else {
        for (const auto* key : {"ok", "status", "checkpoint_id", "observed_at", "clock_source", "checkpoint_age",
            "alignment", "same_worktree", "same_snapshot", "profile_config_current", "generator_current", "index_current",
            "snapshot_inputs_current", "index_freshness", "observation_complete", "snapshot_id", "checkpoint_drift",
            "published_drift", "objective", "current_task", "task_wording_changed", "state", "vcs", "receipts",
            "continuation_ready", "review_required", "require_current", "pack_comparison", "continuation_pack",
            "binding_differences", "next_action"}) {
            if (value.contains(key)) result[key] = value.at(key);
        }
        if (result.contains("published_drift")) {
            result["published_drift"].erase("root");
            if (result["published_drift"].contains("worktree")) result["published_drift"]["worktree"].erase("root");
        }
        if (result.contains("continuation_pack") && !result.at("continuation_pack").is_null()) {
            result["continuation_pack"].erase("root");
            result["continuation_pack"].erase("out_root");
        }
        result["schema_version"] = "shin.task-resume-assessment.v1";
    }
    if (value.contains("next_actions")) result["next_actions"] = public_actions(value.at("next_actions"));
    if (result.contains("receipts")) {
        for (auto& receipt : result["receipts"])
            receipt["trust_source"] = receipt.value("producer_trusted", false) ?
                "server-authenticated-receipt-origin" : "server-no-active-receipt-origin";
    }
    result["transport_projection"] = "host-private-coordinates-omitted";
    return result;
}

} // namespace

Json ContinuityService::save(const Principal& principal, const std::string& project, const std::string& slot,
                             const Json& input, const std::optional<WorkspaceReadRequest>& scope) {
    const auto admission = scope ? store_.scoped_workspace(principal, project, *scope) : Json();
    if (!scope) (void)store_.get_project(principal, project);
    require_fields(input, {"schema_version", "selector", "task_id", "thread_id", "expected_revision", "request_id",
                           "request_deadline", "require_current", "draft"});
    schema(input, "shin.task-checkpoint-write.v1");
    if (!input.at("require_current").is_boolean())
        throw ServiceError(400, "invalid-request", "Checkpoint currentness policy must be boolean.");
    const auto request = mutation(input);
    const auto replay = store_.checkpoint_replay(principal, project, slot, request, "save", scope);
    if (!replay.is_null()) return result_record(replay);
    const auto evaluated = retrieval_.evaluate_continuity(store_, principal, project, input.at("selector"), "checkpoint",
        request.task_id, request.thread_id, input.at("draft").dump(), input.at("require_current").get<bool>(), Json(), Json::array(), scope);
    return result_record(store_.save_checkpoint(principal, project, slot, request, evaluated.at("provenance"),
                                                evaluated.at("value").dump(), scope, admission));
}

Json ContinuityService::resume(const Principal& principal, const std::string& project, const std::string& slot,
                               const Json& input, bool impact) {
    return resume_selected(principal, project, slot, input, impact, std::nullopt);
}

Json ContinuityService::resume_workspace(const Principal& principal, const std::string& project, const std::string& slot,
                                        const WorkspaceReadRequest& scope, const Json& input, bool impact, bool route_only) {
    return resume_selected(principal, project, slot, input, impact, scope, route_only);
}

Json ContinuityService::resume_selected(const Principal& principal, const std::string& project, const std::string& slot,
                                       const Json& input, bool impact, const std::optional<WorkspaceReadRequest>& scope, bool route_only) {
    if (scope) (void)store_.scoped_workspace(principal, project, *scope);
    else (void)store_.get_project(principal, project);
    require_fields(input, {"schema_version", "task_id", "thread_id", "require_current"});
    schema(input, impact ? "shin.task-impact-request.v1" : "shin.task-resume-request.v1");
    const auto task = require_text(input, "task_id", 128);
    const auto thread = require_text(input, "thread_id", 128);
    if (!input.at("require_current").is_boolean())
        throw ServiceError(400, "invalid-request", "Continuity currentness policy must be boolean.");
    const auto record = scope ? store_.scoped_checkpoint_record(principal, project, slot, *scope) :
                                store_.checkpoint_record(principal, project, slot);
    if (record.is_null() || record.at("task_id") != task || record.at("thread_id") != thread)
        throw ServiceError(404, "checkpoint-not-found", "Private checkpoint is unavailable for this task.");
    if (record.at("state") != "active")
        throw ServiceError(410, "checkpoint-cleared", "Private checkpoint was explicitly cleared.");
    if (route_only && record.at("provenance").at("selector").at("kind") != "route")
        throw ServiceError(403, "publication-deferred", "Personal checkpoints require an active workspace route.");
    const auto evaluated = retrieval_.evaluate_continuity(store_, principal, project, record.at("provenance").at("selector"),
        impact ? "impact" : "resume", task, thread, record.at("checkpoint_text").get<std::string>(),
        input.at("require_current").get<bool>(), record.at("provenance"), record.at("receipt_origins"), scope);
    if (scope && store_.scoped_checkpoint_record(principal, project, slot, *scope) != record)
        throw ServiceError(409, "checkpoint-revision-conflict", "Checkpoint changed during evaluation; retry the current slot.");
    Json result{{"schema_version", impact ? "shin.task-impact.v1" : "shin.task-resume.v1"},
        {"checkpoint", checkpoint_public_record(record)}, {"provenance", evaluated.at("provenance")},
        {"assessment", public_assessment(evaluated.at("value"), impact)}, {"read_only", true}};
    if (scope) result["tenant_id"] = scope->tenant_id;
    if (result.dump().size() > 500U * 1024U)
        throw ServiceError(413, "continuity-response-too-large", "Reduce the checkpoint state or task evidence budget.");
    return result;
}

Json ContinuityService::import_receipts(const Principal& principal, const std::string& project, const std::string& slot,
                                       const Json& input, const std::optional<WorkspaceReadRequest>& scope) {
    if (scope) (void)store_.scoped_workspace(principal, project, *scope);
    else (void)store_.get_project(principal, project);
    if (!principal.receipt_producer)
        throw ServiceError(403, "receipt-import-forbidden", "Receipt producer permission is required.");
    require_fields(input, {"schema_version", "task_id", "thread_id", "expected_revision", "request_id", "request_deadline", "receipts"});
    schema(input, "shin.task-receipt-import.v1");
    return result_record(store_.import_checkpoint_receipts(principal, project, slot, mutation(input), input.at("receipts"), scope));
}

Json ContinuityService::clear(const Principal& principal, const std::string& project, const std::string& slot,
                              const Json& input, const std::optional<WorkspaceReadRequest>& scope) {
    if (scope) (void)store_.scoped_workspace(principal, project, *scope);
    else (void)store_.get_project(principal, project);
    require_fields(input, {"schema_version", "task_id", "thread_id", "expected_revision", "request_id", "request_deadline"});
    schema(input, "shin.task-checkpoint-clear.v1");
    return result_record(store_.clear_checkpoint(principal, project, slot, mutation(input), scope));
}

} // namespace shin::server
