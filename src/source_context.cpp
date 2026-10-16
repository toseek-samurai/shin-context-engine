#include "source_context.h"
#include "context_internal.h"
#include "source_read.h"
#include "source_relations.h"

namespace shin::context {
namespace {
constexpr std::size_t kContextBytes = 32 * 1024;
constexpr std::size_t kRelatedFileBytes = 4096;
constexpr std::size_t kRelatedBodies = 4;
constexpr std::size_t kLinks = 16;
constexpr std::size_t kSourceBytes = 4 * 1024 * 1024;

void require_context(bool condition) {
    if (!condition) throw CliError("source-context-binding", "source-context",
        "Source context could not preserve its bounded publication binding.", 1, false,
        "Use a current, complete source publication.");
}
}

Json collect_source_context(const RuntimeOptions& options, const std::string& snapshot,
                            const Json& candidates, const std::map<std::string, std::string>& sources) {
    require_context(candidates.is_array() && candidates.size() <= 8);
    Json context = {{"policy", "selected-source-and-declared-target-ranges-v4"},
        {"answer_complete", false}, {"compiler_semantics", false}, {"readiness_assessed", false},
        {"relation_scope", "resolved-declared-links-only; ambiguous-and-unresolved-not-expanded"},
        {"bodies", Json::array()}, {"relations", Json::array()}, {"omissions", Json::array()},
        {"relation_inventories", Json::array()}, {"eligible_relations", 0},
        {"limits", {{"max_serialized_bytes", kContextBytes}, {"max_related_files", kRelatedBodies},
                    {"max_related_bodies", kRelatedBodies},
                    {"max_related_file_bytes", kRelatedFileBytes}, {"max_relations", kLinks},
                    {"max_materialization_source_bytes", kSourceBytes}}}};
    std::map<std::string, std::string> contents;
    std::size_t source_bytes = 0;
    const auto read = [&](const std::string& path) -> const std::string& {
        require_context(sources.contains(path));
        auto found = contents.find(path);
        if (found == contents.end()) {
            require_context(contents.size() < 12);
            auto content = read_bound_source(options.root, path, sources.at(path), 1024 * 1024);
            require_context(content.size() <= kSourceBytes - source_bytes);
            source_bytes += content.size();
            found = contents.emplace(path, std::move(content)).first;
        }
        return found->second;
    };
    const auto omit = [&](const std::string& path, const char* reason, const char* role) {
        context["omissions"].push_back({{"path", path}, {"reason", reason}, {"role", role}});
    };
    const auto append_body = [&](Json body, const std::string& text, const char* role) {
        body["text"] = text;
        body["role"] = role;
        body["evidence_kind"] = "source-code";
        body["verification_level"] = "source-observed";
        body["state"] = "complete";
        context["bodies"].push_back(std::move(body));
        // Reserve space for bounded metadata; never clip an evidence body.
        if (context["bodies"].dump().size() > 16 * 1024) {
            const auto rejected = context["bodies"].back();
            const auto path = rejected.at("path").get<std::string>();
            context["bodies"].erase(context["bodies"].size() - 1);
            omit(path, "serialized-body-budget", role);
            // Keep the exact missing declaration address, not its rejected text.
            // Otherwise a bounded follow-up cannot distinguish targets in a file.
            for (const char* key : {"start_line", "end_line", "target_symbol"})
                if (rejected.contains(key)) context["omissions"].back()[key] = rejected.at(key);
        }
    };
    std::vector<std::string> seeds;
    for (const auto& candidate : candidates) {
        const auto path = candidate.at("path").get<std::string>();
        const auto& content = read(path);
        const auto begin = candidate.at("byte_start").get<std::size_t>();
        const auto end = candidate.at("byte_end").get<std::size_t>();
        require_context(begin < end && end <= content.size() && end - begin <= 1024 &&
                        candidate.at("source_content_hash") == sources.at(path));
        const auto text = content.substr(begin, end - begin);
        const auto offsets = source_line_offsets(content);
        require_context(fnv1a64(text) == candidate.at("span_hash").get<std::string>() &&
            static_cast<std::size_t>(std::upper_bound(offsets.begin(), offsets.end(), begin) - offsets.begin()) ==
                candidate.at("start_line").get<std::size_t>() &&
            static_cast<std::size_t>(std::upper_bound(offsets.begin(), offsets.end(), end - 1) - offsets.begin()) ==
                candidate.at("end_line").get<std::size_t>());
        if (source_range_secret(content).found) { omit(path, "redacted", "selected"); continue; }
        append_body(candidate, text, "selected");
        if (std::find(seeds.begin(), seeds.end(), path) == seeds.end()) seeds.push_back(path);
    }
    // Reuse the resolved publication graph, but select range-owned edges before
    // applying delivery limits. A file-wide inspect preview can omit these edges.
    auto inspection = [&]() -> RetrievalSession {
        if (options.generated_snapshot)
            return RetrievalSession(options.generated_snapshot, RetrievalSessionOpenMode::verified_snapshot);
        return RetrievalSession(options, RetrievalSessionOpenMode::verified_snapshot);
    }();
    require_context(inspection.freshness().snapshot_id == snapshot);
    const auto graph = load_source_graph(options);
    std::vector<const LinkRow*> retained;
    for (const auto& seed : seeds) {
        std::size_t total = 0, returned = 0;
        const auto outgoing = graph.outgoing.find(seed);
        if (outgoing == graph.outgoing.end()) continue;
        for (const auto index : outgoing->second) {
            const auto& link = graph.links.at(index);
            if (!source_relation_kind(link.kind)) continue;
            const auto line = static_cast<std::size_t>(link.line);
            const bool selected_owner = std::any_of(candidates.begin(), candidates.end(), [&](const Json& c) {
                return c.at("path") == seed && c.at("start_line").get<std::size_t>() <= line &&
                       line <= c.at("end_line").get<std::size_t>();
            });
            if (!selected_owner) continue;
            ++total;
            context["eligible_relations"] = context.at("eligible_relations").get<std::size_t>() + 1;
            if (context["relations"].size() == kLinks) continue;
            ++returned;
            context["relations"].push_back({{"direction", "outgoing"}, {"path", link.resolved_path},
                {"source_path", seed}, {"source_line", line}, {"target_path", link.resolved_path},
                {"kind", link.kind}, {"status", link.status}});
            if (link.target_start_line > 0) {
                auto& edge = context["relations"].back();
                edge["target_start_line"] = link.target_start_line;
                edge["target_end_line"] = link.target_end_line;
                edge["target_range_origin"] = link.target_range_origin;
                edge["target_symbol"] = link.target;
            }
            retained.push_back(&link);
        }
        context["relation_inventories"].push_back({{"path", seed}, {"scope", "selected-source-ranges"},
            {"returned", returned}, {"total", total}, {"omitted", total - returned}, {"complete", total == returned}});
    }
    // Keep existing payload delivery ahead of newly available declaration ranges.
    std::stable_partition(retained.begin(), retained.end(), [](const LinkRow* link) { return link->target_start_line == 0; });
    std::set<std::tuple<std::string, int, int>> related;
    std::size_t attempted_related = 0;
    for (const auto* link : retained) {
        const auto& path = link->resolved_path;
        if (!related.emplace(path, link->target_start_line, link->target_end_line).second) continue;
        const bool declaration = link->target_start_line > 0;
        const auto omit_related = [&](const char* reason) {
            omit(path, reason, "related");
            if (declaration) {
                auto& item = context["omissions"].back();
                item["start_line"] = link->target_start_line;
                item["end_line"] = link->target_end_line;
                item["target_symbol"] = link->target;
            }
        };
        if (!sources.contains(path)) { omit_related("not-scanned-source"); continue; }
        if (attempted_related == kRelatedBodies && !contents.contains(path)) { omit_related("related-body-budget"); continue; }
        const auto& content = read(path);
        if (source_range_secret(content).found) {
            if (attempted_related < kRelatedBodies) ++attempted_related;
            omit_related("redacted"); continue;
        }
        const auto offsets = source_line_offsets(content);
        const auto text = declaration ? source_physical_range(content, offsets, link->target_start_line, link->target_end_line) : content;
        const auto begin = declaration ? offsets.at(static_cast<std::size_t>(link->target_start_line - 1)) : 0;
        const auto end = begin + text.size();
        const bool covered = std::any_of(context["bodies"].begin(), context["bodies"].end(), [&](const Json& body) {
            return body.at("path") == path && body.at("byte_start").get<std::size_t>() <= begin &&
                   body.at("byte_end").get<std::size_t>() >= end;
        });
        if (covered) continue;
        if (attempted_related == kRelatedBodies) { omit_related("related-body-budget"); continue; }
        ++attempted_related;
        if (text.size() > kRelatedFileBytes) { omit_related(declaration ? "related-range-too-large" : "related-file-too-large"); continue; }
        const auto end_line = declaration ? static_cast<std::size_t>(link->target_end_line) : content.empty() ? std::size_t{1} :
            static_cast<std::size_t>(std::upper_bound(offsets.begin(), offsets.end(), content.size() - 1) - offsets.begin());
        append_body({{"path", path}, {"source_content_hash", sources.at(path)}, {"span_hash", fnv1a64(text)},
            {"byte_start", begin}, {"byte_end", end}, {"start_line", declaration ? link->target_start_line : 1}, {"end_line", end_line},
            {"source_path", link->path}, {"source_line", link->line}, {"target_symbol", link->target},
            {"range_origin", declaration ? link->target_range_origin : "whole-related-file"}}, text, "related");
    }
    context["relations_omitted"] = context.at("eligible_relations").get<std::size_t>() - context["relations"].size();
    context["materialization_source_bytes"] = source_bytes;
    context["collection_complete"] = !candidates.empty() && context["omissions"].empty() &&
        context["relations_omitted"] == 0 && std::all_of(context["relation_inventories"].begin(),
        context["relation_inventories"].end(), [](const Json& row) { return row.at("complete") == true; });
    require_context(context.dump().size() <= kContextBytes);
    return context;
}
}
