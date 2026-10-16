#include "shin/server/preparation_service.h"
#include "shin/server/candidate_artifact.h"
#include "shin/server/retention_files.h"

namespace shin::server {
namespace {

bool terminal(const Json& input) {
    const auto& state = input.at("job").at("state");
    return state == "prepared" || state == "failed" || state == "cancelled";
}

Json retention_report(const Json& input) {
    const auto& retention = input.at("retention");
    const bool released = !retention.is_null() && retention.at("state") == "released";
    return {{"schema_version", "shin.candidate-retention.v1"}, {"job_id", input.at("job").at("id")},
        {"state", retention.is_null() ? Json("retained") : retention.at("state")},
        {"plan_id", retention.is_null() ? Json(nullptr) : retention.at("plan_id")},
        {"can_apply", false}, {"reservation_held", !released}, {"artifact_bytes", nullptr}, {"entries", nullptr},
        {"reason_codes", Json::array()}, {"mutation_performed", false}, {"automatic_execution", false}};
}

std::string plan_id(const Json& input, const Json& inventory) {
    return digest_hex(sha256(Json{{"schema_version", "shin.retention-plan.v1"}, {"job", input.at("job")},
        {"artifact_namespace", input.at("artifact_namespace")}, {"artifact_key", input.at("artifact_key")},
        {"inventory", inventory}}.dump()));
}

void require_binding(const Json& input, const std::string& expected_namespace) {
    if (input.at("artifact_namespace") != expected_namespace || !input.at("artifact_key").is_string())
        throw ServiceError(409, "retention-artifact-unbound", "Job has no matching managed artifact namespace.");
}

std::unique_ptr<CandidateArtifactLock> lock_artifact(const std::filesystem::path& root,
    const std::string& identity, const Json& input, CandidateArtifactLock::Mode mode) {
    try {
        return std::make_unique<CandidateArtifactLock>(root, identity, input.at("job").at("id").get<std::uint64_t>(),
                                                      input.at("artifact_key").get<std::string>(), mode);
    } catch (const ServiceError& error) {
        // A cancelled never-claimed row cannot have a delayed writer. Claimed rows need their existing inode.
        if (error.code == "candidate-lock-missing" && input.at("worker_id").is_null()) return {};
        throw;
    }
}

void require_matching_subset(const Json& current, const Json& expected) {
    if (!expected.is_object() || !expected.contains("entries") || !expected.at("entries").is_array())
        throw ServiceError(409, "retention-plan-invalid", "Stored cleanup plan is invalid.");
    for (const auto& entry : current.at("entries")) {
        if (std::find(expected.at("entries").begin(), expected.at("entries").end(), entry) == expected.at("entries").end())
            throw ServiceError(409, "retention-tree-changed", "Candidate changed after the cleanup intent.");
    }
}

void require_plan_identity(const Json& input, const Json& inventory, const std::string& expected_id) {
    if (plan_id(input, inventory) != expected_id)
        throw ServiceError(409, "retention-plan-changed", "Review a current retention plan before applying it.");
}
} // namespace

Json PreparationService::retention_plan(const Principal& principal, std::uint64_t id,
                                       const std::optional<WorkspaceWriteScope>& write) {
    const auto input = store_.preparation_cleanup_input(principal, id, write);
    auto report = retention_report(input);
    if (store_.preparation_is_active(principal, id, write)) {
        report["reason_codes"].push_back("candidate-active");
        return report;
    }
    if (report.at("state") == "released") return report;
    if (!terminal(input)) {
        report["reason_codes"].push_back("retention-job-active");
        return report;
    }
    std::unique_lock admission(retention_mutex_, std::try_to_lock);
    if (!admission.owns_lock()) throw ServiceError(503, "retention-busy", "Another retention operation is in progress.");
    try {
        require_binding(input, artifact_namespace());
        verify_state();
        const auto lifetime = lock_artifact(config_.preparations.state_dir, state_identity_, input,
                                             CandidateArtifactLock::Mode::inspect_existing);
        const auto name = candidate_artifact_name(id, input.at("artifact_key").get<std::string>());
        const auto inventory = inspect_retention_tree(config_.preparations.state_dir / name,
            input.at("job").at("reserved_artifact_bytes").get<std::uint64_t>());
        if (!lifetime && inventory.at("root_present") == true)
            throw ServiceError(409, "retention-tree-unsafe", "An unclaimed job cannot own a candidate tree.");
        if (input.at("retention").is_null()) {
            report["plan_id"] = plan_id(input, inventory);
        } else {
            const auto& retained = input.at("retention");
            require_plan_identity(input, retained.at("inventory"), retained.at("plan_id").get<std::string>());
            require_matching_subset(inventory, retained.at("inventory"));
        }
        verify_state();
        if (lifetime) lifetime->verify();
        if (store_.preparation_cleanup_input(principal, id, write) != input)
            throw ServiceError(409, "retention-job-changed", "Job changed during retention planning.");
        report["can_apply"] = principal.admin || write.has_value();
        report["artifact_bytes"] = inventory.at("payload_bytes");
        report["entries"] = inventory.at("entries").size();
    } catch (const ServiceError& error) { report["reason_codes"].push_back(error.code); }
    catch (const std::exception&) { report["reason_codes"].push_back("retention-inspection-unavailable"); }
    // Scope failures must not turn into a report containing previously read metadata.
    if (write && store_.preparation_cleanup_input(principal, id, write) != input)
        throw ServiceError(409, "retention-job-changed", "Job changed during retention planning.");
    return report;
}

Json PreparationService::apply_retention(const Principal& principal, std::uint64_t id, const Json& request,
                                        const std::optional<WorkspaceWriteScope>& write) {
    if (!write && !principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
    require_fields(request, {"expected_plan_id"});
    const auto expected_id = require_text(request, "expected_plan_id", 64);
    if (expected_id.size() != 64 || !std::all_of(expected_id.begin(), expected_id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) throw ServiceError(400, "invalid-request", "Invalid retention plan identity.");
    auto input = store_.preparation_cleanup_input(principal, id, write);
    if (store_.preparation_is_active(principal, id, write))
        throw ServiceError(409, "candidate-active", "An active generation cannot be deleted.");
    if (!input.at("retention").is_null() && input.at("retention").at("plan_id") != expected_id)
        throw ServiceError(409, "retention-plan-conflict", "Another retention plan already owns this job.");
    if (!input.at("retention").is_null() && input.at("retention").at("state") == "released")
        return retention_report(input);
    if (!terminal(input)) throw ServiceError(409, "retention-job-active", "Only terminal preparations can be cleaned.");
    std::unique_lock admission(retention_mutex_, std::try_to_lock);
    if (!admission.owns_lock()) throw ServiceError(503, "retention-busy", "Another retention operation is in progress.");
    require_binding(input, artifact_namespace());
    verify_state();
    auto lifetime = lock_artifact(config_.preparations.state_dir, state_identity_, input,
                                   CandidateArtifactLock::Mode::cleanup_existing);
    const auto key = input.at("artifact_key").get<std::string>();
    const auto name = candidate_artifact_name(id, key);
    const auto maximum_bytes = input.at("job").at("reserved_artifact_bytes").get<std::uint64_t>();
    const auto current = inspect_retention_tree(config_.preparations.state_dir / name, maximum_bytes);
    if (!lifetime && current.at("root_present") == true)
        throw ServiceError(409, "retention-tree-unsafe", "An unclaimed job cannot own a candidate tree.");
    const auto inventory = input.at("retention").is_null() ? current : input.at("retention").at("inventory");
    require_plan_identity(input, inventory, expected_id);
    require_matching_subset(current, inventory);
    if (!lifetime)
        lifetime = std::make_unique<CandidateArtifactLock>(config_.preparations.state_dir, state_identity_, id, key,
                                                           CandidateArtifactLock::Mode::create_writer);
    // No destructive IO until this intent is confirmed committed. An uncertain commit is retried explicitly.
    const auto intent = store_.begin_retention(principal, id, input, expected_id, inventory, write);
    if (intent.at("state") != "released") {
        verify_state();
        lifetime->verify();
        remove_retention_tree(config_.preparations.state_dir, state_identity_, name, intent.at("inventory"), maximum_bytes);
        verify_state();
        lifetime->verify();
        input["retention"] = store_.finish_retention(principal, id, expected_id, artifact_namespace(), key, write);
    } else {
        input["retention"] = intent;
    }
    auto report = retention_report(input);
    report["mutation_performed"] = intent.at("state") != "released";
    return report;
}

} // namespace shin::server
