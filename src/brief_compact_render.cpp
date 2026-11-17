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

bool excerpt_matches_group(FieldMatchInput& field, const PreparedTermGroup& group) {
    return std::any_of(group.begin(), group.end(), [&](const auto& term) { return field_contains_term(field, term); });
}

}  // namespace

// Match through the ordinary lexical interface at original whitespace offsets.
// Case folding/composition may change byte lengths; normalized offsets must
// never be used to slice original UTF-8 or select a different source range.
std::string compact_section_excerpt(const ContextHit& hit, const QueryRequest& request) {
    const auto& row = hit.section;
    if (row.indexed_text.empty() ||
        std::find(hit.matched_fields.begin(), hit.matched_fields.end(), "indexed_text") == hit.matched_fields.end())
        return row.preview;
    const auto count = [&](FieldMatchInput& field) {
        int found = 0;
        for (const int group : hit.matched_group_indices)
            if (group >= 0 && static_cast<std::size_t>(group) < request.prepared_term_groups.size() &&
                excerpt_matches_group(field, request.prepared_term_groups[group])) ++found;
        return found;
    };
    FieldMatchInput preview("excerpt", row.preview, 0, true);
    const int shown = count(preview);
    if (shown == static_cast<int>(hit.matched_group_indices.size())) return row.preview;
    FieldMatchInput body("excerpt", row.indexed_text, 0, true);
    const PreparedTermGroup* missing = nullptr;
    for (const int group : hit.matched_group_indices) {
        if (group < 0 || static_cast<std::size_t>(group) >= request.prepared_term_groups.size()) continue;
        const auto& terms = request.prepared_term_groups[group];
        if (!excerpt_matches_group(preview, terms) && excerpt_matches_group(body, terms)) { missing = &terms; break; }
    }
    if (!missing) return row.preview;
    std::vector<std::size_t> ends;
    const std::string_view text(row.indexed_text);
    for (std::size_t i = 1; i <= text.size(); ++i)
        if (!is_ascii_space(static_cast<unsigned char>(text[i - 1])) &&
            (i == text.size() || is_ascii_space(static_cast<unsigned char>(text[i])))) ends.push_back(i);
    if (ends.empty()) return row.preview;
    std::size_t low = 0, high = ends.size() - 1;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        FieldMatchInput prefix("excerpt", text.substr(0, ends[middle]), 0, true);
        if (excerpt_matches_group(prefix, *missing)) high = middle;
        else low = middle + 1;
    }
    constexpr std::size_t payload = 900 - 8; // Leave room for both ellipses.
    const auto matched_end = ends[low];
    std::size_t token_start = low == 0 ? 0 : ends[low - 1];
    while (token_start < matched_end && is_ascii_space(static_cast<unsigned char>(text[token_start])))
        ++token_start;
    const auto continuation = [&](std::size_t i) {
        return i < text.size() && (static_cast<unsigned char>(text[i]) & 0xc0U) == 0x80U;
    };
    std::string best = row.preview;
    int best_count = shown;
    const auto consider = [&](std::size_t begin, std::size_t end) {
        while (continuation(begin)) ++begin;
        while (continuation(end)) --end;
        FieldMatchInput window("excerpt", text.substr(begin, end - begin), 0, true);
        const auto hits = count(window);
        if (hits > best_count && excerpt_matches_group(window, *missing)) {
            best_count = hits;
            best = (begin ? "... " : "") + std::string(window.text) + (end < text.size() ? " ..." : "");
        }
    };
    if (matched_end - token_start > payload) {
        // A path or identifier can itself exceed the display budget. Compare
        // overlapping windows in its original bytes, including its final suffix.
        for (auto start = token_start; start + payload < matched_end; start += payload / 2)
            consider(start, start + payload);
        consider(matched_end - payload, matched_end);
    } else {
        auto begin = matched_end > 120 ? matched_end - 120 : 0;
        while (begin < token_start && begin > 0 && !is_ascii_space(static_cast<unsigned char>(text[begin - 1])))
            ++begin;
        begin = std::min(begin, token_start);
        auto end = std::min(text.size(), begin + payload);
        if (end < text.size()) {
            auto boundary = end;
            while (boundary > matched_end && !is_ascii_space(static_cast<unsigned char>(text[boundary]))) --boundary;
            if (boundary > matched_end) end = boundary;
        }
        consider(begin, end);
    }
    return best;
}

Json compact_brief_evidence_json(const BriefSelection& selection, const QueryRequest& request) {
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
                {"excerpt", compact_section_excerpt(hit, request)},
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
    output["retrieval_mode"] = execution.decision.mode;
    if (!result.document_lifecycle.empty()) output["document_lifecycle"] = result.document_lifecycle;
    output["query_coverage"] = compact_brief_coverage_json(execution.coverage);
    if (result.identifier_scope_used) {
        output["identifier_scope"] = {{"key", result.identifier_scope_key},
                                       {"policy", execution.decision.matching_policy}};
    }
    output["evidence_budget"] = {
        {"policy", "global-record-document-diversity-with-type-seeding-v3"},
        {"max_records", request.limit},
        {"returned_records", selection.ordered.size()},
        {"budgeted_out_records", selection.budgeted_out_records}
    };
    output["ranking_policy"] = "global-evidence-order-unique-path-v1";
    output["ranked_paths"] = std::move(ranked_paths);
    output["evidence"] = compact_brief_evidence_json(selection, request);
    output["verification_paths"] = compact_brief_verification_paths(selection);
    return output;
}

}  // namespace shin::context
