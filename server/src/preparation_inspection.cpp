#include "shin/server/preparation_service.h"
#include "shin/server/candidate_artifact.h"
#include "shin/server/route_files.h"
#include "context_internal.h"
#include "generated_io.h"
#include "generated_snapshot.h"
#include "profile_quality.h"
#include "retrieval_session.h"

#include <ctime>
#include <array>

namespace shin::server {
namespace {
namespace core = shin::context;

std::string observed_at() {
    const auto now = std::time(nullptr);
    std::tm utc{};
    char formatted[32]{};
    if (::gmtime_r(&now, &utc) == nullptr ||
        std::strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
        throw ServiceError(500, "clock-unavailable", "Observation time is unavailable.");
    return formatted;
}

Json inspection_report(const Json& job) {
    return {{"schema_version", "shin.preparation-inspection.v1"}, {"scope", "candidate-observation"},
        {"job", job}, {"started_at", observed_at()}, {"finished_at", nullptr},
        {"checks", {{"route", "not-assessed"}, {"writer", "not-assessed"}, {"artifact", "not-assessed"},
                    {"ledger", "not-assessed"}, {"doctor", "not-assessed"}, {"source", "not-assessed"},
                    {"base", "not-assessed"}}},
        {"artifact_snapshot_id", nullptr}, {"artifact_bytes", nullptr},
        {"candidate_verified", false}, {"activation_supported", true}, {"activation_ready", false},
        {"project_quality_assessed", false}, {"mutations_performed", false},
        {"reason_codes", Json::array()}};
}

void record_failure(Json& report, const std::string& stage, const std::string& code) {
    std::string state = "unavailable";
    if (stage == "artifact") state = code == "candidate-artifact-missing" ? "missing" : "invalid";
    if (stage == "writer" && code == "candidate-artifact-busy") state = "busy";
    constexpr std::array<std::string_view, 6> changed_routes{
        "preparation-route-changed", "route-project-mismatch", "root-identity-changed",
        "route-output-changed", "route-profile-changed", "preparation-root-changed"};
    if (stage == "route" && std::find(changed_routes.begin(), changed_routes.end(), code) != changed_routes.end())
        state = "changed";
    report["checks"][stage] = state;
    report["reason_codes"].push_back(code);
}

Json unassessed_suite() {
    return {{"state", "not-assessed"}, {"total", nullptr}, {"passed", nullptr},
            {"failed", nullptr}, {"query_total", nullptr}, {"query_passed", nullptr},
            {"resource_check", nullptr}};
}

Json observe_suite(core::RetrievalSession& session, const std::string& name,
                   core::SuiteEvaluation (*evaluate)(core::RetrievalSession&), Json& reasons) {
    try {
        const auto result = evaluate(session);
        const std::string state = result.total == 0 ? "not-configured" :
                                  result.failed == 0 ? "passed" : "failed";
        if (state != "passed") reasons.push_back("candidate-" + name + "-" + state);
        return {{"state", state}, {"total", result.total}, {"passed", result.passed},
                {"failed", result.failed}, {"query_total", result.query_total},
                {"query_passed", result.query_passed},
                {"resource_check", name == "eval"
                    ? core::eval_resource_check_json(session.options().profile, result) : Json(nullptr)}};
    } catch (const std::exception&) {
        // Profile diagnostics can contain private source paths; HTTP exposes counters and stable codes only.
        auto result = unassessed_suite();
        result["state"] = "unavailable";
        reasons.push_back("candidate-" + name + "-unavailable");
        return result;
    }
}

void observe_quality(Json& report, core::RetrievalSession& session) {
    try {
        const auto profile = core::validate_profile(session.options(), true);
        const bool valid = profile.counts.errors == 0 && profile.counts.warnings == 0;
        report["profile_check"] = {{"state", valid ? "passed" : "failed"},
                                   {"errors", profile.counts.errors}, {"warnings", profile.counts.warnings}};
        if (!valid) {
            report["reason_codes"].push_back("candidate-profile-failed");
            return;
        }
    } catch (const std::exception&) {
        report["profile_check"]["state"] = "unavailable";
        report["reason_codes"].push_back("candidate-profile-unavailable");
        return;
    }
    auto& quality = report["quality"];
    quality["eval"] = observe_suite(session, "eval", core::evaluate_profile_cases, report["reason_codes"]);
    quality["pack_eval"] = observe_suite(session, "pack-eval", core::evaluate_pack_cases, report["reason_codes"]);
    const auto completed = [](const Json& suite) {
        return suite.at("state") == "passed" || suite.at("state") == "failed";
    };
    report["project_quality_assessed"] = completed(quality["eval"]) && completed(quality["pack_eval"]);
    report["quality_passed"] = quality["eval"]["state"] == "passed" && quality["pack_eval"]["state"] == "passed";
}

bool structural_checks_passed(const Json& report) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 7> expected{{
        {"route", "matched"}, {"writer", "quiescent"}, {"artifact", "verified"},
        {"ledger", "matched"}, {"doctor", "passed"}, {"source", "current"}, {"base", "matched"}}};
    return std::all_of(expected.begin(), expected.end(), [&](const auto& check) {
        return report.at("checks").at(check.first) == check.second;
    });
}
} // namespace

Json PreparationService::inspect(const Principal& principal, std::uint64_t id, const std::optional<WorkspaceWriteScope>& write) {
    return observe_candidate(principal, id, Observation::inspection, {}, write);
}

Json PreparationService::preflight(const Principal& principal, std::uint64_t id, const std::optional<WorkspaceWriteScope>& write) {
    return observe_candidate(principal, id, Observation::preflight, {}, write);
}

Json PreparationService::observe_candidate(const Principal& principal, std::uint64_t id, Observation mode,
                                           const std::function<Json(const Json&)>& on_verified,
                                           const std::optional<WorkspaceWriteScope>& write) {
    // Scope filtering is a DB operation and precedes every artifact or route access.
    const auto input = store_.preparation_inspection_input(principal, id, write);
    auto report = inspection_report(input.at("job"));
    if (mode != Observation::inspection) {
        report["schema_version"] = "shin.preparation-preflight.v1";
        report["quality"] = {{"eval", unassessed_suite()}, {"pack_eval", unassessed_suite()}};
        report["profile_check"] = {{"state", "not-assessed"}, {"errors", nullptr}, {"warnings", nullptr}};
        report["quality_passed"] = false;
        report["preflight_passed"] = false;
    }
    if (!input.at("retention").is_null()) {
        if (mode == Observation::activation)
            throw ServiceError(409, "candidate-retention-pending", "Candidate is reserved for retention.");
        report["reason_codes"].push_back(input.at("retention").at("state") == "released" ? "candidate-released" : "candidate-retention-pending");
        report["finished_at"] = observed_at();
        return report;
    }
    if (input.at("job").at("state") != "prepared") {
        if (mode == Observation::activation)
            throw ServiceError(409, "candidate-not-prepared", "Activation requires a prepared candidate.");
        report["reason_codes"].push_back("candidate-not-prepared");
        report["finished_at"] = observed_at();
        return report;
    }
    std::unique_lock admission(inspection_mutex_, std::try_to_lock);
    if (!admission.owns_lock())
        throw ServiceError(503, "preparation-inspection-busy", "Another candidate inspection is in progress.");
    std::string stage = "route";
    try {
        const auto& job = input.at("job");
        const auto project_id = job.at("project_id").get<std::string>();
        const auto& binding = catalog_.find(project_id, job.at("route_id").get<std::string>());
        if (write && binding.route.workspace_id != write->workspace.workspace_id)
            throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
        const auto workspace = write ? write->admission.at("workspace") : store_.get_workspace(principal, project_id, binding.route.workspace_id);
        if (fingerprint(binding) != input.at("route_fingerprint").get<std::string>())
            throw ServiceError(409, "preparation-route-changed", "Preparation route changed since enqueue.");
        catalog_.verify(binding, workspace);
        verify_state();
        if (input.at("artifact_namespace") != artifact_namespace() || !input.at("artifact_key").is_string())
            throw ServiceError(409, "preparation-artifact-unbound", "Preparation has no matching artifact namespace.");
        report["checks"][stage] = "matched";

        stage = "writer";
        const auto key = input.at("artifact_key").get<std::string>();
        const CandidateArtifactLock lifetime(config_.preparations.state_dir, state_identity_, id, key,
                                              CandidateArtifactLock::Mode::inspect_existing);
        report["checks"][stage] = "quiescent";
        auto options = binding.options;
        options.out_root = config_.preparations.state_dir / candidate_artifact_name(id, key);
        stage = "artifact";
        const auto artifact_identity = verify_candidate_layout(options.out_root, config_.retrieval.max_file_bytes);
        // Core's existing read lock also excludes an explicitly invoked Core publisher.
        auto publication_lock = core::lock_index_for_read(options.out_root);
        const auto snapshot = core::GeneratedSnapshot::load(options,
            {config_.retrieval.max_generation_bytes, config_.retrieval.max_file_bytes});
        report["checks"][stage] = "verified";
        report["artifact_snapshot_id"] = snapshot->id();
        report["artifact_bytes"] = snapshot->payload_bytes();
        stage = "ledger";
        if (job.at("artifact_snapshot_id") != snapshot->id() || job.at("artifact_bytes") != snapshot->payload_bytes()) {
            report["checks"][stage] = "mismatch";
            report["reason_codes"].push_back("candidate-ledger-mismatch");
        } else {
            report["checks"][stage] = "matched";
        }

        stage = "base";
        const auto expected_base = input.at("expected_snapshot_id").get<std::string>();
        const auto matches_base = [&](const Json& base) {
            return base.at("revision") == input.at("expected_route_revision") &&
                (base.at("snapshot_id").is_null() ? "" : base.at("snapshot_id").get<std::string>()) == expected_base;
        };
        const auto select_base = [&] { return write ? store_.scoped_active_route(principal, binding.route, write->workspace) :
                                                       store_.active_route(principal, project_id, binding.route.id); };
        const auto base_before = select_base();
        stage = "doctor";
        const auto health = core::inspect_generated_kb_health(options, true);
        const bool healthy = health.counts.errors == 0 && health.counts.warnings == 0;
        report["checks"][stage] = healthy ? "passed" : "failed";
        if (!healthy) report["reason_codes"].push_back("candidate-doctor-failed");
        stage = "source";
        const auto freshness = core::assess_index_freshness(options, true);
        const bool current = freshness.current && freshness.fully_verified && freshness.snapshot_id == snapshot->id();
        report["checks"][stage] = current ? "current" : "drifted";
        if (!current) report["reason_codes"].push_back("candidate-source-drifted");
        if (mode != Observation::inspection && report["reason_codes"].empty() && matches_base(base_before)) {
            // Both suites share one immutable generation. Freshness is assessed again after evaluation.
            core::RetrievalSession session(snapshot, true);
            observe_quality(report, session);
            const auto after = core::assess_index_freshness(options, true);
            if (!after.current || !after.fully_verified || after.snapshot_id != snapshot->id()) {
                report["checks"][stage] = "drifted";
                report["reason_codes"].push_back("candidate-source-drifted");
            }
        }
        stage = "base";
        const auto base_after = select_base();
        const bool base_matches = matches_base(base_before) && matches_base(base_after);
        report["checks"][stage] = base_matches ? "matched" : "changed";
        if (!base_matches) report["reason_codes"].push_back("preparation-base-changed");

        stage = "route";
        verify_state();
        catalog_.verify(binding, workspace);
        stage = "writer";
        lifetime.verify();
        stage = "artifact";
        if (verify_candidate_layout(options.out_root, config_.retrieval.max_file_bytes) != artifact_identity ||
            core::fnv1a64(read_route_file(options.out_root / core::kGeneratedDir / "manifest.json",
                                        config_.retrieval.max_file_bytes)) != snapshot->id())
            throw ServiceError(409, "candidate-artifact-invalid", "Candidate artifact changed during inspection.");
        stage = "ledger";
        if (store_.preparation_inspection_input(principal, id, write) != input) {
            report["checks"][stage] = "mismatch";
            report["reason_codes"].push_back("candidate-ledger-changed");
        }
        report["candidate_verified"] = structural_checks_passed(report);
        if (mode != Observation::inspection)
            report["preflight_passed"] = report["candidate_verified"] == true && report["quality_passed"] == true;
        if (mode == Observation::activation) {
            if (report["preflight_passed"] != true)
                throw ServiceError(409, "activation-preflight-failed", "Candidate failed activation preflight; inspect its preflight report.");
            // The candidate and publication descriptors remain held through the DB commit.
            return on_verified(input);
        }
    } catch (const ServiceError& error) {
        if (write && (error.status == 403 || error.status == 404 || error.code == "workspace-write-scope-changed")) throw;
        if (mode == Observation::activation) throw;
        record_failure(report, stage, error.code);
    } catch (const core::CliError&) {
        if (mode == Observation::activation)
            throw ServiceError(409, "activation-verification-unavailable", "Candidate verification could not complete.");
        record_failure(report, stage, stage == "artifact" ? "candidate-artifact-invalid" : "candidate-verification-unavailable");
    } catch (const std::exception&) {
        if (mode == Observation::activation)
            throw ServiceError(503, "activation-verification-unavailable", "Candidate verification could not complete.");
        record_failure(report, stage, stage == "artifact" ? "candidate-artifact-invalid" : "candidate-verification-unavailable");
    }
    if (write) (void)store_.preparation_inspection_input(principal, id, write);
    report["finished_at"] = observed_at();
    return report;
}

} // namespace shin::server
