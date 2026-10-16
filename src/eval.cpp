#include "context_internal.h"
#include "profile_quality.h"
#include <span>
#ifdef SHIN_EXPERIMENT_EVAL_BATCH
#include "eval_batch_experiment.h"
#endif

namespace shin::context {

namespace {

constexpr std::size_t kEvalFailureLimit = 32;
constexpr std::size_t kEvalFailureReturnedPathLimit = 12;
constexpr std::size_t kEvalFailurePayloadBytes = 24U * 1024U;

struct EvalCeilingViolation {
    std::string metric;
    std::size_t actual = 0;
    int allowed = 0;
    std::string comparator;
    std::string stage;
};

struct EvalCaseResult {
    EvalCase item;
    int variant_index = -1;
    std::vector<RankedBriefPath> ranked_paths;
    std::vector<std::string> returned_paths;
    std::vector<std::string> returned_record_types;
    std::vector<std::string> missing_paths;
    std::vector<std::string> missing_top_paths;
    std::vector<std::string> unexpected_paths;
    std::vector<std::string> missing_record_types;
    std::vector<std::string> ceiling_violations;
    std::vector<EvalCeilingViolation> ceiling_violation_details;
    Json path_ranks = Json::object();
    Json rank_violations = Json::array();
    QueryCoverage query_coverage;
    int matched_sections = 0;
    int matched_facts = 0;
    int returned_sections = 0;
    int returned_facts = 0;
    bool fallback_used = false;
    bool scope_fallback_used = false;
    bool document_owner_scope_used = false;
    bool directory_owner_scope_used = false;
    bool identifier_scope_used = false;
    std::string identifier_scope_key;
    std::size_t identifier_probe_rows = 0;
    bool planner_attempted = false;
    bool query_planner_used = false;
    std::string planner_rejection_reason;
    int scope_group_index = -1;
    std::size_t candidate_rows = 0;
    bool candidate_index_used = false;
    int required_group_matches = 0;
    int total_term_groups = 0;
    int planner_candidate_groups = 0;
    int planner_indexed_groups = 0;
    int planner_covered_groups = 0;
    int planner_selected_documents = 0;
    int relevant_path_count = 0;
    int relevant_paths_found = 0;
    int recalled_at_n = 0;
    int first_relevant_rank = 0;
    double reciprocal_rank = 0.0;
    double recall_at_n = 0.0;
    std::string resource_policy = "strict";
    bool query_passed = false;
    bool quality_passed = false;
    bool resources_passed = false;
    bool passed = false;
    std::vector<EvalCaseResult> variant_results;
};

struct EvalOptions {
    bool strict = false;
    bool require_current = false;
    bool failed_only = false;
    int override_limit = 0;
    int override_per_doc_limit = 0;
};

struct EvalSuiteExecution {
    SuiteEvaluation evaluation;
    std::vector<EvalCaseResult> results;
};

static QueryRequest make_eval_query_request(const RuntimeOptions& options, const EvalCase& item,
                                            const std::string& query, int override_limit,
                                            int override_per_doc_limit) {
    QueryRequest request;
    request.query = query;
    request.terms = parse_terms(request.query);
    request.limit = override_limit > 0 ? override_limit : item.limit;
    request.per_doc_limit = override_per_doc_limit > 0 ? override_per_doc_limit : item.per_doc_limit;
    if (request.limit <= 0) {
        request.limit = 8;
    }
    if (request.per_doc_limit <= 0) {
        request.per_doc_limit = 2;
    }
    apply_profile_query_expansion(options.profile, request);

    return request;
}

#ifdef SHIN_EXPERIMENT_EVAL_BATCH
class EvalPrefetchScope;
static thread_local EvalPrefetchScope* active_eval_prefetch = nullptr;
class EvalPrefetchScope {
public:
    EvalPrefetchScope(const RetrievalSession& session, int override_limit, int override_per_doc_limit)
        : session_(session), previous_(active_eval_prefetch), enabled_(experiment::eval_batch_enabled()) {
        if (enabled_) {
            for (const auto& item : session.options().profile.eval_cases) {
                requests_.push_back(make_eval_query_request(session.options(), item, item.query,
                    override_limit, override_per_doc_limit));
                for (const auto& query : item.query_variants)
                    requests_.push_back(make_eval_query_request(session.options(), item, query,
                        override_limit, override_per_doc_limit));
            }
        }
        active_eval_prefetch = this;
    }
    ~EvalPrefetchScope() { active_eval_prefetch = previous_; }
    EvalPrefetchScope(const EvalPrefetchScope&) = delete;
    EvalPrefetchScope& operator=(const EvalPrefetchScope&) = delete;
    BriefExecution take(const RetrievalSession& session, const QueryRequest& request) {
        if (&session != &session_) throw std::logic_error("eval batch crossed session boundary");
        if (!enabled_) return session.brief_execution(request);
        const auto& expected = requests_.at(cursor_);
        if (expected.query != request.query || expected.limit != request.limit ||
            expected.per_doc_limit != request.per_doc_limit || expected.term_groups != request.term_groups)
            throw std::logic_error("eval batch request order mismatch");
        if (queue_cursor_ == queue_.size()) {
            queue_.clear(); queue_cursor_ = 0;
            const auto end = std::min(requests_.size(), cursor_ + experiment::eval_batch_size());
            const auto window = std::span(requests_).subspan(cursor_, end - cursor_);
            const std::vector<QueryRequest> batch(window.begin(), window.end());
            queue_ = experiment::batch_brief_executions(session, batch);
            if (queue_.size() != batch.size()) throw std::logic_error("eval batch result count mismatch");
        }
        ++cursor_;
        return std::move(queue_.at(queue_cursor_++));
    }
    void verify_complete() const {
        if (enabled_ && cursor_ != requests_.size()) throw std::logic_error("eval batch omitted requests");
    }
private:
    const RetrievalSession& session_;
    EvalPrefetchScope* previous_;
    bool enabled_;
    std::vector<QueryRequest> requests_;
    std::vector<BriefExecution> queue_;
    std::size_t cursor_ = 0, queue_cursor_ = 0;
};
#endif

static EvalCaseResult run_eval_query(const RetrievalSession& session,
                                     const EvalCase& item,
                                     const std::string& query,
                                     int variant_index,
                                     int override_limit,
                                     int override_per_doc_limit) {
    const RuntimeOptions& options = session.options();
    QueryRequest request = make_eval_query_request(options, item, query,
                                                   override_limit, override_per_doc_limit);
#ifdef SHIN_EXPERIMENT_EVAL_BATCH
    BriefExecution execution = active_eval_prefetch ? active_eval_prefetch->take(session, request) :
                                                     session.brief_execution(request);
#else
    BriefExecution execution = session.brief_execution(request);
#endif
    const BriefResult& brief = execution.retrieval;
    EvalCaseResult result;
    result.item = item;
    result.item.query = query;
    result.variant_index = variant_index;
    result.item.limit = request.limit;
    result.item.per_doc_limit = request.per_doc_limit;
    result.matched_sections = brief.matched_sections;
    result.matched_facts = brief.matched_facts;
    const BriefSelection& selection = execution.selection;
    result.returned_sections = static_cast<int>(selection.sections.size());
    result.returned_facts = static_cast<int>(selection.decisions.size() + selection.contracts.size() +
                                             selection.runbooks.size() + selection.facts.size());
    result.fallback_used = brief.fallback_used;
    result.scope_fallback_used = brief.scope_fallback_used;
    result.document_owner_scope_used = brief.document_owner_scope_used;
    result.directory_owner_scope_used = brief.directory_owner_scope_used;
    result.identifier_scope_used = brief.identifier_scope_used;
    result.identifier_scope_key = brief.identifier_scope_key;
    result.identifier_probe_rows = brief.identifier_probe_rows;
    result.planner_attempted = brief.planner_attempted;
    result.query_planner_used = brief.query_planner_used;
    result.planner_rejection_reason = brief.planner_rejection_reason;
    result.scope_group_index = brief.scope_group_index;
    result.candidate_rows = brief.candidate_rows;
    result.candidate_index_used = brief.candidate_index_used;
    result.required_group_matches = brief.required_group_matches;
    result.total_term_groups = brief.total_term_groups;
    result.planner_candidate_groups = brief.planner_candidate_groups;
    result.planner_indexed_groups = brief.planner_indexed_groups;
    result.planner_covered_groups = brief.planner_covered_groups;
    result.planner_selected_documents = brief.planner_selected_documents;
    result.query_coverage = execution.coverage;
    result.ranked_paths = execution.ranked_paths;
    for (const RankedBriefPath& path : result.ranked_paths) {
        result.returned_paths.push_back(path.path);
    }
    result.returned_record_types = execution.returned_record_types;
    std::set<std::string> returned(result.returned_paths.begin(), result.returned_paths.end());
    std::map<std::string, int> returned_ranks;
    for (std::size_t index = 0; index < result.returned_paths.size(); ++index) {
        returned_ranks.emplace(result.returned_paths[index], static_cast<int>(index + 1));
    }
    for (const std::string& path : item.must_paths) {
        if (returned.find(path) == returned.end()) {
            result.missing_paths.push_back(path);
        }
    }
    const int must_top_n = item.must_top_n > 0 ? item.must_top_n : request.limit;
    std::set<std::string> returned_top;
    for (int i = 0; i < must_top_n && i < static_cast<int>(result.returned_paths.size()); ++i) {
        returned_top.insert(result.returned_paths[static_cast<std::size_t>(i)]);
    }
    for (const std::string& path : item.must_top_paths) {
        if (returned_top.find(path) == returned_top.end()) {
            result.missing_top_paths.push_back(path);
        }
    }
    for (const std::string& path : item.must_not_paths) {
        if (returned.find(path) != returned.end()) {
            result.unexpected_paths.push_back(path);
        }
    }
    std::set<std::string> ranked_paths;
    ranked_paths.insert(item.must_paths.begin(), item.must_paths.end());
    ranked_paths.insert(item.must_top_paths.begin(), item.must_top_paths.end());
    for (const RankBeforeExpectation& pair : item.must_rank_before) {
        ranked_paths.insert(pair.higher_path);
        ranked_paths.insert(pair.lower_path);
        const auto higher = returned_ranks.find(pair.higher_path);
        const auto lower = returned_ranks.find(pair.lower_path);
        std::string reason;
        if (higher == returned_ranks.end()) {
            reason = "higher-missing";
        } else if (lower != returned_ranks.end() && higher->second >= lower->second) {
            reason = "order-violated";
        }
        if (!reason.empty()) {
            result.rank_violations.push_back({
                {"higher", pair.higher_path},
                {"lower", pair.lower_path},
                {"reason", reason},
                {"higher_rank", higher == returned_ranks.end() ? Json(nullptr) : Json(higher->second)},
                {"lower_rank", lower == returned_ranks.end() ? Json(nullptr) : Json(lower->second)}
            });
        }
    }
    for (const std::string& path : ranked_paths) {
        const auto rank = returned_ranks.find(path);
        result.path_ranks[path] = rank == returned_ranks.end() ? Json(nullptr) : Json(rank->second);
    }
    std::set<std::string> relevant_paths(item.must_paths.begin(), item.must_paths.end());
    relevant_paths.insert(item.must_top_paths.begin(), item.must_top_paths.end());
    for (const RankBeforeExpectation& pair : item.must_rank_before) {
        relevant_paths.insert(pair.higher_path);
    }
    result.relevant_path_count = static_cast<int>(relevant_paths.size());
    int best_relevant_rank = 0;
    for (const std::string& path : relevant_paths) {
        const auto rank = returned_ranks.find(path);
        if (rank == returned_ranks.end()) {
            continue;
        }
        ++result.relevant_paths_found;
        if (best_relevant_rank == 0 || rank->second < best_relevant_rank) {
            best_relevant_rank = rank->second;
        }
        if (rank->second <= must_top_n) {
            ++result.recalled_at_n;
        }
    }
    if (best_relevant_rank > 0) {
        result.first_relevant_rank = best_relevant_rank;
        result.reciprocal_rank = 1.0 / static_cast<double>(best_relevant_rank);
    }
    if (result.relevant_path_count > 0) {
        result.recall_at_n = static_cast<double>(result.recalled_at_n) /
                             static_cast<double>(result.relevant_path_count);
    }
    std::set<std::string> returned_record_types(result.returned_record_types.begin(),
                                                result.returned_record_types.end());
    for (const std::string& type : item.expected_record_types) {
        const std::string normalized = to_lower(type);
        if (returned_record_types.find(normalized) == returned_record_types.end()) {
            result.missing_record_types.push_back(type);
        }
    }
    if (item.max_matched_sections >= 0 && result.matched_sections > item.max_matched_sections) {
        result.ceiling_violations.push_back("matched_sections");
        result.ceiling_violation_details.push_back(
            {"matched_sections", static_cast<std::size_t>(result.matched_sections),
             item.max_matched_sections, "<=", "scored-matches"});
    }
    if (item.max_matched_facts >= 0 && result.matched_facts > item.max_matched_facts) {
        result.ceiling_violations.push_back("matched_facts");
        result.ceiling_violation_details.push_back(
            {"matched_facts", static_cast<std::size_t>(result.matched_facts),
             item.max_matched_facts, "<=", "scored-matches"});
    }
    if (item.max_candidate_rows >= 0 &&
        result.candidate_rows > static_cast<std::size_t>(item.max_candidate_rows)) {
        result.ceiling_violations.push_back("candidate_rows");
        result.ceiling_violation_details.push_back(
            {"candidate_rows", result.candidate_rows, item.max_candidate_rows, "<=",
             "candidate-discovery"});
    }
    result.resources_passed = result.ceiling_violations.empty();
    result.resource_policy = options.profile.eval_resource_policy;
    if (item.min_covered_groups >= 0 &&
        query_coverage_covered_groups(result.query_coverage) < item.min_covered_groups) {
        result.ceiling_violations.push_back("covered_groups");
        result.ceiling_violation_details.push_back(
            {"covered_groups",
             static_cast<std::size_t>(query_coverage_covered_groups(result.query_coverage)),
             item.min_covered_groups, ">=", "coverage"});
    }
    result.quality_passed = result.missing_paths.empty() && result.missing_top_paths.empty() &&
                    result.unexpected_paths.empty() && result.missing_record_types.empty() &&
                    (item.min_covered_groups < 0 ||
                     query_coverage_covered_groups(result.query_coverage) >= item.min_covered_groups) &&
                    result.rank_violations.empty();
    result.passed = result.quality_passed &&
                    (result.resources_passed || result.resource_policy == "advisory");
    result.query_passed = result.passed;
    return result;
}

static EvalCaseResult run_eval_case(const RetrievalSession& session,
                                    const EvalCase& item,
                                    int override_limit,
                                    int override_per_doc_limit) {
    EvalCaseResult result =
        run_eval_query(session, item, item.query, -1, override_limit, override_per_doc_limit);
    result.variant_results.reserve(item.query_variants.size());
    for (std::size_t index = 0; index < item.query_variants.size(); ++index) {
        result.variant_results.push_back(
            run_eval_query(session, item, item.query_variants[index], static_cast<int>(index),
                           override_limit, override_per_doc_limit));
    }
    result.passed = result.query_passed &&
                    std::all_of(result.variant_results.begin(), result.variant_results.end(),
                                [](const EvalCaseResult& variant) { return variant.query_passed; });
    return result;
}

static Json ceiling_violation_details_json(const EvalCaseResult& result) {
    Json details = Json::array();
    for (const EvalCeilingViolation& violation : result.ceiling_violation_details) {
        details.push_back({{"metric", violation.metric},
                           {"actual", violation.actual},
                           {"allowed", violation.allowed},
                           {"comparator", violation.comparator},
                           {"stage", violation.stage}});
    }
    return details;
}

static bool has_identifier_scope_diagnostics(const EvalCaseResult& result) {
    return result.identifier_scope_used || result.identifier_probe_rows > 0;
}

static void append_identifier_scope_diagnostics(Json& output, const EvalCaseResult& result) {
    output["identifier_scope_stage"] = "selection-discovery";
    output["identifier_scope_used"] = result.identifier_scope_used;
    output["identifier_scope_key"] = result.identifier_scope_key;
    output["identifier_probe_rows"] = result.identifier_probe_rows;
}

static std::string eval_retrieval_mode(const EvalCaseResult& result) {
    return result.identifier_scope_used
               ? "identifier-scoped"
               : (result.directory_owner_scope_used
                      ? "directory-scoped"
                      : (result.document_owner_scope_used
                             ? "owner-scoped"
                             : (result.query_planner_used
                                    ? "planned"
                                    : (result.fallback_used ? "relaxed" : "strict"))));
}

static std::string eval_matching_policy(const EvalCaseResult& result) {
    return result.identifier_scope_used
               ? "confirmed-identifier-scope-v1"
               : (result.directory_owner_scope_used
                      ? "directory-owner-scope-v1"
                      : (result.document_owner_scope_used
                             ? "document-owner-scope-v1"
                             : (result.query_planner_used
                                    ? "document-coverage-planner-v1"
                                    : (result.scope_fallback_used
                                           ? "first-term-must-rest-should-v1"
                                           : "required-groups-v1"))));
}

static void emit_eval_case_json(std::ostream& out, const EvalCaseResult& result) {
    const std::string retrieval_mode = eval_retrieval_mode(result);
    const std::string matching_policy = eval_matching_policy(result);
    Json rank_pairs = Json::array();
    Json rank_assertions = Json::array();
    for (const RankBeforeExpectation& pair : result.item.must_rank_before) {
        rank_pairs.push_back({{"higher", pair.higher_path}, {"lower", pair.lower_path}});
        const Json higher_rank = result.path_ranks.contains(pair.higher_path)
                                     ? result.path_ranks.at(pair.higher_path)
                                     : Json(nullptr);
        const Json lower_rank = result.path_ranks.contains(pair.lower_path)
                                    ? result.path_ranks.at(pair.lower_path)
                                    : Json(nullptr);
        const bool passed = !higher_rank.is_null() &&
                            (lower_rank.is_null() || higher_rank.get<int>() < lower_rank.get<int>());
        rank_assertions.push_back({{"higher", pair.higher_path}, {"lower", pair.lower_path},
                                   {"higher_rank", higher_rank}, {"lower_rank", lower_rank},
                                   {"passed", passed}});
    }
    Json ranked_paths = Json::array();
    for (const RankedBriefPath& path : result.ranked_paths) {
        ranked_paths.push_back({{"path", path.path},
                                {"unique_path_rank", path.unique_path_rank},
                                {"first_evidence_rank", path.first_evidence_rank}});
    }
    out << "{"
        << "\"id\":" << json_string(result.item.id)
        << ",\"variant_index\":" << result.variant_index
        << ",\"query\":" << json_string(result.item.query)
        << ",\"limit\":" << result.item.limit
        << ",\"per_doc_limit\":" << result.item.per_doc_limit
        << ",\"query_passed\":" << (result.query_passed ? "true" : "false")
        << ",\"quality_passed\":" << (result.quality_passed ? "true" : "false")
        << ",\"resources_passed\":" << (result.resources_passed ? "true" : "false")
        << ",\"resource_policy\":" << json_string(result.resource_policy)
        << ",\"passed\":" << (result.passed ? "true" : "false")
        << ",\"retrieval_mode\":" << json_string(retrieval_mode)
        << ",\"query_diagnostics\":{"
        << "\"fallback_used\":" << (result.fallback_used ? "true" : "false")
        << ",\"scope_fallback_used\":" << (result.scope_fallback_used ? "true" : "false")
        << ",\"document_owner_scope_used\":"
        << (result.document_owner_scope_used ? "true" : "false")
        << ",\"directory_owner_scope_used\":"
        << (result.directory_owner_scope_used ? "true" : "false");
    if (has_identifier_scope_diagnostics(result)) {
        out << ",\"identifier_scope_stage\":\"selection-discovery\""
            << ",\"identifier_scope_used\":"
            << (result.identifier_scope_used ? "true" : "false")
            << ",\"identifier_scope_key\":" << json_string(result.identifier_scope_key)
            << ",\"identifier_probe_rows\":" << result.identifier_probe_rows;
    }
    out
        << ",\"planner_attempted\":" << (result.planner_attempted ? "true" : "false")
        << ",\"query_planner_used\":" << (result.query_planner_used ? "true" : "false")
        << ",\"planner_rejection_reason\":"
        << (result.planner_rejection_reason.empty()
                ? "null"
                : json_string(result.planner_rejection_reason))
        << ",\"scope_group_index\":" << result.scope_group_index
        << ",\"matching_policy\":" << json_string(matching_policy)
        << ",\"candidate_rows\":" << result.candidate_rows
        << ",\"candidate_index_used\":" << (result.candidate_index_used ? "true" : "false")
        << ",\"required_group_matches\":" << result.required_group_matches
        << ",\"total_term_groups\":" << result.total_term_groups
        << ",\"planner_candidate_groups\":" << result.planner_candidate_groups
        << ",\"planner_indexed_groups\":" << result.planner_indexed_groups
        << ",\"planner_covered_groups\":" << result.planner_covered_groups
        << ",\"planner_selected_documents\":" << result.planner_selected_documents
        << ",\"matching_mode\":" << json_string(kMatchingMode)
        << ",\"topic_match_policy\":\"booster-only\""
        << "}"
        << ",\"query_coverage\":" << query_coverage_json(result.query_coverage).dump()
        << ",\"matched_sections\":" << result.matched_sections
        << ",\"matched_facts\":" << result.matched_facts
        << ",\"returned_sections\":" << result.returned_sections
        << ",\"returned_facts\":" << result.returned_facts
        << ",\"must_paths\":" << json_string_array(result.item.must_paths)
        << ",\"must_top_paths\":" << json_string_array(result.item.must_top_paths)
        << ",\"must_top_n\":" << result.item.must_top_n
        << ",\"must_not_paths\":" << json_string_array(result.item.must_not_paths)
        << ",\"expected_record_types\":" << json_string_array(result.item.expected_record_types)
        << ",\"must_rank_before\":" << rank_pairs.dump()
        << ",\"returned_record_types\":" << json_string_array(result.returned_record_types)
        << ",\"returned_paths\":" << json_string_array(result.returned_paths)
        << ",\"missing_paths\":" << json_string_array(result.missing_paths)
        << ",\"missing_top_paths\":" << json_string_array(result.missing_top_paths)
        << ",\"unexpected_paths\":" << json_string_array(result.unexpected_paths)
        << ",\"missing_record_types\":" << json_string_array(result.missing_record_types)
        << ",\"max_matched_sections\":" << result.item.max_matched_sections
        << ",\"max_matched_facts\":" << result.item.max_matched_facts
        << ",\"max_candidate_rows\":" << result.item.max_candidate_rows
        << ",\"min_covered_groups\":" << result.item.min_covered_groups
        << ",\"ceiling_violations\":" << json_string_array(result.ceiling_violations)
        << ",\"ceiling_violation_details\":"
        << ceiling_violation_details_json(result).dump()
        << ",\"path_ranks\":" << result.path_ranks.dump()
        << ",\"ranked_paths\":" << ranked_paths.dump()
        << ",\"rank_assertions\":" << rank_assertions.dump()
        << ",\"rank_violations\":" << result.rank_violations.dump()
        << ",\"ranking_policy\":\"global-evidence-order-unique-path-v1\""
        << ",\"rank_metrics\":{\"relevant_paths\":" << result.relevant_path_count
        << ",\"relevant_paths_found\":" << result.relevant_paths_found
        << ",\"recalled_at_n\":" << result.recalled_at_n
        << ",\"at_n\":" << result.item.must_top_n
        << ",\"first_relevant_rank\":"
        << (result.first_relevant_rank > 0 ? std::to_string(result.first_relevant_rank) : "null")
        << ",\"reciprocal_rank\":" << result.reciprocal_rank
        << ",\"recall_at_n\":" << result.recall_at_n
        << "}"
        << ",\"query_variants\":[";
    for (std::size_t index = 0; index < result.variant_results.size(); ++index) {
        emit_eval_case_json(out, result.variant_results[index]);
        if (index + 1 < result.variant_results.size()) {
            out << ",";
        }
    }
    out << "]}";
}

static std::vector<std::string> eval_failure_reasons(const EvalCaseResult& result) {
    std::vector<std::string> reasons;
    if (!result.missing_paths.empty()) {
        reasons.push_back("missing-required-path");
    }
    if (!result.missing_top_paths.empty()) {
        reasons.push_back("missing-top-path");
    }
    if (!result.unexpected_paths.empty()) {
        reasons.push_back("forbidden-path-returned");
    }
    if (!result.missing_record_types.empty()) {
        reasons.push_back("missing-record-type");
    }
    if ((!result.resources_passed && result.resource_policy == "strict") ||
        std::find(result.ceiling_violations.begin(), result.ceiling_violations.end(),
                  "covered_groups") != result.ceiling_violations.end()) {
        reasons.push_back("retrieval-ceiling-violated");
    }
    if (!result.rank_violations.empty()) {
        reasons.push_back("rank-order-violated");
    }
    return reasons;
}

static Json bounded_string_array(const std::vector<std::string>& values, std::size_t limit) {
    Json output = Json::array();
    for (std::size_t index = 0; index < values.size() && index < limit; ++index) {
        output.push_back(values[index]);
    }
    return output;
}

static Json eval_failure_json(const RuntimeOptions& options, const EvalCaseResult& result) {
    Json diagnose = Json::array();
    std::set<std::string> expected_paths;
    expected_paths.insert(result.missing_paths.begin(), result.missing_paths.end());
    expected_paths.insert(result.missing_top_paths.begin(), result.missing_top_paths.end());
    for (const std::string& path : expected_paths) {
        diagnose.push_back(
            {{"reason", "inspect-missing-expected-path"},
             {"path", path},
             {"argv",
              {"shin-context", "--profile", options.profile.id, "--profile-dir",
               fs::absolute(options.profile_dir).lexically_normal().generic_string(), "--root",
               fs::absolute(options.root).lexically_normal().generic_string(), "--out-dir",
               fs::absolute(options.out_root).lexically_normal().generic_string(), "diagnose",
               result.item.query, "--expected-path", path, "--limit",
               std::to_string(result.item.limit), "--per-doc",
               std::to_string(result.item.per_doc_limit), "--require-current", "--strict"}},
             {"automatic_execution", false},
             {"mutation", {{"consumer", false}, {"index", false}}}});
    }
    Json failure = {{"diagnostic_id",
                     fnv1a64(result.item.id + ":" + std::to_string(result.variant_index))},
                    {"detail_level", "full"},
                    {"id", result.item.id},
                    {"variant_index", result.variant_index},
                    {"query", result.item.query},
                    {"resource_policy", result.resource_policy},
                    {"quality_passed", result.quality_passed},
                    {"resources_passed", result.resources_passed},
                    {"retrieval_mode", eval_retrieval_mode(result)},
                    {"failure_reasons", eval_failure_reasons(result)},
                    {"missing_paths", result.missing_paths},
                    {"missing_top_paths", result.missing_top_paths},
                    {"unexpected_paths", result.unexpected_paths},
                    {"missing_record_types", result.missing_record_types},
                    {"ceiling_violations", result.ceiling_violations},
                    {"ceiling_violation_details", ceiling_violation_details_json(result)},
                    {"rank_violations", result.rank_violations},
                    {"path_ranks", result.path_ranks},
                    {"returned_paths",
                     bounded_string_array(result.returned_paths, kEvalFailureReturnedPathLimit)},
                    {"returned_paths_total", result.returned_paths.size()},
                    {"diagnose", std::move(diagnose)}};
    if (has_identifier_scope_diagnostics(result)) {
        failure["matching_policy"] = eval_matching_policy(result);
        append_identifier_scope_diagnostics(failure, result);
    }
    return failure;
}

static Json eval_failure_header_json(const EvalCaseResult& result) {
    Json primary_path = nullptr;
    for (const std::vector<std::string>* paths :
         {&result.missing_paths, &result.missing_top_paths, &result.unexpected_paths}) {
        if (!paths->empty()) {
            primary_path = paths->front();
            break;
        }
    }
    Json header = {{"diagnostic_id",
                    fnv1a64(result.item.id + ":" + std::to_string(result.variant_index))},
                   {"detail_level", "header"},
                   {"id", result.item.id},
                   {"variant_index", result.variant_index},
                   {"resource_policy", result.resource_policy},
                   {"quality_passed", result.quality_passed},
                   {"resources_passed", result.resources_passed},
                   {"failure_reasons", eval_failure_reasons(result)},
                   {"primary_path", std::move(primary_path)},
                   {"ceiling_violation_details", ceiling_violation_details_json(result)},
                   {"details_omitted", true}};
    if (has_identifier_scope_diagnostics(result)) {
        header["retrieval_mode"] = eval_retrieval_mode(result);
        header["matching_policy"] = eval_matching_policy(result);
        append_identifier_scope_diagnostics(header, result);
    }
    return header;
}

static void append_eval_failures(const RuntimeOptions& options, Json& failures,
                                 const EvalCaseResult& result, std::size_t& failure_total,
                                 std::size_t& failure_bytes,
                                 std::size_t& detail_records_omitted) {
    auto append = [&](const EvalCaseResult& item) {
        if (item.query_passed) {
            return;
        }
        ++failure_total;
        if (failures.size() < kEvalFailureLimit) {
            Json candidate = eval_failure_json(options, item);
            const std::size_t separator = failures.empty() ? 0 : 1;
            const std::size_t candidate_bytes = candidate.dump().size() + separator;
            const std::size_t remaining =
                kEvalFailurePayloadBytes - std::min(failure_bytes, kEvalFailurePayloadBytes);
            if (candidate_bytes <= remaining) {
                failure_bytes += candidate_bytes;
                failures.push_back(std::move(candidate));
            } else {
                Json header = eval_failure_header_json(item);
                const std::size_t header_bytes = header.dump().size() + separator;
                if (header_bytes <= remaining) {
                    failure_bytes += header_bytes;
                    failures.push_back(std::move(header));
                    ++detail_records_omitted;
                }
            }
        }
    };
    append(result);
    for (const EvalCaseResult& variant : result.variant_results) {
        append(variant);
    }
}

static EvalOptions parse_eval_options(const std::vector<std::string>& args) {
    EvalOptions options;
    bool strict_seen = false;
    bool current_seen = false;
    bool failed_only_seen = false;
    bool limit_seen = false;
    bool per_doc_seen = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--strict") {
            if (strict_seen) {
                throw_usage_error("duplicate-option", "eval accepts --strict only once.");
            }
            options.strict = true;
            strict_seen = true;
        } else if (args[index] == "--require-current") {
            if (current_seen) {
                throw_usage_error("duplicate-option", "eval accepts --require-current only once.");
            }
            options.require_current = true;
            current_seen = true;
        } else if (args[index] == "--failed-only") {
            if (failed_only_seen) {
                throw_usage_error("duplicate-option", "eval accepts --failed-only only once.");
            }
            options.failed_only = true;
            failed_only_seen = true;
        } else if (args[index] == "--limit" || args[index] == "--per-doc") {
            const bool is_limit = args[index] == "--limit";
            bool& seen = is_limit ? limit_seen : per_doc_seen;
            if (seen || index + 1 >= args.size()) {
                throw_usage_error(seen ? "duplicate-option" : "missing-option-value",
                                  is_limit ? "eval accepts --limit only once."
                                           : "eval accepts --per-doc only once.");
            }
            const int value =
                parse_cli_integer(args[++index], is_limit ? "--limit" : "--per-doc", 1);
            if (is_limit) {
                options.override_limit = value;
            } else {
                options.override_per_doc_limit = value;
            }
            seen = true;
        } else {
            throw_usage_error("unsupported-option",
                              "eval only accepts --strict, --require-current, "
                              "--failed-only, --limit, and --per-doc.");
        }
    }
    return options;
}

