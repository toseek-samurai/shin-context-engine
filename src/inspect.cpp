#include "context_internal.h"
#include "source_relations.h"

#include <tuple>

namespace shin::context {
namespace {

constexpr std::size_t kInspectRecords = 64;
constexpr std::size_t kInspectBytes = 64 * 1024;
constexpr std::size_t kInspectRelations = 16;
constexpr std::size_t kInspectRelationBytes = 8 * 1024;

void relation_coverage(Json& result) {
    result["returned"] = result.at("items").size();
    result["omitted"] = result.at("total").get<std::size_t>() - result.at("items").size();
    result["complete"] = result.at("omitted") == 0;
}

Json inspect_relations(const RuntimeOptions& options, const std::string& path) {
    const auto graph = load_source_graph(options);
    Json result = {{"scope", "one-hop-resolved-declared-links"}, {"compiler_semantics", false},
        {"max_records", kInspectRelations}, {"max_bytes", kInspectRelationBytes},
        {"items", Json::array()}, {"total", 0}};
    std::set<std::size_t> seen;
    const auto append = [&](const auto& adjacency, bool outgoing) {
        const auto found = adjacency.find(path);
        if (found == adjacency.end()) return;
        for (const auto index : found->second) {
            if (!seen.insert(index).second) continue;
            const auto& link = graph.links.at(index);
            const auto& neighbor = outgoing ? link.resolved_path : link.path;
            if (neighbor == path) continue;
            result["total"] = result.at("total").get<std::size_t>() + 1;
            if (result.at("items").size() == kInspectRelations) continue;
            result["items"].push_back({{"direction", outgoing ? "outgoing" : "incoming"},
                {"path", neighbor}, {"source_path", link.path}, {"source_line", link.line},
                {"target_path", link.resolved_path}, {"kind", link.kind}, {"status", link.status}});
            if (link.target_start_line > 0) {
                auto& edge = result["items"].back();
                edge["target_start_line"] = link.target_start_line;
                edge["target_end_line"] = link.target_end_line;
                edge["target_range_origin"] = link.target_range_origin;
            }
        }
    };
    append(graph.outgoing, true);
    append(graph.incoming, false);
    relation_coverage(result);
    while (result.dump(2).size() > kInspectRelationBytes && !result.at("items").empty()) {
        result["items"].erase(result["items"].size() - 1);
        relation_coverage(result);
    }
    return result;
}

void validate_inspect_path(const std::string& path) {
    if (path.size() > 4096 || !profile_relative_path_valid(path) ||
        fs::path(path).lexically_normal().generic_string() != path ||
        std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
        throw_usage_error("invalid-inspect-path", "inspect requires one canonical repository-relative file path.");
    }
}

}  // namespace

Json RetrievalSession::inspect(const std::string& path) const {
    validate_inspect_path(path);
    const auto probe = probe_generated_path(options_, path, kInspectRecords);
    std::vector<Json> records;
    records.reserve(probe.sections.size() + probe.facts.size());
    for (const auto& row : probe.sections) {
        records.push_back({{"record_type", row.row_type}, {"document_id", row.document_id},
            {"path", row.path}, {"title", row.heading}, {"anchor", row.anchor},
            {"chunk_index", row.chunk_index}, {"start_line", row.start_line}, {"end_line", row.end_line},
            {"status", row.status}, {"authority_level", row.authority_level},
            {"evidence_kind", row.evidence_kind}, {"verification_level", row.verification_level}});
    }
    for (const auto& row : probe.facts) {
        Json record = {{"record_type", row.record_type}, {"id", row.id}, {"path", row.path},
            {"title", row.title}, {"start_line", row.start_line}, {"end_line", row.end_line},
            {"source", row.source}, {"status", row.status}, {"authority_level", row.authority_level},
            {"evidence_kind", row.evidence_kind}, {"verification_level", row.verification_level}};
        if (!row.symbol_name.empty()) record["symbol_name"] = row.symbol_name;
        if (!row.span_hash.empty()) record["span_hash"] = row.span_hash;
        if (!row.range_origin.empty()) record["range_origin"] = row.range_origin;
        records.push_back(std::move(record));
    }
    std::sort(records.begin(), records.end(), [](const Json& left, const Json& right) {
        const auto key = [](const Json& row) {
            return std::tuple(row.at("start_line").get<int>(), row.at("end_line").get<int>(),
                              row.at("record_type").get<std::string>(), row.dump());
        };
        return key(left) < key(right);
    });
    Json result = make_result_document("shin.file-inspection.v1", "inspect", true);
    result.update({{"profile", options_.profile.id}, {"root", options_.root.generic_string()},
        {"path", path}, {"index_mode", "generated-index"}, {"snapshot_id", freshness_.snapshot_id},
        {"index_freshness", index_freshness_json(freshness_)},
        {"limits", {{"max_records", kInspectRecords}, {"max_output_bytes", kInspectBytes},
                    {"max_rows_per_store", kInspectRecords}}},
        {"interpretation", {{"scope", "indexed-records-only"}, {"order", "source-range-not-relevance"},
                            {"source_inspection_required", true}, {"answer_completeness", "not-asserted"}}},
        {"records", Json::array()}, {"relations", inspect_relations(options_, path)}});
    auto& output = result["records"];
    const auto update_coverage = [&] {
        result["coverage"] = {{"complete", probe.complete && output.size() == records.size()},
            {"probed_records", records.size()}, {"returned_records", output.size()},
            {"omitted_probed_records", records.size() - output.size()},
            {"unprobed_records", probe.complete ? Json(0) : Json(nullptr)}};
    };
    for (std::size_t i = 0; i < std::min(kInspectRecords, records.size()); ++i)
        output.push_back(std::move(records[i]));
    update_coverage();
    // Keep the established record inventory first; links have their own budget
    // and omission accounting and never trigger recursive reads or traversal.
    while (result.dump(2).size() + 1 > kInspectBytes && !result["relations"]["items"].empty()) {
        auto& relations = result["relations"];
        relations["items"].erase(relations["items"].size() - 1);
        relation_coverage(relations);
    }
    // Drop whole records; never return partial metadata or conceal an unknown tail.
    while (result.dump(2).size() + 1 > kInspectBytes && !output.empty()) {
        output.erase(output.size() - 1);
        update_coverage();
    }
    if (result.dump(2).size() + 1 > kInspectBytes)
        throw CliError("inspect-output-budget", "resource", "Inspection envelope exceeds its byte budget.", 1,
                       false, "Use a shorter explicit repository and publication route.");
    return result;
}

int run_inspect(const RuntimeOptions& options, const std::vector<std::string>& args) {
    if (args.empty() || args.size() > 2 || (args.size() == 2 && args[1] != "--require-current"))
        throw_usage_error("invalid-inspect-arguments", "Use inspect PATH [--require-current].");
    validate_inspect_path(args.front());
    RetrievalSession session(options, args.size() == 2 ? RetrievalSessionOpenMode::verified_current
                                                       : RetrievalSessionOpenMode::verified_snapshot);
    emit_json_document(std::cout, session.inspect(args.front()));
    return 0;
}

}  // namespace shin::context
