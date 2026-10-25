#include "agent_context_pack.h"
#include "context_internal.h"
#include "snapshot_verification.h"

namespace shin::context {
namespace {

constexpr int kDefaultPackDiffDetailLimit = 100;
constexpr int kMaxPackDiffDetailLimit = 1000;

struct PackDiffOptions {
    fs::path from;
    fs::path to;
    fs::path input;
    bool stdin_input = false;
    bool require_current = false;
    bool strict = false;
    int jobs = 0;
    int limit = kDefaultPackDiffDetailLimit;
};

struct EvidenceView {
    const Json* row = nullptr;
    int rank = 0;
};

struct RequirementState {
    std::string id;
    std::string kind;
    bool required = false;
    bool satisfied = false;
    int selected_evidence = 0;
    int total_groups = 0;
    int covered_groups = 0;
};

struct EvidenceChange {
    int order = 0;
    std::string path;
    int line = 0;
    std::string evidence_id;
    Json detail = Json::object();
};

PackDiffOptions parse_pack_diff_options(const std::vector<std::string>& args) {
    PackDiffOptions options;
    bool from_seen = false;
    bool to_seen = false;
    bool input_seen = false;
    bool jobs_seen = false;
    bool limit_seen = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--from" || args[index] == "--to") {
            const bool is_from = args[index] == "--from";
            bool& seen = is_from ? from_seen : to_seen;
            if (seen || index + 1 >= args.size()) {
                throw_usage_error(seen ? "duplicate-option" : "missing-option-value",
                                  seen ? args[index] + " may be supplied only once."
                                       : args[index] + " requires an output directory.");
            }
            const std::string& option = args[index];
            fs::path value = canonical_snapshot_root(args[++index], option);
            if (is_from) {
                options.from = std::move(value);
            } else {
                options.to = std::move(value);
            }
            seen = true;
        } else if (args[index] == "--input") {
            if (input_seen || index + 1 >= args.size()) {
                throw_usage_error(input_seen ? "duplicate-option" : "missing-option-value",
                                  input_seen ? "--input may be supplied only once."
                                             : "--input requires a file path or -.");
            }
            const std::string& value = args[++index];
            options.stdin_input = value == "-";
            if (!options.stdin_input) {
                options.input = fs::absolute(value);
            }
            input_seen = true;
        } else if (args[index] == "--jobs") {
            if (jobs_seen || index + 1 >= args.size()) {
                throw_usage_error(jobs_seen ? "duplicate-option" : "missing-option-value",
                                  jobs_seen ? "--jobs may be supplied only once."
                                            : "--jobs requires a value.");
            }
            options.jobs = parse_cli_integer(args[++index], "--jobs", 1);
            if (options.jobs > kMaxPackJobs) {
                throw_usage_error("invalid-option-value", "--jobs must not exceed 8.");
            }
            jobs_seen = true;
        } else if (args[index] == "--limit") {
            if (limit_seen || index + 1 >= args.size()) {
                throw_usage_error(limit_seen ? "duplicate-option" : "missing-option-value",
                                  limit_seen ? "--limit may be supplied only once."
                                             : "--limit requires a value.");
            }
            options.limit = parse_cli_integer(args[++index], "--limit", 1);
            if (options.limit > kMaxPackDiffDetailLimit) {
                throw_usage_error("invalid-option-value",
                                  "pack-diff --limit must not exceed 1000.");
            }
            limit_seen = true;
        } else if (args[index] == "--require-current") {
            options.require_current = true;
        } else if (args[index] == "--strict") {
            options.strict = true;
        } else {
            throw_usage_error(
                "unsupported-option",
                "pack-diff only accepts --from, --to, --input, --jobs, --limit, "
                "--require-current, and --strict.");
        }
    }
    if (!from_seen || !to_seen || !input_seen) {
        throw_usage_error(
            "missing-option",
            "pack-diff requires --from OUT_DIR, --to OUT_DIR, and --input FILE or -.");
    }
    return options;
}