static bool eval_query_within_runtime_bounds(const Profile& profile,
                                             const std::string& query) {
    if (query.size() > kMaxEvalQueryBytes) {
        return false;
    }
    QueryRequest request;
    request.query = query;
    request.terms = parse_terms(query);
    if (request.terms.empty()) {
        return false;
    }
    apply_profile_query_expansion(profile, request);
    if (request.term_groups.size() > kMaxQueryGroups) {
        return false;
    }
    std::size_t expanded_terms = 0;
    for (const PreparedTermGroup& group : request.prepared_term_groups) {
        if (group.size() > kMaxExpandedQueryTerms - expanded_terms) {
            return false;
        }
        expanded_terms += group.size();
    }
    return true;
}

static bool eval_rank_constraints_within_runtime_bounds(const EvalCase& item) {
    if (item.must_rank_before.size() > kMaxEvalRankPairs || item.must_top_n > item.limit) {
        return false;
    }
    std::set<std::string> forbidden(item.must_not_paths.begin(), item.must_not_paths.end());
    for (const std::string& path : item.must_paths) {
        if (forbidden.find(path) != forbidden.end()) {
            return false;
        }
    }
    for (const std::string& path : item.must_top_paths) {
        if (forbidden.find(path) != forbidden.end()) {
            return false;
        }
    }
    std::map<std::string, std::vector<std::string>> edges;
    std::set<std::string> pairs;
    for (const RankBeforeExpectation& pair : item.must_rank_before) {
        if (pair.higher_path.empty() || pair.lower_path.empty() ||
            pair.higher_path == pair.lower_path ||
            !profile_relative_path_valid(pair.higher_path) ||
            !profile_relative_path_valid(pair.lower_path) ||
            forbidden.find(pair.higher_path) != forbidden.end() ||
            forbidden.find(pair.lower_path) != forbidden.end() ||
            !pairs.insert(pair.higher_path + "\n" + pair.lower_path).second) {
            return false;
        }
        edges[pair.higher_path].push_back(pair.lower_path);
    }
    std::map<std::string, int> states;
    std::function<bool(const std::string&)> visit = [&](const std::string& path) {
        int& state = states[path];
        if (state == 1) {
            return false;
        }
        if (state == 2) {
            return true;
        }
        state = 1;
        for (const std::string& next : edges[path]) {
            if (!visit(next)) {
                return false;
            }
        }
        state = 2;
        return true;
    };
    return std::all_of(edges.begin(), edges.end(), [&](const auto& edge) {
        return visit(edge.first);
    });
}

