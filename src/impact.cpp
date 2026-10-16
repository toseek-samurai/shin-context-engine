#include "context_internal.h"
#include "impact.h"
#include "source_relations.h"
#include "worktree_state.h"
#include "execution_evidence.h"

namespace shin::context {
namespace {

struct ImpactCommandOptions {
    ImpactEvaluationOptions evaluation;
    std::optional<SessionCommandOptions> receipt_input;
};

ImpactCommandOptions parse_impact_options(const std::vector<std::string>& args) {
    ImpactCommandOptions result;
    std::set<std::string> seen;
    std::vector<std::string> receipt_args;
    bool receipts_seen = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& option = args[i];
        if (option != "--trusted-producer" && !seen.insert(option).second) throw_usage_error("duplicate-option", "impact options may appear only once.");
        if (option == "--require-current") result.evaluation.require_current = true;
        else if (option == "--strict") result.evaluation.strict = true;
        else if (option == "--limit" || option == "--depth" || option == "--max-evidence-bytes") {
            if (++i == args.size()) throw_usage_error("missing-option-value", "impact option requires a value.");
            const int minimum = option == "--depth" ? 0 : option == "--max-evidence-bytes" ? 2 : 1;
            const int value = parse_cli_integer(args[i], option, minimum);
            const int maximum = option == "--depth" ? 2 : option == "--limit" ? 128 : 131072;
            if (value > maximum) throw_usage_error("invalid-option-value", "impact option exceeds its bounded maximum.");
            if (option == "--limit") result.evaluation.limit = value;
            else if (option == "--depth") result.evaluation.depth = value;
            else result.evaluation.evidence_bytes = static_cast<std::size_t>(value);
        } else if (option == "--receipts" || option == "--task-id" || option == "--thread-id" ||
                   option == "--trusted-producer" || option == "--observed-at") {
            if (++i == args.size()) throw_usage_error("missing-option-value", "Receipt assessment option requires a value.");
            if (option == "--receipts") receipts_seen = true;
            receipt_args.push_back(option == "--receipts" ? "--input" : option);
            receipt_args.push_back(args[i]);
        } else {
            throw_usage_error("unsupported-option", "Unsupported impact or receipt assessment option.");
        }
    }
    if (!receipt_args.empty()) {
        if (!receipts_seen) throw_usage_error("missing-option", "Receipt assessment requires --receipts.");
        result.receipt_input = parse_session_options(receipt_args, true, "--input");
        if (result.receipt_input->stdin_input) throw_usage_error("unsupported-option", "Impact requires an external receipt bundle file.");
        result.evaluation.receipt_options = static_cast<const SessionEvaluationOptions&>(*result.receipt_input);
    }
    return result;
}

RuntimeOptions prepare_impact_options(const RuntimeOptions& input) {
    if (input.force_source_scan) throw_usage_error("unsupported-option", "impact requires a published snapshot.");
    RuntimeOptions options = input;
    options.root = fs::canonical(options.root);
    options.out_root = fs::weakly_canonical(options.out_root);
    if (!generated_index_available(options)) {
        throw_index_error("index-unavailable", "Impact requires an existing published snapshot.", false);
    }
    return options;
}

const WorktreeSourceState* current_source(const WorktreeInventory& inventory, const std::string& path) {
    const auto source = inventory.source_facts.find(path);
    if (source != inventory.source_facts.end()) return &source->second;
    const auto document = inventory.documents.find(path);
    return document == inventory.documents.end() ? nullptr : &document->second;
}

std::map<std::string, std::string> baseline_hashes(const Json& manifest) {
    std::map<std::string, std::string> hashes;
    for (const char* field : {"sources", "source_fact_sources"}) {
        for (const Json& source : manifest.at(field)) {
            hashes[json_string_value(source, "path")] = json_string_value(source, "content_hash");
        }
    }
    return hashes;
}

std::string source_state(const WorktreeInventory& inventory,
                         const std::map<std::string, std::string>& baseline,
                         const std::string& path) {
    const WorktreeSourceState* current = current_source(inventory, path);
    if (current == nullptr) return baseline.contains(path) ? "removed" : "unobserved";
    if (current->content_hash.empty()) return "unobserved";
    const auto previous = baseline.find(path);
    if (previous == baseline.end()) return "added";
    return current->content_hash == previous->second ? "unchanged" : "changed";
}

struct ImpactPath {
    int depth = 0;
    std::set<std::size_t> relations;
};

std::map<std::string, ImpactPath> impacted_paths(
    const Json& changes, const SourceGraph& graph, int max_depth,
    const std::set<std::string>& owned_paths) {
    std::map<std::string, ImpactPath> result;
    std::set<std::string> frontier;
    for (const Json& detail : changes.at("details")) {
        const std::string path = json_string_value(detail, "path");
        if (!path.empty()) {
            result[path] = {};
            frontier.insert(path);
        }
    }
    for (int depth = 1; depth <= max_depth; ++depth) {
        std::set<std::string> next;
        for (const std::string& path : frontier) {
            for (const auto* adjacency : {&graph.incoming, &graph.outgoing}) {
                const auto found = adjacency->find(path);
                if (found == adjacency->end()) continue;
                for (const std::size_t index : found->second) {
                    const LinkRow& relation = graph.links[index];
                    const std::string& other = relation.path == path ? relation.resolved_path : relation.path;
                    if (other == path || !owned_paths.contains(other)) continue;
                    auto [candidate, inserted] = result.try_emplace(other, ImpactPath{depth, {}});
                    if (inserted) next.insert(other);
                    candidate->second.relations.insert(index);
                }
            }
        }
        frontier = std::move(next);
    }
    return result;
}

Json scope_json(const FactRow& row, const std::string& basis, const std::string& comparison) {
    return {{"symbol", truncate_text(row.symbol_name.empty() ? row.title : row.symbol_name, 240)},
            {"record_type", row.record_type}, {"start_line", row.start_line}, {"end_line", row.end_line},
            {"range_origin", row.range_origin}, {"span_hash", row.span_hash.empty() ? Json(nullptr) : Json(row.span_hash)},
            {"basis", basis}, {"comparison", comparison}, {"evidence_kind", row.evidence_kind},
            {"verification_level", row.verification_level}, {"status", row.status},
            {"authority_level", row.authority_level}};
}

std::string read_observed_source(const fs::path& path, const WorktreeSourceState& expected) {
    const FileFingerprint before = fingerprint_file(path);
    if (!expected.has_metadata || before.metadata != expected.metadata || before.content_hash != expected.content_hash) {
        throw_file_changed_during_verification(path);
    }
    std::ifstream stream(path, std::ios::binary);
    std::string content(static_cast<std::size_t>(expected.byte_count), '\0');
    if (!stream || !stream.read(content.data(), static_cast<std::streamsize>(content.size())) ||
        stream.peek() != std::char_traits<char>::eof() || fnv1a64(content) != expected.content_hash) {
        throw_file_changed_during_verification(path);
    }
    return content;
}

Json path_evidence(const RuntimeOptions& options, const std::string& path, const ImpactPath& candidate,
                   const SourceGraph& graph, const WorktreeInventory& inventory,
                   const std::map<std::string, std::string>& baseline, std::size_t& read_bytes) {
    const std::string state = source_state(inventory, baseline, path);
    const WorktreeSourceState* current = current_source(inventory, path);
    Json record = {{"path", path}, {"depth", candidate.depth},
                   {"reason", candidate.depth == 0 ? "changed-profile-input" : "declared-dependency-neighbor"},
                   {"source_state", state}, {"source_hash", current == nullptr || current->content_hash.empty()
                       ? Json(nullptr) : Json(current->content_hash)},
                   {"relations", Json::array()}, {"scopes", Json::array()},
                   {"relations_omitted", candidate.relations.size() > 8 ? candidate.relations.size() - 8 : 0},
                   {"current_reread_omitted", false}, {"syntax_observation_complete", true},
                   {"scope_coverage", "recommendations-not-complete-text-diff"}, {"test_source", false}};
    for (const std::size_t index : candidate.relations) {
        if (record["relations"].size() == 8) break;
        const LinkRow& relation = graph.links[index];
        record["relations"].push_back({{"from", relation.path}, {"to", relation.resolved_path},
            {"kind", relation.kind}, {"line", relation.line}, {"basis", "published-snapshot"},
            {"source_state", source_state(inventory, baseline, relation.path)},
            {"verification_level", source_relation_kind(relation.kind) ? "source-observed" : "declared"},
            {"runtime_reachability_proven", false}});
    }
    const GeneratedPathProbe prior = probe_generated_path(options, path, 256);
    std::set<std::string> previous_spans;
    for (const FactRow& row : prior.facts) {
        if (!row.span_hash.empty()) previous_spans.insert(row.record_type + "\n" + row.symbol_name + "\n" + row.span_hash);
        if (row.evidence_kind == "test-source") record["test_source"] = true;
    }
    record["baseline_scope_probe_complete"] = prior.complete;
    std::vector<FactRow> rows;
    if (current != nullptr && !current->content_hash.empty() && inventory.source_facts.contains(path) &&
        current->byte_count <= 2U * 1024U * 1024U && read_bytes + current->byte_count <= 16U * 1024U * 1024U) {
        const fs::path full_path = options.root / path;
        std::error_code error;
        if (fs::is_symlink(fs::symlink_status(full_path, error)) || error) {
            throw_file_changed_during_verification(full_path);
        }
        const std::string content = read_observed_source(full_path, *current);
        read_bytes += content.size();
        SourceRelationInputs unused_relations;
        inspect_source_fact_content(options.profile, options.root, path, content, rows, unused_relations);
        std::vector<FactRow> changed;
        for (FactRow& row : rows) {
            if (row.evidence_kind == "test-source") record["test_source"] = true;
            if (row.record_type.ends_with("-syntax-incomplete")) record["syntax_observation_complete"] = false;
            const std::string key = row.record_type + "\n" + row.symbol_name + "\n" + row.span_hash;
            if (state == "unchanged" || row.span_hash.empty() || !previous_spans.contains(key)) changed.push_back(std::move(row));
        }
        rows = std::move(changed);
        record["current_scope_candidates"] = rows.size();
        std::sort(rows.begin(), rows.end(), [](const FactRow& a, const FactRow& b) {
            auto priority = [](const FactRow& row) {
                return row.range_origin == "syntax" ? 0 : row.range_origin == "payload-file" ? 1 : 2;
            };
            return std::tuple(priority(a), a.end_line - a.start_line, a.start_line, a.id) <
                   std::tuple(priority(b), b.end_line - b.start_line, b.start_line, b.id);
        });
        record["scopes_omitted"] = rows.size() > 6 ? rows.size() - 6 : 0;
        for (std::size_t i = 0; i < std::min<std::size_t>(rows.size(), 6); ++i) {
            record["scopes"].push_back(scope_json(rows[i], "observed-worktree", state == "unchanged"
                ? "source-bytes-current" : "span-not-seen-in-bounded-baseline"));
        }
        record["unlocalized_source_change"] = state != "unchanged" && rows.empty();
    } else {
        record["current_reread_omitted"] = current != nullptr && !current->content_hash.empty() &&
                                           inventory.source_facts.contains(path);
        record["current_scope_candidates"] = nullptr;
        record["scopes_omitted"] = prior.facts.empty()
            ? (prior.sections.size() > 3 ? prior.sections.size() - 3 : 0)
            : (prior.facts.size() > 6 ? prior.facts.size() - 6 : 0);
        record["unlocalized_source_change"] = state != "unchanged";
        record["syntax_observation_complete"] = std::none_of(prior.facts.begin(), prior.facts.end(), [](const FactRow& row) {
            return row.record_type.ends_with("-syntax-incomplete");
        });
        const std::string basis = state == "unchanged" ? "current-published-source" : "historical-snapshot";
        for (std::size_t i = 0; i < std::min<std::size_t>(prior.facts.size(), 6); ++i) {
            record["scopes"].push_back(scope_json(prior.facts[i], basis, state));
        }
        if (record["scopes"].empty()) {
            for (std::size_t i = 0; i < std::min<std::size_t>(prior.sections.size(), 3); ++i) {
                const SectionRow& section = prior.sections[i];
                record["scopes"].push_back({{"start_line", section.start_line}, {"end_line", section.end_line},
                    {"heading", truncate_text(section.heading, 240)}, {"basis", basis}, {"comparison", state},
                    {"evidence_kind", section.evidence_kind}, {"verification_level", section.verification_level},
                    {"status", section.status}, {"authority_level", section.authority_level}});
            }
        }
    }
    return record;
}

void validate_impact_evaluation_options(const ImpactEvaluationOptions& command,
                                        const std::optional<std::string_view>& receipt_bundle) {
    if (command.limit < 1 || command.limit > 128 || command.depth < 0 || command.depth > 2 ||
        command.evidence_bytes < 2 || command.evidence_bytes > 131072) {
        throw_usage_error("invalid-impact-options", "Impact evaluation options exceed their bounded contract.");
    }
    if (command.receipt_options.has_value() != receipt_bundle.has_value()) {
        throw_usage_error("invalid-impact-receipts", "Impact receipt options and receipt bundle must be supplied together.");
    }
    if (command.receipt_options) validate_session_evaluation_options(*command.receipt_options, true);
}

}  // namespace