Json completion_json(const PackCompletion& completion) {
    return {
        {"complete", completion.complete},
        {"required_requirements", completion.required_requirements},
        {"satisfied_required_requirements",
         completion.satisfied_required_requirements},
        {"unsatisfied_required_requirement_ids",
         completion.unsatisfied_required_requirement_ids},
        {"optional_requirements", completion.optional_requirements},
        {"satisfied_optional_requirements",
         completion.satisfied_optional_requirements},
        {"unsatisfied_optional_requirement_ids",
         completion.unsatisfied_optional_requirement_ids}
    };
}

Json budget_json(const TaskRequest& request, const AgentContextPack& pack) {
    return {
        {"max_evidence_bytes", request.budget.max_evidence_bytes},
        {"evidence_bytes", pack.evidence_bytes},
        {"max_records", request.budget.max_records},
        {"eligible_records", pack.eligible_records},
        {"returned_records", pack.evidence_ids.size()},
        {"byte_budget_constrained", pack.byte_budget_constrained},
        {"record_budget_constrained", pack.record_budget_constrained}
    };
}

Json endpoint_json(const RuntimeOptions& options,
                   const RetrievalSession& session,
                   const TaskRequest& request,
                   const AgentContextPack& pack,
                   bool source_current_required) {
    const Json& manifest = session.manifest();
    const Json* integrity = find_json_field(manifest, "generated_file_integrity");
    const Json* files = integrity == nullptr ? nullptr : find_json_field(*integrity, "files");
    const std::string active_profile_config_hash = profile_config_hash(options.profile);
    const std::string snapshot_profile_config_hash =
        json_string_value(manifest, "profile_config_hash");
    return {
        {"out_root", options.out_root.generic_string()},
        {"snapshot_id", session.freshness().snapshot_id},
        {"schema_version", json_string_value(manifest, "schema_version")},
        {"profile", json_string_value(manifest, "profile")},
        {"profile_config_hash", snapshot_profile_config_hash},
        {"matches_active_profile_config",
         snapshot_profile_config_hash == active_profile_config_hash},
        {"generator_version", json_string_value(manifest, "generator_version")},
        {"integrity_status", "verified"},
        {"verified_file_count", files != nullptr && files->is_array() ? files->size() : 0},
        {"source_freshness", source_current_required ? Json("current")
                                                     : Json("not-assessed")},
        {"completion", completion_json(pack.completion)},
        {"budget", budget_json(request, pack)},
        {"receipt", {
            {"receipt_id", pack.receipt_id},
            {"request_fingerprint", pack.request_fingerprint},
            {"evidence_set_fingerprint", pack.evidence_set_fingerprint}
        }}
    };
}

void require_compatible_snapshot(const RuntimeOptions& options,
                                 const RetrievalSession& session,
                                 const std::string& option) {
    const Json& manifest = session.manifest();
    if (json_string_value(manifest, "profile") != options.profile.id) {
        throw CliError("pack-diff-profile-mismatch", "configuration",
                       option + " snapshot profile does not match the active profile.", 1,
                       false, "Select the profile that published both compared snapshots.");
    }
    if (json_string_value(manifest, "generator_version") != kGeneratorVersion) {
        throw CliError("pack-diff-generator-mismatch", "configuration",
                       option + " snapshot generator is not readable by this release.", 1,
                       false, "Rebuild both snapshots with the current Shin generator.");
    }
}

std::map<std::string, EvidenceView> evidence_views(const Json& evidence) {
    std::map<std::string, EvidenceView> views;
    int rank = 0;
    for (const Json& row : evidence) {
        ++rank;
        const std::string evidence_id = json_string_value(row, "evidence_id");
        if (evidence_id.empty() || !views.emplace(evidence_id, EvidenceView{&row, rank}).second) {
            throw CliError("pack-diff-evidence-invalid", "internal",
                           "Agent Context Pack produced an invalid evidence identity.", 1,
                           false, "Report the task request as a Shin Context Engine defect.");
        }
    }
    return views;
}

std::vector<std::string> evidence_requirement_ids(const Json& evidence) {
    std::set<std::string> ids;
    const Json* matches = find_json_field(evidence, "matches");
    if (matches != nullptr && matches->is_array()) {
        for (const Json& match : *matches) {
            const std::string id = json_string_value(match, "requirement_id");
            if (!id.empty()) {
                ids.insert(id);
            }
        }
    }
    return {ids.begin(), ids.end()};
}

