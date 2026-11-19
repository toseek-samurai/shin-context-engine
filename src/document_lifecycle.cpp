#include "document_lifecycle.h"
#include "context_internal.h"

namespace shin::context {
namespace {
bool accepted(const std::string& value) {
    auto status = to_lower(trim(value));
    status.resize(status.find_first_of(" ,;:.\t\n") == std::string::npos ? status.size() : status.find_first_of(" ,;:.\t\n"));
    return status == "accepted" || status == "active" || status == "approved" || status == "implemented";
}
struct Declaration { std::string kind; bool whole = false; };
Declaration declaration(const SourceDoc& doc, const LinkRow& link) {
    // Header declarations may be ordinary prose and wrap across lines. Restrict
    // them to the preamble, and inspect text BEFORE this link, not later clauses.
    const auto line = link.line;
    if (line < 1 || line > 64 || static_cast<std::size_t>(line) > doc.lines.size()) return {};
    for (int i = 0; i < line; ++i) {
        auto text = trim(doc.lines[static_cast<std::size_t>(i)]);
        if (starts_with(text, "##") || starts_with(text, "```") || starts_with(text, "~~~")) return {};
    }
    const auto& current = doc.lines[static_cast<std::size_t>(line - 1)];
    const auto column = current.find("[" + link.text + "](" + link.target + ")");
    if (column == std::string::npos) return {};
    auto first = static_cast<std::size_t>(line - 1);
    while (first > 0 && !trim(doc.lines[first - 1]).empty() && !starts_with(trim(doc.lines[first - 1]), "#")) --first;
    std::string text;
    for (auto i = first; i + 1 < static_cast<std::size_t>(line); ++i) text += doc.lines[i] + " ";
    text += current.substr(0, column);
    text = to_lower(compact_whitespace(text));
    if (starts_with(text, "- ") || starts_with(text, "* ")) text = trim(text.substr(2));
    text.erase(std::remove(text.begin(), text.end(), '*'), text.end());
    Declaration found;
    std::size_t found_at = 0;
    for (const char* raw : {"superseded by", "amended by", "supersedes", "amends"}) {
        const std::string kind(raw);
        const auto position = text.rfind(kind);
        if (position == std::string::npos || (position && text[position - 1] != ' ')) continue;
        const auto after = position + kind.size();
        if (after < text.size() && text[after] != ' ' && text[after] != ':') continue;
        const bool reverse = kind == "superseded by" || kind == "amended by";
        if (position != 0 && !reverse && !starts_with(text, "status:")) continue;
        if (!found.kind.empty() && position < found_at) continue;
        auto remainder = trim(text.substr(after));
        if (starts_with(remainder, ":")) remainder = trim(remainder.substr(1));
        // A named concern (e.g. "authorization is superseded by") is partial.
        // Only a bare document-level declaration can retire the whole document.
        const bool whole = position == 0 && (remainder.empty() || starts_with(remainder, "["));
        found = {kind, whole}; found_at = position;
    }
    return found;
}
bool reachable(const std::map<std::string, std::set<std::string>>& graph,
               const std::string& start, const std::string& target) {
    std::vector<std::string> pending{start};
    std::set<std::string> seen;
    while (!pending.empty()) {
        auto path = std::move(pending.back()); pending.pop_back();
        if (path == target) return true;
        if (!seen.insert(path).second) continue;
        auto entry = graph.find(path);
        if (entry != graph.end()) pending.insert(pending.end(), entry->second.begin(), entry->second.end());
    }
    return false;
}
} // namespace

Json prepare_document_lifecycle(GeneratedRows& rows) {
    std::map<std::string, const SourceDoc*> docs;
    for (const auto& doc : rows.docs)
        if (doc.kind == "markdown") docs.emplace(doc.path, &doc);
    Json relations = Json::array();
    std::set<std::string> seen;
    std::size_t omitted = 0;
    std::map<std::string, std::set<std::string>> graph;
    for (const auto& link : rows.links) {
        if (link.kind != "markdown-link" || link.status != "ok" || link.path == link.resolved_path ||
            !docs.contains(link.path) || !docs.contains(link.resolved_path)) continue;
        const auto declared = declaration(*docs.at(link.path), link);
        const auto& kind = declared.kind;
        if (kind.empty()) continue;
        const bool reverse = kind == "superseded by" || kind == "amended by";
        const auto& newer = reverse ? link.resolved_path : link.path;
        const auto& older = reverse ? link.path : link.resolved_path;
        const std::string type = starts_with(kind, "supersed") ? "supersedes" : "amends";
        const auto hash = link.target.find('#');
        const std::string anchor = !reverse && hash != std::string::npos ? link.target.substr(hash + 1) : "";
        if (!seen.insert(newer + "\n" + older + "\n" + type + "\n" + anchor).second) continue;
        if (relations.size() >= 1024) { ++omitted; continue; }
        const bool confirmed = accepted(explicit_document_status(docs.at(newer)->content));
        const std::string scope = !declared.whole ? "partial" : anchor.empty() ? "document" : "fragment";
        relations.push_back({{"newer", newer}, {"older", older}, {"kind", type}, {"anchor", anchor},
            {"declared_in", link.path}, {"line", link.line}, {"scope", scope}, {"state", confirmed ? "confirmed" : "unconfirmed"}});
        if (confirmed && type == "supersedes" && scope != "partial") graph[newer].insert(older);
    }
    for (auto& relation : relations) {
        const auto newer = relation.at("newer").get<std::string>();
        const auto older = relation.at("older").get<std::string>();
        if (omitted > 0 && relation.at("state") == "confirmed") relation["state"] = "incomplete";
        if (relation.at("state") != "confirmed" || relation.at("kind") != "supersedes" || relation.at("scope") == "partial") continue;
        if (reachable(graph, older, newer)) { relation["state"] = "ambiguous"; continue; }
        const auto anchor = relation.at("anchor").get<std::string>();
        std::vector<std::pair<int,int>> ranges;
        for (const auto& row : rows.sections) {
            if (row.path != older || (!anchor.empty() && row.anchor != anchor)) continue;
            ranges.emplace_back(row.start_line, row.end_line);
        }
        if (!anchor.empty() && !ranges.empty()) {
            relation["older_start_line"] = std::min_element(ranges.begin(), ranges.end())->first;
            relation["older_end_line"] = std::max_element(ranges.begin(), ranges.end(),
                [](const auto& left, const auto& right) { return left.second < right.second; })->second;
        }
        for (auto& row : rows.sections) {
            if (row.path == older && (anchor.empty() || std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) {
                    return row.start_line >= range.first && row.end_line <= range.second;
                }))) { row.status = "superseded"; row.authority_level = "history"; }
        }
        const auto applies = [&](const auto& row) {
            return row.path == older && (anchor.empty() || std::any_of(ranges.begin(), ranges.end(),
                [&](const auto& range) { return row.line >= range.first && row.line <= range.second; }));
        };
        const auto retire = [&](auto& records) {
            for (auto& row : records) if (applies(row)) { row.status = "superseded"; row.authority_level = "history"; }
        };
        retire(rows.decisions); retire(rows.contracts); retire(rows.runbooks);
        for (auto& row : rows.facts)
            if (row.verification_level == "declared" && applies(row)) { row.status = "superseded"; row.authority_level = "history"; }
    }
    return {{"policy", "declared-document-relations-v1"}, {"relations", std::move(relations)}, {"omitted", omitted}};
}

