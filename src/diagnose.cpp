#include "context_internal.h"

namespace shin::context {
namespace {

constexpr std::size_t kMaxDiagnoseQueryBytes = 16384;
constexpr int kDiagnoseProbeLimit = 100;
constexpr int kDiagnoseProbePerDocument = 8;

struct DiagnoseRequest {
    QueryRequest query;
    std::string expected_path;
    bool strict = false;
};

struct IndexedPathEvidence {
    int section_rows = 0;
    int fact_rows = 0;
    int decision_rows = 0;
    int contract_rows = 0;
    int runbook_rows = 0;
    int best_score = 0;
    int best_matched_groups = 0;
    int best_line = 0;
    std::string best_record_type;
    std::string best_evidence_kind;
    std::string best_verification_level;
    std::vector<std::string> best_matched_fields;
    std::vector<int> best_matched_group_indices;
    bool probe_complete = true;
    int probe_rows_examined = 0;
    CandidateRows row_ids;
    std::set<int> document_groups;

    int total_rows() const {
        return section_rows + fact_rows + decision_rows + contract_rows +
               runbook_rows;
    }
};

struct PublishedSourceState {
    bool document = false;
    bool source_fact = false;
    bool source_fact_configured = false;
    std::string source_fact_status;
};

DiagnoseRequest parse_diagnose_request(const Profile& profile,
                                       const std::vector<std::string>& args) {
    std::vector<std::string> query_args;
    std::string expected_path;
    bool strict = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--strict") {
            strict = true;
            continue;
        }
        if (args[index] != "--expected-path") {
            query_args.push_back(args[index]);
            continue;
        }
        if (!expected_path.empty()) {
            throw_usage_error("duplicate-option", "--expected-path may be supplied only once.");
        }
        if (index + 1 >= args.size()) {
            throw_usage_error("missing-option-value", "--expected-path requires a relative path.");
        }
        const std::string& raw_path = args[++index];
        const std::string normalized = fs::path(raw_path).lexically_normal().generic_string();
        if (raw_path.size() > 4096 || !profile_relative_path_valid(raw_path) ||
            normalized != raw_path) {
            throw_usage_error("invalid-expected-path",
                              "Expected path must be a canonical safe repository-relative path no longer than 4096 bytes.");
        }
        expected_path = raw_path;
    }
    if (expected_path.empty()) {
        throw_usage_error("missing-option", "diagnose requires --expected-path PATH.");
    }
    QueryRequest query = make_command_query_request(profile, query_args, 6, 2);
    if (query.query.size() > kMaxDiagnoseQueryBytes) {
        throw CliError("diagnose-query-too-large", "resource",
                       "diagnose query exceeds 16384 bytes.", 1, false,
                       "Use a narrower retrieval query.");
    }
    return {std::move(query), std::move(expected_path), strict};
}

int path_rank(const std::vector<RankedBriefPath>& paths, const std::string& expected_path) {
    for (const RankedBriefPath& path : paths) {
        if (path.path == expected_path) {
            return path.unique_path_rank;
        }
    }
    return 0;
}

void retain_best_match(IndexedPathEvidence& evidence,
                       const MatchScore& score,
                       int line,
                       const std::string& record_type,
                       const std::string& evidence_kind,
                       const std::string& verification_level) {
    evidence.document_groups.insert(score.matched_group_indices.begin(), score.matched_group_indices.end());
    if (score.matched_groups < evidence.best_matched_groups ||
        (score.matched_groups == evidence.best_matched_groups && score.total <= evidence.best_score)) {
        return;
    }
    evidence.best_score = score.total;
    evidence.best_matched_groups = score.matched_groups;
    evidence.best_line = line;
    evidence.best_record_type = record_type;
    evidence.best_evidence_kind = evidence_kind;
    evidence.best_verification_level = verification_level;
    evidence.best_matched_fields = score.matched_fields;
    evidence.best_matched_group_indices = score.matched_group_indices;
}

