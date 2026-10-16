#include "context_internal.h"
#ifdef SHIN_EXPERIMENT_AGGREGATE_RECOVERY
#include "aggregate_recovery_experiment.h"
#endif
#ifdef SHIN_EXPERIMENT_EVAL_BATCH
#include "eval_batch_experiment.h"
#include "eval_batch_route_probe.h"
#endif

namespace shin::context {

static void emit_retrieval_document(std::ostream& out, const Json& document) {
    out << "{\n";
    std::size_t index = 0;
    for (auto field = document.begin(); field != document.end(); ++field, ++index) {
        out << "  " << json_string(field.key()) << ": " << field->dump();
        if (index + 1 < document.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "}\n";
}

static BriefExecution make_brief_execution(const QueryRequest& request,
                                            BriefResult retrieval);

struct BriefCommandRequest {
    BriefProjection projection = BriefProjection::full;
    std::vector<std::string> query_args;
};

static BriefCommandRequest parse_brief_command_request(const std::vector<std::string>& args) {
    BriefCommandRequest request;
    bool projection_seen = false;
    bool options_ended = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (options_ended) {
            request.query_args.push_back(args[index]);
            continue;
        }
        if (args[index] == "--") {
            options_ended = true;
            request.query_args.push_back(args[index]);
            continue;
        }
        if (args[index] != "--projection") {
            request.query_args.push_back(args[index]);
            continue;
        }
        if (projection_seen || index + 1 >= args.size()) {
            throw_usage_error(projection_seen ? "duplicate-option" : "missing-option-value",
                              projection_seen ? "--projection may be supplied only once."
                                              : "--projection requires full or compact.");
        }
        const std::string& value = args[++index];
        if (value == "full") {
            request.projection = BriefProjection::full;
        } else if (value == "compact") {
            request.projection = BriefProjection::compact;
        } else {
            throw_usage_error("invalid-option-value", "--projection requires full or compact.");
        }
        projection_seen = true;
    }
    return request;
}

static Json make_context_json(const RuntimeOptions& options,
                              const QueryRequest& request,
                              const std::string& index_mode,
                              const IndexFreshness& freshness,
                              int source_count,
                              int matched_sections,
                              int matched_facts,
                              const std::vector<ContextHit>& selected_hits,
                              const std::vector<FactHit>& fact_hits,
                              const RetrievalDiagnostics& diagnostics) {
    const int returned = static_cast<int>(selected_hits.size());
    const int returned_facts = static_cast<int>(fact_hits.size());
    std::vector<std::string> related_documents;
    std::set<std::string> seen_documents;
    for (const ContextHit& hit : selected_hits) {
        add_related_document(related_documents, seen_documents, hit.section.path);
    }
    QueryCoverage coverage = make_query_coverage(request);
    for (const ContextHit& hit : selected_hits) {
        add_query_coverage_evidence(coverage, hit.section.path, hit.matched_group_indices);
    }
    for (const FactHit& hit : fact_hits) {
        add_query_coverage_evidence(coverage, hit.fact.path, hit.matched_group_indices);
    }
    Json output = make_result_document("shin.context-pack.v1", "context", true);
    output["profile"] = options.profile.id;
    output["index_mode"] = index_mode;
    output["index_freshness"] = index_freshness_json(freshness);
    output["snapshot_id"] = freshness.snapshot_id.empty() ? Json(nullptr) : Json(freshness.snapshot_id);
    output["query"] = request.query;
    output["retrieval_mode"] = diagnostics.fallback_used ? "relaxed" : "strict";
    output["query_diagnostics"] = {
        {"fallback_used", diagnostics.fallback_used},
        {"candidate_rows", diagnostics.candidate_rows},
        {"candidate_index_used", diagnostics.candidate_index_used},
        {"required_group_matches", diagnostics.required_group_matches},
        {"total_term_groups", diagnostics.total_term_groups},
        {"matching_mode", kMatchingMode},
        {"topic_match_policy", "booster-only"}
    };
    output["query_coverage"] = query_coverage_json(coverage);
    output["limit"] = request.limit;
    output["per_doc_limit"] = request.per_doc_limit;
    output["source_count"] = source_count;
    output["matched_sections"] = matched_sections;
    output["returned_sections"] = returned;
    output["matched_facts"] = matched_facts;
    output["returned_facts"] = returned_facts;
    output["related_documents"] = related_documents;
    output["sections"] = Json::array();
    output["facts"] = Json::array();
    for (const ContextHit& hit : selected_hits) {
        output["sections"].push_back(section_hit_json(hit));
    }
    for (const FactHit& hit : fact_hits) {
        output["facts"].push_back(fact_hit_json(hit));
    }
    return output;
}

static int emit_context_json(const RuntimeOptions& options,
                             const QueryRequest& request,
                             const std::string& index_mode,
                             const IndexFreshness& freshness,
                             int source_count,
                             int matched_sections,
                             int matched_facts,
                             const std::vector<ContextHit>& selected_hits,
                             const std::vector<FactHit>& fact_hits,
                             const RetrievalDiagnostics& diagnostics) {
    emit_retrieval_document(std::cout,
                            make_context_json(options, request, index_mode, freshness, source_count,
                                              matched_sections, matched_facts, selected_hits,
                                              fact_hits, diagnostics));
    return 0;
}

static ContextIndexResult collect_context_from_index(const RuntimeOptions& options,
                                              const QueryRequest& request,
                                              int required_group_matches,
                                              bool fallback_used) {
    ContextIndexResult result;
    result.diagnostics = make_retrieval_diagnostics(request, required_group_matches, fallback_used);
    const CandidateRows candidates =
        load_generated_query_candidates(options, request.prepared_term_groups, required_group_matches);
    result.diagnostics.candidate_rows = candidates.discovered_row_count;
    result.diagnostics.candidate_index_used = candidates.from_terms_index;
    for_each_generated_section_matching_terms(options, candidates, request.prepared_term_groups,
                                              required_group_matches,
                                              [&](const SectionRow& section) {
        MatchScore match = score_section_match(section, request.prepared_term_groups, required_group_matches);
        if (match.total > 0) {
            ++result.matched_sections;
            push_top_context_hit(result.section_hits_by_doc[section.path],
                                 {section, match.total, match.components, match.matched_fields,
                                  match.matched_group_indices},
                                 request.per_doc_limit);
        }
    });
    for_each_generated_fact_matching_terms(options, candidates, request.prepared_term_groups,
                                           required_group_matches,
                                           [&](const FactRow& fact) {
        MatchScore match = score_fact_match(fact, request.prepared_term_groups, required_group_matches);
        if (match.total > 0) {
            ++result.matched_facts;
            push_top_fact_hit(result.fact_hits,
                              {fact, match.total, match.components, match.matched_fields,
                               match.matched_group_indices},
                              request.limit);
        }
    });
    return result;
}

ContextIndexResult collect_context_from_index_with_fallback(const RuntimeOptions& options,
                                                            const QueryRequest& request) {
    ContextIndexResult result =
        collect_context_from_index(options, request, strict_required_group_matches(request), false);
    std::set<std::string> result_paths;
    for (const auto& [path, hits] : result.section_hits_by_doc) {
        if (!hits.empty()) {
            result_paths.insert(path);
        }
    }
    for (const FactHit& hit : result.fact_hits) {
        result_paths.insert(hit.fact.path);
    }
    const bool has_results = !result.section_hits_by_doc.empty() || !result.fact_hits.empty();
    if ((has_results && !should_expand_query_coverage(request, result_paths.size())) ||
        !can_relax_query(request)) {
        return result;
    }
    return collect_context_from_index(options, request, relaxed_required_group_matches(request), true);
}

static int run_context_from_index(const RuntimeOptions& options, const QueryRequest& request) {
    RetrievalSession session(options, request.require_current);
    emit_retrieval_document(std::cout, session.context(request));
    return 0;
}

Json RetrievalSession::context(const QueryRequest& request) const {
    if (request.require_current) require_current_index_or_throw(freshness_);
    ContextIndexResult result = collect_context_from_index_with_fallback(options_, request);
    const std::vector<ContextHit> selected_hits =
        select_context_sections(result.section_hits_by_doc, request.limit, request.per_doc_limit);
    return make_context_json(options_, request, "generated-index", freshness_, freshness_.source_count,
                             result.matched_sections, result.matched_facts, selected_hits,
                             result.fact_hits, result.diagnostics);
}

static ContextIndexResult collect_context_from_sources(const RuntimeOptions&,
                                                        const QueryRequest& request,
                                                        const SourceScanCorpus& corpus,
                                                        int required_group_matches,
                                                        bool fallback_used) {
    ContextIndexResult result;
    result.diagnostics = make_retrieval_diagnostics(request, required_group_matches, fallback_used);
    for (const SectionRow& section : corpus.sections) {
        MatchScore match = score_section_match(section, request.prepared_term_groups, required_group_matches);
        if (match.total > 0) {
            ++result.matched_sections;
            push_top_context_hit(result.section_hits_by_doc[section.path],
                                 {section, match.total, match.components, match.matched_fields,
                                  match.matched_group_indices}, request.per_doc_limit);
        }
    }
    for (const FactRow& fact : corpus.facts) {
        MatchScore match = score_fact_match(fact, request.prepared_term_groups, required_group_matches);
        if (match.total > 0) {
            ++result.matched_facts;
            push_top_fact_hit(result.fact_hits,
                              {fact, match.total, match.components, match.matched_fields,
                               match.matched_group_indices}, request.limit);
        }
    }
    return result;
}

static ContextIndexResult collect_context_from_sources_with_fallback(
    const RuntimeOptions& options,
    const QueryRequest& request,
    const SourceScanCorpus& corpus) {
    ContextIndexResult result = collect_context_from_sources(
        options, request, corpus, strict_required_group_matches(request), false);
    std::set<std::string> result_paths;
    for (const auto& [path, hits] : result.section_hits_by_doc) {
        if (!hits.empty()) {
            result_paths.insert(path);
        }
    }
    for (const FactHit& hit : result.fact_hits) {
        result_paths.insert(hit.fact.path);
    }
    const bool has_results = !result.section_hits_by_doc.empty() || !result.fact_hits.empty();
    if ((has_results && !should_expand_query_coverage(request, result_paths.size())) ||
        !can_relax_query(request)) {
        return result;
    }
    return collect_context_from_sources(
        options, request, corpus, relaxed_required_group_matches(request), true);
}

static int run_context_from_sources(const RuntimeOptions& options, const QueryRequest& request) {
    const SourceScanCorpus corpus = build_source_scan_corpus(options.profile, options.root);
    const IndexFreshness freshness = source_scan_freshness(corpus);
    if (request.require_current) {
        require_complete_source_scan_or_throw(freshness);
    }
    ContextIndexResult result = collect_context_from_sources_with_fallback(options, request, corpus);
    std::vector<ContextHit> selected_hits =
        select_context_sections(result.section_hits_by_doc, request.limit, request.per_doc_limit);
    return emit_context_json(options, request, "source-scan", freshness,
                             static_cast<int>(corpus.documents.size()),
                             result.matched_sections, result.matched_facts,
                             selected_hits, result.fact_hits, result.diagnostics);
}

int run_context(const RuntimeOptions& options, const std::vector<std::string>& args) {
    QueryRequest request = make_command_query_request(options.profile, args, 8, 3);
    if (should_use_generated_index(options)) {
        return run_context_from_index(options, request);
    }
    return run_context_from_sources(options, request);
}

static Json make_brief_json(const RuntimeOptions& options,
                            const QueryRequest& request,
                            const std::string& index_mode,
                            const IndexFreshness& freshness,
                            int source_count,
                            const BriefExecution& execution,
                            BriefProjection projection) {
    if (projection == BriefProjection::compact) {
        return make_compact_brief_json(options, request, index_mode, freshness, execution);
    }
    const BriefResult& result = execution.retrieval;
    const BriefSelection& selection = execution.selection;
    const int returned_sections = static_cast<int>(selection.sections.size());
    const int returned_facts = static_cast<int>(selection.decisions.size() + selection.contracts.size() +
                                                selection.runbooks.size() + selection.facts.size());
    const std::string retrieval_mode = result.directory_owner_scope_used
                                           ? "directory-scoped"
                                           : (result.document_owner_scope_used
                                                  ? "owner-scoped"
                                           : (result.query_planner_used
                                                  ? "planned"
                                                  : (result.fallback_used ? "relaxed" : "strict")));
    const std::string matching_policy = result.directory_owner_scope_used
                                            ? "directory-owner-scope-v1"
                                            : (result.document_owner_scope_used
                                                   ? "document-owner-scope-v1"
                                            : (result.query_planner_used
                                                   ? "document-coverage-planner-v1"
                                                   : (result.scope_fallback_used
                                                          ? "first-term-must-rest-should-v1"
                                                          : "required-groups-v1")));

    std::vector<std::string> related_documents;
    related_documents.reserve(execution.ranked_paths.size());
    Json ranked_paths = Json::array();
    for (const RankedBriefPath& path : execution.ranked_paths) {
        related_documents.push_back(path.path);
        ranked_paths.push_back({{"path", path.path},
                                {"unique_path_rank", path.unique_path_rank},
                                {"first_evidence_rank", path.first_evidence_rank}});
    }
    Json output = make_result_document("shin.context-brief.v1", "brief", true);
    output["projection"] = "full";
    output["profile"] = options.profile.id;
    output["index_mode"] = index_mode;
    output["index_freshness"] = index_freshness_json(freshness);
    output["snapshot_id"] = freshness.snapshot_id.empty() ? Json(nullptr) : Json(freshness.snapshot_id);
    output["query"] = request.query;
    output["retrieval_mode"] = result.identifier_scope_used ? "identifier-scoped" : retrieval_mode;
    output["query_diagnostics"] = {
        {"fallback_used", result.fallback_used},
        {"scope_fallback_used", result.scope_fallback_used},
        {"document_owner_scope_used", result.document_owner_scope_used},
        {"directory_owner_scope_used", result.directory_owner_scope_used},
        {"planner_attempted", result.planner_attempted},
        {"query_planner_used", result.query_planner_used},
        {"planner_rejection_reason", result.planner_rejection_reason.empty()
                                         ? Json(nullptr)
                                         : Json(result.planner_rejection_reason)},
        {"scope_group_index", result.scope_group_index},
        {"matching_policy", result.identifier_scope_used ? "confirmed-identifier-scope-v1" : matching_policy},
        {"candidate_rows", result.candidate_rows},
        {"candidate_index_used", result.candidate_index_used},
        {"required_group_matches", result.required_group_matches},
        {"total_term_groups", result.total_term_groups},
        {"planner_candidate_groups", result.planner_candidate_groups},
        {"planner_indexed_groups", result.planner_indexed_groups},
        {"planner_covered_groups", result.planner_covered_groups},
        {"planner_selected_documents", result.planner_selected_documents},
        {"matching_mode", kMatchingMode},
        {"topic_match_policy", "booster-only"}
    };
    if (result.identifier_probe_rows > 0) {
        output["query_diagnostics"]["identifier_scope_used"] = result.identifier_scope_used;
        output["query_diagnostics"]["identifier_scope_key"] = result.identifier_scope_key;
        output["query_diagnostics"]["identifier_probe_rows"] = result.identifier_probe_rows;
    }
    output["query_coverage"] = query_coverage_json(execution.coverage);
    output["limit"] = request.limit;
    output["per_doc_limit"] = request.per_doc_limit;
    output["evidence_budget"] = {
        {"policy", "global-record-document-diversity-with-type-seeding-v3"},
        {"max_records", request.limit},
        {"eligible_records", selection.eligible_records},
        {"returned_records", returned_sections + returned_facts},
        {"budgeted_out_records", selection.budgeted_out_records}
    };
    output["source_count"] = source_count;
    output["matched_sections"] = result.matched_sections;
    output["returned_sections"] = returned_sections;
    output["matched_facts"] = result.matched_facts;
    output["deduplicated_facts"] = result.deduplicated_facts;
    output["returned_facts"] = returned_facts;
    output["matched_by_type"] = {
        {"decisions", result.matched_decisions},
        {"contracts", result.matched_contracts},
        {"runbooks", result.matched_runbooks},
        {"facts", result.matched_other_facts}
    };
    output["returned_by_type"] = {
        {"decisions", selection.decisions.size()},
        {"contracts", selection.contracts.size()},
        {"runbooks", selection.runbooks.size()},
        {"facts", selection.facts.size()}
    };
    output["ranking_policy"] = "global-evidence-order-unique-path-v1";
    output["ranked_paths"] = std::move(ranked_paths);
    output["related_documents"] = related_documents;
    output["sections"] = Json::array();
    output["decisions"] = Json::array();
    output["contracts"] = Json::array();
    output["runbooks"] = Json::array();
    output["facts"] = Json::array();
    for (const ContextHit& hit : selection.sections) {
        output["sections"].push_back(section_hit_json(hit));
    }
    auto append_facts = [&](const std::string& key, const std::vector<FactHit>& hits) {
        for (const FactHit& hit : hits) {
            output[key].push_back(fact_hit_json(hit));
        }
    };
    append_facts("decisions", selection.decisions);
    append_facts("contracts", selection.contracts);
    append_facts("runbooks", selection.runbooks);
    append_facts("facts", selection.facts);
    return output;
}

static int emit_brief_json(const RuntimeOptions& options,
                           const QueryRequest& request,
                           const std::string& index_mode,
                           const IndexFreshness& freshness,
                           int source_count,
                           const BriefResult& result,
                           BriefProjection projection) {
    emit_retrieval_document(std::cout,
                            make_brief_json(options, request, index_mode, freshness, source_count,
                                            make_brief_execution(request, result), projection));
    return 0;
}

static BriefResult collect_brief_from_candidates(const RuntimeOptions& options,
                                     const QueryRequest& request,
                                     const CandidateRows& candidates,
                                     int required_group_matches,
                                     bool fallback_used,
                                     std::string_view identifier_key = {}) {
    BriefResult result;
    result.fallback_used = fallback_used;
    result.required_group_matches = required_group_matches;
    result.total_term_groups = strict_required_group_matches(request);
    result.candidate_rows = identifier_key.empty() ? candidates.discovered_row_count : 0;
    result.candidate_index_used = candidates.from_terms_index;
    for_each_generated_section_matching_terms(options, candidates, request.prepared_term_groups,
                                              required_group_matches,
                                              [&](const SectionRow& section) {
        if (!identifier_key.empty()) return;
        MatchScore match = score_section_match(section, request.prepared_term_groups, required_group_matches);
        push_brief_section_match(result, section, match, request.per_doc_limit);
    });
    for_each_generated_fact_matching_terms(options, candidates, request.prepared_term_groups,
                                           required_group_matches,
                                           [&](const FactRow& fact) {
        MatchScore match = score_fact_match(fact, request.prepared_term_groups, required_group_matches);
        if (!identifier_key.empty()) {
            bool confirmed = false;
            if (!starts_with(fact.source, "source-fact:")) return;
            for (const auto& identifier : fact.identifier_terms) {
                const auto tokens = parse_terms(identifier);
                if (tokens.size() == 1 && prepare_match_term(tokens.front()).normalized == identifier_key) {
                    confirmed = true;
                    break;
                }
            }
            if (!confirmed) return;
            ++result.candidate_rows;
            result.identifier_scope_used = true;
        }
        push_brief_fact_match(result, fact, match, request.limit, request.per_doc_limit);
    });
    return result;
}

[[maybe_unused]] static BriefResult collect_brief_from_index(const RuntimeOptions& options,
                                     const QueryRequest& request,
                                     int required_group_matches,
                                     bool fallback_used) {
    const auto candidates = load_generated_query_candidates(options, request.prepared_term_groups,
                                                             required_group_matches);
    return collect_brief_from_candidates(options, request, candidates, required_group_matches, fallback_used);
}

static BriefResult collect_confirmed_identifier(const RuntimeOptions& options, const QueryRequest& request) {
    constexpr std::size_t kMaxIdentifierProbeRows = 256;
    BriefResult result;
    if (request.terms.empty() || request.terms.size() > 8 ||
        request.term_groups != expand_query_term_groups(Profile{}, request.terms)) return result;
    std::string key;
    std::size_t segments = 0;
    for (const auto& term : request.terms) {
        if (term.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) return result;
        const auto normalized = prepare_match_term(term).normalized;
        segments += static_cast<std::size_t>(std::count(normalized.begin(), normalized.end(), ' ')) + 1;
        if (!key.empty()) key += ' ';
        key += normalized;
    }
    if (segments < 2 || segments > 8 || key.size() > 128) return result;
    auto compound = request;
    const auto variants = expand_query_term_groups(Profile{}, {key}).front();
    compound.term_groups.assign(request.term_groups.size(), variants);
    compound.prepared_term_groups = prepare_term_groups(compound.term_groups);
    for (auto& group : compound.prepared_term_groups) {
        for (auto& term : group) {
            term.phrase_keys.clear();
            term.allow_prefix = false;
        }
    }
    const auto candidates = load_generated_query_candidates(options, compound.prepared_term_groups,
                                                             strict_required_group_matches(compound));
    result.identifier_probe_rows = candidates.discovered_row_count;
    // No broad row scan may be introduced solely to infer query intent.
    if (!candidates.from_terms_index || candidates.discovered_row_count > kMaxIdentifierProbeRows) return result;
    result = collect_brief_from_candidates(options, compound, candidates,
                                           strict_required_group_matches(compound), false, key);
    result.identifier_probe_rows = candidates.discovered_row_count;
    if (result.identifier_scope_used) result.identifier_scope_key = key;
    return result;
}

static int brief_returned_count(const BriefResult& result, const QueryRequest& request) {
    const BriefSelection selection =
        select_brief_evidence(result, request.limit, request.per_doc_limit,
                              request.allowed_evidence_types);
    return static_cast<int>(selection.sections.size() + selection.decisions.size() +
                            selection.contracts.size() + selection.runbooks.size() +
                            selection.facts.size());
}

static std::size_t brief_distinct_path_count(const BriefResult& result) {
    std::set<std::string> paths;
    for (const auto& [path, hits] : result.section_hits_by_doc) {
        if (!hits.empty()) {
            paths.insert(path);
        }
    }
    for (const std::vector<FactHit>* hits :
         {&result.decisions, &result.contracts, &result.runbooks, &result.facts}) {
        for (const FactHit& hit : *hits) {
            paths.insert(hit.fact.path);
        }
    }
    return paths.size();
}

static void retain_rejected_planner_diagnostics(BriefResult& target,
                                                const BriefResult& planned,
                                                std::string rejection_reason) {
    target.planner_attempted = true;
    target.planner_rejection_reason = std::move(rejection_reason);
    target.candidate_rows = std::max(target.candidate_rows, planned.candidate_rows);
    target.candidate_index_used = target.candidate_index_used || planned.candidate_index_used;
    target.planner_candidate_groups = planned.planner_candidate_groups;
    target.planner_indexed_groups = planned.planner_indexed_groups;
    target.planner_covered_groups = planned.planner_covered_groups;
    target.planner_selected_documents = planned.planner_selected_documents;
    target.planner_candidate_rows_by_group = planned.planner_candidate_rows_by_group;
    target.planner_dispositions_by_group = planned.planner_dispositions_by_group;
}

static bool brief_has_only_indexed_body_matches(const BriefResult& result,
                                                const QueryRequest& request,
                                                bool include_document_identity = true) {
    if (result.matched_facts != 0 || result.section_hits_by_doc.empty()) return false;
    for (const auto& [path, hits] : result.section_hits_by_doc) {
        (void)path;
        for (const auto& hit : hits) {
            if (std::find(hit.matched_fields.begin(), hit.matched_fields.end(), "indexed_text") ==
                hit.matched_fields.end()) return false;
            // Score diagnostics contain bonuses and only the winning field per group.
            if (section_has_non_body_query_match(hit.section, request.prepared_term_groups,
                                                include_document_identity)) return false;
        }
    }
    return true;
}

#ifdef SHIN_EXPERIMENT_EVAL_BATCH
#define SHIN_PROBE_ROUTE(route, ...) experiment::probe_route(experiment::ProbeRoute::route, [&] { return __VA_ARGS__; })
#else
#define SHIN_PROBE_ROUTE(route, ...) (__VA_ARGS__)
#endif

static CandidateRows select_prepared_brief_candidates(const RuntimeOptions& options,
                                                       const QueryRequest& request,
                                                       const std::vector<CandidateRows>& groups,
                                                       int required) {
    auto candidates = select_generated_group_candidates(groups, required);
    // A broad individual group may have been discarded before the final
    // threshold was applied. Retry the existing aggregate selector: it retains
    // the same completeness, matching threshold and final candidate-row cap.
    // Re-reading cannot repair incomplete coverage or shrink a final union
    // already computed from complete group sets. Only retry discarded groups.
    bool recover = std::all_of(groups.begin(), groups.end(), [](const auto& group) {
                       return group.term_index_coverage_complete;
                   }) &&
                   std::any_of(groups.begin(), groups.end(), [](const auto& group) {
                       return !group.from_terms_index;
                   });
#ifdef SHIN_EXPERIMENT_AGGREGATE_RECOVERY
    recover = recover && experiment::aggregate_recovery_enabled();
#endif
    if (recover) {
#ifdef SHIN_EXPERIMENT_AGGREGATE_RECOVERY
        const auto before_rows = candidates.discovered_row_count;
        const bool before_indexed = candidates.from_terms_index;
#endif
        candidates = load_generated_query_candidates(options, request.prepared_term_groups, required);
#ifdef SHIN_EXPERIMENT_AGGREGATE_RECOVERY
        experiment::record_aggregate_retry(request, required, before_rows, before_indexed, candidates);
#endif
    }
    return candidates;
}

// Both builds use this one adaptive policy. Only the experimental executable
// suspends at ordinary collection stages; production expands to return/direct calls.
#ifdef SHIN_EXPERIMENT_EVAL_BATCH
#define SHIN_STAGE_RETURN co_return
#define SHIN_STAGE_AWAIT co_await
#define SHIN_STAGE_COLLECT experiment::StageAwaiter
experiment::BriefTask experiment::collect_brief_task(const RuntimeOptions& options,
                                                     const QueryRequest& request,
                                                     const Json* manifest) {
#else
#define SHIN_STAGE_RETURN return
#define SHIN_STAGE_AWAIT
#define SHIN_STAGE_COLLECT collect_brief_from_candidates
BriefResult collect_brief_from_index_with_fallback(const RuntimeOptions& options,
                                                    const QueryRequest& request,
                                                    const Json* manifest) {
#endif
    BriefResult owner_scoped =
        SHIN_PROBE_ROUTE(owner, collect_brief_from_index_document_owner_scope(options, request, manifest));
    if (brief_returned_count(owner_scoped, request) > 0) {
        SHIN_STAGE_RETURN owner_scoped;
    }
    const bool explicit_directory_scope =
        !request.terms.empty() &&
        (request.terms.front().find('/') != std::string::npos ||
         request.terms.front().find('\\') != std::string::npos) &&
        !fs::path(request.terms.front()).has_extension();
    const bool scoped_policy = options.profile.compound_query_first_term_scope &&
                               !request.prepared_term_groups.empty() &&
                               (explicit_directory_scope || should_expand_query_coverage(request, 0));
    if (scoped_policy) {
        BriefResult scoped = SHIN_PROBE_ROUTE(directory, collect_brief_from_index_first_term_scope(options, request, manifest));
        if (scoped.directory_owner_scope_used || brief_returned_count(scoped, request) > 0) {
            SHIN_STAGE_RETURN scoped;
        }
    }

    BriefResult identifier = SHIN_PROBE_ROUTE(identifier, collect_confirmed_identifier(options, request));
    if (identifier.identifier_scope_used && brief_returned_count(identifier, request) > 0) SHIN_STAGE_RETURN identifier;
    const auto identifier_probe_rows = identifier.identifier_probe_rows;
    std::optional<std::vector<CandidateRows>> prepared_groups;
    if (strict_required_group_matches(request) >= kCompoundQueryCoverageThreshold) {
        prepared_groups = load_generated_query_group_candidates(options, request.prepared_term_groups);
    }
    const auto collect = [&](int required, bool fallback) {
#ifdef SHIN_EXPERIMENT_EVAL_BATCH
        auto candidates = prepared_groups ? select_prepared_brief_candidates(options, request, *prepared_groups, required) :
            load_generated_query_candidates(options, request.prepared_term_groups, required);
        return experiment::StageAwaiter(options, request, std::move(candidates), required, fallback);
#else
        if (!prepared_groups) return collect_brief_from_index(options, request, required, fallback);
        const auto candidates = select_prepared_brief_candidates(options, request, *prepared_groups, required);
        return collect_brief_from_candidates(options, request, candidates, required, fallback);
#endif
    };
    BriefResult result = SHIN_STAGE_AWAIT collect(strict_required_group_matches(request), false);
    result.identifier_probe_rows = identifier_probe_rows;
    const bool has_results = brief_returned_count(result, request) > 0;
    const int groups = strict_required_group_matches(request);
    if (has_results && groups >= 4 && groups < kCompoundQueryCoverageThreshold &&
        brief_distinct_path_count(result) < 2 && brief_has_only_indexed_body_matches(result, request)) {
        // Supplement an existing full match with majority coverage, not two-word noise.
        const int required = std::max(relaxed_required_group_matches(request), (groups + 1) / 2);
        const auto candidates = load_generated_query_candidates(
            options, request.prepared_term_groups, required);
        result.candidate_rows = std::max(result.candidate_rows, candidates.discovered_row_count);
        // A prose-only match in one document must not suppress other primary evidence.
        // With no facts, the section map retains all matching paths before global selection.
        // This supplemental pass is indexed and bounded, never a generated-row scan.
        if (candidates.from_terms_index && candidates.term_index_coverage_complete &&
            candidates.discovered_row_count <= kMaxIndexedCandidateRows) {
            auto expanded = SHIN_STAGE_AWAIT SHIN_STAGE_COLLECT(options, request, candidates, required, true);
            expanded.identifier_probe_rows = identifier_probe_rows;
            SHIN_STAGE_RETURN expanded;
        }
    }
    if ((has_results && !should_expand_query_coverage(request, brief_distinct_path_count(result))) ||
        !can_relax_query(request)) {
        if (has_results && groups >= 4 &&
            brief_distinct_path_count(result) >= 2 &&
            brief_returned_count(result, request) < request.limit) {
            // Keep terse and focused results stable; supplement ambiguous multi-part requests only.
            BriefResult planned = SHIN_PROBE_ROUTE(planner, collect_brief_from_index_document_coverage_plan(
                options, request, true, groups, true,
                prepared_groups ? &*prepared_groups : nullptr, manifest,
                // Shared filename words do not make a multi-owner prose query owner-scoped.
                brief_has_only_indexed_body_matches(result, request, false)));
            planned.identifier_probe_rows = identifier_probe_rows;
            auto execution = make_brief_execution(request, std::move(planned));
            if (!execution.selection.ordered.empty() &&
                query_coverage_covered_groups(execution.coverage) >= groups) {
                SHIN_STAGE_RETURN std::move(execution.retrieval);
            }
            retain_rejected_planner_diagnostics(result, execution.retrieval,
                !execution.retrieval.planner_rejection_reason.empty() ?
                    execution.retrieval.planner_rejection_reason : "insufficient-document-coverage");
        }
        SHIN_STAGE_RETURN result;
    }
    BriefResult relaxed = SHIN_STAGE_AWAIT collect(relaxed_required_group_matches(request), true);
    relaxed.identifier_probe_rows = identifier_probe_rows;
    const bool relaxed_has_results = brief_returned_count(relaxed, request) > 0;
    const bool planner_candidate = strict_required_group_matches(request) >= kCompoundQueryCoverageThreshold &&
        (!relaxed_has_results || brief_distinct_path_count(relaxed) < 2 ||
         brief_returned_count(relaxed, request) < request.limit);
    if (planner_candidate) {
        const bool preserve_row_ranking = relaxed_has_results && brief_distinct_path_count(relaxed) >= 2;
        BriefResult planned = SHIN_PROBE_ROUTE(planner, collect_brief_from_index_document_coverage_plan(
            options, request, preserve_row_ranking, relaxed_required_group_matches(request), false,
            prepared_groups ? &*prepared_groups : nullptr, manifest));
        planned.identifier_probe_rows = identifier_probe_rows;
        const int minimum_coverage = std::max(2, relaxed_required_group_matches(request));
        auto planned_execution = make_brief_execution(request, std::move(planned));
        const bool planned_has_results = !planned_execution.selection.ordered.empty();
        if (planned_has_results && query_coverage_covered_groups(planned_execution.coverage) >= minimum_coverage) {
            SHIN_STAGE_RETURN std::move(planned_execution.retrieval);
        }
        BriefResult& fallback = relaxed_has_results ? relaxed : (has_results ? result : relaxed);
        retain_rejected_planner_diagnostics(
            fallback, planned_execution.retrieval,
            !planned_execution.retrieval.planner_rejection_reason.empty() ? planned_execution.retrieval.planner_rejection_reason :
            (planned_has_results ? "insufficient-document-coverage" : "no-returned-evidence"));
    }
    if (relaxed_has_results) {
        SHIN_STAGE_RETURN relaxed;
    }
    SHIN_STAGE_RETURN has_results ? result : relaxed;
}

#undef SHIN_PROBE_ROUTE
#undef SHIN_STAGE_RETURN
#undef SHIN_STAGE_AWAIT
#undef SHIN_STAGE_COLLECT

#ifdef SHIN_EXPERIMENT_EVAL_BATCH
BriefResult experiment::direct_brief_stage(const RuntimeOptions& options, const QueryRequest& request,
                                           const CandidateRows& candidates, int required, bool fallback) {
    return collect_brief_from_candidates(options, request, candidates, required, fallback);
}

BriefResult collect_brief_from_index_with_fallback(const RuntimeOptions& options,
                                                   const QueryRequest& request, const Json* manifest) {
    const std::vector<QueryRequest> requests{request};
    auto results = experiment::run_brief_tasks(options, requests, manifest, false);
    return std::move(results.front());
}
#endif

static const std::string& brief_evidence_path(const BriefSelection& selection,
                                              const BriefEvidenceRef& reference) {
    switch (reference.type) {
        case BriefEvidenceType::contract:
            return selection.contracts.at(reference.index).fact.path;
        case BriefEvidenceType::decision:
            return selection.decisions.at(reference.index).fact.path;
        case BriefEvidenceType::runbook:
            return selection.runbooks.at(reference.index).fact.path;
        case BriefEvidenceType::fact:
            return selection.facts.at(reference.index).fact.path;
        case BriefEvidenceType::section:
            return selection.sections.at(reference.index).section.path;
    }
    throw std::logic_error("Unknown brief evidence type.");
}

static void collect_record_types_from_hits(std::vector<std::string>& record_types, const std::vector<FactHit>& hits) {
    for (const FactHit& hit : hits) {
        append_unique_string(record_types, to_lower(hit.fact.record_type));
        append_unique_string(record_types, to_lower(brief_fact_group(hit.fact)));
    }
}

static std::vector<std::string> returned_record_types_for_brief(const BriefSelection& selection) {
    std::vector<std::string> record_types;
    collect_record_types_from_hits(record_types, selection.decisions);
    collect_record_types_from_hits(record_types, selection.contracts);
    collect_record_types_from_hits(record_types, selection.runbooks);
    collect_record_types_from_hits(record_types, selection.facts);
    std::sort(record_types.begin(), record_types.end());
    return record_types;
}

static BriefExecution make_brief_execution(const QueryRequest& request,
                                            BriefResult retrieval) {
    BriefExecution execution;
    execution.retrieval = std::move(retrieval);
    execution.selection =
        select_brief_evidence(execution.retrieval, request.limit, request.per_doc_limit,
                              request.allowed_evidence_types, true);
    execution.coverage = make_query_coverage(request);
    for (const ContextHit& hit : execution.selection.sections) {
        add_query_coverage_evidence(execution.coverage, hit.section.path,
                                    hit.matched_group_indices);
    }
    for (const std::vector<FactHit>* hits :
         {&execution.selection.decisions, &execution.selection.contracts,
          &execution.selection.runbooks, &execution.selection.facts}) {
        for (const FactHit& hit : *hits) {
            add_query_coverage_evidence(execution.coverage, hit.fact.path,
                                        hit.matched_group_indices);
        }
    }
    std::set<std::string> seen_paths;
    for (std::size_t evidence_index = 0;
         evidence_index < execution.selection.ordered.size(); ++evidence_index) {
        const std::string& path =
            brief_evidence_path(execution.selection, execution.selection.ordered[evidence_index]);
        if (path.empty() || !seen_paths.insert(path).second) {
            continue;
        }
        execution.ranked_paths.push_back({path,
                                          static_cast<int>(execution.ranked_paths.size() + 1),
                                          static_cast<int>(evidence_index + 1)});
    }
    if (execution.retrieval.query_planner_used) {
        finalize_planner_unit_coverage(execution);
    }
    merge_planner_query_coverage(execution.coverage, execution.retrieval);
    execution.returned_record_types = returned_record_types_for_brief(execution.selection);
    return execution;
}

#ifdef SHIN_EXPERIMENT_EVAL_BATCH
std::vector<BriefExecution> experiment::batch_brief_executions(
        const RetrievalSession& session, const std::vector<QueryRequest>& requests) {
    for (const auto& request : requests)
        if (request.require_current) require_current_index_or_throw(session.freshness());
    auto results = run_brief_tasks(session.options(), requests, &session.manifest(), eval_batch_enabled());
    std::vector<BriefExecution> executions;
    executions.reserve(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i)
        executions.push_back(make_brief_execution(requests[i], std::move(results.at(i))));
    return executions;
}

std::vector<Json> experiment::batch_brief_documents(
        const RetrievalSession& session, const std::vector<QueryRequest>& requests) {
    auto executions = batch_brief_executions(session, requests);
    std::vector<Json> documents;
    documents.reserve(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i)
        documents.push_back(make_brief_json(session.options(), requests[i], "generated-index",
            session.freshness(), session.freshness().source_count, executions[i], BriefProjection::full));
    return documents;
}
#endif

static int run_brief_from_index(const RuntimeOptions& options,
                                const QueryRequest& request,
                                BriefProjection projection) {
    RetrievalSession session(options, request.require_current);
    emit_retrieval_document(std::cout, session.brief(request, projection));
    return 0;
}

BriefExecution RetrievalSession::brief_execution(const QueryRequest& request) const {
    if (request.require_current) require_current_index_or_throw(freshness_);
    return make_brief_execution(request,
                                collect_brief_from_index_with_fallback(options_, request, &manifest()));
}

Json RetrievalSession::brief(const QueryRequest& request, BriefProjection projection) const {
    return make_brief_json(options_, request, "generated-index", freshness_, freshness_.source_count,
                           brief_execution(request), projection);
}

static BriefResult collect_brief_from_sources(const RuntimeOptions&,
                                               const QueryRequest& request,
                                               const SourceScanCorpus& corpus,
                                               int required_group_matches,
                                               bool fallback_used) {
    BriefResult result;
    result.fallback_used = fallback_used;
    result.required_group_matches = required_group_matches;
    result.total_term_groups = strict_required_group_matches(request);
    for (const SectionRow& section : corpus.sections) {
        MatchScore match = score_section_match(
            section, request.prepared_term_groups, required_group_matches);
        push_brief_section_match(result, section, match, request.per_doc_limit);
    }
    for (const FactRow& fact : corpus.facts) {
        MatchScore match = score_fact_match(
            fact, request.prepared_term_groups, required_group_matches);
        push_brief_fact_match(result, fact, match, request.limit, request.per_doc_limit);
    }
    return result;
}

static BriefResult collect_brief_from_sources_with_fallback(
    const RuntimeOptions& options,
    const QueryRequest& request,
    const SourceScanCorpus& corpus) {
    BriefResult result = collect_brief_from_sources(
        options, request, corpus, strict_required_group_matches(request), false);
    const bool has_results = brief_returned_count(result, request) > 0;
    if ((has_results && !should_expand_query_coverage(request, brief_distinct_path_count(result))) ||
        !can_relax_query(request)) {
        return result;
    }
    BriefResult relaxed = collect_brief_from_sources(
        options, request, corpus, relaxed_required_group_matches(request), true);
    return brief_returned_count(relaxed, request) > 0 ? relaxed : result;
}

static int run_brief_from_sources(const RuntimeOptions& options,
                                  const QueryRequest& request,
                                  BriefProjection projection) {
    const SourceScanCorpus corpus = build_source_scan_corpus(options.profile, options.root);
    const IndexFreshness freshness = source_scan_freshness(corpus);
    if (request.require_current) {
        require_complete_source_scan_or_throw(freshness);
    }
    BriefResult result = collect_brief_from_sources_with_fallback(options, request, corpus);
    return emit_brief_json(options, request, "source-scan", freshness,
                           static_cast<int>(corpus.documents.size()), result, projection);
}

int run_brief(const RuntimeOptions& options, const std::vector<std::string>& args) {
    const BriefCommandRequest command = parse_brief_command_request(args);
    QueryRequest request = make_command_query_request(options.profile, command.query_args, 6, 2);
    if (should_use_generated_index(options)) {
        return run_brief_from_index(options, request, command.projection);
    }
    return run_brief_from_sources(options, request, command.projection);
}

} // namespace shin::context