Json evidence_detail(const std::string& change,
                     const EvidenceView& view) {
    const Json& row = *view.row;
    Json detail = {
        {"change", change},
        {"evidence_id", row.at("evidence_id")},
        {"evidence_type", row.at("evidence_type")},
        {"rank", view.rank},
        {"location", row.at("location")},
        {"requirement_ids", evidence_requirement_ids(row)},
        {"record", row.at("record")}
    };
    return detail;
}

EvidenceChange make_evidence_change(int order,
                                    const std::string& evidence_id,
                                    Json detail) {
    const Json& location = detail.at("location");
    return {
        order,
        json_string_value(location, "path"),
        json_int_value(location, "start_line", 0),
        evidence_id,
        std::move(detail)
    };
}

Json evidence_changes_json(const AgentContextPack& from,
                           const AgentContextPack& to,
                           int limit,
                           int& retained,
                           int& added,
                           int& removed,
                           int& rank_changed,
                           int& match_edges_changed) {
    const auto from_views = evidence_views(from.evidence);
    const auto to_views = evidence_views(to.evidence);
    std::vector<EvidenceChange> changes;
    for (const auto& [evidence_id, view] : from_views) {
        const auto target = to_views.find(evidence_id);
        if (target == to_views.end()) {
            ++removed;
            changes.push_back(make_evidence_change(
                0, evidence_id, evidence_detail("removed", view)));
            continue;
        }
        ++retained;
        if (view.rank != target->second.rank) {
            ++rank_changed;
            Json detail = evidence_detail("rank-changed", target->second);
            detail["from_rank"] = view.rank;
            detail["to_rank"] = target->second.rank;
            changes.push_back(make_evidence_change(2, evidence_id, std::move(detail)));
        }
        if (view.row->at("matches") != target->second.row->at("matches")) {
            ++match_edges_changed;
            Json detail = evidence_detail("match-edges-changed", target->second);
            detail["from_matches"] = view.row->at("matches");
            detail["to_matches"] = target->second.row->at("matches");
            changes.push_back(make_evidence_change(3, evidence_id, std::move(detail)));
        }
    }
    for (const auto& [evidence_id, view] : to_views) {
        if (!from_views.contains(evidence_id)) {
            ++added;
            changes.push_back(make_evidence_change(
                1, evidence_id, evidence_detail("added", view)));
        }
    }
    std::sort(changes.begin(), changes.end(), [](const EvidenceChange& left,
                                                  const EvidenceChange& right) {
        return std::tie(left.order, left.path, left.line, left.evidence_id) <
               std::tie(right.order, right.path, right.line, right.evidence_id);
    });
    Json details = Json::array();
    const std::size_t returned = std::min<std::size_t>(changes.size(), limit);
    for (std::size_t index = 0; index < returned; ++index) {
        details.push_back(std::move(changes[index].detail));
    }
    return {
        {"exact_retained", retained},
        {"added", added},
        {"removed", removed},
        {"rank_changed", rank_changed},
        {"match_edges_changed", match_edges_changed},
        {"detail_limit", limit},
        {"details_returned", details.size()},
        {"details_omitted", changes.size() - details.size()},
        {"details", std::move(details)}
    };
}

std::map<std::string, RequirementState> requirement_states(const Json& requirements) {
    std::map<std::string, RequirementState> states;
    for (const Json& row : requirements) {
        const std::string id = json_string_value(row, "id");
        const Json* coverage = find_json_field(row, "query_coverage");
        RequirementState state;
        state.id = id;
        state.kind = json_string_value(row, "kind");
        state.required = row.value("required", false);
        state.satisfied = row.value("satisfied", false);
        state.selected_evidence = json_int_value(row, "selected_evidence", 0);
        if (coverage != nullptr && coverage->is_object()) {
            state.total_groups = json_int_value(*coverage, "total_groups", 0);
            state.covered_groups = json_int_value(*coverage, "covered_groups", 0);
        }
        if (id.empty() || !states.emplace(id, std::move(state)).second) {
            throw CliError("pack-diff-requirement-invalid", "internal",
                           "Agent Context Pack produced an invalid requirement identity.", 1,
                           false, "Report the task request as a Shin Context Engine defect.");
        }
    }
    return states;
}

