#include "context_internal.h"

namespace shin::context {

QueryCoverage make_query_coverage(const QueryRequest& request) {
    QueryCoverage coverage;
    coverage.groups.reserve(request.term_groups.size());
    for (std::size_t index = 0; index < request.term_groups.size(); ++index) {
        QueryGroupCoverage group;
        group.index = static_cast<int>(index);
        if (index < request.terms.size()) {
            group.term = request.terms[index];
        } else if (!request.term_groups[index].empty()) {
            group.term = request.term_groups[index].front();
        }
        coverage.groups.push_back(std::move(group));
    }
    return coverage;
}

void add_query_coverage_evidence(QueryCoverage& coverage,
                                 std::string_view path,
                                 const std::vector<int>& matched_group_indices) {
    if (path.empty()) {
        return;
    }
    for (int group_index : matched_group_indices) {
        if (group_index < 0 || static_cast<std::size_t>(group_index) >= coverage.groups.size()) {
            continue;
        }
        coverage.groups[static_cast<std::size_t>(group_index)].returned_evidence_paths.insert(
            std::string(path));
    }
}

void merge_planner_query_coverage(QueryCoverage& coverage, const BriefResult& result) {
    const std::size_t candidate_count =
        std::min(coverage.groups.size(), result.planner_candidate_rows_by_group.size());
    for (std::size_t index = 0; index < candidate_count; ++index) {
        QueryGroupCoverage& group = coverage.groups[index];
        group.planner_candidate_rows = result.planner_candidate_rows_by_group[index];
        group.planner_candidate_available = true;
    }

    const std::size_t disposition_count =
        std::min(coverage.groups.size(), result.planner_dispositions_by_group.size());
    for (std::size_t index = 0; index < disposition_count; ++index) {
        coverage.groups[index].planner_disposition = result.planner_dispositions_by_group[index];
    }

    const std::size_t evidence_count =
        std::min(coverage.groups.size(), result.planner_selected_document_paths_by_group.size());
    for (std::size_t index = 0; index < evidence_count; ++index) {
        QueryGroupCoverage& group = coverage.groups[index];
        group.selected_document_paths.insert(
            result.planner_selected_document_paths_by_group[index].begin(),
            result.planner_selected_document_paths_by_group[index].end());
    }
}

int query_coverage_covered_groups(const QueryCoverage& coverage) {
    return static_cast<int>(std::count_if(coverage.groups.begin(), coverage.groups.end(),
                                          [](const QueryGroupCoverage& group) {
                                              return !group.returned_evidence_paths.empty() ||
                                                     !group.selected_document_paths.empty();
                                          }));
}

Json query_coverage_json(const QueryCoverage& coverage) {
    Json groups = Json::array();
    int returned_evidence_covered_groups = 0;
    int selected_document_covered_groups = 0;
    for (const QueryGroupCoverage& group : coverage.groups) {
        const bool returned_evidence_covered = !group.returned_evidence_paths.empty();
        const bool selected_document_covered = !group.selected_document_paths.empty();
        returned_evidence_covered_groups += returned_evidence_covered ? 1 : 0;
        selected_document_covered_groups += selected_document_covered ? 1 : 0;
        Json item = {
            {"index", group.index},
            {"term", group.term},
            {"covered", returned_evidence_covered || selected_document_covered},
            {"returned_evidence_covered", returned_evidence_covered},
            {"selected_document_covered", selected_document_covered},
            {"returned_evidence_paths", group.returned_evidence_paths},
            {"selected_document_paths", group.selected_document_paths}
        };
        if (group.planner_candidate_available) {
            item["planner_candidate_rows"] = group.planner_candidate_rows;
        }
        if (!group.planner_disposition.empty()) {
            item["planner_disposition"] = group.planner_disposition;
        }
        groups.push_back(std::move(item));
    }
    const int covered_groups = query_coverage_covered_groups(coverage);
    return {
        {"schema_version", "shin.query-group-coverage.v1"},
        {"policy", "returned-evidence-and-planned-document-coverage-v1"},
        {"total_groups", coverage.groups.size()},
        {"covered_groups", covered_groups},
        {"returned_evidence_covered_groups", returned_evidence_covered_groups},
        {"selected_document_covered_groups", selected_document_covered_groups},
        {"uncovered_groups", static_cast<int>(coverage.groups.size()) - covered_groups},
        {"groups", std::move(groups)}
    };
}

}  // namespace shin::context