void validate_eval_suite(const Profile& profile, int override_limit) {
    if (profile.eval_resource_policy != "strict" && profile.eval_resource_policy != "advisory") {
        throw CliError("invalid-eval-configuration", "configuration",
                       "Unknown eval resource policy.", 2, false, "Use strict or advisory.");
    }
    if (!profile.eval_case_config_errors.empty()) {
        throw CliError("invalid-eval-configuration", "configuration",
                       "Profile evalCases contains invalid field types or unknown fields.", 2,
                       false,
                       "Run profile-check --strict and correct every invalid-eval-field issue.");
    }
    std::size_t execution_count = profile.eval_cases.size();
    for (const EvalCase& item : profile.eval_cases) {
        execution_count += item.query_variants.size();
        const bool invalid_bounds = item.limit <= 0 || item.per_doc_limit <= 0 ||
                                    item.must_top_n <= 0 || item.max_matched_sections < -1 ||
                                    item.max_matched_facts < -1 || item.max_candidate_rows < -1 ||
                                    item.min_covered_groups < -1 ||
                                    item.must_rank_before.size() > kMaxEvalRankPairs;
        std::set<std::string> distinct_queries = {trim(to_lower(item.query))};
        bool invalid_query = !eval_query_within_runtime_bounds(profile, item.query);
        for (const std::string& variant : item.query_variants) {
            invalid_query = invalid_query ||
                            !eval_query_within_runtime_bounds(profile, variant) ||
                            !distinct_queries.insert(trim(to_lower(variant))).second;
        }
        const bool invalid_override = override_limit > 0 && override_limit < item.must_top_n;
        if (item.query_variants.size() > kMaxEvalQueryVariants || invalid_bounds || invalid_query ||
            invalid_override || !eval_rank_constraints_within_runtime_bounds(item)) {
            throw CliError("invalid-eval-configuration", "configuration",
                           "Profile evalCases violates a bounded execution constraint.", 2,
                           false, "Run profile-check --strict and correct the eval case.");
        }
    }
    if (profile.eval_cases.size() > kMaxEvalCases || execution_count > kMaxEvalExecutions) {
        throw CliError("eval-suite-too-large", "resource",
                       "Profile evalCases exceeds the bounded local evaluation limits.", 1,
                       false, "Use at most 128 cases and 256 total base plus variant executions.");
    }
}