Json requirement_state_json(const RequirementState& state) {
    return {
        {"required", state.required},
        {"satisfied", state.satisfied},
        {"selected_evidence", state.selected_evidence},
        {"query_coverage", {
            {"total_groups", state.total_groups},
            {"covered_groups", state.covered_groups},
            {"uncovered_groups", state.total_groups - state.covered_groups}
        }}
    };
}

Json requirement_changes_json(const AgentContextPack& from,
                              const AgentContextPack& to,
                              int& changed,
                              int& required_regressions,
                              int& required_improvements,
                              int& optional_regressions,
                              int& optional_improvements) {
    const auto from_states = requirement_states(from.requirements);
    const auto to_states = requirement_states(to.requirements);
    if (from_states.size() != to_states.size()) {
        throw CliError("pack-diff-requirement-set-changed", "internal",
                       "One task request produced different requirement sets.", 1, false,
                       "Report the task request as a Shin Context Engine defect.");
    }
    Json details = Json::array();
    for (const auto& [id, left] : from_states) {
        const auto right_entry = to_states.find(id);
        if (right_entry == to_states.end() || left.required != right_entry->second.required ||
            left.kind != right_entry->second.kind) {
            throw CliError("pack-diff-requirement-set-changed", "internal",
                           "One task request produced incompatible requirement metadata.", 1,
                           false, "Report the task request as a Shin Context Engine defect.");
        }
        const RequirementState& right = right_entry->second;
        if (left.satisfied && !right.satisfied) {
            left.required ? ++required_regressions : ++optional_regressions;
        } else if (!left.satisfied && right.satisfied) {
            left.required ? ++required_improvements : ++optional_improvements;
        }
        if (left.satisfied == right.satisfied &&
            left.selected_evidence == right.selected_evidence &&
            left.covered_groups == right.covered_groups &&
            left.total_groups == right.total_groups) {
            continue;
        }
        ++changed;
        details.push_back({
            {"id", id},
            {"kind", left.kind},
            {"from", requirement_state_json(left)},
            {"to", requirement_state_json(right)}
        });
    }
    return {
        {"total", from_states.size()},
        {"changed", changed},
        {"required_regressions", required_regressions},
        {"required_improvements", required_improvements},
        {"optional_regressions", optional_regressions},
        {"optional_improvements", optional_improvements},
        {"details", std::move(details)}
    };
}

}  // namespace

