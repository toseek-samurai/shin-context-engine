#include "context_internal.h"
#include "session_contract.h"
#include "execution_evidence.h"

namespace shin::context {
namespace {

Json binding_differences(const Json& expected, const Json& actual, bool include_fingerprint) {
    Json differences = Json::array();
    for (auto field = expected.begin(); field != expected.end(); ++field) {
        if ((!include_fingerprint && field.key() == "profile_config_hash") ||
            (actual.contains(field.key()) && actual.at(field.key()) == field.value())) {
            continue;
        }
        differences.push_back(field.key());
    }
    return differences;
}

Json checkpoint_path_drift(const RuntimeOptions& options, const WorktreeInventory& inventory,
                           const Json& checkpoint) {
    Json paths = Json::array();
    std::size_t changed = 0;
    std::size_t unknown = 0;
    for (const Json& prior : checkpoint.at("tracked_sources")) {
        const std::string path = prior.at("path").get<std::string>();
        const Json current = session_source_state(options, inventory, path);
        const bool unobserved = prior.at("observation") == "unobserved" || current.at("observation") == "unobserved";
        const bool same = prior == current;
        changed += !same && !unobserved ? 1 : 0;
        unknown += unobserved ? 1 : 0;
        paths.push_back({{"path", path}, {"status", unobserved ? "unverified" : same ? "unchanged" : "changed"},
                         {"baseline", prior}, {"current", current}});
    }
    return {{"scope", "tracked-paths-only"}, {"complete_historical_diff", false},
            {"changed_paths", changed}, {"unverified_paths", unknown}, {"paths", std::move(paths)}};
}

Json assess_assertions(const Json& assertions, const Json& drift, const Json& receipts,
                       bool checkpoint_current) {
    std::map<std::string, std::string> path_status;
    for (const Json& path : drift.at("paths")) {
        const bool observed = path.at("baseline").at("observation") == "observed" &&
                              path.at("current").at("observation") == "observed";
        path_status[path.at("path").get<std::string>()] = observed ? path.at("status").get<std::string>() : "unverified";
    }
    std::map<std::string, bool> receipt_current;
    for (const Json& receipt : receipts) {
        receipt_current[receipt.at("receipt_id").get<std::string>()] = receipt.at("passed_current").get<bool>();
    }
    Json result = Json::array();
    for (const Json& assertion : assertions) {
        bool references_current = checkpoint_current && !assertion.at("paths").empty();
        for (const Json& path : assertion.at("paths")) {
            const auto state = path_status.find(path.get<std::string>());
            references_current = references_current && state != path_status.end() && state->second == "unchanged";
        }
        bool receipts_current = checkpoint_current && !assertion.at("receipt_ids").empty();
        for (const Json& id : assertion.at("receipt_ids")) {
            receipts_current = receipts_current && receipt_current.at(id.get<std::string>());
        }
        Json assessed = assertion;
        assessed["verification_level"] = "session-asserted";
        assessed["references_current"] = references_current;
        assessed["receipts_current"] = receipts_current;
        assessed["claim_verified"] = false;
        assessed["review_required"] = !checkpoint_current ||
                                       (!assertion.at("paths").empty() && !references_current) ||
                                       (!assertion.at("receipt_ids").empty() && !receipts_current) ||
                                       (assertion.at("paths").empty() && assertion.at("receipt_ids").empty()) ||
                                       assertion.at("kind") == "blocked";
        result.push_back(std::move(assessed));
    }
    return result;
}

Json resume_actions(const RuntimeOptions& options, bool index_current, bool checkpoint_expired,
                     bool drifted, bool pack_complete, const Json& assertions, const Json& receipts,
                     const Json& proposed) {
    Json actions = Json::array();
    auto append = [&](const std::string& code, const std::string& text) {
        actions.push_back({{"code", code}, {"text", text}, {"source", "shin-policy"}, {"automatic_execution", false}});
    };
    if (!index_current) {
        append("inspect-impact", "Review changed members and declared dependency neighbors before replacing the snapshot.");
        actions.back()["next_command"] = {
            {"argv", {"shin-context", "--profile", options.profile.id, "--profile-dir", options.profile_dir.generic_string(),
                      "--root", options.root.generic_string(), "--out-dir", options.out_root.generic_string(), "impact"}},
            {"shell", false}, {"consumer_mutation", false}, {"index_mutation", false}, {"automatic_execution", false}
        };
        append("rebuild-index", "Explicitly publish the current worktree and rerun normal strict readiness gates.");
        actions.back()["next_command"] = {
            {"argv", {"shin-context", "--profile", options.profile.id, "--profile-dir", options.profile_dir.generic_string(),
                      "--root", options.root.generic_string(), "--out-dir", options.out_root.generic_string(), "build"}},
            {"shell", false}, {"consumer_mutation", false}, {"index_mutation", true}, {"automatic_execution", false}
        };
    }
    if (checkpoint_expired) {
        append("review-checkpoint-age", "Reconfirm the objective, assertions, and pending work before continuing.");
    }
    if (drifted) {
        append("review-worktree-drift", "Read changed verification paths and reconcile the saved task state.");
    }
    Json gate_ids = Json::array();
    for (const Json& receipt : receipts) {
        if (!receipt.at("passed_current").get<bool>()) {
            gate_ids.push_back(receipt.at("receipt_id"));
        }
    }
    if (!gate_ids.empty()) {
        append("review-execution-receipts", "Review untrusted, failed, expired, or drifted receipts and rerun required gates.");
        actions.back()["receipt_ids"] = std::move(gate_ids);
    }
    for (const Json& action : execution_receipt_actions(receipts)) actions.push_back(action);
    Json blocker_ids = Json::array();
    for (const Json& assertion : assertions) {
        if (assertion.at("kind") == "blocked") {
            blocker_ids.push_back(assertion.at("id"));
        }
    }
    if (!blocker_ids.empty()) {
        append("resolve-declared-blockers", "Recheck declared external blockers with the operator or host.");
        actions.back()["assertion_ids"] = std::move(blocker_ids);
    }
    if (index_current && !pack_complete) {
        append("review-continuation-gaps", "Resolve uncovered required evidence before using the continuation Pack.");
    }
    for (const Json& proposal : proposed) {
        actions.push_back({{"id", proposal.at("id")}, {"text", proposal.at("text")},
                           {"source", "session-proposed"}, {"review_required", true}, {"automatic_execution", false}});
    }
    return actions;
}

}  // namespace

Json evaluate_resume(const RuntimeOptions& input_options, const SessionEvaluationOptions& requested_command,
                     std::string_view input) {
    validate_session_evaluation_options(requested_command, true);
    SessionEvaluationOptions command = requested_command;
    const RuntimeOptions options = canonical_session_options(input_options);
    const Json checkpoint = parse_session_document(input);
    const SessionDraft draft = validate_session_checkpoint(checkpoint, options.profile);
    const Json binding = session_binding(options, command);
    const Json differences = binding_differences(binding, checkpoint.at("binding"), false);
    Json output = make_result_document("shin.session-resume.v1", "resume", differences.empty());
    output["policy"] = session_policy();
    output["checkpoint_id"] = checkpoint.at("checkpoint_id");
    output["clock_source"] = command.explicit_clock ? "explicit" : "system-clock";
    output["binding_differences"] = differences;
    output["continuation_ready"] = false;
    output["continuation_pack"] = nullptr;
    if (!differences.empty()) {
        output["observed_at"] = session_observed_at(command);
        output["status"] = "mismatched";
        output["next_action"] = "select-checkpoint-for-current-task";
        require_session_output_bounds(output);
        return output;
    }

    const auto owned_session = open_session_evaluation(options);
    RetrievalSession& session = *owned_session;
    const WorktreeInventory inventory = observe_worktree(options);
    Json comparison = compare_worktree(options, session.manifest(), inventory, 32);
    const bool observation_complete = worktree_observation_complete(inventory);
    const bool snapshot_inputs_current = comparison.at("current").get<bool>() && observation_complete;
    IndexFreshness freshness = session.freshness();
    if (snapshot_inputs_current) {
        freshness = assess_index_freshness(options, true);
    }
    const bool index_current = snapshot_inputs_current && freshness.current && freshness.fully_verified &&
                               freshness.snapshot_id == session.freshness().snapshot_id;
    const bool profile_current = binding.at("profile_config_hash") == checkpoint.at("binding").at("profile_config_hash");
    const bool same_worktree = comparison.at("worktree").at("worktree_id") == checkpoint.at("baseline").at("worktree_id");
    const bool same_snapshot = checkpoint.at("baseline").at("snapshot_id") == session.freshness().snapshot_id;
    const bool generator_current = checkpoint.at("generator_version") == kGeneratorVersion;
    const bool drifted = !same_worktree || !same_snapshot || !profile_current || !generator_current;
    const Json drift = checkpoint_path_drift(options, inventory, checkpoint);
    Json receipt_scope = binding;
    receipt_scope["worktree_id"] = comparison.at("worktree").at("worktree_id");
    receipt_scope["snapshot_id"] = session.freshness().snapshot_id;
    bool pack_complete = false;
    if (index_current) {
        const AgentContextPack pack = execute_agent_context_pack(options, draft.request, session, command.jobs);
        PackRenderOptions render{PackProjection::compact, true, true};
        Json rendered = render_agent_context_pack(options, render, draft.request, session, pack);
        rendered["index_freshness"] = index_freshness_json(freshness);
        rendered["freshness_scope"] = "session-exact-generated-output";
        pack_complete = pack.completion.complete;
        output["continuation_pack"] = std::move(rendered);
        const Json& baseline_pack = checkpoint.at("pack_baseline");
        output["pack_comparison"] = baseline_pack.is_null() ? Json(nullptr) : Json{
            {"same_request", baseline_pack.at("request_fingerprint") == pack.request_fingerprint},
            {"same_evidence", baseline_pack.at("evidence_set_fingerprint") == pack.evidence_set_fingerprint},
            {"same_receipt", baseline_pack.at("receipt_id") == pack.receipt_id}
        };
    }
    require_worktree_stable(options, inventory);
    const auto assessed = assess_execution_receipts(options, draft.receipts, receipt_scope, command, observation_complete);
    const Json& receipts = assessed.receipts;
    require_worktree_stable(options, inventory);
    command.observed_at = assessed.observed_at;
    const std::string age = session_age(checkpoint.at("captured_at").get<std::int64_t>(), draft.ttl_seconds, command.observed_at);
    const Json assertions = assess_assertions(draft.state.at("assertions"), drift, receipts,
                                               age == "current" && profile_current && generator_current);
    const bool assertion_review = std::any_of(assertions.begin(), assertions.end(), [](const Json& assertion) {
        return assertion.at("review_required").get<bool>();
    });
    const bool receipt_review = std::any_of(receipts.begin(), receipts.end(), [](const Json& receipt) {
        return !receipt.at("passed_current").get<bool>();
    });
    const bool has_blockers = std::any_of(assertions.begin(), assertions.end(), [](const Json& assertion) {
        return assertion.at("kind") == "blocked";
    });
    output["status"] = age != "current" ? age : drifted ? "drifted" : "current";
    output["observed_at"] = command.observed_at;
    output["checkpoint_age"] = age;
    output["alignment"] = drifted ? "drifted" : "aligned";
    output["same_worktree"] = same_worktree;
    output["same_snapshot"] = same_snapshot;
    output["profile_config_current"] = profile_current;
    output["generator_current"] = generator_current;
    output["index_current"] = index_current;
    output["snapshot_inputs_current"] = snapshot_inputs_current;
    output["index_freshness"] = index_freshness_json(freshness);
    output["observation_complete"] = observation_complete;
    output["snapshot_id"] = session.freshness().snapshot_id;
    output["binding"] = binding;
    output["checkpoint_drift"] = drift;
    output["published_drift"] = std::move(comparison);
    output["objective"] = draft.request.objective;
    output["current_task"] = command.current_task.empty() ? draft.request.objective : command.current_task;
    output["task_wording_changed"] = !command.current_task.empty() && command.current_task != draft.request.objective;
    output["state"] = draft.state;
    output["state"]["assertions"] = assertions;
    output["vcs"] = {{"metadata", draft.vcs}, {"verification_level", "session-asserted"}, {"current_vcs_checked", false}};
    output["receipts"] = receipts;
    output["continuation_ready"] = index_current && age == "current" && pack_complete && !has_blockers &&
                                     !output.at("task_wording_changed").get<bool>();
    output["review_required"] = drifted || age != "current" || assertion_review || receipt_review ||
                                output.at("task_wording_changed").get<bool>();
    output["next_actions"] = resume_actions(options, index_current, age != "current", drifted, pack_complete,
                                              assertions, receipts, draft.state.at("next_actions"));
    output["require_current"] = command.require_current;
    output["ok"] = !command.require_current || index_current;
    require_session_output_bounds(output);
    return output;
}

int run_resume(const RuntimeOptions& input_options, const std::vector<std::string>& args) {
    const SessionCommandOptions command = parse_session_options(args, true);
    const RuntimeOptions options = canonical_session_options(input_options);
    const Json checkpoint = read_session_input(command, options.root);
    const Json output = evaluate_resume(options, command, checkpoint.dump());
    emit_session_document(output);
    return output.at("ok").get<bool>() ? 0 : 1;
}

}  // namespace shin::context