EvalSuiteExecution execute_eval_suite(const RetrievalSession& session,
                                      int override_limit,
                                      int override_per_doc_limit,
                                      bool retain_details) {
    const Profile& profile = session.options().profile;
    validate_eval_suite(profile, override_limit);
#ifdef SHIN_EXPERIMENT_EVAL_BATCH
    EvalPrefetchScope prefetch(session, override_limit, override_per_doc_limit);
#endif

    EvalSuiteExecution suite;
    if (retain_details) {
        suite.results.reserve(profile.eval_cases.size());
    }
    for (const EvalCase& item : profile.eval_cases) {
        EvalCaseResult result = run_eval_case(session, item, override_limit, override_per_doc_limit);
        suite.evaluation.total += 1;
        suite.evaluation.passed += result.passed ? 1U : 0U;
        suite.evaluation.query_total += 1 + result.variant_results.size();
        suite.evaluation.query_passed += result.query_passed ? 1U : 0U;
        suite.evaluation.query_passed += static_cast<std::size_t>(std::count_if(
            result.variant_results.begin(), result.variant_results.end(),
            [](const EvalCaseResult& variant) { return variant.query_passed; }));
        auto count_checks = [&](const EvalCaseResult& query) {
            suite.evaluation.quality_failed_queries += query.quality_passed ? 0U : 1U;
            suite.evaluation.resource_failed_queries += query.resources_passed ? 0U : 1U;
        };
        count_checks(result);
        for (const auto& variant : result.variant_results) count_checks(variant);
        if (!result.passed && suite.evaluation.failed_case_ids.size() < kMaxEvalCases) {
            suite.evaluation.failed_case_ids.push_back(result.item.id);
        }
        if (retain_details) {
            suite.results.push_back(std::move(result));
        }
    }
    suite.evaluation.failed = suite.evaluation.total - suite.evaluation.passed;
#ifdef SHIN_EXPERIMENT_EVAL_BATCH
    prefetch.verify_complete();
#endif
    return suite;
}

}  // namespace

