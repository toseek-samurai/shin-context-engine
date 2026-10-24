#include "context_internal.h"

namespace shin::context {

namespace {

struct PlannedEvidence {
    using Value = std::variant<ContextHit, FactHit>;

    explicit PlannedEvidence(ContextHit hit) : value(std::move(hit)) {}
    explicit PlannedEvidence(FactHit hit) : value(std::move(hit)) {}

    Value value;

    bool is_fact() const {
        return std::holds_alternative<FactHit>(value);
    }

    int score() const {
        return std::visit([](const auto& hit) { return hit.score; }, value);
    }

    const std::vector<int>& matched_groups() const {
        return std::visit([](const auto& hit) -> const std::vector<int>& {
            return hit.matched_group_indices;
        }, value);
    }

    int line() const {
        return std::visit([](const auto& hit) { return hit_line(hit); }, value);
    }

    std::string_view stable_id() const {
        return std::visit([](const auto& hit) -> std::string_view {
            using Hit = std::decay_t<decltype(hit)>;
            if constexpr (std::is_same_v<Hit, FactHit>) {
                return hit.fact.id;
            } else {
                return hit.section.anchor;
            }
        }, value);
    }

    std::pair<std::string_view, std::string_view> provenance() const {
        return std::visit([](const auto& hit) {
            using Hit = std::decay_t<decltype(hit)>;
            if constexpr (std::is_same_v<Hit, FactHit>) {
                return std::pair<std::string_view, std::string_view>{
                    hit.fact.evidence_kind, hit.fact.verification_level};
            } else {
                return std::pair<std::string_view, std::string_view>{
                    hit.section.evidence_kind, hit.section.verification_level};
            }
        }, value);
    }

private:
    static int hit_line(const ContextHit& hit) { return hit.section.line; }
    static int hit_line(const FactHit& hit) { return hit.fact.line; }
};

struct PlannedUnit {
    std::set<int> matched_groups;
    std::map<int, PlannedEvidence> best_evidence_by_group;
    std::map<std::string, PlannedEvidence> best_evidence_by_type;
    int best_score = 0;
};

struct CandidateGroup {
    int group_index = -1;
    CandidateRows rows;
};

std::size_t candidate_row_count(const CandidateRows& candidates) {
    return candidates.sections.size() + candidates.facts.size() + candidates.decisions.size() +
           candidates.contracts.size() + candidates.runbooks.size();
}

void merge_candidate_set(std::set<int>& target, const std::set<int>& source) {
    target.insert(source.begin(), source.end());
}

bool try_merge_candidate_group(CandidateRows& target, const CandidateRows& source) {
    CandidateRows merged = target;
    merge_candidate_set(merged.sections, source.sections);
    merge_candidate_set(merged.facts, source.facts);
    merge_candidate_set(merged.decisions, source.decisions);
    merge_candidate_set(merged.contracts, source.contracts);
    merge_candidate_set(merged.runbooks, source.runbooks);
    const std::size_t count = candidate_row_count(merged);
    if (count > kMaxIndexedCandidateRows) {
        return false;
    }
    merged.discovered_row_count = count;
    merged.term_index_coverage_complete = true;
    merged.from_terms_index = true;
    target = std::move(merged);
    return true;
}

bool planned_evidence_better(const PlannedEvidence& left, const PlannedEvidence& right) {
    if (left.score() != right.score()) {
        return left.score() > right.score();
    }
    if (left.matched_groups().size() != right.matched_groups().size()) {
        return left.matched_groups().size() > right.matched_groups().size();
    }
    if (left.is_fact() != right.is_fact()) {
        return left.is_fact();
    }
    if (left.line() != right.line()) {
        return left.line() < right.line();
    }
    if (left.stable_id() != right.stable_id()) {
        return left.stable_id() < right.stable_id();
    }
    return left.provenance() < right.provenance();
}

std::string planned_evidence_type_key(const PlannedEvidence& evidence) {
    if (evidence.is_fact()) {
        const FactRow& fact = std::get<FactHit>(evidence.value).fact;
        return brief_fact_group(fact) + "\n" + fact.evidence_kind + "\n" + fact.verification_level;
    }
    const SectionRow& section = std::get<ContextHit>(evidence.value).section;
    return "sections\n" + section.evidence_kind + "\n" + section.verification_level;
}

void retain_planned_evidence(PlannedUnit& document, const PlannedEvidence& evidence) {
    document.best_score = std::max(document.best_score, evidence.score());
    const std::string type = planned_evidence_type_key(evidence);
    const auto current_type = document.best_evidence_by_type.find(type);
    if (current_type == document.best_evidence_by_type.end() ||
        planned_evidence_better(evidence, current_type->second)) {
        document.best_evidence_by_type.insert_or_assign(type, evidence);
    }
    for (int group_index : evidence.matched_groups()) {
        document.matched_groups.insert(group_index);
        auto current = document.best_evidence_by_group.find(group_index);
        if (current == document.best_evidence_by_group.end() ||
            planned_evidence_better(evidence, current->second)) {
            document.best_evidence_by_group.insert_or_assign(group_index, evidence);
        }
    }
}

int count_new_groups(const PlannedUnit& document, const std::set<int>& covered_groups) {
    return static_cast<int>(std::count_if(document.matched_groups.begin(), document.matched_groups.end(),
                                          [&](int group) {
                                              return covered_groups.find(group) == covered_groups.end();
                                          }));
}

bool planned_unit_better(const RetrievalUnitKey& left_key,
                             const PlannedUnit& left,
                             const RetrievalUnitKey& right_key,
                             const PlannedUnit& right,
                             const std::set<int>& covered_groups) {
    const int left_new = count_new_groups(left, covered_groups);
    const int right_new = count_new_groups(right, covered_groups);
    if (left_new != right_new) {
        return left_new > right_new;
    }
    if (left.matched_groups.size() != right.matched_groups.size()) {
        return left.matched_groups.size() > right.matched_groups.size();
    }
    if (left.best_score != right.best_score) {
        return left.best_score > right.best_score;
    }
    return left_key < right_key;
}

std::optional<PlannedEvidence> representative_evidence(const PlannedUnit& document,
                                                        const std::set<int>& covered_groups) {
    std::optional<PlannedEvidence> best;
    for (const auto& [group_index, evidence] : document.best_evidence_by_group) {
        if (covered_groups.find(group_index) != covered_groups.end()) {
            continue;
        }
        if (!best.has_value() || planned_evidence_better(evidence, *best)) {
            best = evidence;
        }
    }
    if (best.has_value()) {
        return best;
    }
    for (const auto& [group_index, evidence] : document.best_evidence_by_group) {
        (void)group_index;
        if (!best.has_value() || planned_evidence_better(evidence, *best)) {
            best = evidence;
        }
    }
    return best;
}

void append_planned_section(BriefResult& result, ContextHit hit) {
    auto& retained = result.section_hits_by_doc[hit.section.path];
    if (std::none_of(retained.begin(), retained.end(), [&](const auto& previous) {
            return previous.section == hit.section;
        })) retained.push_back(std::move(hit));
}

void append_planned_fact(BriefResult& result, FactHit hit) {
    const std::string group = brief_fact_group(hit.fact);
    if (!result.seen_fact_keys.insert(hit.fact).second) return;
    if (group == "decisions") {
        result.decisions.push_back(std::move(hit));
    } else if (group == "contracts") {
        result.contracts.push_back(std::move(hit));
    } else if (group == "runbooks") {
        result.runbooks.push_back(std::move(hit));
    } else {
        result.facts.push_back(std::move(hit));
    }
}

void count_planned_fact_group(BriefResult& result, const FactRow& fact) {
    const std::string group = brief_fact_group(fact);
    if (group == "decisions") {
        ++result.matched_decisions;
    } else if (group == "contracts") {
        ++result.matched_contracts;
    } else if (group == "runbooks") {
        ++result.matched_runbooks;
    } else {
        ++result.matched_other_facts;
    }
}

}  // namespace

BriefResult collect_brief_from_index_document_coverage_plan(const RuntimeOptions& options,
                                                             const QueryRequest& request,
                                                             bool preserve_row_ranking,
                                                             int required_group_matches,
                                                             bool missing_documents_only,
                                                             const std::vector<CandidateRows>* prepared_groups,
                                                             const Json* manifest,
                                                             bool include_near_complete_sections) {
    BriefResult result;
    result.fallback_used = true;
    result.planner_attempted = true;
    result.query_planner_used = true;
    result.required_group_matches = 1;
    result.total_term_groups = strict_required_group_matches(request);
    if (request.prepared_term_groups.empty()) {
        return result;
    }
    result.planner_candidate_rows_by_group.resize(request.prepared_term_groups.size());
    result.planner_dispositions_by_group.resize(request.prepared_term_groups.size(), "no-postings");
    result.planner_selected_document_paths_by_group.resize(request.prepared_term_groups.size());

    std::vector<CandidateRows> group_rows =
        prepared_groups ? *prepared_groups :
        load_generated_query_group_candidates(options, request.prepared_term_groups);
    if (std::any_of(group_rows.begin(), group_rows.end(), [](const auto& group) {
            return !group.term_index_coverage_complete;
        })) {
        result.planner_rejection_reason = "document-postings-incomplete";
        return result;
    }
    const bool capped_group = std::any_of(group_rows.begin(), group_rows.end(), [](const auto& group) {
        return !group.from_terms_index;
    });
    CandidateRows candidates;
    if (preserve_row_ranking) {
        candidates = capped_group
            ? load_generated_query_candidates(options, request.prepared_term_groups, required_group_matches)
            : select_generated_group_candidates(group_rows, required_group_matches);
        if (!candidates.from_terms_index || !candidates.term_index_coverage_complete) {
            result.candidate_rows = candidates.discovered_row_count;
            result.planner_rejection_reason = candidates.term_index_coverage_complete
                ? "document-candidate-budget-exceeded" : "document-postings-incomplete";
            return result;
        }
    }
    const CandidateRows strict_candidates = candidates;
    const bool supplement_near = preserve_row_ranking && include_near_complete_sections &&
        required_group_matches >= 4;
    if (supplement_near) {
        auto near = capped_group
            ? load_generated_query_candidates(options, request.prepared_term_groups, required_group_matches - 1)
            : select_generated_group_candidates(group_rows, required_group_matches - 1);
        if (!near.from_terms_index || !near.term_index_coverage_complete) {
            result.candidate_rows = near.discovered_row_count;
            result.planner_rejection_reason = near.term_index_coverage_complete
                ? "document-candidate-budget-exceeded" : "document-postings-incomplete";
            return result;
        }
        candidates.sections = std::move(near.sections);
        candidates.discovered_row_count = candidate_row_count(candidates);
    }
    result.planner_document_paths = whole_document_paths(options, manifest);
    const std::size_t ordinary_candidate_count = candidate_row_count(strict_candidates);
    if (preserve_row_ranking || capped_group) {
        const auto threshold = preserve_row_ranking
            ? (supplement_near ? required_group_matches - 1 : required_group_matches) : 1;
        const auto* ordinary = missing_documents_only ? &strict_candidates : nullptr;
        auto qualified = capped_group
            ? recover_document_coverage_candidates(options, request.prepared_term_groups, threshold,
                                                   result.planner_document_paths, ordinary)
            : filter_document_coverage_candidates(options, group_rows, threshold,
                                                  result.planner_document_paths, ordinary);
        if (!qualified.has_value()) {
            result.planner_rejection_reason = "document-postings-incomplete";
            return result;
        }
        group_rows = std::move(*qualified);
        for (const auto& group : group_rows) {
            if (!group.from_terms_index) {
                result.candidate_rows = group.discovered_row_count;
                result.planner_rejection_reason = "document-candidate-budget-exceeded";
                return result;
            }
        }
    }
    std::vector<CandidateGroup> candidate_groups;
    candidate_groups.reserve(group_rows.size());
    for (std::size_t index = 0; index < group_rows.size(); ++index) {
        CandidateRows& rows = group_rows[index];
        result.planner_candidate_rows_by_group[index] = rows.discovered_row_count;
        if (rows.discovered_row_count > 0) {
            ++result.planner_candidate_groups;
        }
        if (!rows.from_terms_index || rows.discovered_row_count == 0) {
            if (rows.discovered_row_count > 0) {
                result.planner_dispositions_by_group[index] = "group-limit-exceeded";
            }
            continue;
        }
        result.planner_dispositions_by_group[index] = "pending";
        candidate_groups.push_back({static_cast<int>(index), std::move(rows)});
    }
    std::sort(candidate_groups.begin(), candidate_groups.end(),
              [](const CandidateGroup& left, const CandidateGroup& right) {
                  if (left.rows.discovered_row_count != right.rows.discovered_row_count) {
                      return left.rows.discovered_row_count < right.rows.discovered_row_count;
                  }
                  return left.group_index < right.group_index;
              });

    for (const CandidateGroup& group : candidate_groups) {
        if (try_merge_candidate_group(candidates, group.rows)) {
            ++result.planner_indexed_groups;
            result.planner_dispositions_by_group[static_cast<std::size_t>(group.group_index)] =
                "merged";
        } else {
            result.planner_dispositions_by_group[static_cast<std::size_t>(group.group_index)] =
                "union-limit-exceeded";
            result.planner_rejection_reason = "document-candidate-budget-exceeded";
            result.candidate_rows = candidates.discovered_row_count;
            return result;
        }
    }
    result.candidate_rows = candidates.discovered_row_count;
    result.candidate_index_used = candidates.from_terms_index;
    if (missing_documents_only && candidate_row_count(candidates) == ordinary_candidate_count) {
        result.planner_rejection_reason = "no-additional-document-candidates";
        return result;
    }
    if (!candidates.from_terms_index || candidates.discovered_row_count == 0) {
        return result;
    }

    std::map<RetrievalUnitKey, PlannedUnit> units;
    std::set<RetrievalUnitKey> anchored_documents;
    std::vector<ContextHit> near_sections;
    for_each_generated_section_matching_terms(options, candidates, request.prepared_term_groups, 1,
                                               [&](const SectionRow& section) {
        MatchScore match = score_section_match(section, request.prepared_term_groups, 1);
        if (match.total <= 0) {
            return;
        }
        ++result.matched_sections;
        ContextHit hit{section, match.total, match.components, match.matched_fields,
                       match.matched_group_indices, complete_brief_match_priority(result, match)};
        // Coverage planning supplements ordinary matches; it must not discard them.
        if (match.matched_group_indices.size() >=
                static_cast<std::size_t>(required_group_matches) ||
            (include_near_complete_sections && required_group_matches >= 4 &&
             match.matched_group_indices.size() >= static_cast<std::size_t>(required_group_matches - 1) &&
             section_has_non_body_query_match(section, request.prepared_term_groups, false))) {
            result.section_hits_by_doc[section.path].push_back(hit);
        }
        const auto unit = retrieval_unit_key(section, result.planner_document_paths);
        if (supplement_near && unit.whole_document) {
            if (section_has_non_body_query_match(section, request.prepared_term_groups, false))
                anchored_documents.insert(unit);
            if (match.matched_group_indices.size() == static_cast<std::size_t>(required_group_matches - 1))
                near_sections.push_back(hit);
        }
        retain_planned_evidence(units[unit], PlannedEvidence(std::move(hit)));
    });

    // A sibling heading can anchor a near-complete body in the same declared
    // document unit. Filename words alone and other provenance units cannot.
    for (auto& hit : near_sections) {
        if (anchored_documents.contains(retrieval_unit_key(hit.section, result.planner_document_paths)))
            append_planned_section(result, std::move(hit));
    }

    std::set<FactRow> matched_fact_keys;
    for_each_generated_fact_matching_terms(options, candidates, request.prepared_term_groups, 1,
                                            [&](const FactRow& fact) {
        MatchScore match = score_fact_match(fact, request.prepared_term_groups, 1);
        if (match.total <= 0) {
            return;
        }
        if (!matched_fact_keys.insert(fact).second) {
            ++result.deduplicated_facts;
            return;
        }
        ++result.matched_facts;
        count_planned_fact_group(result, fact);
        FactHit hit{fact, match.total, match.components, match.matched_fields,
                    match.matched_group_indices, complete_brief_match_priority(result, match)};
        if (match.matched_group_indices.size() >=
                static_cast<std::size_t>(required_group_matches)) {
            append_planned_fact(result, hit);
        }
        retain_planned_evidence(units[retrieval_unit_key(fact, result.planner_document_paths)],
                                PlannedEvidence(std::move(hit)));
    });

    std::set<RetrievalUnitKey> selected_units;
    std::map<std::string, int> selected_per_path;
    std::set<int> covered_groups;
    if (preserve_row_ranking) {
        const auto minimum = static_cast<std::size_t>(required_group_matches);
        for (const auto& [unit, document] : units) {
            if (document.matched_groups.size() < minimum) continue;
            for (const auto& [type, evidence] : document.best_evidence_by_type) {
                (void)type;
                if (evidence.is_fact()) {
                    append_planned_fact(result, std::get<FactHit>(evidence.value));
                } else {
                    append_planned_section(result, std::get<ContextHit>(evidence.value));
                }
            }
            selected_units.insert(unit);
            result.planner_unit_coverage[unit] = document.matched_groups;
            for (const int group : document.matched_groups) {
                covered_groups.insert(group);
            }
        }
    }
    const int unit_limit = std::max(1, request.limit);
    while (!preserve_row_ranking && static_cast<int>(selected_units.size()) < unit_limit) {
        auto best = units.end();
        for (auto current = units.begin(); current != units.end(); ++current) {
            if (selected_units.contains(current->first) ||
                selected_per_path[current->first.path] >= request.per_doc_limit ||
                count_new_groups(current->second, covered_groups) == 0) {
                continue;
            }
            if (best == units.end() ||
                planned_unit_better(current->first, current->second,
                                        best->first, best->second, covered_groups)) {
                best = current;
            }
        }
        if (best == units.end()) {
            break;
        }

        PlannedUnit& document = best->second;
        std::optional<PlannedEvidence> evidence = representative_evidence(document, covered_groups);
        if (!evidence.has_value()) {
            selected_units.insert(best->first);
            continue;
        }
        if (evidence->is_fact()) {
            FactHit hit = std::get<FactHit>(std::move(evidence->value));
            append_planned_fact(result, std::move(hit));
        } else {
            ContextHit hit = std::get<ContextHit>(std::move(evidence->value));
            append_planned_section(result, std::move(hit));
        }
        selected_units.insert(best->first);
        ++selected_per_path[best->first.path];
        result.planner_unit_coverage[best->first] = document.matched_groups;
        for (int group_index : document.matched_groups) {
            covered_groups.insert(group_index);
        }
    }

    std::sort(result.decisions.begin(), result.decisions.end(), fact_hit_better);
    std::sort(result.contracts.begin(), result.contracts.end(), fact_hit_better);
    std::sort(result.runbooks.begin(), result.runbooks.end(), fact_hit_better);
    std::sort(result.facts.begin(), result.facts.end(), fact_hit_better);
    result.planner_covered_groups = static_cast<int>(covered_groups.size());
    result.planner_selected_documents = 0;
    return result;
}

void finalize_planner_unit_coverage(BriefExecution& execution) {
    auto& result = execution.retrieval;
    std::set<RetrievalUnitKey> returned_units;
    for (const auto& hit : execution.selection.sections)
        returned_units.insert(retrieval_unit_key(hit.section, result.planner_document_paths));
    for (const auto* hits : {&execution.selection.facts, &execution.selection.decisions,
                             &execution.selection.contracts, &execution.selection.runbooks}) {
        for (const auto& hit : *hits) returned_units.insert(retrieval_unit_key(hit.fact, result.planner_document_paths));
    }
    std::set<std::string> documents;
    result.planner_selected_document_paths_by_group.assign(execution.coverage.groups.size(), {});
    for (const auto& [unit, groups] : result.planner_unit_coverage) {
        if (!unit.whole_document || !returned_units.contains(unit)) continue;
        documents.insert(unit.path);
        for (const int group : groups) {
            result.planner_selected_document_paths_by_group.at(static_cast<std::size_t>(group)).insert(unit.path);
        }
    }
    result.planner_selected_documents = static_cast<int>(documents.size());
    int covered = 0;
    for (std::size_t group = 0; group < execution.coverage.groups.size(); ++group) {
        if (!execution.coverage.groups[group].returned_evidence_paths.empty() ||
            !result.planner_selected_document_paths_by_group[group].empty()) ++covered;
    }
    result.planner_covered_groups = covered;
}

}  // namespace shin::context