IndexedPathEvidence inspect_indexed_path(const RuntimeOptions& options,
                                         const QueryRequest& request,
                                         const std::string& expected_path) {
    IndexedPathEvidence evidence;
    const GeneratedPathProbe probe = probe_generated_path(options, expected_path);
    evidence.probe_complete = probe.complete;
    evidence.probe_rows_examined = probe.rows_examined;
    evidence.row_ids = probe.row_ids;
    for (const SectionRow& row : probe.sections) {
        ++evidence.section_rows;
        retain_best_match(evidence, score_section_match(row, request.prepared_term_groups, 1),
                          row.line, row.row_type, row.evidence_kind,
                          row.verification_level);
    }
    for (const FactRow& row : probe.facts) {
        const std::string group = brief_fact_group(row);
        if (group == "decisions") {
            ++evidence.decision_rows;
        } else if (group == "contracts") {
            ++evidence.contract_rows;
        } else if (group == "runbooks") {
            ++evidence.runbook_rows;
        } else {
            ++evidence.fact_rows;
        }
        retain_best_match(evidence, score_fact_match(row, request.prepared_term_groups, 1),
                          row.line, row.record_type, row.evidence_kind,
                          row.verification_level);
    }
    return evidence;
}

PublishedSourceState published_source_state(const Json& manifest, const Profile& profile,
                                            const std::string& expected_path) {
    PublishedSourceState state;
    state.source_fact_configured =
        path_matches_any_profile_prefix(expected_path, profile.source_fact_roots);
    const Json* sources = find_json_field(manifest, "sources");
    if (sources != nullptr && sources->is_array()) {
        for (const Json& source : *sources) {
            if (json_string_value(source, "path") == expected_path) {
                state.document = true;
                break;
            }
        }
    }
    const Json* source_facts = find_json_field(manifest, "source_fact_sources");
    if (source_facts != nullptr && source_facts->is_array()) {
        for (const Json& source : *source_facts) {
            if (json_string_value(source, "path") == expected_path) {
                state.source_fact = true;
                state.source_fact_status = json_string_value(source, "status");
                break;
            }
        }
    }
    return state;
}

bool selected_by_planner(const QueryCoverage& coverage, const std::string& expected_path) {
    return std::any_of(coverage.groups.begin(), coverage.groups.end(),
                       [&](const QueryGroupCoverage& group) {
        return group.selected_document_paths.find(expected_path) !=
               group.selected_document_paths.end();
    });
}

std::vector<std::string> diagnose_reason_codes(
    bool met, int probe_rank, bool route_changed, bool source_exists, bool policy_allowed,
    const PublishedSourceState& published, const IndexedPathEvidence& evidence,
    int required_group_matches, bool planner_used, bool planner_selected) {
    if (met) {
        return {"returned"};
    }
    std::vector<std::string> reasons;
    if (!source_exists) {
        reasons.push_back("source-missing");
    }
    if (!policy_allowed && !published.source_fact_configured) {
        reasons.push_back("excluded-by-profile");
    }
    if (published.source_fact_status == "skipped-large") {
        reasons.push_back("source-fact-skipped-large");
    }
    if (!published.document && !published.source_fact) {
        reasons.push_back("not-in-published-snapshot");
    }
    if (evidence.total_rows() == 0) {
        reasons.push_back("no-indexed-records");
    }
    if (!evidence.probe_complete) {
        reasons.push_back("probe-limit-exceeded");
    }
    if (evidence.best_matched_groups == 0) {
        reasons.push_back("no-query-groups-matched");
    } else if (evidence.best_matched_groups < required_group_matches) {
        reasons.push_back("below-required-threshold");
    }
    if (probe_rank > 0) {
        reasons.push_back("returned-with-expanded-budget");
        if (route_changed) reasons.push_back("retrieval-route-changed");
    }
    if (planner_used && !planner_selected) {
        reasons.push_back("not-selected-by-planner");
    }
    if (reasons.empty()) {
        reasons.push_back("matched-but-not-returned");
    }
    return reasons;
}

std::string_view diagnose_retrieval_mode(const BriefResult& retrieval) {
    if (retrieval.directory_owner_scope_used) return "directory-scoped";
    if (retrieval.document_owner_scope_used) return "owner-scoped";
    if (retrieval.identifier_scope_used) return "identifier-scoped";
    if (retrieval.query_planner_used) return "planned";
    return retrieval.fallback_used ? "relaxed" : "strict";
}