BriefResult supplement_document_lifecycle(const RuntimeOptions& options, const QueryRequest& request,
                                         const Json& manifest, BriefResult result) {
    if (!manifest.contains("document_lifecycle")) return result;
    const auto& lifecycle = manifest.at("document_lifecycle");
    const auto selection = select_brief_evidence(result, request.limit, request.per_doc_limit, request.allowed_evidence_types, true);
    std::set<std::string> paths;
    for (const auto& hit : selection.sections) paths.insert(hit.section.path);
    for (const auto* hits : {&selection.decisions, &selection.contracts, &selection.runbooks, &selection.facts})
        for (const auto& hit : *hits) paths.insert(hit.fact.path);
    Json notes = Json::array();
    std::set<const Json*> noted;
    std::size_t omitted_notes = 0, note_bytes = 0;
    std::set<std::tuple<std::string,int,int>> proven;
    std::set<std::string> proof_paths;
    std::set<std::string> companions;
    int priority = 1;
    int support_ceiling = std::numeric_limits<int>::max();
    for (const auto& hit : selection.sections) {
        priority = std::max(priority, hit.score + 1);
        support_ceiling = std::min(support_ceiling, hit.score - 1);
    }
    for (const auto* hits : {&selection.decisions, &selection.contracts, &selection.runbooks, &selection.facts})
        for (const auto& hit : *hits) {
            priority = std::max(priority, hit.score + 1);
            support_ceiling = std::min(support_ceiling, hit.score - 1);
        }
    // Bounded traversal also follows replacement chains. It never reads a source
    // outside this publication or invents lexical coverage for a related record.
    std::size_t visited_rows = 0;
    std::size_t visited_sections = 0, visited_facts = 0;
    const auto prior_rows = result.query_plan.value("candidate_row_visits", std::size_t{0});
    const auto row_budget = result.query_plan.value("optional_row_visit_budget", 8 * kMaxIndexedCandidateRows);
    for (int pass = 0; pass < 4; ++pass) {
        bool added = false;
        for (const auto& relation : lifecycle.at("relations")) {
            const auto older = relation.at("older").get<std::string>();
            const auto newer = relation.at("newer").get<std::string>();
            bool older_selected = paths.contains(older);
            const auto anchor = relation.value("anchor", std::string{});
            const auto in_scope = [&](const auto& row) {
                return row.path == older && (anchor.empty() || (relation.contains("older_start_line") &&
                    row.end_line >= relation.at("older_start_line").get<int>() &&
                    row.start_line <= relation.at("older_end_line").get<int>()));
            };
            if (relation.at("state") == "confirmed" && relation.at("kind") == "supersedes" && relation.at("scope") != "partial") {
                bool needs_proof = false;
                const auto bind = [&](const auto& row) {
                    if (row.status == "superseded" && in_scope(row) && proven.emplace(row.path, row.start_line, row.end_line).second)
                        needs_proof = true;
                };
                for (const auto& hit : selection.sections) bind(hit.section);
                for (const auto* hits : {&selection.decisions, &selection.contracts, &selection.runbooks, &selection.facts})
                    for (const auto& hit : *hits) bind(hit.fact);
                if (needs_proof) { proof_paths.insert(relation.at("declared_in")); proof_paths.insert(newer); }
            }
            if (older_selected && !anchor.empty() && !companions.contains(older)) {
                older_selected = std::any_of(selection.sections.begin(), selection.sections.end(), [&](const auto& hit) {
                    return in_scope(hit.section) || (hit.section.path == older && hit.section.anchor == anchor);
                });
                for (const auto* hits : {&selection.decisions, &selection.contracts, &selection.runbooks, &selection.facts})
                    for (const auto& hit : *hits)
                        if (hit.fact.path == older && relation.contains("older_start_line") &&
                            hit.fact.end_line >= relation.at("older_start_line").get<int>() &&
                            hit.fact.start_line <= relation.at("older_end_line").get<int>()) older_selected = true;
            }
            if (!older_selected && !paths.contains(newer)) continue;
            if (noted.insert(&relation).second) {
                const auto bytes = relation.dump().size() + 1;
                if (notes.size() < 8 && bytes <= 4096 - note_bytes) {
                    notes.push_back(relation); note_bytes += bytes;
                } else ++omitted_notes;
            }
            if (!older_selected || paths.contains(newer)) continue;
            if (relation.at("state") != "confirmed" || companions.size() >= 4) continue;
            const bool replacement = relation.at("kind") == "supersedes" && relation.at("scope") != "partial";
            // A partial amendment does not invalidate the ordinary answer or
            // justify displacing it. Surface its notice when no spare slot exists.
            if (!replacement && selection.ordered.size() >= static_cast<std::size_t>(request.limit)) continue;
            if (kMaxIndexedCandidateRows > row_budget - std::min(prior_rows + visited_rows, row_budget)) {
                result.query_plan["optional_work_limited"] = true;
                continue;
            }
            paths.insert(newer); companions.insert(newer); added = true;
            std::string query = newer;
            for (const auto& term : request.terms) {
                const bool owner = std::any_of(paths.begin(), paths.end(), [&](const auto& path) {
                    return term == path || term == basename_of(path) || term == fs::path(path).stem().string();
                });
                if (!owner) query += " " + term;
            }
            auto linked = make_command_query_request(options.profile, {"--", query}, request.limit, request.per_doc_limit);
            linked.allowed_evidence_types = request.allowed_evidence_types;
            auto related = collect_brief_from_index_document_owner_scope(options, linked, &manifest);
            visited_rows += related.candidate_rows;
            visited_sections += related.matched_sections;
            visited_facts += related.matched_facts;
            for (auto& [path, hits] : related.section_hits_by_doc) {
                if (!replacement && selection.sections.empty()) continue;
                for (auto& hit : hits) {
                    auto score = score_section_match(hit.section, request.prepared_term_groups, 1, nullptr, request.scoring.get());
                    hit.score = !replacement ? std::min(score.total, support_ceiling) :
                        hit.section.authority_level == "history" ? score.total : score.total + priority;
                    if (replacement && result.document_owner_scope_used) hit.score = std::min(hit.score, priority - 2);
                    hit.score_components = score.components;
                    hit.score_components["document_relation"] += hit.score - score.total;
                    hit.matched_fields = score.matched_fields;
                    hit.matched_group_indices = score.matched_group_indices;
                    hit.complete_query_priority = false;
                    result.section_hits_by_doc[path].push_back(std::move(hit));
                }
            }
            const auto append_facts = [&](auto& source, auto& target, bool had_family) {
                if (!replacement && !had_family) return;
                for (auto& hit : source) {
                    auto score = score_fact_match(hit.fact, request.prepared_term_groups, 1, nullptr, request.scoring.get());
                    hit.score = !replacement ? std::min(score.total, support_ceiling) :
                        hit.fact.authority_level == "history" ? score.total : score.total + priority;
                    if (replacement && result.document_owner_scope_used) hit.score = std::min(hit.score, priority - 2);
                    hit.score_components = score.components;
                    hit.score_components["document_relation"] += hit.score - score.total;
                    hit.matched_fields = score.matched_fields;
                    hit.matched_group_indices = score.matched_group_indices;
                    hit.complete_query_priority = false;
                    const auto existing = std::find_if(target.begin(), target.end(), [&](const auto& other) { return other.fact == hit.fact; });
                    if (existing == target.end()) target.push_back(std::move(hit));
                    else if (fact_hit_better(hit, *existing)) *existing = std::move(hit);
                }
                std::sort(target.begin(), target.end(), fact_hit_better);
            };
            append_facts(related.decisions, result.decisions, !selection.decisions.empty());
            append_facts(related.contracts, result.contracts, !selection.contracts.empty());
            append_facts(related.runbooks, result.runbooks, !selection.runbooks.empty());
            append_facts(related.facts, result.facts, !selection.facts.empty());
            result.matched_sections += related.matched_sections;
            result.matched_facts += related.matched_facts;
            result.matched_decisions += related.matched_decisions;
            result.matched_contracts += related.matched_contracts;
            result.matched_runbooks += related.matched_runbooks;
            result.matched_other_facts += related.matched_other_facts;
        }
        if (!added) break;
    }
    Json dependencies = Json::array();
    for (const auto& path : proof_paths) dependencies.push_back({{"path", path}, {"reason", "effective-lifecycle-status"}});
    if (!notes.empty() || !dependencies.empty() || omitted_notes > 0 || lifecycle.value("omitted", 0) > 0) result.document_lifecycle = {
        {"relations", std::move(notes)}, {"omitted", lifecycle.value("omitted", std::size_t{0}) + omitted_notes},
        {"relations_omitted", omitted_notes}, {"inventory_omitted", lifecycle.value("omitted", 0)},
        {"verification_dependencies", std::move(dependencies)},
        {"notice", "Declared amendments and replacements qualify these fragments. Read related current decisions; lexical coverage is not answer completeness."}};
    if (!result.query_plan.empty() && visited_rows > 0) {
        result.query_plan["stages"].push_back({{"stage", "document-relations"}, {"candidate_rows", visited_rows},
            {"matched_sections", visited_sections}, {"matched_facts", visited_facts}});
        result.query_plan["candidate_row_visits"] = result.query_plan.value("candidate_row_visits", std::size_t{0}) + visited_rows;
        result.query_plan["matched_section_visits"] = result.query_plan.value("matched_section_visits", std::size_t{0}) + visited_sections;
        result.query_plan["matched_fact_visits"] = result.query_plan.value("matched_fact_visits", std::size_t{0}) + visited_facts;
    }
    return result;
}
} // namespace shin::context
