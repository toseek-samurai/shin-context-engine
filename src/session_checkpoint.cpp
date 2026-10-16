#include "context_internal.h"
#include "session_contract.h"

namespace shin::context {

Json session_source_state(const RuntimeOptions& options, const WorktreeInventory& inventory,
                          const std::string& path) {
    const WorktreeSourceState* state = nullptr;
    if (const auto document = inventory.documents.find(path); document != inventory.documents.end()) {
        state = &document->second;
    } else if (const auto fact = inventory.source_facts.find(path); fact != inventory.source_facts.end()) {
        state = &fact->second;
    }
    Json result = {{"path", path}, {"observation", "unobserved"},
                   {"content_hash", nullptr}, {"bytes", nullptr}};
    if (state != nullptr && !state->content_hash.empty()) {
        result["observation"] = "observed";
        result["content_hash"] = state->content_hash;
        result["bytes"] = state->byte_count;
    } else if (!profile_path_contains_symlink(options.root, path)) {
        std::error_code error;
        const auto status = fs::symlink_status(options.root / path, error);
        if (status.type() == fs::file_type::not_found &&
            (!error || error == std::errc::no_such_file_or_directory)) {
            result["observation"] = "missing";
        }
    }
    return result;
}

Json session_tracked_sources(const RuntimeOptions& options, const WorktreeInventory& inventory,
                             const Json& state, const AgentContextPack* pack) {
    std::set<std::string> paths;
    for (const Json& path : state.at("changed_paths")) {
        paths.insert(path.get<std::string>());
    }
    for (const Json& assertion : state.at("assertions")) {
        for (const Json& path : assertion.at("paths")) {
            paths.insert(path.get<std::string>());
        }
    }
    if (pack != nullptr) {
        for (const Json& path : pack->verification_paths) {
            paths.insert(path.at("path").get<std::string>());
        }
    }
    if (paths.size() > kMaxSessionTrackedPaths) {
        throw_usage_error("session-path-budget-exceeded", "Checkpoint tracks at most 256 distinct paths.");
    }
    Json sources = Json::array();
    for (const std::string& path : paths) {
        sources.push_back(session_source_state(options, inventory, path));
    }
    return sources;
}

Json evaluate_checkpoint(const RuntimeOptions& input_options, const SessionEvaluationOptions& command,
                         std::string_view input) {
    validate_session_evaluation_options(command, false);
    const RuntimeOptions options = canonical_session_options(input_options);
    const SessionDraft draft = parse_session_draft(parse_session_document(input), options.profile);
    const auto owned_session = open_session_evaluation(options);
    RetrievalSession& session = *owned_session;
    const WorktreeInventory inventory = observe_worktree(options);
    const Json comparison = compare_worktree(options, session.manifest(), inventory, 32);
    const bool complete = worktree_observation_complete(inventory);
    bool current = comparison.at("current").get<bool>() && complete;
    if (current) {
        const IndexFreshness freshness = assess_index_freshness(options, true);
        current = freshness.current && freshness.fully_verified && freshness.snapshot_id == session.freshness().snapshot_id;
    }
    if (command.require_current && !current) {
        throw_index_error("checkpoint-index-not-current", "Checkpoint requires a complete current index.");
    }

    std::optional<AgentContextPack> pack;
    if (comparison.at("comparison").at("profile_config_current").get<bool>()) {
        pack = execute_agent_context_pack(options, draft.request, session, command.jobs);
    }
    Json pack_baseline = nullptr;
    if (pack) {
        pack_baseline = {{"receipt_id", pack->receipt_id},
                         {"request_fingerprint", pack->request_fingerprint},
                         {"evidence_set_fingerprint", pack->evidence_set_fingerprint},
                         {"evidence_ids", pack->evidence_ids}};
    }
    Json checkpoint = {
        {"schema_version", "shin.session-checkpoint.v1"},
        {"engine_version", kProductVersion}, {"generator_version", kGeneratorVersion},
        {"captured_at", 0}, {"ttl_seconds", draft.ttl_seconds},
        {"binding", session_binding(options, command)},
        {"baseline", {{"snapshot_id", session.freshness().snapshot_id},
                      {"worktree_id", comparison.at("worktree").at("worktree_id")},
                      {"index_current", current}, {"observation_complete", complete}}},
        {"task_request", draft.request.canonical_document}, {"state", draft.state},
        {"receipts", draft.receipts}, {"vcs", draft.vcs},
        {"tracked_sources", session_tracked_sources(options, inventory, draft.state, pack ? &*pack : nullptr)},
        {"pack_baseline", std::move(pack_baseline)}
    };
    require_worktree_stable(options, inventory);
    checkpoint["captured_at"] = session_observed_at(command);
    checkpoint["checkpoint_id"] = fnv1a64(checkpoint.dump());
    if (checkpoint.dump().size() > kMaxSessionInputBytes) {
        throw_usage_error("session-input-too-large", "Canonical checkpoint exceeds its 256 KiB read limit.");
    }
    return checkpoint;
}

int run_checkpoint(const RuntimeOptions& input_options, const std::vector<std::string>& args) {
    const SessionCommandOptions command = parse_session_options(args, false);
    const RuntimeOptions options = canonical_session_options(input_options);
    const Json draft = read_session_input(command, options.root);
    emit_session_document(evaluate_checkpoint(options, command, draft.dump()));
    return 0;
}

}  // namespace shin::context
