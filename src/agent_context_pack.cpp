#include "agent_context_pack.h"
#include "context_internal.h"

namespace shin::context {
namespace {

struct EvidenceMatch {
    std::size_t requirement_index = 0;
    int query_rank = 0;
    int score = 0;
    std::vector<std::string> matched_fields;
    std::vector<int> matched_group_indices;
};

struct EvidenceCandidate {
    std::string evidence_id;
    std::string evidence_type;
    std::string path;
    int line = 1;
    Json record;
    std::vector<EvidenceMatch> matches;
    std::size_t best_requirement_index = 0;
    int best_query_rank = 0;
    int best_score = 0;
    Json serialized;
    std::size_t serialized_bytes = 0;
};

struct PackSelection {
    std::vector<std::size_t> selected_indices;
    std::vector<bool> selected;
    std::size_t evidence_bytes = 2;
    bool byte_budget_constrained = false;
    bool record_budget_constrained = false;
    bool per_document_budget_constrained = false;
    std::map<std::string, int> selected_per_document;
};

struct VerificationPath {
    std::string path;
    std::set<std::pair<int, int>> ranges;
    std::vector<std::string> evidence_ids;
};

struct PackAssembly {
    std::vector<EvidenceCandidate> candidates;
    std::vector<std::vector<std::size_t>> requirement_candidates;
    PackSelection selection;
    Json requirements = Json::array();
    Json evidence = Json::array();
    Json verification_paths = Json::array();
    std::vector<std::string> evidence_ids;
    std::size_t evidence_bytes = 2;
    PackCompletion completion;
    std::string request_fingerprint;
    std::string evidence_set_fingerprint;
    std::string receipt_id;
};

int evidence_type_order(std::string_view type) noexcept {
    if (type == "contract") {
        return 0;
    }
    if (type == "decision") {
        return 1;
    }
    if (type == "runbook") {
        return 2;
    }
    if (type == "fact") {
        return 3;
    }
    return 4;
}

int requirement_kind_order(TaskRequirementKind kind) noexcept {
    switch (kind) {
        case TaskRequirementKind::scope:
            return 0;
        case TaskRequirementKind::must:
            return 1;
        case TaskRequirementKind::should:
            return 2;
    }
    return 3;
}

int evidence_authority_order(const Json& record) {
    const std::string authority = json_string_value(record, "authority_level");
    if (authority == "source-of-truth") {
        return 0;
    }
    if (authority == "accepted") {
        return 1;
    }
    if (authority == "advisory") {
        return 2;
    }
    if (authority == "history") {
        return 4;
    }
    return 3;
}

std::string evidence_type_name(BriefEvidenceType type) {
    switch (type) {
        case BriefEvidenceType::contract:
            return "contract";
        case BriefEvidenceType::decision:
            return "decision";
        case BriefEvidenceType::runbook:
            return "runbook";
        case BriefEvidenceType::fact:
            return "fact";
        case BriefEvidenceType::section:
            return "section";
    }
    throw std::logic_error("Unknown brief evidence type.");
}

const ContextHit& referenced_section(const BriefSelection& selection,
                                     const BriefEvidenceRef& reference) {
    if (reference.type != BriefEvidenceType::section) {
        throw std::logic_error("Brief evidence is not a section.");
    }
    return selection.sections.at(reference.index);
}

const FactHit& referenced_fact(const BriefSelection& selection,
                               const BriefEvidenceRef& reference) {
    switch (reference.type) {
        case BriefEvidenceType::contract:
            return selection.contracts.at(reference.index);
        case BriefEvidenceType::decision:
            return selection.decisions.at(reference.index);
        case BriefEvidenceType::runbook:
            return selection.runbooks.at(reference.index);
        case BriefEvidenceType::fact:
            return selection.facts.at(reference.index);
        case BriefEvidenceType::section:
            break;
    }
    throw std::logic_error("Brief evidence is not a fact.");
}

Json stable_section_record(const ContextHit& hit) {
    Json record = section_hit_json(hit);
    record.erase("score");
    record.erase("score_components");
    record.erase("matched_fields");
    record.erase("matched_group_indices");
    return record;
}

Json stable_fact_record(const FactHit& hit) {
    Json record = fact_hit_json(hit);
    record.erase("score");
    record.erase("score_components");
    record.erase("matched_fields");
    record.erase("matched_group_indices");
    return record;
}

std::string retrieval_mode(const BriefResult& result) {
    if (result.identifier_scope_used) return "identifier-scoped";
    if (result.directory_owner_scope_used) {
        return "directory-scoped";
    }
    if (result.document_owner_scope_used) {
        return "owner-scoped";
    }
    if (result.query_planner_used) {
        return "planned";
    }
    return result.fallback_used ? "relaxed" : "strict";
}

std::string matching_policy(const BriefResult& result) {
    if (result.identifier_scope_used) return "confirmed-identifier-scope-v1";
    if (result.directory_owner_scope_used) {
        return "directory-owner-scope-v1";
    }
    if (result.document_owner_scope_used) {
        return "document-owner-scope-v1";
    }
    if (result.query_planner_used) {
        return "document-coverage-planner-v1";
    }
    return result.scope_fallback_used ? "first-term-must-rest-should-v1"
                                      : "required-groups-v1";
}

bool evidence_type_allowed(const TaskRequest& request, const std::string& type) {
    return std::find(request.evidence_types.begin(), request.evidence_types.end(), type) !=
           request.evidence_types.end();
}

std::pair<Json, EvidenceMatch> evidence_record_and_match(
    const BriefExecution& execution,
    const BriefEvidenceRef& reference,
    std::size_t requirement_index,
    int query_rank) {
    EvidenceMatch match;
    match.requirement_index = requirement_index;
    match.query_rank = query_rank;
    if (reference.type == BriefEvidenceType::section) {
        const ContextHit& hit = referenced_section(execution.selection, reference);
        match.score = hit.score;
        match.matched_fields = hit.matched_fields;
        match.matched_group_indices = hit.matched_group_indices;
        return {stable_section_record(hit), std::move(match)};
    }
    const FactHit& hit = referenced_fact(execution.selection, reference);
    match.score = hit.score;
    match.matched_fields = hit.matched_fields;
    match.matched_group_indices = hit.matched_group_indices;
    return {stable_fact_record(hit), std::move(match)};
}

void add_evidence_candidates(
    const TaskRequest& request,
    const std::vector<BriefExecution>& executions,
    std::vector<EvidenceCandidate>& candidates,
    std::vector<std::vector<std::size_t>>& requirement_candidates) {
    std::map<std::string, std::size_t> candidate_by_key;
    std::map<std::string, std::string> key_by_evidence_id;
    for (std::size_t requirement_index = 0;
         requirement_index < request.requirements.size(); ++requirement_index) {
        const BriefExecution& execution = executions[requirement_index];
        std::set<std::size_t> seen_for_requirement;
        for (std::size_t rank = 0; rank < execution.selection.ordered.size(); ++rank) {
            const BriefEvidenceRef& reference = execution.selection.ordered[rank];
            const std::string type = evidence_type_name(reference.type);
            if (!evidence_type_allowed(request, type)) {
                continue;
            }
            auto [record, match] = evidence_record_and_match(
                execution, reference, requirement_index, static_cast<int>(rank + 1));
            const std::string canonical_key = type + "\n" + record.dump();
            auto existing = candidate_by_key.find(canonical_key);
            std::size_t candidate_index = 0;
            if (existing == candidate_by_key.end()) {
                EvidenceCandidate candidate;
                candidate.evidence_type = type;
                candidate.record = std::move(record);
                // Deduplicate by the complete indexed record, but publish its fixed display
                // projection. Search-only text must not consume the agent's evidence budget.
                if (reference.type == BriefEvidenceType::section) {
                    candidate.record.erase("indexed_text");
                }
                candidate.path = json_string_value(candidate.record, "path");
                candidate.line = json_int_value(candidate.record, "line", 1);
                candidate.evidence_id = fnv1a64(canonical_key);
                const auto collision = key_by_evidence_id.find(candidate.evidence_id);
                if (collision != key_by_evidence_id.end() && collision->second != canonical_key) {
                    throw CliError("context-pack-evidence-id-collision", "internal",
                                   "Two different evidence records produced one FNV identity.",
                                   1, false,
                                   "Report the task request as a Shin Context Engine defect.");
                }
                key_by_evidence_id[candidate.evidence_id] = canonical_key;
                candidate.best_requirement_index = requirement_index;
                candidate.best_query_rank = match.query_rank;
                candidate.best_score = match.score;
                candidate.matches.push_back(std::move(match));
                candidate_index = candidates.size();
                candidate_by_key.emplace(canonical_key, candidate_index);
                candidates.push_back(std::move(candidate));
            } else {
                candidate_index = existing->second;
                if (seen_for_requirement.contains(candidate_index)) {
                    continue;
                }
                EvidenceCandidate& candidate = candidates[candidate_index];
                candidate.matches.push_back(std::move(match));
                const EvidenceMatch& added = candidate.matches.back();
                const TaskRequirement& current_best =
                    request.requirements[candidate.best_requirement_index];
                const TaskRequirement& challenger = request.requirements[requirement_index];
                const auto best_key = std::tuple(
                    requirement_kind_order(current_best.kind), current_best.ordinal,
                    candidate.best_query_rank, -candidate.best_score);
                const auto challenger_key = std::tuple(
                    requirement_kind_order(challenger.kind), challenger.ordinal,
                    added.query_rank, -added.score);
                if (challenger_key < best_key) {
                    candidate.best_requirement_index = requirement_index;
                    candidate.best_query_rank = added.query_rank;
                    candidate.best_score = added.score;
                }
            }
            seen_for_requirement.insert(candidate_index);
            requirement_candidates[requirement_index].push_back(candidate_index);
        }
    }
}

Json candidate_json(const EvidenceCandidate& candidate,
                    const TaskRequest& request) {
    Json matches = Json::array();
    for (const EvidenceMatch& match : candidate.matches) {
        const TaskRequirement& requirement = request.requirements[match.requirement_index];
        matches.push_back({
            {"requirement_id", requirement.id},
            {"requirement_kind", task_requirement_kind_name(requirement.kind)},
            {"query_rank", match.query_rank},
            {"score", match.score},
            {"matched_fields", match.matched_fields},
            {"matched_group_indices", match.matched_group_indices}
        });
    }
    const int start_line = json_int_value(candidate.record, "start_line", candidate.line);
    const int end_line = json_int_value(candidate.record, "end_line", candidate.line);
    return {
        {"evidence_id", candidate.evidence_id},
        {"evidence_type", candidate.evidence_type},
        {"location", {
            {"path", candidate.path},
            {"start_line", start_line},
            {"end_line", end_line}
        }},
        {"record", candidate.record},
        {"matches", std::move(matches)}
    };
}

void finalize_candidate_json(std::vector<EvidenceCandidate>& candidates,
                             const TaskRequest& request) {
    for (EvidenceCandidate& candidate : candidates) {
        candidate.serialized = candidate_json(candidate, request);
        candidate.serialized_bytes = candidate.serialized.dump().size();
    }
}

bool candidate_better(const EvidenceCandidate& left,
                      const EvidenceCandidate& right,
                      const TaskRequest& request) {
    const TaskRequirement& left_requirement =
        request.requirements[left.best_requirement_index];
    const TaskRequirement& right_requirement =
        request.requirements[right.best_requirement_index];
    const auto left_key = std::tuple(
        requirement_kind_order(left_requirement.kind), left_requirement.ordinal,
        left.best_query_rank, -left.best_score, evidence_type_order(left.evidence_type),
        left.path, left.line, left.evidence_id);
    const auto right_key = std::tuple(
        requirement_kind_order(right_requirement.kind), right_requirement.ordinal,
        right.best_query_rank, -right.best_score, evidence_type_order(right.evidence_type),
        right.path, right.line, right.evidence_id);
    return left_key < right_key;
}

const EvidenceMatch& candidate_match_for_requirement(
    const EvidenceCandidate& candidate,
    std::size_t requirement_index) {
    const auto match = std::find_if(
        candidate.matches.begin(), candidate.matches.end(),
        [&](const EvidenceMatch& value) {
            return value.requirement_index == requirement_index;
        });
    if (match == candidate.matches.end()) {
        throw std::logic_error(
            "Context Pack requirement candidate has no matching edge.");
    }
    return *match;
}

void sort_requirement_candidates(
    std::vector<std::vector<std::size_t>>& requirement_candidates,
    const std::vector<EvidenceCandidate>& candidates) {
    for (std::size_t requirement_index = 0;
         requirement_index < requirement_candidates.size(); ++requirement_index) {
        std::vector<std::size_t>& indices =
            requirement_candidates[requirement_index];
        std::sort(indices.begin(), indices.end(),
                  [&](std::size_t left_index, std::size_t right_index) {
                      const EvidenceCandidate& left = candidates[left_index];
                      const EvidenceCandidate& right = candidates[right_index];
                      const EvidenceMatch& left_match =
                          candidate_match_for_requirement(left, requirement_index);
                      const EvidenceMatch& right_match =
                          candidate_match_for_requirement(right, requirement_index);
                      return std::tuple(
                                 left_match.query_rank, -left_match.score,
                                 evidence_type_order(left.evidence_type),
                                 evidence_authority_order(left.record), left.path,
                                 left.line, left.evidence_id) <
                             std::tuple(
                                 right_match.query_rank, -right_match.score,
                                 evidence_type_order(right.evidence_type),
                                 evidence_authority_order(right.record), right.path,
                                 right.line, right.evidence_id);
                  });
    }
}

bool try_select_candidate(PackSelection& selection,
                          const EvidenceCandidate& candidate,
                          std::size_t candidate_index,
                          const TaskEvidenceBudget& budget) {
    if (selection.selected[candidate_index]) {
        return true;
    }
    if (selection.selected_per_document[candidate.path] >=
        budget.per_document_records) {
        selection.per_document_budget_constrained = true;
        return false;
    }
    if (selection.selected_indices.size() >= static_cast<std::size_t>(budget.max_records)) {
        selection.record_budget_constrained = true;
        return false;
    }
    const std::size_t separator = selection.selected_indices.empty() ? 0 : 1;
    if (candidate.serialized_bytes > budget.max_evidence_bytes ||
        separator > budget.max_evidence_bytes - candidate.serialized_bytes ||
        selection.evidence_bytes >
            budget.max_evidence_bytes - candidate.serialized_bytes - separator) {
        selection.byte_budget_constrained = true;
        return false;
    }
    selection.selected[candidate_index] = true;
    selection.selected_indices.push_back(candidate_index);
    selection.evidence_bytes += candidate.serialized_bytes + separator;
    ++selection.selected_per_document[candidate.path];
    return true;
}

bool requirement_satisfied_with_replacement(
    std::size_t requirement_index,
    const PackSelection& selection,
    const std::vector<EvidenceCandidate>& candidates,
    std::size_t removed_index,
    std::size_t added_index,
    const TaskRequest& request) {
    std::set<int> covered_groups;
    int matched_records = 0;
    auto add_match = [&](std::size_t candidate_index) {
        const EvidenceCandidate& candidate = candidates[candidate_index];
        const auto match = std::find_if(
            candidate.matches.begin(), candidate.matches.end(),
            [&](const EvidenceMatch& value) {
                return value.requirement_index == requirement_index;
            });
        if (match == candidate.matches.end()) {
            return;
        }
        ++matched_records;
        covered_groups.insert(match->matched_group_indices.begin(),
                              match->matched_group_indices.end());
    };
    for (std::size_t candidate_index : selection.selected_indices) {
        if (candidate_index != removed_index) {
            add_match(candidate_index);
        }
    }
    add_match(added_index);

    if (matched_records == 0) {
        return false;
    }
    const TaskRequirement& requirement = request.requirements[requirement_index];
    const int required_groups = requirement.require_full_coverage
                                    ? static_cast<int>(requirement.query.term_groups.size())
                                    : requirement.min_covered_groups;
    return required_groups == 0 ||
           static_cast<int>(covered_groups.size()) >= required_groups;
}

bool try_replace_same_document_candidate(
    PackSelection& selection,
    const std::vector<std::size_t>& candidate_indices,
    const std::vector<EvidenceCandidate>& candidates,
    const TaskEvidenceBudget& budget,
    const TaskRequest& request,
    std::size_t requirement_index) {
    for (std::size_t candidate_index : candidate_indices) {
        if (selection.selected[candidate_index]) {
            return true;
        }
        const EvidenceCandidate& candidate = candidates[candidate_index];
        if (selection.selected_per_document[candidate.path] <
            budget.per_document_records) {
            continue;
        }
        for (std::size_t position = 0; position < selection.selected_indices.size(); ++position) {
            const std::size_t removed_index = selection.selected_indices[position];
            const EvidenceCandidate& removed = candidates[removed_index];
            if (removed.path != candidate.path) {
                continue;
            }
            bool preserves_required = true;
            for (std::size_t seeded = 0; seeded <= requirement_index; ++seeded) {
                if (task_requirement_is_required(request.requirements[seeded].kind) &&
                    !requirement_satisfied_with_replacement(
                        seeded, selection, candidates, removed_index, candidate_index,
                        request)) {
                    preserves_required = false;
                    break;
                }
            }
            if (!preserves_required) {
                continue;
            }
            const std::size_t bytes_without_removed =
                selection.evidence_bytes - removed.serialized_bytes;
            if (candidate.serialized_bytes > budget.max_evidence_bytes ||
                bytes_without_removed >
                    budget.max_evidence_bytes - candidate.serialized_bytes) {
                continue;
            }
            selection.selected[removed_index] = false;
            selection.selected[candidate_index] = true;
            selection.selected_indices[position] = candidate_index;
            selection.evidence_bytes = bytes_without_removed + candidate.serialized_bytes;
            selection.per_document_budget_constrained = true;
            return true;
        }
    }
    return false;
}

void seed_requirement(PackSelection& selection,
                      const std::vector<std::size_t>& candidate_indices,
                      const std::vector<EvidenceCandidate>& candidates,
                      const TaskEvidenceBudget& budget,
                      const TaskRequest& request,
                      std::size_t requirement_index) {
    for (std::size_t candidate_index : candidate_indices) {
        if (try_select_candidate(selection, candidates[candidate_index], candidate_index,
                                 budget)) {
            return;
        }
    }
    if (task_requirement_is_required(request.requirements[requirement_index].kind)) {
        try_replace_same_document_candidate(selection, candidate_indices, candidates,
                                            budget, request, requirement_index);
    }
}

PackSelection select_pack_evidence(
    const TaskRequest& request,
    const std::vector<EvidenceCandidate>& candidates,
    const std::vector<std::vector<std::size_t>>& requirement_candidates) {
    PackSelection selection;
    selection.selected.assign(candidates.size(), false);

    for (std::size_t index = 0; index < request.requirements.size(); ++index) {
        if (task_requirement_is_required(request.requirements[index].kind)) {
            seed_requirement(selection, requirement_candidates[index], candidates,
                             request.budget, request, index);
        }
    }
    for (std::size_t index = 0; index < request.requirements.size(); ++index) {
        if (!task_requirement_is_required(request.requirements[index].kind)) {
            seed_requirement(selection, requirement_candidates[index], candidates,
                             request.budget, request, index);
        }
    }

    std::vector<std::size_t> ranked(candidates.size());
    for (std::size_t index = 0; index < ranked.size(); ++index) {
        ranked[index] = index;
    }
    std::sort(ranked.begin(), ranked.end(), [&](std::size_t left, std::size_t right) {
        return candidate_better(candidates[left], candidates[right], request);
    });
    for (std::size_t candidate_index : ranked) {
        try_select_candidate(selection, candidates[candidate_index], candidate_index,
                             request.budget);
    }
    return selection;
}

Json requirement_diagnostics(const TaskRequirement& requirement,
                             std::size_t requirement_index,
                             const BriefExecution& execution,
                             const std::vector<std::size_t>& candidate_indices,
                             const PackSelection& selection,
                             const std::vector<EvidenceCandidate>& candidates) {
    Json selected_ids = Json::array();
    std::set<int> selected_covered_groups;
    for (std::size_t candidate_index : candidate_indices) {
        if (selection.selected[candidate_index]) {
            const EvidenceCandidate& candidate = candidates[candidate_index];
            selected_ids.push_back(candidate.evidence_id);
            for (const EvidenceMatch& match : candidate.matches) {
                if (match.requirement_index == requirement_index) {
                    selected_covered_groups.insert(match.matched_group_indices.begin(),
                                                   match.matched_group_indices.end());
                }
            }
        }
    }
    const std::size_t selected_count = selected_ids.size();
    const int total_groups = static_cast<int>(requirement.query.term_groups.size());
    const int covered_groups = static_cast<int>(selected_covered_groups.size());
    const bool coverage_complete = covered_groups == total_groups;
    const int required_covered_groups = requirement.require_full_coverage
                                            ? total_groups
                                            : requirement.min_covered_groups;
    const bool coverage_policy_satisfied =
        required_covered_groups == 0 || covered_groups >= required_covered_groups;
    const bool evidence_satisfied = selected_count > 0;
    const bool satisfied = evidence_satisfied && coverage_policy_satisfied;
    std::vector<int> covered_group_indices(selected_covered_groups.begin(),
                                           selected_covered_groups.end());
    std::vector<int> uncovered_group_indices;
    for (int index = 0; index < total_groups; ++index) {
        if (!selected_covered_groups.contains(index)) {
            uncovered_group_indices.push_back(index);
        }
    }
    const std::string coverage_mode = requirement.require_full_coverage
                                          ? "all-groups"
                                          : (requirement.min_covered_groups > 0
                                                 ? "minimum-groups"
                                                 : "any-evidence");
    const BriefResult& result = execution.retrieval;
    Json output{
        {"id", requirement.id},
        {"kind", task_requirement_kind_name(requirement.kind)},
        {"required", task_requirement_is_required(requirement.kind)},
        {"query", requirement.query.query},
        {"evidence_satisfied", evidence_satisfied},
        {"satisfied", satisfied},
        {"status", !evidence_satisfied
                       ? (candidate_indices.empty() ? "no-eligible-evidence"
                                                    : "budget-excluded")
                       : (!coverage_policy_satisfied
                              ? "coverage-incomplete"
                              : (coverage_complete ? "satisfied"
                                                   : "satisfied-with-uncovered-groups"))},
        {"coverage_complete", coverage_complete},
        {"coverage_policy", {
            {"mode", coverage_mode},
            {"minimum_covered_groups", required_covered_groups}
        }},
        {"selected_query_coverage", {
            {"schema_version", "shin.selected-query-group-coverage.v1"},
            {"policy", "selected-pack-evidence-match-edges-v1"},
            {"total_groups", total_groups},
            {"covered_groups", covered_groups},
            {"uncovered_groups", total_groups - covered_groups},
            {"covered_group_indices", std::move(covered_group_indices)},
            {"uncovered_group_indices", std::move(uncovered_group_indices)}
        }},
        {"retrieval_mode", retrieval_mode(result)},
        {"query_diagnostics", {
            {"fallback_used", result.fallback_used},
            {"scope_fallback_used", result.scope_fallback_used},
            {"document_owner_scope_used", result.document_owner_scope_used},
            {"directory_owner_scope_used", result.directory_owner_scope_used},
            {"planner_attempted", result.planner_attempted},
            {"query_planner_used", result.query_planner_used},
            {"planner_rejection_reason", result.planner_rejection_reason.empty()
                                             ? Json(nullptr)
                                             : Json(result.planner_rejection_reason)},
            {"matching_policy", matching_policy(result)},
            {"candidate_rows", result.candidate_rows},
            {"candidate_index_used", result.candidate_index_used},
            {"required_group_matches", result.required_group_matches},
            {"total_term_groups", result.total_term_groups}
        }},
        {"query_coverage", query_coverage_json(execution.coverage)},
        {"retrieved_evidence", execution.selection.ordered.size()},
        {"eligible_evidence", candidate_indices.size()},
        {"selected_evidence", selected_count},
        {"budgeted_out_evidence", candidate_indices.size() - selected_count},
        {"selected_evidence_ids", std::move(selected_ids)}
    };
    if (result.identifier_probe_rows > 0) {
        output["query_diagnostics"]["identifier_scope_used"] = result.identifier_scope_used;
        output["query_diagnostics"]["identifier_scope_key"] = result.identifier_scope_key;
        output["query_diagnostics"]["identifier_probe_rows"] = result.identifier_probe_rows;
    }
    return output;
}

Json make_verification_paths(const std::vector<std::size_t>& selected_indices,
                             const std::vector<EvidenceCandidate>& candidates) {
    std::vector<VerificationPath> paths;
    std::map<std::string, std::size_t> path_index;
    for (std::size_t candidate_index : selected_indices) {
        const EvidenceCandidate& candidate = candidates[candidate_index];
        auto [entry, inserted] = path_index.emplace(candidate.path, paths.size());
        if (inserted) {
            paths.push_back({candidate.path, {}, {}});
        }
        VerificationPath& path = paths[entry->second];
        const int start_line = json_int_value(candidate.record, "start_line", candidate.line);
        const int end_line = json_int_value(candidate.record, "end_line", candidate.line);
        path.ranges.emplace(start_line, end_line);
        path.evidence_ids.push_back(candidate.evidence_id);
    }

    Json output = Json::array();
    for (const VerificationPath& path : paths) {
        Json ranges = Json::array();
        for (const auto& [start_line, end_line] : path.ranges) {
            ranges.push_back({{"start_line", start_line}, {"end_line", end_line}});
        }
        output.push_back({
            {"path", path.path},
            {"ranges", std::move(ranges)},
            {"evidence_ids", path.evidence_ids}
        });
    }
    return output;
}

PackAssembly assemble_context_pack(const RuntimeOptions& options,
                                   const TaskRequest& request,
                                   const RetrievalSession& session,
                                   const std::vector<BriefExecution>& executions) {
    PackAssembly assembly;
    assembly.requirement_candidates.resize(request.requirements.size());
    add_evidence_candidates(request, executions, assembly.candidates,
                            assembly.requirement_candidates);
    sort_requirement_candidates(assembly.requirement_candidates,
                                assembly.candidates);
    finalize_candidate_json(assembly.candidates, request);
    assembly.selection = select_pack_evidence(
        request, assembly.candidates, assembly.requirement_candidates);

    assembly.evidence_ids.reserve(assembly.selection.selected_indices.size());
    for (std::size_t candidate_index : assembly.selection.selected_indices) {
        assembly.evidence.push_back(assembly.candidates[candidate_index].serialized);
        assembly.evidence_ids.push_back(assembly.candidates[candidate_index].evidence_id);
    }
    assembly.evidence_bytes = assembly.evidence.dump().size();
    if (assembly.evidence_bytes != assembly.selection.evidence_bytes ||
        assembly.evidence_bytes > request.budget.max_evidence_bytes) {
        throw CliError("context-pack-budget-invariant", "internal",
                       "Serialized context evidence violated its byte-budget invariant.",
                       1, false,
                       "Report the task request as a Shin Context Engine defect.");
    }

    for (std::size_t index = 0; index < request.requirements.size(); ++index) {
        Json diagnostic = requirement_diagnostics(
            request.requirements[index], index, executions[index],
            assembly.requirement_candidates[index], assembly.selection,
            assembly.candidates);
        const bool satisfied = diagnostic.at("satisfied").get<bool>();
        if (task_requirement_is_required(request.requirements[index].kind)) {
            ++assembly.completion.required_requirements;
            if (diagnostic.at("coverage_complete").get<bool>()) {
                ++assembly.completion.required_requirements_with_complete_coverage;
            } else {
                assembly.completion.required_requirement_ids_with_uncovered_groups.push_back(
                    request.requirements[index].id);
            }
            if (satisfied) {
                ++assembly.completion.satisfied_required_requirements;
            } else {
                assembly.completion.unsatisfied_required_requirement_ids.push_back(
                    request.requirements[index].id);
            }
        } else {
            ++assembly.completion.optional_requirements;
            if (satisfied) {
                ++assembly.completion.satisfied_optional_requirements;
            } else {
                assembly.completion.unsatisfied_optional_requirement_ids.push_back(
                    request.requirements[index].id);
            }
        }
        assembly.requirements.push_back(std::move(diagnostic));
    }
    assembly.completion.complete =
        assembly.completion.unsatisfied_required_requirement_ids.empty();
    assembly.completion.coverage_complete =
        assembly.completion.required_requirement_ids_with_uncovered_groups.empty();
    assembly.verification_paths = make_verification_paths(
        assembly.selection.selected_indices, assembly.candidates);

    assembly.request_fingerprint = fnv1a64(request.canonical_document.dump());
    assembly.evidence_set_fingerprint = fnv1a64(assembly.evidence.dump());
    Json receipt_material = {
        {"schema_version", "shin.context-pack-receipt.v1"},
        {"engine_version", kProductVersion},
        {"generator_version", kGeneratorVersion},
        {"profile", options.profile.id},
        {"snapshot_id", session.freshness().snapshot_id},
        {"request_fingerprint", assembly.request_fingerprint},
        {"evidence_set_fingerprint", assembly.evidence_set_fingerprint},
        {"evidence_ids", assembly.evidence_ids}
    };
    assembly.receipt_id = fnv1a64(receipt_material.dump());
    return assembly;
}

}  // namespace

unsigned int pack_active_jobs(const TaskRequest& request, int requested_jobs) {
    if (requested_jobs < 0 || requested_jobs > kMaxPackJobs) {
        throw_usage_error("invalid-option-value",
                          "Pack jobs must be 0 (automatic) or between 1 and 8.");
    }
    const unsigned int automatic_jobs = std::min<unsigned int>(
        static_cast<unsigned int>(kMaxPackJobs),
        worker_count_for_items(request.requirements.size(), 1));
    const unsigned int jobs = static_cast<unsigned int>(
        requested_jobs > 0 ? requested_jobs : std::max(1U, automatic_jobs));
    return std::min<unsigned int>(jobs,
                                  static_cast<unsigned int>(request.requirements.size()));
}

AgentContextPack execute_agent_context_pack(const RuntimeOptions& options,
                                            const TaskRequest& request,
                                            const RetrievalSession& session,
                                            int requested_jobs) {
    AgentContextPack pack;
    pack.jobs = pack_active_jobs(request, requested_jobs);
    std::vector<BriefExecution> executions(request.requirements.size());
    const SteadyClock::time_point retrieval_start = SteadyClock::now();
    parallel_for_worker_indices(
        request.requirements.size(), pack.jobs,
        [&](unsigned int, std::size_t index) {
            QueryRequest query = request.requirements[index].query;
            query.allowed_evidence_types = request.evidence_types;
            executions[index] = session.brief_execution(query);
        });
    pack.retrieval_ms = elapsed_ms_since(retrieval_start);

    const SteadyClock::time_point selection_start = SteadyClock::now();
    PackAssembly assembly = assemble_context_pack(options, request, session, executions);
    pack.requirements = std::move(assembly.requirements);
    pack.evidence = std::move(assembly.evidence);
    pack.verification_paths = std::move(assembly.verification_paths);
    pack.evidence_ids = std::move(assembly.evidence_ids);
    pack.completion = std::move(assembly.completion);
    pack.eligible_records = assembly.candidates.size();
    pack.evidence_bytes = assembly.evidence_bytes;
    pack.byte_budget_constrained = assembly.selection.byte_budget_constrained;
    pack.record_budget_constrained = assembly.selection.record_budget_constrained;
    pack.per_document_budget_constrained =
        assembly.selection.per_document_budget_constrained;
    pack.request_fingerprint = std::move(assembly.request_fingerprint);
    pack.evidence_set_fingerprint = std::move(assembly.evidence_set_fingerprint);
    pack.receipt_id = std::move(assembly.receipt_id);
    pack.selection_ms = elapsed_ms_since(selection_start);
    return pack;
}

}  // namespace shin::context