Json evaluate_impact(const RuntimeOptions& input, const ImpactEvaluationOptions& command,
                     std::optional<std::string_view> receipt_bundle) {
    validate_impact_evaluation_options(command, receipt_bundle);
    const RuntimeOptions requested_options = prepare_impact_options(input);
    const std::unique_ptr<RetrievalSession> session = open_session_evaluation(requested_options);
    const RuntimeOptions& options = session->options();
    const std::string snapshot_id = session->freshness().snapshot_id;
    const Json saved_receipts = receipt_bundle ? parse_execution_bundle(parse_session_document(*receipt_bundle))
                                               : Json::array();
    const WorktreeInventory inventory = observe_worktree(options);
    Json changes = compare_worktree(options, session->manifest(), inventory, std::min(1000, command.limit * 2 + 8));
    const bool profile_current = changes.at("comparison").at("profile_config_current").get<bool>();
    bool index_current = changes.at("current").get<bool>() && worktree_observation_complete(inventory);
    if (index_current) {
        const IndexFreshness freshness = assess_index_freshness(options, true);
        index_current = freshness.current && freshness.fully_verified && freshness.snapshot_id == snapshot_id;
    }
    const auto baseline = baseline_hashes(session->manifest());
    std::set<std::string> owned;
    for (const auto& [path, hash] : baseline) { (void)hash; owned.insert(path); }
    for (const auto& [path, state] : inventory.documents) { (void)state; owned.insert(path); }
    for (const auto& [path, state] : inventory.source_facts) { (void)state; owned.insert(path); }
    const SourceGraph graph = load_source_graph(options);
    const auto candidates = impacted_paths(changes, graph, profile_current ? command.depth : 0, owned);
    std::vector<std::pair<std::string, ImpactPath>> ordered(candidates.begin(), candidates.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return std::tie(a.second.depth, a.first) < std::tie(b.second.depth, b.first);
    });
    Json evidence = Json::array();
    Json test_sources = Json::array();
    std::size_t bytes = 2;
    std::size_t read_bytes = 0;
    bool byte_limited = false;
    bool evidence_complete = true;
    if (profile_current) {
        for (const auto& [path, candidate] : ordered) {
            if (evidence.size() == static_cast<std::size_t>(command.limit)) break;
            Json record = path_evidence(options, path, candidate, graph, inventory, baseline, read_bytes);
            const std::size_t size = record.dump().size() + (evidence.empty() ? 0 : 1);
            if (bytes + size > command.evidence_bytes) { byte_limited = true; break; }
            bytes += size;
            evidence_complete = evidence_complete && record.at("baseline_scope_probe_complete").get<bool>() &&
                !record.at("current_reread_omitted").get<bool>() && record.at("syntax_observation_complete").get<bool>() &&
                record.at("relations_omitted").get<std::size_t>() == 0 &&
                (record.at("scopes_omitted").is_null() || record.at("scopes_omitted").get<std::size_t>() == 0);
            if (record.at("test_source").get<bool>()) test_sources.push_back({{"path", path},
                {"reason", "changed-or-statically-related-test-source"}, {"execution", "not-run"}, {"requires_review", true}});
            evidence.push_back(std::move(record));
        }
    }
    require_worktree_stable(options, inventory);
    const bool selection_complete = profile_current && worktree_observation_complete(inventory) &&
        changes.at("details_omitted").get<int>() == 0 && evidence.size() == ordered.size();
    const bool inspection_complete = selection_complete && evidence_complete;
    const bool ok = (!command.require_current || index_current) && (!command.strict || inspection_complete);
    Json output = make_result_document("shin.impact-pack.v1", "impact", ok);
    output["profile"] = options.profile.id;
    output["root"] = options.root.generic_string();
    output["out_root"] = options.out_root.generic_string();
    output["snapshot_id"] = snapshot_id;
    output["worktree"] = changes.at("worktree");
    output["index_current"] = index_current;
    output["changes"] = std::move(changes);
    output["policy"] = {{"read_only", true}, {"publication", false}, {"actions_executed", false},
        {"graph_basis", "published-snapshot"}, {"runtime_reachability_proven", false},
        {"complete_semantic_impact", false}, {"depth", command.depth}};
    output["reread"] = std::move(evidence);
    output["tests_to_review"] = std::move(test_sources);
    output["coverage"] = {{"selection_complete", selection_complete}, {"evidence_complete", evidence_complete},
        {"inspection_complete", inspection_complete}, {"candidate_paths", ordered.size()},
        {"returned_paths", output.at("reread").size()}, {"omitted_paths", ordered.size() - output.at("reread").size()},
        {"evidence_bytes", bytes}, {"max_evidence_bytes", command.evidence_bytes}, {"byte_budget_constrained", byte_limited},
        {"profile_configuration_current", profile_current}, {"unresolved_snapshot_relations", graph.unresolved}};
    output["next_actions"] = Json::array();
    if (!index_current) output["next_actions"].push_back({{"code", "review-changes-then-publish"},
        {"text", "Review current paths; explicitly rebuild and rerun ready before ordinary strict retrieval."},
        {"automatic_execution", false}});
    if (!inspection_complete) output["next_actions"].push_back({{"code", "review-impact-omissions"},
        {"text", "Narrow the profile or review omitted changes before treating this bounded list as complete."},
        {"automatic_execution", false}});
    if (command.receipt_options) {
        SessionEvaluationOptions receipt_command = *command.receipt_options;
        Json scope = session_binding(options, receipt_command);
        scope["worktree_id"] = output.at("worktree").at("worktree_id");
        scope["snapshot_id"] = snapshot_id;
        auto assessed = assess_execution_receipts(options, saved_receipts, scope, receipt_command,
                                                  worktree_observation_complete(inventory));
        output["receipts"] = std::move(assessed.receipts);
        output["receipts_observed_at"] = assessed.observed_at;
        for (const Json& action : execution_receipt_actions(output.at("receipts"))) output["next_actions"].push_back(action);
        require_worktree_stable(options, inventory);
    }
    output["require_current"] = command.require_current;
    output["strict"] = command.strict;
    const std::string serialized = output.dump();
    if (serialized.size() > 512U * 1024U) throw CliError("impact-output-too-large", "resource",
        "Impact envelope exceeds 512 KiB.", 1, false, "Reduce --limit or narrow the profile.");
    return output;
}

int run_impact(const RuntimeOptions& input, const std::vector<std::string>& args) {
    const ImpactCommandOptions command = parse_impact_options(args);
    const RuntimeOptions options = prepare_impact_options(input);
    std::optional<std::string> receipt_bundle;
    if (command.receipt_input) {
        receipt_bundle = read_session_input(*command.receipt_input, options.root).dump();
    }
    const Json output = evaluate_impact(options, command.evaluation,
                                        receipt_bundle ? std::optional<std::string_view>(*receipt_bundle) : std::nullopt);
    std::cout << output.dump() << '\n';
    return output.at("ok").get<bool>() ? 0 : 1;
}

}  // namespace shin::context