bool regular_source_path_exists(const fs::path& path) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    return !error && fs::is_regular_file(status) && !fs::is_symlink(status);
}

Json ordinary_candidate_probe(const RuntimeOptions& options, const QueryRequest& request,
                              const IndexedPathEvidence& evidence, int required) {
    const auto candidates = load_generated_query_candidates(options, request.prepared_term_groups, required);
    std::size_t admitted = 0;
    for (const auto member : {&CandidateRows::sections, &CandidateRows::facts, &CandidateRows::decisions,
                              &CandidateRows::contracts, &CandidateRows::runbooks}) {
        for (const int id : evidence.row_ids.*member) {
            if ((candidates.*member).contains(id)) ++admitted;
        }
    }
    return {{"required_group_matches", required},
            {"candidate_index_used", candidates.from_terms_index},
            {"candidate_rows", candidates.discovered_row_count},
            {"expected_path_admitted_rows", candidates.from_terms_index ? Json(admitted) : Json(nullptr)},
            {"path_probe_complete", evidence.probe_complete},
            {"stage", candidates.from_terms_index ? "term-postings" : "generated-row-scan"}};
}

}  // namespace

int run_diagnose(const RuntimeOptions& options, const std::vector<std::string>& args) {
    const DiagnoseRequest request = parse_diagnose_request(options.profile, args);
    Json output;
    bool met = false;
    {
        RetrievalSession session(options, request.query.require_current);
        const BriefExecution execution = session.brief_execution(request.query);
        const int returned_rank = path_rank(execution.ranked_paths, request.expected_path);
        met = returned_rank > 0;
        int probe_rank = 0;
        std::optional<BriefExecution> expanded;
        if (!met) {
            QueryRequest probe = request.query;
            probe.limit = std::max(probe.limit, kDiagnoseProbeLimit);
            probe.per_doc_limit = std::max(probe.per_doc_limit, kDiagnoseProbePerDocument);
            expanded = session.brief_execution(probe);
            probe_rank = path_rank(expanded->ranked_paths, request.expected_path);
        }

        const IndexedPathEvidence evidence =
            inspect_indexed_path(session.options(), request.query, request.expected_path);
        const bool source_exists =
            regular_source_path_exists(session.options().root / request.expected_path);
        const bool policy_allowed =
            document_path_allowed(session.options().profile, request.expected_path);
        const PublishedSourceState published = published_source_state(
            session.manifest(), session.options().profile, request.expected_path);
        const BriefResult& retrieval = execution.retrieval;
        const auto retrieval_mode = diagnose_retrieval_mode(retrieval);
        const bool route_changed = expanded &&
            diagnose_retrieval_mode(expanded->retrieval) != retrieval_mode;
        const bool planner_selected =
            selected_by_planner(execution.coverage, request.expected_path);
        const std::vector<std::string> reasons = diagnose_reason_codes(
            met, probe_rank, route_changed, source_exists, policy_allowed, published, evidence,
            retrieval.required_group_matches, retrieval.query_planner_used, planner_selected);
        std::vector<std::string> ranked_paths;
        Json path_ranks = Json::array();
        for (const RankedBriefPath& path : execution.ranked_paths) {
            ranked_paths.push_back(path.path);
            path_ranks.push_back({{"path", path.path},
                                  {"unique_path_rank", path.unique_path_rank},
                                  {"first_evidence_rank", path.first_evidence_rank}});
        }
        output = make_result_document("shin.diagnose.v1", "diagnose", met);
        output["strict"] = request.strict;
        output["profile"] = session.options().profile.id;
        output["root"] = session.options().root.generic_string();
        output["out_root"] = session.options().out_root.generic_string();
        output["snapshot_id"] = session.freshness().snapshot_id;
        output["index_freshness"] = index_freshness_json(session.freshness());
        output["freshness_scope"] = "session-open";
        output["query"] = request.query.query;
        output["expected_path"] = request.expected_path;
        output["expectation"] = {{"met", met}, {"reason_codes", reasons}};
        output["outcome"] = reasons.front();
        output["returned_path_rank"] = returned_rank > 0 ? Json(returned_rank) : Json(nullptr);
        output["ranking_policy"] = "global-evidence-order-unique-path-v1";
        output["source"] = {
            {"exists", source_exists},
            {"document_policy_allowed", policy_allowed},
            {"published_document", published.document},
            {"source_fact_configured", published.source_fact_configured},
            {"published_source_fact", published.source_fact},
            {"source_fact_status", published.source_fact_status.empty()
                                       ? Json(nullptr)
                                       : Json(published.source_fact_status)}
        };
        output["indexed_rows"] = {
            {"total", evidence.total_rows()},
            {"sections", evidence.section_rows},
            {"facts", evidence.fact_rows},
            {"decisions", evidence.decision_rows},
            {"contracts", evidence.contract_rows},
            {"runbooks", evidence.runbook_rows},
            {"probe_complete", evidence.probe_complete},
            {"probe_rows_examined", evidence.probe_rows_examined}
        };
        output["matching"] = {
            {"policy", "indexed-lexical-fields-v1"},
            {"scope", "independent-expected-path-probe-not-selection"},
            {"document_matched_groups", evidence.document_groups.size()},
            {"required_group_matches", retrieval.required_group_matches},
            {"total_term_groups", retrieval.total_term_groups},
            {"best_matched_groups", evidence.best_matched_groups},
            {"best_score", evidence.best_score},
            {"best_line", evidence.best_line > 0 ? Json(evidence.best_line) : Json(nullptr)},
            {"best_record_type", evidence.best_record_type.empty()
                                     ? Json(nullptr)
                                     : Json(evidence.best_record_type)},
            {"best_evidence_kind", evidence.best_evidence_kind.empty()
                                       ? Json(nullptr)
                                       : Json(evidence.best_evidence_kind)},
            {"best_verification_level", evidence.best_verification_level.empty()
                                             ? Json(nullptr)
                                             : Json(evidence.best_verification_level)},
            {"best_matched_fields", evidence.best_matched_fields},
            {"best_matched_group_indices", evidence.best_matched_group_indices}
        };
        output["ordinary_row_candidate_probe"] = {
            {"scope", "ordinary-row-clauses-not-owner-or-document-planner-selection"},
            {"strict", ordinary_candidate_probe(session.options(), request.query, evidence,
                                                 strict_required_group_matches(request.query))},
            {"relaxed", ordinary_candidate_probe(session.options(), request.query, evidence,
                                                  relaxed_required_group_matches(request.query))}
        };
        output["budget_probe"] = {
            {"executed", expanded.has_value()},
            {"limit", kDiagnoseProbeLimit},
            {"per_doc_limit", kDiagnoseProbePerDocument},
            {"retrieval_mode", expanded ? Json(diagnose_retrieval_mode(expanded->retrieval)) : Json(nullptr)},
            {"route_changed", expanded ? Json(route_changed) : Json(nullptr)},
            {"candidate_rows", expanded ? Json(expanded->retrieval.candidate_rows) : Json(nullptr)},
            {"required_group_matches", expanded ? Json(expanded->retrieval.required_group_matches) : Json(nullptr)},
            {"returned_path_rank", probe_rank > 0 ? Json(probe_rank) : Json(nullptr)}
        };
        output["retrieval"] = {
            {"mode", retrieval_mode},
            {"fallback_used", retrieval.fallback_used},
            {"query_planner_used", retrieval.query_planner_used},
            {"planner_rejection_reason", retrieval.planner_rejection_reason.empty()
                                             ? Json(nullptr)
                                             : Json(retrieval.planner_rejection_reason)},
            {"candidate_rows", retrieval.candidate_rows},
            {"query_coverage", query_coverage_json(execution.coverage)},
            {"ranked_paths", path_ranks},
            {"related_documents", ranked_paths},
            {"selected_by_planner", planner_selected}
        };
        if (retrieval.identifier_probe_rows > 0) {
            output["retrieval"]["identifier_scope_used"] = retrieval.identifier_scope_used;
            output["retrieval"]["identifier_scope_key"] = retrieval.identifier_scope_key;
            output["retrieval"]["identifier_probe_rows"] = retrieval.identifier_probe_rows;
        }
    }
    emit_json_document(std::cout, output);
    return request.strict && !met ? 1 : 0;
}

}  // namespace shin::context