#ifdef SHIN_EXPERIMENT_EVAL_BATCH
Json experiment::eval_details(const RetrievalSession& session) {
    const auto suite = execute_eval_suite(session, 0, 0, true);
    Json results = Json::array();
    for (const auto& result : suite.results) {
        std::ostringstream out;
        emit_eval_case_json(out, result);
        results.push_back(Json::parse(out.str()));
    }
    return results;
}
#endif

SuiteEvaluation evaluate_profile_cases(RetrievalSession& session) {
    return execute_eval_suite(session, 0, 0, false).evaluation;
}

int run_eval(const RuntimeOptions& options, const std::vector<std::string>& args) {
    const EvalOptions eval_options = parse_eval_options(args);
    validate_eval_suite(options.profile, eval_options.override_limit);
    if (!generated_index_available(options)) {
        throw_index_error("index-unavailable", "eval requires a complete generated index.");
    }
    RetrievalSession session(options, eval_options.strict || eval_options.require_current);
    const IndexFreshness& freshness = session.freshness();

    EvalSuiteExecution suite = execute_eval_suite(session, eval_options.override_limit,
                                                  eval_options.override_per_doc_limit, true);
    const std::vector<EvalCaseResult>& results = suite.results;
    const int passed = static_cast<int>(suite.evaluation.passed);
    const int query_total = static_cast<int>(suite.evaluation.query_total);
    const int query_passed = static_cast<int>(suite.evaluation.query_passed);
    int ranked_query_count = 0;
    int expected_rank_paths = 0;
    int found_rank_paths = 0;
    double reciprocal_rank_sum = 0.0;
    struct RecallAggregate {
        int queries = 0;
        int expected = 0;
        int recalled = 0;
    };
    std::map<int, RecallAggregate> recall_by_n;
    auto collect_rank_metrics = [&](const EvalCaseResult& query_result) {
        if (query_result.relevant_path_count == 0) {
            return;
        }
        ++ranked_query_count;
        expected_rank_paths += query_result.relevant_path_count;
        found_rank_paths += query_result.relevant_paths_found;
        reciprocal_rank_sum += query_result.reciprocal_rank;
        RecallAggregate& aggregate = recall_by_n[query_result.item.must_top_n];
        ++aggregate.queries;
        aggregate.expected += query_result.relevant_path_count;
        aggregate.recalled += query_result.recalled_at_n;
    };
    for (const EvalCaseResult& result : results) {
        collect_rank_metrics(result);
        for (const EvalCaseResult& variant : result.variant_results) {
            collect_rank_metrics(variant);
        }
    }
    const int total = static_cast<int>(results.size());
    const int failed = total - passed;
    const bool freshness_ok = !(eval_options.strict || eval_options.require_current) ||
                              (freshness.current && freshness.fully_verified);
    const bool ok = total > 0 && failed == 0 && freshness_ok;
    const bool quality_ok = total > 0 && suite.evaluation.quality_failed_queries == 0;
    const Json resource_check = eval_resource_check_json(options.profile, suite.evaluation);
    Json warnings = Json::array();
    std::size_t warning_bytes = 0;
    auto collect_warning = [&](const EvalCaseResult& row) {
        if (row.resource_policy != "advisory" || row.resources_passed) return;
        Json details = ceiling_violation_details_json(row);
        details.erase(std::remove_if(details.begin(), details.end(), [](const Json& detail) {
            return detail.at("metric") == "covered_groups";
        }), details.end());
        Json warning = {{"id", row.item.id}, {"variant_index", row.variant_index},
                        {"code", "eval-resource-ceiling-exceeded"}, {"violations", details}};
        const auto bytes = warning.dump().size() + 1;
        if (warnings.size() < 8 && warning_bytes + bytes <= 4096) {
            warnings.push_back(std::move(warning));
            warning_bytes += bytes;
        }
    };
    for (const auto& row : results) {
        collect_warning(row);
        for (const auto& variant : row.variant_results) collect_warning(variant);
    }
    const std::size_t warning_total = resource_check.at("warning_queries");
    const Json warning_budget = {{"total", warning_total}, {"returned", warnings.size()},
        {"omitted", warning_total - warnings.size()}, {"max_records", 8}, {"max_bytes", 4096}};
    auto rounded_ratio = [](double value) {
        return std::round(value * 1'000'000.0) / 1'000'000.0;
    };
    Json rank_summary = {
        {"policy", "global-evidence-order-unique-path-v1"},
        {"ranked_queries", ranked_query_count},
        {"expected_paths", expected_rank_paths},
        {"found_paths", found_rank_paths},
        {"mean_reciprocal_rank",
         ranked_query_count > 0
             ? rounded_ratio(reciprocal_rank_sum / static_cast<double>(ranked_query_count))
             : 0.0},
        {"recall_by_n", Json::array()}
    };
    for (const auto& [at_n, aggregate] : recall_by_n) {
        rank_summary["recall_by_n"].push_back({
            {"at_n", at_n},
            {"queries", aggregate.queries},
            {"expected_paths", aggregate.expected},
            {"recalled_paths", aggregate.recalled},
            {"recall", aggregate.expected > 0
                           ? rounded_ratio(static_cast<double>(aggregate.recalled) /
                                           static_cast<double>(aggregate.expected))
                           : 0.0}
        });
    }
    if (eval_options.failed_only) {
        Json failures = Json::array();
        std::size_t failure_total = 0;
        std::size_t failure_bytes = 0;
        std::size_t detail_records_omitted = 0;
        for (const EvalCaseResult& result : results) {
            append_eval_failures(options, failures, result, failure_total, failure_bytes,
                                 detail_records_omitted);
        }
        Json output = make_result_document("shin.eval-failures.v1", "eval", ok);
        output["projection"] = "failed-only";
        output["quality_ok"] = quality_ok;
        output["resource_check"] = resource_check;
        output["warnings"] = warnings;
        output["warning_budget"] = warning_budget;
        output["profile"] = options.profile.id;
        output["root"] = options.root.generic_string();
        output["out_root"] = options.out_root.generic_string();
        output["strict"] = eval_options.strict;
        output["require_current"] = eval_options.require_current;
        output["snapshot_id"] =
            freshness.snapshot_id.empty() ? Json(nullptr) : Json(freshness.snapshot_id);
        output["index_freshness"] = index_freshness_json(freshness);
        output["summary"] = {{"total", total}, {"passed", passed}, {"failed", failed}};
        output["query_summary"] = {{"total", query_total},
                                   {"passed", query_passed},
                                   {"failed", query_total - query_passed}};
        output["suite_failure_reasons"] = Json::array();
        if (total == 0) {
            output["suite_failure_reasons"].push_back("empty-suite");
        }
        if ((eval_options.strict || eval_options.require_current) && !freshness.current) {
            output["suite_failure_reasons"].push_back("index-not-current");
        }
        if ((eval_options.strict || eval_options.require_current) &&
            !freshness.fully_verified) {
            output["suite_failure_reasons"].push_back("index-not-fully-verified");
        }
        if (failure_total > 0) {
            output["suite_failure_reasons"].push_back("query-failures");
        }
        output["failure_budget"] = {{"policy", "whole-record-byte-budget-with-header-v2"},
                                    {"max_records", kEvalFailureLimit},
                                    {"max_bytes", kEvalFailurePayloadBytes},
                                    {"eligible_records", failure_total},
                                    {"returned_records", failures.size()},
                                    {"omitted_records", failure_total - failures.size()},
                                    {"detail_records_omitted", detail_records_omitted},
                                    {"used_bytes", failure_bytes},
                                    {"truncated", failure_total != failures.size() ||
                                                      detail_records_omitted > 0}};
        output["failures"] = std::move(failures);
        emit_json_document(std::cout, output);
        return ((eval_options.strict || eval_options.require_current) && !ok) ? 1 : 0;
    }
    std::cout << "{\n"
              << "  \"schema_version\": \"shin.eval.v1\",\n"
              << "  \"tool\": \"shin-context\",\n"
              << "  \"command\": \"eval\",\n"
              << "  \"profile\": " << json_string(options.profile.id) << ",\n"
              << "  \"root\": " << json_string(options.root.generic_string()) << ",\n"
              << "  \"out_root\": " << json_string(options.out_root.generic_string()) << ",\n"
              << "  \"strict\": " << (eval_options.strict ? "true" : "false") << ",\n"
              << "  \"require_current\": " << (eval_options.require_current ? "true" : "false") << ",\n"
              << "  \"index_freshness\": ";
    emit_index_freshness_json(std::cout, freshness);
    std::cout << ",\n"
              << "  \"snapshot_id\": "
              << (freshness.snapshot_id.empty() ? "null" : json_string(freshness.snapshot_id)) << ",\n"
              << "  \"ok\": " << (ok ? "true" : "false") << ",\n"
              << "  \"quality_ok\": " << (quality_ok ? "true" : "false") << ",\n"
              << "  \"resource_check\": " << resource_check.dump() << ",\n"
              << "  \"warnings\": " << warnings.dump() << ",\n"
              << "  \"warning_budget\": " << warning_budget.dump() << ",\n"
              << "  \"summary\": {\"total\":" << total
              << ",\"passed\":" << passed
              << ",\"failed\":" << failed
              << "},\n"
              << "  \"query_summary\": {\"total\":" << query_total
              << ",\"passed\":" << query_passed
              << ",\"failed\":" << (query_total - query_passed)
              << "},\n"
              << "  \"rank_summary\": " << rank_summary.dump() << ",\n"
              << "  \"cases\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        std::cout << "    ";
        emit_eval_case_json(std::cout, results[i]);
        if (i + 1 < results.size()) {
            std::cout << ",";
        }
        std::cout << "\n";
    }
    std::cout << "  ]\n"
              << "}\n";
    return ((eval_options.strict || eval_options.require_current) && !ok) ? 1 : 0;
}

}  // namespace shin::context
