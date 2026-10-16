#include "context_internal.h"

namespace shin::context {
namespace {

Json compact_brief_coverage_json(const QueryCoverage& coverage) {
    Json uncovered_groups = Json::array();
    int returned_evidence_covered_groups = 0;
    int selected_document_covered_groups = 0;
    for (const QueryGroupCoverage& group : coverage.groups) {
        const bool returned = !group.returned_evidence_paths.empty();
        const bool selected = !group.selected_document_paths.empty();
        returned_evidence_covered_groups += returned ? 1 : 0;
        selected_document_covered_groups += selected ? 1 : 0;
        if (!returned && !selected) {
            uncovered_groups.push_back({{"index", group.index}, {"term", group.term}});
        }
    }
    return {
        {"schema_version", "shin.query-group-coverage-compact.v1"},
        {"policy", "returned-evidence-and-planned-document-coverage-v1"},
        {"total_groups", coverage.groups.size()},
        {"covered_groups", query_coverage_covered_groups(coverage)},
        {"returned_evidence_covered_groups", returned_evidence_covered_groups},
        {"selected_document_covered_groups", selected_document_covered_groups},
        {"uncovered_group_count", uncovered_groups.size()},
        {"uncovered_groups", std::move(uncovered_groups)}
    };
}

std::string brief_evidence_type_name(BriefEvidenceType type) {
    switch (type) {
        case BriefEvidenceType::contract: return "contract";
        case BriefEvidenceType::decision: return "decision";
        case BriefEvidenceType::runbook: return "runbook";
        case BriefEvidenceType::fact: return "fact";
        case BriefEvidenceType::section: return "section";
    }
    throw std::logic_error("Unknown brief evidence type.");
}

const FactHit& brief_fact_hit(const BriefSelection& selection,
                             const BriefEvidenceRef& reference) {
    switch (reference.type) {
        case BriefEvidenceType::contract: return selection.contracts.at(reference.index);
        case BriefEvidenceType::decision: return selection.decisions.at(reference.index);
        case BriefEvidenceType::runbook: return selection.runbooks.at(reference.index);
        case BriefEvidenceType::fact: return selection.facts.at(reference.index);
        case BriefEvidenceType::section: break;
    }
    throw std::logic_error("Section evidence does not contain a fact hit.");
}

struct CompactVerificationPath {
    std::string path;
    std::set<std::pair<int, int>> ranges;
    std::vector<std::string> evidence_ids;
};

Json compact_brief_verification_paths(const BriefSelection& selection) {
    std::vector<CompactVerificationPath> paths;
    std::map<std::string, std::size_t> path_indices;
    for (const BriefEvidenceRef& reference : selection.ordered) {
        std::string path;
        std::string evidence_id;
        int start_line = 1;
        int end_line = 1;
        if (reference.type == BriefEvidenceType::section) {
            const SectionRow& section = selection.sections.at(reference.index).section;
            path = section.path;
            start_line = section.start_line;
            end_line = section.end_line;
            evidence_id = "section:" + section.document_id + ":" +
                          std::to_string(start_line) + ":" + std::to_string(end_line) + ":" +
                          section.row_type;
        } else {
            const FactRow& fact = brief_fact_hit(selection, reference).fact;
            path = fact.path;
            start_line = fact.start_line;
            end_line = fact.end_line;
            evidence_id = fact.id;
        }
        auto [entry, inserted] = path_indices.emplace(path, paths.size());
        if (inserted) {
            paths.push_back({path, {}, {}});
        }
        CompactVerificationPath& verification = paths[entry->second];
        verification.ranges.emplace(start_line, end_line);
        verification.evidence_ids.push_back(std::move(evidence_id));
    }

    Json output = Json::array();
    for (const CompactVerificationPath& path : paths) {
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

Json compact_brief_evidence_json(const BriefSelection& selection) {
    Json evidence = Json::array();
    for (std::size_t index = 0; index < selection.ordered.size(); ++index) {
        const BriefEvidenceRef& reference = selection.ordered[index];
        if (reference.type == BriefEvidenceType::section) {
            const ContextHit& hit = selection.sections.at(reference.index);
            evidence.push_back({
                {"rank", index + 1},
                {"type", "section"},
                {"path", hit.section.path},
                {"line", hit.section.line},
                {"start_line", hit.section.start_line},
                {"end_line", hit.section.end_line},
                {"heading", hit.section.heading},
                {"excerpt", hit.section.preview},
                {"indexed_text_bytes", hit.section.indexed_text.size()},
                {"indexed_text_omitted_bytes", hit.section.indexed_text_omitted_bytes},
                {"status", hit.section.status},
                {"authority_level", hit.section.authority_level},
                {"evidence_kind", hit.section.evidence_kind},
                {"verification_level", hit.section.verification_level},
                {"matched_group_indices", hit.matched_group_indices}
            });
            continue;
        }
        const FactHit& hit = brief_fact_hit(selection, reference);
        evidence.push_back({
            {"rank", index + 1},
            {"type", brief_evidence_type_name(reference.type)},
            {"record_type", hit.fact.record_type},
            {"id", hit.fact.id},
            {"path", hit.fact.path},
            {"line", hit.fact.line},
            {"start_line", hit.fact.start_line},
            {"end_line", hit.fact.end_line},
            {"title", hit.fact.title},
            {"excerpt", hit.fact.summary},
            {"status", hit.fact.status},
            {"authority_level", hit.fact.authority_level},
            {"evidence_kind", hit.fact.evidence_kind},
            {"verification_level", hit.fact.verification_level},
            {"source", hit.fact.source},
            {"matched_group_indices", hit.matched_group_indices}
        });
        if (!hit.fact.range_origin.empty()) evidence.back()["range_origin"] = hit.fact.range_origin;
    }
    return evidence;
}

}  // namespace

Json make_compact_brief_json(const RuntimeOptions& options,
                             const QueryRequest& request,
                             const std::string& index_mode,
                             const IndexFreshness& freshness,
                             const BriefExecution& execution) {
    const BriefResult& result = execution.retrieval;
    const BriefSelection& selection = execution.selection;
    Json ranked_paths = Json::array();
    for (const RankedBriefPath& path : execution.ranked_paths) {
        ranked_paths.push_back({{"path", path.path},
                                {"unique_path_rank", path.unique_path_rank},
                                {"first_evidence_rank", path.first_evidence_rank}});
    }
    Json output = make_result_document("shin.context-brief-compact.v1", "brief", true);
    output["projection"] = "compact";
    output["profile"] = options.profile.id;
    output["index_mode"] = index_mode;
    output["index_freshness"] = index_freshness_json(freshness);
    output["snapshot_id"] = freshness.snapshot_id.empty() ? Json(nullptr) : Json(freshness.snapshot_id);
    output["query"] = request.query;
    output["retrieval_mode"] = result.directory_owner_scope_used
                                   ? "directory-scoped"
                                   : (result.document_owner_scope_used
                                          ? "owner-scoped"
                                   : (result.query_planner_used
                                          ? "planned"
                                          : (result.fallback_used ? "relaxed" : "strict")));
    output["query_coverage"] = compact_brief_coverage_json(execution.coverage);
    if (result.identifier_scope_used) {
        output["retrieval_mode"] = "identifier-scoped";
        output["identifier_scope"] = {{"key", result.identifier_scope_key},
                                       {"policy", "confirmed-identifier-scope-v1"}};
    }
    output["evidence_budget"] = {
        {"policy", "global-record-document-diversity-with-type-seeding-v3"},
        {"max_records", request.limit},
        {"returned_records", selection.ordered.size()},
        {"budgeted_out_records", selection.budgeted_out_records}
    };
    output["ranking_policy"] = "global-evidence-order-unique-path-v1";
    output["ranked_paths"] = std::move(ranked_paths);
    output["evidence"] = compact_brief_evidence_json(selection);
    output["verification_paths"] = compact_brief_verification_paths(selection);
    return output;
}

}  // namespace shin::context
