#pragma once
#include "shin/observer/local_project.h"
#include <limits>

namespace shin::observer {
inline bool source_observation_limited(const Json& observation) {
    if (!observation.is_object() || observation.value("status", "") != "incomplete" ||
        observation.value("reason_code", "") != "incomplete-source-observation") return false;
    const auto counts = observation.find("source_counts");
    if (counts == observation.end() || !counts->is_object()) return false;
    for (const auto* key : {"documents", "candidate_files", "source_facts", "capped_files", "skipped_files"}) {
        const auto value = counts->find(key);
        if (value == counts->end() || !value->is_number_integer() || *value < 0 ||
            *value > std::numeric_limits<int>::max()) return false;
    }
    const auto candidates = counts->at("candidate_files").get<std::int64_t>();
    const auto sources = counts->at("source_facts").get<std::int64_t>();
    const auto capped = counts->at("capped_files").get<std::int64_t>();
    const auto skipped = counts->at("skipped_files").get<std::int64_t>();
    return candidates == sources + capped && skipped <= sources && (capped > 0 || skipped > 0);
}

// Observation is scheduling evidence, not authority. A retained reader still
// authenticates its exact approved head and hashes every selected source.
inline Json retained_source_limit_observation(const Config& config, const fs::path& workspace,
                                              const std::string& root_identity, const std::string& profile_hash) {
    const auto state = local_watch_status(workspace / "watch").at("state");
    if (!state.is_object() || state.value("root", "") != config.root.string()) return nullptr;
    const auto last = state.value("last_result", Json());
    if (!last.is_object()) return nullptr;
    const auto observation = last.value("observation", Json());
    if (source_observation_limited(observation) && observation.value("profile_config_hash", "") == profile_hash)
        return observation;
    // Older controllers dropped the observation when retaining this refusal.
    // Recover on cold admission only; a normal resident request never rescans.
    const auto error = last.value("error", Json());
    if (error.is_object() && error.value("code", "") == "incomplete-source-observation") {
        const auto current = sample_worktree(config, root_identity);
        if (source_observation_limited(current) && current.value("profile_config_hash", "") == profile_hash)
            return current;
    }
    return nullptr;
}

// Presentation of retained context only. The caller independently verifies the
// publisher grant, exact snapshot head, approved profile and selected sources.
inline Json retained_context_status(const Config& config, const std::string& snapshot,
                                    const std::string& approved_hash, const std::string& proposed_hash,
                                    const Json& diagnostic, const Json& observation = Json()) {
    Json result{{"root", config.root.string()}, {"profile", config.profile}, {"snapshot_id", snapshot},
        {"index", config.out_dir.string()}, {"local_ready", true}, {"canonical_ready", false}, {"retrieval_ready", true},
        {"state", "partial-ready"}, {"readiness_scope", "approved-snapshot-selected-files"},
        {"project_quality", "candidate-not-qualified"}, {"publication", "managed"},
        {"profile_revision", {{"state", diagnostic.is_null() ? "pending" : "rejected"},
            {"approved_hash", approved_hash}, {"proposed_hash", proposed_hash}}},
        {"source_coverage", {{"state", "partial"}, {"reason", "profile-revision-pending"},
            {"omissions_scope", "selected-results"},
            {"notice", "The current file scope is not fully qualified. Only unchanged verified files from the approved snapshot are returned; report partial coverage."}}},
        {"readiness", {{"ready", false}, {"ok", false}, {"scope", "retained-approved-publication"}}},
        {"next_action", "use-verified-partial-context-review-profile-diagnostic"}};
    if (source_observation_limited(observation)) {
        Json counts = Json::object();
        for (const auto* key : {"documents", "candidate_files", "source_facts", "capped_files", "skipped_files"})
            counts[key] = observation.at("source_counts").at(key);
        result["project_quality"] = "current-source-not-qualified";
        result["profile_revision"]["state"] = "approved";
        result["source_coverage"] = {{"state", "partial"}, {"reason", "source-observation-limited"},
            {"source_counts", std::move(counts)}, {"omissions_scope", "selected-results"},
            {"notice", "Configured source limits were reached. Only unchanged verified files from the approved snapshot remain available; new and changed files are withheld. Report partial coverage."}};
        result["next_action"] = "use-verified-partial-context-review-source-limits";
    }
    return result;
}
} // namespace shin::observer