int run_pack_diff(const RuntimeOptions& options,
                  const std::vector<std::string>& args) {
    const PackDiffOptions diff_options = parse_pack_diff_options(args);
    const Json input = read_task_request_document(diff_options.input,
                                                  diff_options.stdin_input);
    const TaskRequest request = parse_task_request(input, options.profile);

    RuntimeOptions from_options = options;
    RuntimeOptions to_options = options;
    from_options.out_root = diff_options.from;
    to_options.out_root = diff_options.to;

    std::unique_ptr<RetrievalSession> first_session;
    std::unique_ptr<RetrievalSession> second_session;
    RetrievalSession* from_session = nullptr;
    RetrievalSession* to_session = nullptr;
    const bool same_root = diff_options.from == diff_options.to;
    const auto from_mode = RetrievalSessionOpenMode::verified_snapshot;
    const auto to_mode = diff_options.require_current
                             ? RetrievalSessionOpenMode::verified_current
                             : RetrievalSessionOpenMode::verified_snapshot;
    if (same_root) {
        first_session = std::make_unique<RetrievalSession>(to_options, to_mode);
        from_session = first_session.get();
        to_session = first_session.get();
    } else if (diff_options.from.generic_string() < diff_options.to.generic_string()) {
        first_session = std::make_unique<RetrievalSession>(from_options, from_mode);
        second_session = std::make_unique<RetrievalSession>(to_options, to_mode);
        from_session = first_session.get();
        to_session = second_session.get();
    } else {
        first_session = std::make_unique<RetrievalSession>(to_options, to_mode);
        second_session = std::make_unique<RetrievalSession>(from_options, from_mode);
        to_session = first_session.get();
        from_session = second_session.get();
    }

    require_compatible_snapshot(options, *from_session, "--from");
    require_compatible_snapshot(options, *to_session, "--to");
    AgentContextPack from_pack = execute_agent_context_pack(
        from_options, request, *from_session, diff_options.jobs);
    AgentContextPack to_pack = same_root
                                   ? from_pack
                                   : execute_agent_context_pack(
                                         to_options, request, *to_session,
                                         diff_options.jobs);

    int retained = 0;
    int added = 0;
    int removed = 0;
    int rank_changed = 0;
    int match_edges_changed = 0;
    Json evidence_changes = evidence_changes_json(
        from_pack, to_pack, diff_options.limit, retained, added, removed,
        rank_changed, match_edges_changed);
    int requirements_changed = 0;
    int required_regressions = 0;
    int required_improvements = 0;
    int optional_regressions = 0;
    int optional_improvements = 0;
    Json requirement_changes = requirement_changes_json(
        from_pack, to_pack, requirements_changed, required_regressions,
        required_improvements, optional_regressions, optional_improvements);

    const bool target_acceptable = to_pack.completion.complete &&
                                   required_regressions == 0;
    const bool context_changed = from_pack.evidence_set_fingerprint !=
                                     to_pack.evidence_set_fingerprint ||
                                 rank_changed > 0 || match_edges_changed > 0 ||
                                 requirements_changed > 0;
    Json output = make_result_document(
        "shin.agent-context-pack-diff.v1", "pack-diff", target_acceptable);
    output["profile"] = options.profile.id;
    output["root"] = options.root.generic_string();
    output["request_id"] = request.request_id;
    output["objective"] = request.objective;
    output["strict"] = diff_options.strict;
    output["require_current"] = diff_options.require_current;
    output["execution"] = {
        {"same_effective_request", true},
        {"same_active_profile", true},
        {"snapshot_integrity_verified", true},
        {"source_freshness_scope", "target-only-when-required"},
        {"active_profile_config_hash", profile_config_hash(options.profile)},
        {"jobs", from_pack.jobs}
    };
    output["from"] = endpoint_json(
        from_options, *from_session, request, from_pack, false);
    output["to"] = endpoint_json(
        to_options, *to_session, request, to_pack, diff_options.require_current);
    output["comparison"] = {
        {"same_snapshot_id", from_session->freshness().snapshot_id ==
                                 to_session->freshness().snapshot_id},
        {"snapshot_changed", from_session->freshness().snapshot_id !=
                                 to_session->freshness().snapshot_id},
        {"profile_config_changed",
         json_string_value(from_session->manifest(), "profile_config_hash") !=
             json_string_value(to_session->manifest(), "profile_config_hash")},
        {"receipt_changed", from_pack.receipt_id != to_pack.receipt_id},
        {"evidence_set_changed", from_pack.evidence_set_fingerprint !=
                                     to_pack.evidence_set_fingerprint},
        {"agent_context_changed", context_changed},
        {"target_complete", to_pack.completion.complete},
        {"required_regression", required_regressions > 0}
    };
    output["requirements"] = std::move(requirement_changes);
    output["evidence"] = std::move(evidence_changes);
    output["interpretation"] = {
        {"policy", "same-request-same-active-profile-v1"},
        {"strict_failure",
         "Target Pack is incomplete or a required requirement regressed."},
        {"semantic_change_claim", false},
        {"note",
         "Evidence replacement, rank movement, or match-edge movement is reported "
         "for agent review and does not fail strict mode while required task coverage remains complete."}
    };
    if (output.dump(2).size() > kMaxPackOutputBytes) {
        throw CliError("pack-diff-output-too-large", "resource",
                       "Pack diff output exceeds the 16 MiB serialized limit.", 1,
                       false, "Reduce --limit or the task evidence budget.");
    }
    emit_json_document(std::cout, output);
    return diff_options.strict && !target_acceptable ? 1 : 0;
}

}  // namespace shin::context
