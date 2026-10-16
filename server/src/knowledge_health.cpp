#include "shin/server/knowledge_health.h"
#include "shin/server/retrieval_service.h"
#include "shin/server/active_snapshot.h"
#include "context_internal.h"
#include "generated_io.h"
#include "profile_quality.h"

namespace shin::server {
namespace core = shin::context;

Json knowledge_generation(const BoundRoute& binding, const Json& active) {
    return {{"project_id", binding.route.project_id}, {"route_id", binding.route.id},
        {"workspace_id", binding.route.workspace_id}, {"source_host_id", binding.route.source_host_id},
        {"profile_id", binding.options.profile.id}, {"route_fingerprint", binding.fingerprint},
        {"preparation_id", active.at("preparation_id")}, {"snapshot_id", active.at("snapshot_id")},
        {"revision", active.at("revision")}};
}

bool knowledge_same_source_generation(const Json& observed, const Json& selected) {
    // Selection revisions belong to independent route/publication counters.
    // Only identical source and immutable artifact provenance can share health.
    for (const auto* field : {"project_id", "route_id", "workspace_id", "source_host_id", "profile_id",
                             "route_fingerprint", "preparation_id", "snapshot_id"}) {
        if (!observed.contains(field) || !selected.contains(field) || observed.at(field).is_null() ||
            observed.at(field) != selected.at(field)) return false;
    }
    return true;
}

Json RetrievalService::inspect_route_health(Store& store, const Principal& principal,
                                          const std::string& project, const std::string& route) {
    if (foreground_.load() != 0)
        throw ServiceError(503, "knowledge-foreground-busy", "Foreground retrieval has priority.");
    (void)store.get_project(principal, project);
    const auto select = [&] {
        auto active = store.active_route(principal, project, route);
        active["source_route_id"] = route;
        return active;
    };
    const auto active = select();
    const ActiveSnapshot access(config_, catalog_, cache_, store, principal, project, route, false,
                                active, state_identity_, select);
    const auto& binding = access.binding();
    core::RetrievalSession session(access.snapshot(), false);
    const auto& options = session.options();
    const auto publication_lock = core::lock_index_for_read(options.out_root);
    const auto vacuum = core::inspect_generated_kb_vacuum(options);
    const auto doctor = core::inspect_generated_kb_health(options, true);
    const auto profile = core::validate_profile(options, true);
    const bool layout_ok = !vacuum.summary.changes_needed && vacuum.summary.broken_local_links == 0 &&
                            vacuum.expected_snapshot_id == access.snapshot()->id();
    const bool doctor_ok = doctor.counts.errors == 0 && doctor.counts.warnings == 0;
    const bool profile_ok = profile.counts.errors == 0 && profile.counts.warnings == 0;
    Json quality{{"state", "not-assessed"}, {"eval_total", nullptr}, {"eval_failed", nullptr},
                 {"pack_total", nullptr}, {"pack_failed", nullptr}, {"resource_check", nullptr}};
    bool quality_ok = false;
    if (layout_ok && doctor_ok && profile_ok) {
        if (options.profile.bootstrap_only) {
            quality["state"] = "bootstrap-not-configured";
            quality_ok = true;
        } else {
            const auto eval = core::evaluate_profile_cases(session);
            const auto pack = core::evaluate_pack_cases(session);
            quality_ok = eval.total != 0 && pack.total != 0 && eval.failed == 0 && pack.failed == 0;
            quality = {{"state", quality_ok ? "passed" : "failed"}, {"eval_total", eval.total},
                {"eval_failed", eval.failed}, {"pack_total", pack.total}, {"pack_failed", pack.failed},
                {"resource_check", core::eval_resource_check_json(options.profile, eval)}};
        }
    }
    const auto freshness = core::assess_index_freshness(options, true);
    const bool current = freshness.current && freshness.fully_verified && freshness.snapshot_id == access.snapshot()->id();
    access.verify();
    if (select() != active)
        throw ServiceError(409, "active-generation-changed", "Active generation changed during health assessment.");
    const bool healthy = layout_ok && doctor_ok && profile_ok && quality_ok && current;
    Json reasons = Json::array();
    if (!layout_ok) reasons.push_back("knowledge-vacuum-changes");
    if (!doctor_ok) reasons.push_back("knowledge-doctor-failed");
    if (!profile_ok) reasons.push_back("knowledge-profile-failed");
    if (!quality_ok) reasons.push_back("knowledge-quality-not-passed");
    if (!current) reasons.push_back("knowledge-index-not-current");
    return {{"healthy", healthy}, {"generation", knowledge_generation(binding, active)},
        {"index_current", current}, {"expected_snapshot_id", vacuum.expected_snapshot_id},
        {"vacuum", {{"changes_needed", vacuum.summary.changes_needed}, {"missing_files", vacuum.summary.missing_files},
            {"stale_files", vacuum.summary.stale_files}, {"orphan_files", vacuum.summary.orphan_files},
            {"broken_local_links", vacuum.summary.broken_local_links}}},
        {"doctor", {{"errors", doctor.counts.errors}, {"warnings", doctor.counts.warnings}}},
        {"profile", {{"errors", profile.counts.errors}, {"warnings", profile.counts.warnings}}},
        {"quality", quality}, {"reason_codes", reasons}, {"mutations_performed", false}};
}

} // namespace shin::server
