#include "context_internal.h"
#include "source_relations.h"
#include "source_read.h"
#include <iterator>

namespace shin::context {

static SectionRow make_section_row(const Profile& profile,
                            const SourceDoc& doc,
                            const std::string& heading,
                            const std::string& preview,
                            int line,
                            int level,
                            int start_line,
                            int end_line,
                            const std::string& status,
                            const std::string& row_type,
                            int chunk_index) {
    SectionRow row;
    row.document_id = doc.id;
    row.path = doc.path;
    row.status = status;
    row.authority_level = infer_authority_level(
        doc.doc_type, "section", doc.path, row.status);
    row.evidence_kind = doc.evidence_kind;
    row.verification_level = doc.verification_level;
    row.heading = heading;
    row.anchor = slugify(heading);
    row.preview = preview;
    row.topics = detect_topics(profile, doc.path, heading, preview);
    row.line = line;
    row.level = level;
    row.start_line = start_line;
    row.end_line = std::max(start_line, end_line);
    row.chunk_index = chunk_index;
    row.row_type = row_type;
    return row;
}

static std::size_t next_indexed_markdown_heading(const Profile& profile,
                                          const std::vector<int>& heading_levels,
                                          std::size_t start) {
    for (std::size_t i = start; i < heading_levels.size(); ++i) {
        const int level = heading_levels[i];
        if (level > 0 && level <= profile.markdown_max_heading_level) {
            return i;
        }
    }
    return heading_levels.size();
}

static std::size_t last_non_empty_line_exclusive(const std::vector<std::string>& lines,
                                          std::size_t start,
                                          std::size_t end) {
    std::size_t capped_end = std::min(end, lines.size());
    while (capped_end > start) {
        if (!trim(lines[capped_end - 1]).empty()) {
            return capped_end;
        }
        --capped_end;
    }
    return start;
}

static bool markdown_section_should_chunk(const Profile& profile,
                                   const std::vector<std::string>& lines,
                                   std::size_t start,
                                   std::size_t end) {
    if (profile.markdown_chunk_lines <= 0 || profile.markdown_chunk_min_lines <= 0) {
        return false;
    }
    const std::size_t capped_end = std::min(end, lines.size());
    if (capped_end <= start) {
        return false;
    }
    int non_empty_lines = 0;
    std::size_t chars = 0;
    for (std::size_t i = start; i < capped_end; ++i) {
        std::string compact = compact_whitespace(lines[i]);
        if (compact.empty()) {
            continue;
        }
        ++non_empty_lines;
        chars += compact.size();
    }
    return non_empty_lines >= profile.markdown_chunk_min_lines ||
           (profile.markdown_chunk_chars > 0 &&
            chars >= static_cast<std::size_t>(profile.markdown_chunk_chars));
}

static void append_markdown_chunk_rows(const Profile& profile,
                                const SourceDoc& doc,
                                std::vector<SectionRow>& rows,
                                const std::string& heading,
                                int level,
                                std::size_t body_start,
                                std::size_t body_end,
                                const std::string& status) {
    if (!markdown_section_should_chunk(profile, doc.lines, body_start, body_end)) {
        return;
    }
    const int chunk_lines = std::max(1, profile.markdown_chunk_lines);
    int chunk_index = 1;
    std::size_t chunk_start = body_start;
    while (chunk_start < body_end) {
        std::size_t chunk_end = std::min(body_end, chunk_start + static_cast<std::size_t>(chunk_lines));
        chunk_end = last_non_empty_line_exclusive(doc.lines, chunk_start, chunk_end);
        if (chunk_end <= chunk_start) {
            chunk_start += static_cast<std::size_t>(chunk_lines);
            continue;
        }
        const std::string preview = preview_from_line_range(doc.lines, chunk_start, chunk_end, 900);
        if (!preview.empty()) {
            rows.push_back(make_section_row(profile,
                                            doc,
                                            heading + " chunk " + std::to_string(chunk_index),
                                            preview,
                                            static_cast<int>(chunk_start + 1),
                                            level,
                                            static_cast<int>(chunk_start + 1),
                                            static_cast<int>(chunk_end),
                                            status,
                                            "chunk",
                                            chunk_index));
        }
        chunk_start = std::max(chunk_end, chunk_start + static_cast<std::size_t>(chunk_lines));
        ++chunk_index;
    }
}

struct JsonLineDepth {
    int key_depth = -1;
    int end_depth = 0;
};

static JsonLineDepth scan_json_line_depth(const std::string& line, int initial_depth, bool capture_key_depth) {
    int depth = initial_depth;
    int key_depth = -1;
    bool in_string = false;
    bool escaped = false;
    for (char c : line) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            if (capture_key_depth && key_depth < 0) {
                key_depth = depth;
            }
            in_string = true;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            depth = std::max(0, depth - 1);
        }
    }
    return {key_depth, depth};
}

struct JsonKeySpan {
    std::string key;
    std::size_t begin;
    std::size_t end;
    int depth;
};

static std::vector<JsonKeySpan> json_key_spans(const std::vector<std::string>& lines) {
    std::vector<JsonKeySpan> spans;
    std::vector<std::size_t> open;
    int depth = 0;
    for (std::size_t line = 0; line < lines.size(); ++line) {
        auto key = extract_json_key_line(lines[line]);
        const auto scanned = scan_json_line_depth(lines[line], depth, !key.empty());
        depth = scanned.end_depth;
        if (!key.empty()) {
            spans.push_back({std::move(key), line, line + 1, scanned.key_depth});
            if (depth > scanned.key_depth) open.push_back(spans.size() - 1);
        }
        while (!open.empty() && spans[open.back()].depth >= depth) {
            spans[open.back()].end = line + 1;
            open.pop_back();
        }
    }
    for (const auto index : open) spans[index].end = lines.size();
    return spans;
}

static void populate_markdown_indexed_text(const SourceDoc& doc,
                                           std::vector<SectionRow>& rows) {
    std::size_t remaining = kMaxDocumentIndexedTextBytes;
    for (auto& row : rows) {
        const auto begin = static_cast<std::size_t>(
            row.row_type == "section" ? row.line : row.start_line - 1);
        const auto end = std::min(doc.lines.size(), static_cast<std::size_t>(row.end_line));
        std::string body;
        for (auto line = begin; line < end; ++line) {
            const auto text = redact_sensitive_text(compact_whitespace(doc.lines[line]));
            if (text.empty()) continue;
            if (!body.empty()) body += ' ';
            body += text;
        }
        // Sanitize the complete range before clipping, including secrets across the cutoff.
        body = redact_sensitive_text(body);
        auto kept = std::min({body.size(), remaining, kMaxSectionIndexedTextBytes});
        if (kept < body.size()) {
            // A whitespace boundary preserves UTF-8 and never publishes a partial token.
            while (kept > 0 && body[kept] != ' ') --kept;
        }
        row.indexed_text = body.substr(0, kept);
        row.indexed_text_omitted_bytes = static_cast<int>(std::min(
            body.size() - kept, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        remaining -= kept;
    }
}

std::vector<SectionRow> build_sections(const Profile& profile,
                                       const SourceDoc& doc,
                                       StructuredDataCounts* structured_counts) {
    std::vector<SectionRow> rows;
    if (doc.kind == "markdown") {
        const std::vector<int> heading_levels = unfenced_markdown_heading_levels(doc.lines);
        for (std::size_t i = 0; i < doc.lines.size(); ++i) {
            const int level = heading_levels[i];
            if (level == 0 || level > profile.markdown_max_heading_level) {
                continue;
            }
            std::string heading = trim(doc.lines[i].substr(static_cast<std::size_t>(level) + 1));
            const std::size_t body_start = i + 1;
            const std::size_t body_end = next_indexed_markdown_heading(profile, heading_levels, body_start);
            const std::size_t section_end = last_non_empty_line_exclusive(doc.lines, i, body_end);
            const std::string preview = preview_from_line_range(doc.lines, body_start, body_end, 420);
            const std::string declared_section_status =
                explicit_section_status(doc.lines, i, section_end);
            const std::string section_status =
                declared_section_status.empty() ? doc.status : declared_section_status;
            rows.push_back(make_section_row(profile,
                                            doc,
                                            heading,
                                            preview,
                                            static_cast<int>(i + 1),
                                            level,
                                            static_cast<int>(i + 1),
                                            static_cast<int>(std::max(section_end, i + 1)),
                                            section_status,
                                            "section",
                                            0));
            append_markdown_chunk_rows(
                profile, doc, rows, heading, level, body_start, body_end, section_status);
        }
    } else if (profile.json_key_sections &&
               (profile.json_key_include_prefixes.empty() ||
                path_matches_any_profile_prefix(doc.path, profile.json_key_include_prefixes))) {
        int indexed_sections = 0;
        for (const auto& span : json_key_spans(doc.lines)) {
            if (structured_counts != nullptr) {
                ++structured_counts->json_key_candidate_sections;
            }
            const bool within_depth = profile.json_key_max_depth == 0 ||
                                      (span.depth >= 0 && span.depth <= profile.json_key_max_depth);
            const bool within_limit = profile.json_key_max_sections_per_file == 0 ||
                                      indexed_sections < profile.json_key_max_sections_per_file;
            if (!within_depth || !within_limit) {
                if (structured_counts != nullptr) {
                    ++structured_counts->json_key_capped_sections;
                }
                continue;
            }
            rows.push_back(make_section_row(profile,
                                            doc,
                                            span.key,
                                            preview_from_line_range(doc.lines, span.begin, span.end, 420),
                                            static_cast<int>(span.begin + 1),
                                            1,
                                            static_cast<int>(span.begin + 1),
                                            static_cast<int>(span.end),
                                            doc.status,
                                            "json-key",
                                            0));
            ++indexed_sections;
            if (structured_counts != nullptr) {
                ++structured_counts->json_key_indexed_sections;
            }
        }
    }
    if (rows.empty()) {
        const std::size_t end = last_non_empty_line_exclusive(doc.lines, 0, doc.lines.size());
        const std::string preview =
            doc.kind == "markdown" ? preview_from_line_range(doc.lines, 0, doc.lines.size(), 420) : doc.summary;
        rows.push_back(make_section_row(profile,
                                        doc,
                                        doc.title,
                                        preview.empty() ? doc.summary : preview,
                                        1,
                                        1,
                                        1,
                                        static_cast<int>(std::max<std::size_t>(end, 1)),
                                        doc.status,
                                        "document",
                                        0));
        if (doc.kind == "markdown") {
            append_markdown_chunk_rows(
                profile, doc, rows, doc.title, 1, 0, doc.lines.size(), doc.status);
        }
    }
    if (doc.kind == "markdown") populate_markdown_indexed_text(doc, rows);
    return rows;
}

static std::string strip_anchor_and_query(std::string target) {
    std::size_t hash = target.find('#');
    if (hash != std::string::npos) {
        target = target.substr(0, hash);
    }
    std::size_t query = target.find('?');
    if (query != std::string::npos) {
        target = target.substr(0, query);
    }
    return target;
}

static LinkRow classify_link(const fs::path& root,
                      const std::string& path,
                      int line,
                      const std::string& kind,
                      const std::string& text,
                      const std::string& target) {
    LinkRow row;
    row.path = path;
    row.line = line;
    row.kind = kind;
    row.text = text;
    row.target = target;
    std::string clean = trim(target);
    if (starts_with(clean, "<") && ends_with(clean, ">")) {
        clean = clean.substr(1, clean.size() - 2);
    }
    std::string lower = to_lower(clean);
    if (starts_with(lower, "http://") || starts_with(lower, "https://") || starts_with(lower, "mailto:") ||
        starts_with(lower, "app://")) {
        row.status = "external";
        return row;
    }
    std::string target_path = strip_anchor_and_query(clean);
    if (target_path.empty()) {
        row.status = "ok";
        row.resolved_path = path;
        return row;
    }
    if (starts_with(target_path, "/")) {
        target_path.erase(target_path.begin());
    }
    fs::path resolved_rel;
    if (starts_with(target_path, "docs/") || starts_with(target_path, "README.md") ||
        starts_with(target_path, "tests/") || starts_with(target_path, "include/") ||
        starts_with(target_path, "src/") || starts_with(target_path, "deploy/")) {
        resolved_rel = fs::path(target_path).lexically_normal();
    } else {
        resolved_rel = (fs::path(path).parent_path() / target_path).lexically_normal();
    }
    row.resolved_path = resolved_rel.generic_string();
    row.status = fs::exists(root / resolved_rel) ? "ok" : "broken";
    return row;
}

static bool is_doc_path_char(char c) {
    unsigned char raw = static_cast<unsigned char>(c);
    return std::isalnum(raw) != 0 || c == '.' || c == '_' || c == '-' || c == '/';
}

static std::vector<std::pair<std::string, std::string>> extract_markdown_links(const std::string& line) {
    std::vector<std::pair<std::string, std::string>> links;
    std::size_t pos = 0;
    while (pos < line.size()) {
        std::size_t open_text = line.find('[', pos);
        if (open_text == std::string::npos) {
            break;
        }
        std::size_t close_text = line.find(']', open_text + 1);
        if (close_text == std::string::npos || close_text + 1 >= line.size() || line[close_text + 1] != '(') {
            pos = open_text + 1;
            continue;
        }
        std::size_t open_target = close_text + 2;
        std::size_t close_target = line.find(')', open_target);
        if (close_target == std::string::npos) {
            break;
        }
        links.push_back({line.substr(open_text + 1, close_text - open_text - 1),
                         line.substr(open_target, close_target - open_target)});
        pos = close_target + 1;
    }
    return links;
}

static std::vector<std::string> extract_doc_path_references(const std::string& line) {
    std::vector<std::string> refs;
    std::size_t pos = 0;
    while (pos < line.size()) {
        std::size_t doc_pos = line.find("docs/machine/", pos);
        std::size_t readme_pos = line.find("README.md", pos);
        if (doc_pos == std::string::npos && readme_pos == std::string::npos) {
            break;
        }
        if (readme_pos != std::string::npos && (doc_pos == std::string::npos || readme_pos < doc_pos)) {
            refs.push_back("README.md");
            pos = readme_pos + 9;
            continue;
        }
        std::size_t end = doc_pos;
        while (end < line.size() && is_doc_path_char(line[end])) {
            ++end;
        }
        std::string candidate = line.substr(doc_pos, end - doc_pos);
        std::size_t json_pos = candidate.find(".json");
        if (json_pos != std::string::npos) {
            refs.push_back(candidate.substr(0, json_pos + 5));
        }
        pos = end;
    }
    return refs;
}

static bool json_path_references_belong_to_external_namespace(const SourceDoc& doc) {
    Json document;
    if (!try_parse_json_text(doc.content, document) || !document.is_object()) {
        return false;
    }
    std::string schema = json_string_value(document, "schemaVersion");
    if (schema.empty()) {
        schema = json_string_value(document, "schema_version");
    }
    return schema == "shin.profile.v1" || schema == "shin.profile-routing.v1";
}

std::vector<LinkRow> build_links(const Profile& profile, const fs::path& root, const SourceDoc& doc) {
    std::vector<LinkRow> rows;
    bool in_fence = false;
    const bool external_path_namespace =
        doc.kind == "json" && json_path_references_belong_to_external_namespace(doc);
    for (std::size_t i = 0; i < doc.lines.size(); ++i) {
        const std::string& line = doc.lines[i];
        if (starts_with(trim(line), "```")) {
            in_fence = !in_fence;
            continue;
        }
        if (in_fence) {
            continue;
        }
        if (doc.kind == "markdown") {
            for (const auto& [text, target] : extract_markdown_links(line)) {
                rows.push_back(classify_link(root, doc.path, static_cast<int>(i + 1), "markdown-link",
                                             text, target));
            }
        }
        if (doc.kind == "json" && profile.json_path_references &&
            !external_path_namespace) {
            std::set<std::string> seen_on_line;
            for (const std::string& target : extract_doc_path_references(line)) {
                if (!seen_on_line.insert(target).second) {
                    continue;
                }
                rows.push_back(classify_link(root, doc.path, static_cast<int>(i + 1), "path-reference",
                                             target, target));
            }
        }
    }
    return rows;
}

static std::string compact_json_context_value(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean() || value.is_number()) {
        return value.dump();
    }
    if (!value.is_array()) {
        return "";
    }
    std::string result;
    int included = 0;
    for (const Json& item : value) {
        const std::string text = compact_json_context_value(item);
        if (text.empty()) {
            continue;
        }
        if (!result.empty()) {
            result += ", ";
        }
        result += text;
        if (++included >= 8) {
            break;
        }
    }
    return result;
}

static std::string first_present_json_context_value(const Json& object,
                                             const std::vector<std::string>& keys) {
    for (const std::string& key : keys) {
        const Json* value = find_json_field(object, key);
        if (value == nullptr) {
            continue;
        }
        const std::string text = compact_json_context_value(*value);
        if (!text.empty()) {
            return truncate_text(redact_sensitive_text(compact_whitespace(text)), 420);
        }
    }
    return "";
}

static std::string jsonl_context_summary(const Json& object) {
    std::string summary;
    for (const char* key : {"tag", "kind", "queryId", "pageId", "sectionId", "filePath", "path",
                            "rootId", "rootPath", "fromId", "toId", "resourceDisplayName",
                            "payloadPaths", "semanticTags", "deps", "groups", "tables", "snippet",
                            "moustacheExpressions", "architecture", "topLevelKind", "symbolCount",
                            "pageCount", "queryCount", "resourceDisplayNames", "nearestScreenId",
                            "nearestViewId"}) {
        const Json* value = find_json_field(object, key);
        if (value == nullptr) {
            continue;
        }
        const std::string text = compact_json_context_value(*value);
        if (text.empty()) {
            continue;
        }
        if (!summary.empty()) {
            summary += "; ";
        }
        summary += std::string(key) + "=" + text;
        if (summary.size() >= 420) {
            break;
        }
    }
    return truncate_text(redact_sensitive_text(compact_whitespace(summary)), 420);
}

DocumentFactBuildResult build_fact_rows(const Profile& profile, const SourceDoc& doc) {
    DocumentFactBuildResult result;
    if (doc.kind != "jsonl") {
        return result;
    }
    std::set<std::string> fact_ids;
    int considered_rows = 0;
    for (std::size_t i = 0; i < doc.lines.size(); ++i) {
        const std::string line = trim(doc.lines[i]);
        if (line.empty() || line.front() != '{') {
            continue;
        }
        ++result.candidate_rows;
        if (profile.jsonl_max_rows_per_file > 0 && considered_rows >= profile.jsonl_max_rows_per_file) {
            ++result.capped_rows;
            continue;
        }
        ++considered_rows;
        const Json object = parse_json_line(line, doc.path + ":" + std::to_string(i + 1));
        std::string record_type = first_present_json_context_value(
            object, {"type", "record_type", "recordType", "kind", "tag"});
        std::string source_id = first_present_json_context_value(
            object, {"id", "key", "name", "nodeId", "node_id", "queryId", "query_id",
                     "pageId", "page_id", "sectionId", "section_id", "filePath", "file_path",
                     "path", "resourceDisplayName", "resource_name", "rootId", "root_id"});
        if (source_id.empty()) {
            const std::string from = first_present_json_context_value(object, {"fromId", "from_id"});
            const std::string to = first_present_json_context_value(object, {"toId", "to_id"});
            if (!from.empty() && !to.empty()) {
                source_id = from + "-to-" + to;
            }
        }
        if (source_id.empty()) {
            source_id = "line-" + std::to_string(i + 1);
        }
        if (record_type.empty()) {
            record_type = slugify(fs::path(doc.path).stem().string());
            if (record_type.empty()) {
                record_type = doc.doc_type;
            }
        }
        std::string source_slug = slugify(source_id);
        if (source_slug.empty()) {
            source_slug = "line-" + std::to_string(i + 1);
        }
        const std::string base_id = doc.id + "-" + source_slug;
        std::string id = base_id;
        if (!fact_ids.insert(id).second) {
            id = base_id + "-line-" + std::to_string(i + 1);
            int disambiguator = 2;
            while (!fact_ids.insert(id).second) {
                id = base_id + "-line-" + std::to_string(i + 1) + "-" +
                     std::to_string(disambiguator++);
            }
        }
        std::string title = first_present_json_context_value(
            object, {"title", "name", "id", "queryId", "pageId", "sectionId", "resourceDisplayName",
                     "filePath", "path", "rootId", "gateKey", "topic", "contract", "runId", "tag"});
        std::string summary = first_present_json_context_value(
            object, {"summary", "decision", "text", "expectation", "currentAuthority", "description"});
        if (summary.empty()) {
            summary = jsonl_context_summary(object);
        }
        if (title.empty()) {
            title = truncate_text(summary.empty() ? source_id : summary, 120);
        }
        const std::string status = first_present_json_context_value(object, {"status", "state"});
        const std::string authority_level = first_present_json_context_value(
            object, {"authority_level", "authorityLevel", "authority", "confidence"});
        const std::string explicit_source = first_present_json_context_value(object, {"source"});
        result.rows.push_back({id,
                               record_type,
                               doc.path,
                               status,
                               first_present_json_context_value(object, {"area", "owner"}),
                               redact_sensitive_text(title),
                               redact_sensitive_text(summary),
                               explicit_source.empty() ? "jsonl:" + doc.path + "#" + source_id : explicit_source,
                               authority_level,
                               doc.topics,
                               static_cast<int>(i + 1),
                               "", "", static_cast<int>(i + 1), static_cast<int>(i + 1), {}});
        if (result.rows.back().status.empty()) {
            result.rows.back().status = doc.status;
        }
        if (result.rows.back().area.empty()) {
            result.rows.back().area = doc.area;
        }
        if (result.rows.back().authority_level.empty()) {
            result.rows.back().authority_level = fact_authority_level_or_default(
                doc, result.rows.back().record_type, result.rows.back().status,
                "");
        }
        const EvidenceProvenance provenance = infer_record_evidence_provenance(
            result.rows.back().path, doc.doc_type, result.rows.back().record_type,
            result.rows.back().status, result.rows.back().authority_level, false);
        result.rows.back().evidence_kind = provenance.evidence_kind;
        result.rows.back().verification_level = provenance.verification_level;
        ++result.indexed_rows;
    }
    return result;
}

namespace {

template <typename Row>
FactRow typed_query_fact(const Row& row, const std::string& record_type) {
    FactRow fact;
    fact.id = row.id;
    fact.record_type = record_type;
    fact.path = row.path;
    fact.status = row.status;
    fact.area = row.area;
    fact.title = row.title;
    fact.summary = row.summary;
    fact.source = row.path;
    fact.authority_level = row.authority_level;
    fact.topics = row.topics;
    fact.line = fact.start_line = fact.end_line = row.line;
    fact.evidence_kind = row.evidence_kind;
    fact.verification_level = row.verification_level;
    return fact;
}

}  // namespace

FactRow query_fact_row(const DecisionRow& row) {
    return typed_query_fact(row, "decision");
}

FactRow query_fact_row(const TypedReferenceRow& row) {
    return typed_query_fact(row, row.type);
}

static bool is_identifier_char(char c) {
    unsigned char raw = static_cast<unsigned char>(c);
    return std::isalnum(raw) != 0 || c == '_' || c == '$' || c == '~';
}

static bool is_word_boundary(const std::string& text, std::size_t pos) {
    return pos >= text.size() || !is_identifier_char(text[pos]);
}

static std::size_t find_word(const std::string& text, const std::string& word) {
    std::size_t pos = text.find(word);
    while (pos != std::string::npos) {
        const bool left_ok = pos == 0 || !is_identifier_char(text[pos - 1]);
        const bool right_ok = is_word_boundary(text, pos + word.size());
        if (left_ok && right_ok) {
            return pos;
        }
        pos = text.find(word, pos + word.size());
    }
    return std::string::npos;
}

static std::string strip_source_line_comment(const std::string& line) {
    bool in_string = false;
    char quote = '\0';
    bool escaped = false;
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_string && c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            if (!in_string) {
                in_string = true;
                quote = c;
            } else if (quote == c) {
                in_string = false;
            }
            continue;
        }
        if (!in_string && c == '/' && line[i + 1] == '/') {
            return line.substr(0, i);
        }
    }
    return line;
}

static std::string extract_identifier_after_word(const std::string& line, const std::string& word) {
    std::size_t pos = find_word(line, word);
    if (pos == std::string::npos) {
        return "";
    }
    pos += word.size();
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])) != 0) {
        ++pos;
    }
    std::size_t start = pos;
    while (pos < line.size() && is_identifier_char(line[pos])) {
        ++pos;
    }
    if (pos == start) {
        return "";
    }
    return line.substr(start, pos - start);
}

struct IdentifierHit {
    std::string name;
    std::size_t start = std::string::npos;
};

static IdentifierHit extract_identifier_before_paren(const std::string& line) {
    const std::size_t paren = line.find('(');
    if (paren == std::string::npos || paren == 0) {
        return {};
    }
    std::size_t end = paren;
    while (end > 0 && std::isspace(static_cast<unsigned char>(line[end - 1])) != 0) {
        --end;
    }
    std::size_t start = end;
    while (start > 0 && is_identifier_char(line[start - 1])) {
        --start;
    }
    if (start == end) {
        return {};
    }
    return {line.substr(start, end - start), start};
}

static bool is_control_or_call_keyword(const std::string& lower_name) {
    static const std::set<std::string> reserved = {
        "if", "for", "while", "switch", "catch", "return", "new", "throw", "assert",
        "sizeof", "static_cast", "reinterpret_cast", "dynamic_cast", "const_cast",
        "super", "this", "else", "do", "case", "delete"
    };
    return reserved.find(lower_name) != reserved.end();
}

static bool looks_like_declaration_line(const std::string& line) {
    const std::string compact = compact_whitespace(line);
    if (compact.empty()) {
        return false;
    }
    const std::string lower = to_lower(compact);
    if (starts_with(lower, "return ") || starts_with(lower, "throw ") || starts_with(lower, "if ") ||
        starts_with(lower, "if(") || starts_with(lower, "for ") || starts_with(lower, "for(") ||
        starts_with(lower, "while ") || starts_with(lower, "while(") || starts_with(lower, "switch ") ||
        starts_with(lower, "switch(") || starts_with(lower, "catch ") || starts_with(lower, "catch(")) {
        return false;
    }
    return compact.find('(') != std::string::npos && compact.find(')') != std::string::npos;
}

static bool has_cpp_declaration_prefix(const std::string& line, const IdentifierHit& hit) {
    if (hit.start == std::string::npos || hit.start == 0) {
        return false;
    }
    const std::string raw_prefix = line.substr(0, hit.start);
    const std::string prefix = trim(raw_prefix);
    if (prefix.empty()) {
        return false;
    }
    return raw_prefix.find(' ') != std::string::npos || raw_prefix.find('\t') != std::string::npos ||
           prefix.find('*') != std::string::npos || prefix.find('&') != std::string::npos ||
           prefix.find('>') != std::string::npos;
}

static bool is_capitalized_identifier(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    return std::isupper(static_cast<unsigned char>(name.front())) != 0;
}

static bool is_react_hook_name(const std::string& name) {
    return name.size() > 3 && starts_with(name, "use") &&
           std::isupper(static_cast<unsigned char>(name[3])) != 0;
}

static bool is_probably_test_path(const std::string& path) {
    const std::string lower = to_lower(path);
    return lower.find(".test.") != std::string::npos || lower.find(".spec.") != std::string::npos ||
           lower.find("/test/") != std::string::npos || lower.find("/tests/") != std::string::npos ||
           lower.find("__tests__") != std::string::npos;
}

static std::string extract_first_quoted_text(const std::string& line, std::size_t start) {
    bool escaped = false;
    char quote = '\0';
    std::size_t body_start = std::string::npos;
    for (std::size_t i = start; i < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quote != '\0') {
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == quote) {
                return line.substr(body_start, i - body_start);
            }
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            quote = c;
            body_start = i + 1;
        }
    }
    return "";
}

static std::string extract_module_specifier(const std::string& line) {
    const std::size_t from_pos = line.find(" from ");
    if (from_pos != std::string::npos) {
        return extract_first_quoted_text(line, from_pos + 6);
    }
    if (starts_with(line, "import ")) {
        return extract_first_quoted_text(line, 6);
    }
    return "";
}

static bool is_localish_module_specifier(const std::string& module) {
    return starts_with(module, ".") || starts_with(module, "@/") || starts_with(module, "src/");
}

static std::string extract_first_string_argument(const std::string& line) {
    const std::size_t paren = line.find('(');
    if (paren == std::string::npos) {
        return "";
    }
    return extract_first_quoted_text(line, paren + 1);
}

struct TsVariableDecl {
    bool found = false;
    bool exported = false;
    std::string name;
};

static TsVariableDecl extract_ts_variable_decl(const std::string& line) {
    std::string rest = line;
    TsVariableDecl decl;
    if (starts_with(rest, "export ")) {
        decl.exported = true;
        rest = trim(rest.substr(7));
    }
    if (starts_with(rest, "declare ")) {
        rest = trim(rest.substr(8));
    }
    std::size_t keyword_size = 0;
    if (starts_with(rest, "const ")) {
        keyword_size = 6;
    } else if (starts_with(rest, "let ") || starts_with(rest, "var ")) {
        keyword_size = 4;
    } else {
        return decl;
    }
    std::size_t pos = keyword_size;
    while (pos < rest.size() && std::isspace(static_cast<unsigned char>(rest[pos])) != 0) {
        ++pos;
    }
    const std::size_t start = pos;
    while (pos < rest.size() && is_identifier_char(rest[pos])) {
        ++pos;
    }
    if (pos > start) {
        decl.found = true;
        decl.name = rest.substr(start, pos - start);
    }
    return decl;
}

static bool has_ts_declaration_prefix(const std::string& line, const std::string& kind) {
    std::string rest = trim(line);
    if (starts_with(rest, "export ")) {
        rest = trim(rest.substr(7));
    }
    if (starts_with(rest, "default ")) {
        rest = trim(rest.substr(8));
    }
    if (starts_with(rest, "declare ")) {
        rest = trim(rest.substr(8));
    }
    if (kind == "class" && starts_with(rest, "abstract ")) {
        rest = trim(rest.substr(9));
    }
    if (kind == "function" && starts_with(rest, "async ")) {
        rest = trim(rest.substr(6));
    }
    return starts_with(rest, kind) && is_word_boundary(rest, kind.size());
}

static bool is_arrow_function_assignment(const std::string& line) {
    const std::size_t arrow = line.find("=>");
    const std::size_t assignment = line.find('=');
    return arrow != std::string::npos && assignment != std::string::npos && assignment < arrow;
}

static std::string classify_ts_callable_kind(const std::string& name) {
    if (is_react_hook_name(name)) {
        return "hook";
    }
    if (is_capitalized_identifier(name)) {
        return "component";
    }
    return "arrow-function";
}

static std::string extract_default_export_name(const std::string& line) {
    if (!starts_with(line, "export default ")) {
        return "";
    }
    const std::string rest = trim(line.substr(15));
    for (const char* kind : {"function", "class"}) {
        const std::string name = extract_identifier_after_word(rest, kind);
        if (!name.empty()) {
            return name;
        }
    }
    const std::string lower = to_lower(rest);
    if (starts_with(lower, "function") || starts_with(lower, "class") ||
        starts_with(lower, "async function")) {
        return "default";
    }
    std::size_t pos = 0;
    while (pos < rest.size() && std::isspace(static_cast<unsigned char>(rest[pos])) != 0) {
        ++pos;
    }
    const std::size_t start = pos;
    while (pos < rest.size() && is_identifier_char(rest[pos])) {
        ++pos;
    }
    if (pos > start) {
        return rest.substr(start, pos - start);
    }
    return "default";
}

static std::string classify_test_invocation(const std::string& line) {
    const std::string compact = compact_whitespace(line);
    if (starts_with(compact, "describe(") || starts_with(compact, "describe.") ||
        starts_with(compact, "test.describe(") || starts_with(compact, "test.describe.")) {
        return "test-suite";
    }
    if (starts_with(compact, "it(") || starts_with(compact, "it.") ||
        starts_with(compact, "test(") || starts_with(compact, "test.")) {
        return "test-case";
    }
    return "";
}

void add_source_fact_row(std::vector<FactRow>& rows,
                         const Profile& profile,
                         const std::string& path,
                         int line,
                         const std::string& language,
                         const std::string& kind,
                         const std::string& name,
                         const std::string& declaration,
                         const std::string& status,
                         const std::string& authority_level,
                         int end_line) {
    if (name.empty()) {
        return;
    }
    const std::string record_type = language + "-" + kind;
    const std::string title = language + " " + kind + " " + name;
    const std::string summary = truncate_text(redact_sensitive_text(compact_whitespace(declaration)), 420);
    FactRow row{"source-" + slugify(path + "-" + std::to_string(line) + "-" + record_type + "-" + name),
                record_type,
                path,
                status,
                detect_area(path),
                title,
                summary,
                "source-fact:" + language,
                authority_level,
                detect_topics(profile, path, title, summary),
                line,
                "", "", line, end_line >= line ? end_line : line, {}};
    const EvidenceProvenance provenance = infer_record_evidence_provenance(
        row.path, "", row.record_type, row.status, row.authority_level, true);
    row.evidence_kind = provenance.evidence_kind;
    row.verification_level = provenance.verification_level;
    if (language != "retool") row.symbol_name = name;
    rows.push_back(std::move(row));
}

static bool is_kotlin_test_annotation(const std::string& line) {
    return contains_any(line, {"@Test", "@ParameterizedTest", "@RepeatedTest", "@TestFactory"});
}

static std::string extract_kotlin_function_name(const std::string& line) {
    const std::size_t fun = find_word(line, "fun");
    const std::size_t paren = line.find('(', fun == std::string::npos ? 0 : fun + 3);
    if (fun == std::string::npos || paren == std::string::npos) {
        return "";
    }
    const std::size_t closing_tick = line.rfind('`', paren);
    if (closing_tick != std::string::npos && closing_tick > fun) {
        const std::size_t opening_tick = line.rfind('`', closing_tick - 1);
        if (opening_tick != std::string::npos && opening_tick > fun) {
            return line.substr(opening_tick + 1, closing_tick - opening_tick - 1);
        }
    }
    const std::string name = extract_identifier_before_paren(line).name;
    return name == "fun" ? "" : name;
}

static void scan_kotlin_source_facts(const Profile& profile,
                                     const std::string& path,
                                     const std::vector<std::string>& lines,
                                     std::vector<FactRow>& rows) {
    const bool test_path = is_probably_test_path(path);
    bool pending_test_annotation = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string line = trim(strip_source_line_comment(lines[i]));
        if (line.empty() || starts_with(line, "*") || starts_with(line, "/*")) {
            continue;
        }
        if (starts_with(line, "@")) {
            pending_test_annotation = pending_test_annotation || is_kotlin_test_annotation(line);
            const bool has_inline_declaration = find_word(line, "fun") != std::string::npos ||
                                                find_word(line, "class") != std::string::npos ||
                                                find_word(line, "interface") != std::string::npos ||
                                                find_word(line, "object") != std::string::npos ||
                                                find_word(line, "typealias") != std::string::npos;
            if (!has_inline_declaration) {
                continue;
            }
        }
        if (starts_with(line, "package ")) {
            std::string package_name = trim(line.substr(8));
            const std::size_t delimiter = package_name.find_first_of("; \t");
            if (delimiter != std::string::npos) {
                package_name = package_name.substr(0, delimiter);
            }
            add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "kotlin", "package",
                                package_name, line);
            pending_test_annotation = false;
            continue;
        }
        const std::string typealias_name = extract_identifier_after_word(line, "typealias");
        if (!typealias_name.empty()) {
            add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "kotlin", "typealias",
                                typealias_name, line);
            pending_test_annotation = false;
            continue;
        }

        bool type_declaration = false;
        const bool enum_class = find_word(line, "enum") != std::string::npos &&
                                find_word(line, "class") != std::string::npos;
        const bool annotation_class = find_word(line, "annotation") != std::string::npos &&
                                      find_word(line, "class") != std::string::npos;
        const bool value_class = find_word(line, "value") != std::string::npos &&
                                 find_word(line, "class") != std::string::npos;
        if (enum_class || annotation_class || value_class) {
            const std::string name = extract_identifier_after_word(line, "class");
            const char* kind = enum_class ? "enum" : (annotation_class ? "annotation" : "value-class");
            add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "kotlin", kind, name, line);
            type_declaration = !name.empty();
        } else {
            const std::string class_name = extract_identifier_after_word(line, "class");
            if (!class_name.empty()) {
                add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "kotlin", "class",
                                    class_name, line);
                type_declaration = true;
            }
        }
        for (const char* kind : {"interface", "object"}) {
            const std::string name = extract_identifier_after_word(line, kind);
            if (!name.empty()) {
                add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "kotlin", kind, name, line);
                type_declaration = true;
            }
        }
        if (type_declaration) {
            pending_test_annotation = false;
            continue;
        }

        const std::string function_name = extract_kotlin_function_name(line);
        if (!function_name.empty()) {
            add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "kotlin", "function",
                                function_name, line);
            if (test_path && pending_test_annotation) {
                add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "kotlin", "test-case",
                                    function_name, line);
            }
            pending_test_annotation = false;
            continue;
        }
        pending_test_annotation = false;
    }
}

static IdentifierHit extract_csharp_callable_name(const std::string& line) {
    const std::size_t paren = line.find('(');
    if (paren == std::string::npos || paren == 0) {
        return {};
    }
    std::size_t end = paren;
    while (end > 0 && is_ascii_space(static_cast<unsigned char>(line[end - 1]))) {
        --end;
    }
    if (end > 0 && line[end - 1] == '>') {
        int depth = 0;
        std::size_t cursor = end;
        while (cursor > 0) {
            --cursor;
            if (line[cursor] == '>') {
                ++depth;
            } else if (line[cursor] == '<') {
                --depth;
                if (depth == 0) {
                    end = cursor;
                    break;
                }
            }
        }
    }
    while (end > 0 && is_ascii_space(static_cast<unsigned char>(line[end - 1]))) {
        --end;
    }
    std::size_t start = end;
    while (start > 0 && is_identifier_char(line[start - 1])) {
        --start;
    }
    return start == end ? IdentifierHit{} : IdentifierHit{line.substr(start, end - start), start};
}

static IdentifierHit extract_csharp_property_name(const std::string& line) {
    std::size_t end = line.find('{');
    const std::size_t arrow = line.find("=>");
    if (end == std::string::npos || (arrow != std::string::npos && arrow < end)) {
        end = arrow;
    }
    if (end == std::string::npos || line.find('(') < end) {
        return {};
    }
    const std::string lower = to_lower(line.substr(end));
    if (arrow == std::string::npos && !contains_any(lower, {"get;", "get ", "set;", "set ", "init;", "init "})) {
        return {};
    }
    while (end > 0 && is_ascii_space(static_cast<unsigned char>(line[end - 1]))) {
        --end;
    }
    std::size_t start = end;
    while (start > 0 && is_identifier_char(line[start - 1])) {
        --start;
    }
    return start == end ? IdentifierHit{} : IdentifierHit{line.substr(start, end - start), start};
}

static bool is_csharp_test_attribute(const std::string& line) {
    return contains_any(line, {"[Fact", "[Theory", "[Test", "[TestCase"});
}

static bool is_csharp_non_declaration_prefix(const std::string& prefix) {
    const std::string lower = to_lower(trim(prefix));
    auto begins_with_word = [&](std::string_view word) {
        return lower == word || (starts_with(lower, word) && lower.size() > word.size() &&
                                 is_ascii_space(static_cast<unsigned char>(lower[word.size()])));
    };
    const bool expression_continuation = !lower.empty() &&
                                         (lower.front() == '}' || lower.front() == ')' || lower.front() == ']' ||
                                          lower.front() == ',' || lower.front() == '.');
    return lower.empty() || expression_continuation || begins_with_word("await") || begins_with_word("return") ||
           begins_with_word("yield") || begins_with_word("throw") || begins_with_word("new") ||
           begins_with_word("using") || begins_with_word("lock") || starts_with(lower, "??") ||
           starts_with(lower, "?:") || lower.find("??") != std::string::npos;
}

static void scan_csharp_source_facts(const Profile& profile,
                              const std::string& path,
                              const std::vector<std::string>& lines,
                              std::vector<FactRow>& rows) {
    bool pending_test_attribute = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string line = trim(strip_source_line_comment(lines[i]));
        if (line.empty() || starts_with(line, "*") || starts_with(line, "/*") || starts_with(line, "#")) {
            continue;
        }
        if (starts_with(line, "[")) {
            pending_test_attribute = pending_test_attribute || is_csharp_test_attribute(line);
            continue;
        }
        if (starts_with(line, "namespace ")) {
            std::string name = trim(line.substr(10));
            const std::size_t delimiter = name.find_first_of(";{");
            if (delimiter != std::string::npos) {
                name = trim(name.substr(0, delimiter));
            }
            add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "csharp", "namespace", name, line);
            pending_test_attribute = false;
            continue;
        }

        bool type_declaration = false;
        const std::size_t record_position = find_word(line, "record");
        for (const char* kind : {"class", "interface", "struct", "enum"}) {
            const std::size_t kind_position = find_word(line, kind);
            if (record_position != std::string::npos && kind_position > record_position &&
                (std::string_view(kind) == "class" || std::string_view(kind) == "struct")) {
                continue;
            }
            const std::string name = extract_identifier_after_word(line, kind);
            if (!name.empty()) {
                add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "csharp", kind, name, line);
                type_declaration = true;
            }
        }
        std::string record_name = extract_identifier_after_word(line, "record");
        if (record_name == "class" || record_name == "struct") {
            record_name = extract_identifier_after_word(line, record_name);
        }
        if (!record_name.empty()) {
            add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "csharp", "record",
                                record_name, line);
            type_declaration = true;
        }
        const bool delegate_declaration = find_word(line, "delegate") != std::string::npos;
        if (delegate_declaration) {
            const IdentifierHit delegate = extract_csharp_callable_name(line);
            add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "csharp", "delegate",
                                delegate.name, line);
        }
        if (type_declaration || delegate_declaration) {
            pending_test_attribute = false;
            continue;
        }

        const std::size_t paren = line.find('(');
        const std::size_t assignment = line.find('=');
        if (paren != std::string::npos && (assignment == std::string::npos || assignment > paren)) {
            const IdentifierHit callable = extract_csharp_callable_name(line);
            const std::string lower_name = to_lower(callable.name);
            const bool member_access = callable.start != std::string::npos && callable.start > 0 &&
                                       line[callable.start - 1] == '.';
            const std::string prefix = callable.start == std::string::npos ? "" : line.substr(0, callable.start);
            if (!callable.name.empty() && !member_access && !is_control_or_call_keyword(lower_name) &&
                !is_csharp_non_declaration_prefix(prefix)) {
                add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "csharp", "method",
                                    callable.name, line);
                if (pending_test_attribute) {
                    add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "csharp", "test-case",
                                        callable.name, line);
                }
                pending_test_attribute = false;
                continue;
            }
        }

        const IdentifierHit property = extract_csharp_property_name(line);
        if (!property.name.empty() && !is_csharp_non_declaration_prefix(line.substr(0, property.start))) {
            add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "csharp", "property",
                                property.name, line);
        }
        pending_test_attribute = false;
    }
}

static void scan_typescript_source_facts(const Profile& profile,
                                  const std::string& path,
                                  const std::vector<std::string>& lines,
                                  std::vector<FactRow>& rows) {
    const bool test_path = is_probably_test_path(path);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string line = trim(strip_source_line_comment(lines[i]));
        if (line.empty() || starts_with(line, "*") || starts_with(line, "/*")) {
            continue;
        }
        if (starts_with(line, "import ")) {
            const std::string module = extract_module_specifier(line);
            if (!module.empty() && is_localish_module_specifier(module)) {
                add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "ts", "import",
                                    module, line);
            }
        }
        if (starts_with(line, "export ") && line.find(" from ") != std::string::npos) {
            const std::string module = extract_module_specifier(line);
            add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "ts", "re-export",
                                module.empty() ? "export-list" : module, line);
        }
        const std::string default_export = extract_default_export_name(line);
        if (!default_export.empty()) {
            add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "ts", "default-export",
                                default_export, line);
        }
        for (const char* kind : {"class", "interface", "type", "enum", "function"}) {
            const std::string name = extract_identifier_after_word(line, kind);
            const std::string kind_text = kind;
            const bool explicit_declaration = has_ts_declaration_prefix(line, kind_text);
            if (!name.empty() && explicit_declaration &&
                (starts_with(line, "export ") || kind_text == "class" || kind_text == "interface" ||
                 kind_text == "function")) {
                add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "ts", kind, name, line);
                if (starts_with(line, "export ")) {
                    add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "ts", "export",
                                        name, line);
                }
                if (kind_text == "function") {
                    if (is_react_hook_name(name)) {
                        add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "ts", "hook",
                                            name, line);
                    } else if (is_capitalized_identifier(name)) {
                        add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "ts", "component",
                                            name, line);
                    }
                }
            }
        }
        const TsVariableDecl var_decl = extract_ts_variable_decl(line);
        if (var_decl.found) {
            if (var_decl.exported) {
                add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "ts", "export",
                                    var_decl.name, line);
            }
            if (is_arrow_function_assignment(line) &&
                (var_decl.exported || is_react_hook_name(var_decl.name) || is_capitalized_identifier(var_decl.name))) {
                add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "ts",
                                    classify_ts_callable_kind(var_decl.name), var_decl.name, line);
            }
        }
        if (test_path) {
            const std::string test_kind = classify_test_invocation(line);
            if (!test_kind.empty()) {
                std::string title = extract_first_string_argument(line);
                if (title.empty()) {
                    title = test_kind;
                }
                add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "ts", test_kind,
                                    title, line);
            }
        }
    }
}

static void scan_cpp_source_facts(const Profile& profile,
                           const std::string& path,
                           const std::vector<std::string>& lines,
                           std::vector<FactRow>& rows) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string line = trim(strip_source_line_comment(lines[i]));
        if (line.empty() || starts_with(line, "*") || starts_with(line, "/*") || starts_with(line, "#")) {
            continue;
        }
        for (const char* kind : {"class", "struct", "enum"}) {
            const std::string name = extract_identifier_after_word(line, kind);
            if (!name.empty()) {
                add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "cpp", kind, name, line);
            }
        }
        if (!looks_like_declaration_line(line)) {
            continue;
        }
        const std::size_t paren = line.find('(');
        const std::size_t assignment = line.find('=');
        if (assignment != std::string::npos && assignment < paren) {
            continue;
        }
        IdentifierHit hit = extract_identifier_before_paren(line);
        const std::string lower_name = to_lower(hit.name);
        if (hit.name.empty() || is_control_or_call_keyword(lower_name)) {
            continue;
        }
        if (hit.start > 0 && (line[hit.start - 1] == '.' || line[hit.start - 1] == '>')) {
            continue;
        }
        if (!has_cpp_declaration_prefix(line, hit)) {
            continue;
        }
        const std::string ext = extension_of(path);
        const bool is_header = ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx" ||
                               ext == ".ipp" || ext == ".inl";
        const bool declaration_hint = ends_with(line, "{") || (is_header && ends_with(line, ";")) ||
                                      line.find(" noexcept") != std::string::npos;
        if (declaration_hint) {
            add_source_fact_row(rows, profile, path, static_cast<int>(i + 1), "cpp", "function", hit.name, line);
        }
    }
}

static bool is_typescript_javascript_extension(std::string_view extension) {
    static const std::set<std::string_view> extensions = {
        ".cjs", ".cts", ".js", ".jsx", ".mjs", ".mts", ".ts", ".tsx"
    };
    return extensions.contains(extension);
}

static bool is_cpp_extension(std::string_view extension) {
    static const std::set<std::string_view> extensions = {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"
    };
    return extensions.contains(extension);
}

bool should_skip_source_fact_dir(const std::string& name) {
    static const std::set<std::string> skipped_names = {
        ".cache", ".git", ".gradle", ".idea", ".mvn", ".next", ".nuxt", ".output", ".tmp",
        ".venv", ".vs", ".nuget", "artifacts", "bin", "build", "coverage", "coverage-report", "dist",
        "external", "node_modules", "obj", "out", "playwright-report", "storybook-static", "target",
        "test-results", "testresults", "third_party", "vendor", "venv"
    };
    const std::string lower = to_lower(name);
    return skipped_names.find(lower) != skipped_names.end() || starts_with(lower, "build-cmake");
}

SourceFactPathResult enumerate_source_fact_paths(const Profile& profile, const fs::path& root) {
    SourceFactPathResult result;
    if (profile.source_fact_roots.empty() || profile.source_fact_extensions.empty()) {
        return result;
    }
    const fs::path lexical_root = root.lexically_normal();
    auto has_symlink_component = [&](const std::string& relative) {
        fs::path current = lexical_root;
        for (const fs::path& component : fs::path(relative)) {
            current /= component;
            std::error_code ec;
            const fs::file_status status = fs::symlink_status(current, ec);
            if (ec) {
                const auto condition = ec.default_error_condition();
                if (condition == std::errc::no_such_file_or_directory ||
                    condition == std::errc::not_a_directory) {
                    return false;
                }
                throw_source_observation_error("enumerate-source-root", ec, false);
            }
            if (fs::is_symlink(status)) {
                return true;
            }
            if (!fs::exists(status)) {
                return false;
            }
        }
        return false;
    };
    for (const std::string& source_root : profile.source_fact_roots) {
        if (!profile_relative_path_valid(source_root, true) ||
            has_symlink_component(source_root) ||
            shin_internal_control_path(source_root)) {
            continue;
        }
        const fs::path absolute = (lexical_root / source_root).lexically_normal();
        std::error_code root_status_ec;
        const fs::file_status root_status = fs::symlink_status(absolute, root_status_ec);
        if (root_status_ec) {
            const auto condition = root_status_ec.default_error_condition();
            if (condition == std::errc::no_such_file_or_directory ||
                condition == std::errc::not_a_directory) {
                continue;
            }
            throw_source_observation_error("enumerate-source-root", root_status_ec, false);
        }
        if (!fs::exists(root_status) || fs::is_symlink(root_status)) {
            continue;
        }
        if (fs::is_regular_file(root_status)) {
            if (!shin_internal_control_path(source_root) &&
                extension_allowed(profile.source_fact_extensions, source_root)) {
                result.paths.push_back(fs::path(source_root).generic_string());
            }
            continue;
        }
        if (!fs::is_directory(root_status)) {
            continue;
        }
        std::error_code iterator_ec;
        fs::recursive_directory_iterator it(absolute, fs::directory_options::none, iterator_ec), end;
        if (iterator_ec) {
            throw_source_observation_error("enumerate-source-root", iterator_ec);
        }
        while (it != end) {
            std::error_code entry_ec;
            const bool symlink = it->is_symlink(entry_ec);
            if (entry_ec) {
                throw_source_observation_error("enumerate-source-entry", entry_ec);
            }
            if (symlink) {
                it.disable_recursion_pending();
            } else {
                const std::string name = it->path().filename().string();
                const fs::path relative_path = it->path().lexically_relative(lexical_root);
                const std::string rel = relative_path.generic_string();
                if (!profile_relative_path_valid(rel)) {
                    throw_source_observation_io_error("enumerate-source-entry");
                }
                const bool directory = it->is_directory(entry_ec);
                if (entry_ec) {
                    throw_source_observation_error("enumerate-source-entry", entry_ec);
                }
                if (directory) {
                    if (should_skip_source_fact_dir(name) || rel == "docs/kb/generated" ||
                        shin_internal_control_path(rel)) {
                        it.disable_recursion_pending();
                    }
                } else {
                    const bool regular = it->is_regular_file(entry_ec);
                    if (entry_ec) {
                        throw_source_observation_error("enumerate-source-entry", entry_ec);
                    }
                    if (regular && !shin_internal_control_path(rel) &&
                        extension_allowed(profile.source_fact_extensions, rel)) {
                        result.paths.push_back(rel);
                    }
                }
            }
            it.increment(iterator_ec);
            if (iterator_ec) {
                throw_source_observation_error("enumerate-source-entry", iterator_ec);
            }
        }
    }
    std::sort(result.paths.begin(), result.paths.end());
    result.paths.erase(std::unique(result.paths.begin(), result.paths.end()), result.paths.end());
    result.candidate_file_count = static_cast<int>(result.paths.size());
    if (profile.source_fact_max_files > 0 &&
        static_cast<int>(result.paths.size()) > profile.source_fact_max_files) {
        result.capped_file_count = static_cast<int>(result.paths.size()) - profile.source_fact_max_files;
        result.paths.resize(static_cast<std::size_t>(profile.source_fact_max_files));
    }
    return result;
}

static bool load_source_fact_content(const Profile& profile,
                              const fs::path& root,
                              const std::string& path,
                              SourceFactSource& source,
                              std::string& content) {
    const fs::path full_path = root / path;
    source.path = path;
    std::error_code ec;
    const auto file_size = fs::file_size(full_path, ec);
    if (!ec) {
        source.byte_count = file_size;
    }
    if (!ec && profile.source_fact_max_file_bytes > 0 &&
        file_size > static_cast<std::uintmax_t>(profile.source_fact_max_file_bytes)) {
        source.status = "skipped-large";
        return false;
    }
    content = read_file(full_path);
    source.byte_count = content.size();
    source.content_hash = fnv1a64(content);
    source.status = "scanned";
    return true;
}

void inspect_source_fact_content(const Profile& profile, const fs::path& root,
                                 const std::string& path, const std::string& content,
                                 std::vector<FactRow>& rows, SourceRelationInputs& relations) {
    const std::vector<std::string> lines = split_lines(content);
    const std::string ext = extension_of(path);
    std::string language;
    for (const std::string& extractor : profile.source_fact_extractors) {
        if (extractor == "language") {
            if (ext == ".java") {
                language = "java";
            } else if (ext == ".kt" || ext == ".kts") {
                language = "kotlin";
                scan_kotlin_source_facts(profile, path, lines, rows);
            } else if (ext == ".cs") {
                language = "csharp";
                scan_csharp_source_facts(profile, path, lines, rows);
            } else if (is_typescript_javascript_extension(ext)) {
                language = "ts";
                scan_typescript_source_facts(profile, path, lines, rows);
            } else if (is_cpp_extension(ext)) {
                language = "cpp";
                scan_cpp_source_facts(profile, path, lines, rows);
            }
        } else if (extractor == "retool") {
            scan_retool_source_facts(profile, root, path, content, lines, rows, relations);
        }
    }
    if (!language.empty()) {
        if (language == "java" || language == "ts" || language == "cpp") {
            apply_syntax_source_facts(profile, path, content, lines, language, rows, relations);
        }
        enrich_language_source_facts(profile, path, language, lines, rows);
    }
    std::vector<std::size_t> offsets{0};
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') offsets.push_back(i + 1);
    }
    std::map<std::pair<int, int>, std::string> hashes;
    for (FactRow& row : rows) {
        if (row.path != path || row.start_line < 1 || row.end_line < row.start_line ||
            static_cast<std::size_t>(row.end_line) > offsets.size()) continue;
        const std::pair<int, int> range{row.start_line, row.end_line};
        auto found = hashes.find(range);
        if (found == hashes.end()) {
            const std::size_t begin = offsets[static_cast<std::size_t>(row.start_line - 1)];
            const std::size_t end = static_cast<std::size_t>(row.end_line) < offsets.size()
                ? offsets[static_cast<std::size_t>(row.end_line)] : content.size();
            found = hashes.emplace(range, fnv1a64(std::string_view(content).substr(begin, end - begin))).first;
        }
        row.span_hash = found->second;
        if (row.range_origin.empty()) {
            row.range_origin = row.record_type == "retool-payload" ? "payload-file" : "lexical";
        }
    }
}

SourceFactBuildResult build_source_fact_rows(const Profile& profile, const fs::path& root) {
    SourceFactBuildResult result;
    SourceFactPathResult paths = enumerate_source_fact_paths(profile, root);
    result.candidate_file_count = paths.candidate_file_count;
    result.capped_file_count = paths.capped_file_count;
    auto scan_path = [&](const std::string& path, std::vector<FactRow>& rows,
                         SourceFactSource& source, SourceRelationInputs& relations) -> bool {
        std::string content;
        if (!load_source_fact_content(profile, root, path, source, content)) return false;
        const auto secret = source_range_secret(content);
        if (secret.found) {
            // Preserve the byte binding for freshness and redacted follow-up reads,
            // but publish no searchable facts or relations from this file.
            source.quarantine_reason = secret.kind;
            return true;
        }
        inspect_source_fact_content(profile, root, path, content, rows, relations);
        return true;
    };

    std::vector<std::vector<FactRow>> rows_by_file(paths.paths.size());
    std::vector<SourceFactSource> sources_by_file(paths.paths.size());
    std::vector<SourceRelationInputs> relations_by_file(paths.paths.size());
    std::vector<unsigned char> scan_status(paths.paths.size(), 0);
    parallel_for_indices(paths.paths.size(), [&](std::size_t index) {
        scan_status[index] = scan_path(paths.paths[index], rows_by_file[index], sources_by_file[index],
                                      relations_by_file[index]) ? 1 : 2;
    });
    std::size_t total_rows = 0;
    for (const auto& rows : rows_by_file) total_rows += rows.size();
    result.rows.reserve(total_rows);
    result.sources.reserve(sources_by_file.size());
    for (std::size_t index = 0; index < rows_by_file.size(); ++index) {
        if (scan_status[index] == 1) {
            ++result.scanned_file_count;
        } else if (scan_status[index] == 2) {
            ++result.skipped_large_file_count;
        }
        std::vector<FactRow>& rows = rows_by_file[index];
        result.rows.insert(result.rows.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
        result.sources.push_back(std::move(sources_by_file[index]));
    }
    if (std::find(profile.source_fact_extractors.begin(), profile.source_fact_extractors.end(), "retool") !=
        profile.source_fact_extractors.end()) {
        finalize_retool_source_facts(profile, result.rows);
    }
    std::set<std::string> indexed_paths;
    for (const auto& source : result.sources) {
        // Preserve inbound path bindings so readers can report a redacted
        // dependency instead of silently losing its omission. Quarantined files
        // contribute no outgoing relations or declaration records.
        if (source.status == "scanned") indexed_paths.insert(source.path);
    }
    result.relations = resolve_source_relations(relations_by_file, indexed_paths);
    return result;
}

bool source_fact_extractor_known(std::string_view extractor) noexcept {
    return extractor == "language" || extractor == "retool";
}

static bool should_skip_discovery_dir(const std::string& name, const std::string& rel_path) {
    static const std::set<std::string> skipped_names = {
        ".cache", ".git", ".gradle", ".idea", ".mvn", ".next", ".nuxt", ".output", ".tmp",
        ".venv", ".vs", ".nuget", "artifacts", "bin", "build", "coverage-report", "dist", "external",
        "node_modules", "obj", "out", "playwright-report", "storybook-static", "target", "test-results",
        "testresults", "third_party", "vendor", "venv"
    };
    const std::string lower_name = to_lower(name);
    const std::string lower_path = to_lower(rel_path);
    return skipped_names.find(lower_name) != skipped_names.end() || starts_with(lower_name, "build-cmake") ||
           lower_path == "docs/kb/generated";
}

static void add_unique(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

static int path_depth(const fs::path& rel_path) {
    int depth = 0;
    for (const auto& part : rel_path) {
        (void)part;
        ++depth;
    }
    return depth;
}

static bool discovery_regular_file(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    return !ec && fs::is_regular_file(status);
}

static bool discovery_directory(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    return !ec && fs::is_directory(status);
}

DiscoveryResult discover_project(const fs::path& root) {
    DiscoveryResult result;
    result.has_git = discovery_directory(root / ".git") || discovery_regular_file(root / ".git");
    result.has_docs_dir = discovery_directory(root / "docs");
    result.has_readme = discovery_regular_file(root / "README.md") || discovery_regular_file(root / "readme.md");
    result.has_agents = discovery_regular_file(root / "AGENTS.md");
    result.has_pom = discovery_regular_file(root / "pom.xml");
    result.has_gradle = discovery_regular_file(root / "build.gradle") ||
                        discovery_regular_file(root / "build.gradle.kts") ||
                        discovery_regular_file(root / "settings.gradle") ||
                        discovery_regular_file(root / "settings.gradle.kts");
    result.has_package_json = discovery_regular_file(root / "package.json");
    result.has_cmake = discovery_regular_file(root / "CMakeLists.txt");
    result.has_makefile = discovery_regular_file(root / "Makefile") || discovery_regular_file(root / "makefile");
    result.has_retool_source_roots = discovery_directory(root / "apps") &&
                                     discovery_directory(root / "resources") &&
                                     discovery_directory(root / "workflows");
    const fs::path generated_kb_root = root / "docs" / "kb" / "generated";
    if (discovery_directory(generated_kb_root)) {
        for (const auto& entry : fs::directory_iterator(generated_kb_root)) {
            std::error_code entry_ec;
            if (!entry.is_symlink(entry_ec) && entry.is_regular_file(entry_ec)) {
                ++result.generated_kb_files;
            }
        }
    }

    if (!fs::exists(root)) {
        throw CliError("root-unavailable", "configuration", "Root path does not exist: " + root.string(),
                       2, false, "Select an existing --root path and retry.");
    }

    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        std::error_code entry_ec;
        if (it->is_symlink(entry_ec)) {
            it.disable_recursion_pending();
            continue;
        }
        const fs::path rel_path = fs::relative(it->path(), root);
        const std::string rel = rel_path.generic_string();
        const std::string name = it->path().filename().string();
        if (it->is_directory()) {
            if (should_skip_discovery_dir(name, rel)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file()) {
            continue;
        }

        result.max_depth_scanned = std::max(result.max_depth_scanned, path_depth(rel_path));
        const std::string lower_name = to_lower(name);
        const std::string lower_rel = to_lower(rel);
        const std::string ext = extension_of(rel);
        if (ext == ".md") {
            ++result.markdown_files;
        } else if (ext == ".json" || ext == ".jsonl") {
            ++result.json_files;
        } else if (ext == ".yaml" || ext == ".yml") {
            ++result.yaml_files;
        } else if (ext == ".java") {
            ++result.java_files;
        } else if (ext == ".kt" ||
                   (ext == ".kts" && lower_name != "build.gradle.kts" &&
                    lower_name != "settings.gradle.kts")) {
            ++result.kotlin_files;
        } else if (ext == ".cs") {
            ++result.csharp_files;
        } else if (is_cpp_extension(ext)) {
            ++result.cpp_files;
        } else if (is_typescript_javascript_extension(ext)) {
            ++result.ts_files;
        }
        if (ext == ".rsx" && starts_with(lower_rel, "apps/")) {
            ++result.rsx_files;
        }
        if (lower_name == "workflow.yml" && starts_with(lower_rel, "workflows/")) {
            ++result.retool_workflow_files;
        }
        if (lower_name == "resource.json" && starts_with(lower_rel, "resources/")) {
            ++result.retool_resource_files;
        }
        if (lower_name == "pom.xml") {
            ++result.pom_files;
        } else if (lower_name == "build.gradle" || lower_name == "build.gradle.kts" ||
                   lower_name == "settings.gradle" || lower_name == "settings.gradle.kts") {
            ++result.gradle_files;
        } else if (lower_name == "package.json") {
            ++result.package_json_files;
        } else if (ext == ".sln" || ext == ".slnx") {
            ++result.dotnet_solution_files;
        } else if (ext == ".csproj") {
            ++result.csproj_files;
        }
        if (starts_with(lower_rel, "docs/")) {
            ++result.docs_files;
            if (ext == ".md" || ext == ".json" || ext == ".jsonl" || ext == ".yaml" || ext == ".yml") {
                ++result.indexable_docs_files;
            }
            if (starts_with(lower_rel, "docs/kb/generated/")) {
                ++result.generated_kb_files;
            }
        }
        if (starts_with(lower_rel, "profiles/") && ext == ".json") {
            ++result.profile_files;
        }
    }

    if (result.has_pom || result.pom_files > 0) {
        add_unique(result.build_systems, "maven");
        if (result.kotlin_files > 0) {
            add_unique(result.project_kinds,
                       result.pom_files > 1 ? "kotlin-maven-multi-module" : "kotlin-maven");
        }
        if (result.java_files > 0) {
            add_unique(result.project_kinds,
                       result.pom_files > 1 ? "java-maven-multi-module" : "java-maven");
        }
        if (result.kotlin_files == 0 && result.java_files == 0) {
            add_unique(result.project_kinds,
                       result.pom_files > 1 ? "maven-multi-module" : "maven-project");
        }
        add_unique(result.recommended_extractors, "maven-pom");
    }
    if (result.has_gradle || result.gradle_files > 0) {
        add_unique(result.build_systems, "gradle");
        if (result.kotlin_files > 0) {
            add_unique(result.project_kinds, "kotlin-gradle");
        }
        if (result.java_files > 0) {
            add_unique(result.project_kinds, "java-gradle");
        }
        if (result.kotlin_files == 0 && result.java_files == 0) {
            add_unique(result.project_kinds, "gradle-project");
        }
        add_unique(result.recommended_extractors, "gradle-build");
    }
    if (result.has_package_json || result.package_json_files > 0) {
        add_unique(result.build_systems, "node");
        add_unique(result.project_kinds, "node-js");
        add_unique(result.recommended_extractors, "package-json");
    }
    if (result.ts_files > 0) {
        add_unique(result.project_kinds, "typescript-javascript-source");
        add_unique(result.recommended_extractors, "typescript-javascript-source-facts");
    }
    if (result.has_cmake) {
        add_unique(result.build_systems, "cmake");
        add_unique(result.project_kinds, "cpp-cmake");
    }
    if (result.cpp_files > 0) {
        add_unique(result.recommended_extractors, "cpp-source-facts");
    }
    if (result.java_files > 0) {
        add_unique(result.recommended_extractors, "java-source-facts");
        if (!result.has_pom && result.pom_files == 0 && !result.has_gradle && result.gradle_files == 0) {
            add_unique(result.project_kinds, "java-source");
        }
    }
    if (result.kotlin_files > 0) {
        add_unique(result.recommended_extractors, "kotlin-source-facts");
        if (!result.has_gradle && result.gradle_files == 0) {
            add_unique(result.project_kinds, "kotlin-source");
        }
    }
    if (result.has_makefile) {
        add_unique(result.build_systems, "make");
    }
    result.has_dotnet_solution = result.dotnet_solution_files > 0;
    if (result.has_dotnet_solution || result.csproj_files > 0 || result.csharp_files > 0) {
        add_unique(result.build_systems, "dotnet");
        add_unique(result.project_kinds,
                   result.csproj_files > 1 ? "dotnet-multi-project" : "dotnet-project");
        add_unique(result.recommended_extractors, "dotnet-project");
        add_unique(result.recommended_extractors, "csharp-source-facts");
    }
    if (result.docs_files > 0 || result.markdown_files > 0) {
        add_unique(result.recommended_extractors, "machine-docs");
    }
    if (result.has_retool_source_roots || result.rsx_files > 0 || result.retool_workflow_files > 0) {
        add_unique(result.project_kinds, "retool-source");
        add_unique(result.signals, "retool-source-roots");
        add_unique(result.recommended_extractors, "retool-source-facts");
    }
    if (!result.has_readme && result.docs_files < 3 && result.markdown_files < 3 && result.profile_files == 0) {
        result.warnings.push_back("low-doc-coverage");
    }
    if ((result.has_pom || result.pom_files > 0) && result.docs_files < 3) {
        result.warnings.push_back("java-project-needs-source-derived-kb");
    }
    if ((result.has_dotnet_solution || result.csproj_files > 0) && result.docs_files < 3) {
        result.warnings.push_back("dotnet-project-needs-source-derived-kb");
    }
    if (result.generated_kb_files > 0) {
        add_unique(result.signals, "existing-generated-kb");
    }
    if (result.has_docs_dir) {
        add_unique(result.signals, "docs-directory");
    }
    if (result.has_readme) {
        add_unique(result.signals, "readme");
    }
    if (result.has_agents) {
        add_unique(result.signals, "agents-entrypoint");
    }

    const bool has_java = result.java_files > 0;
    const bool has_kotlin = result.kotlin_files > 0;
    const bool has_node = result.ts_files > 0;
    const bool has_cpp = result.cpp_files > 0;
    const bool has_dotnet = result.csharp_files > 0;
    const bool has_retool = result.rsx_files > 0 || result.retool_workflow_files > 0 ||
                            result.retool_resource_files > 0;
    const int language_families = static_cast<int>(has_java) + static_cast<int>(has_kotlin) +
                                  static_cast<int>(has_node) +
                                  static_cast<int>(has_cpp) + static_cast<int>(has_dotnet);
    const bool has_documents = result.has_readme || result.has_agents || result.indexable_docs_files > 0;
    const bool has_sources = language_families > 0 || has_retool;

    if (has_retool) {
        result.recommended_profiles.push_back("generic-retool");
    }
    const std::array<std::pair<const char*, int>, 5> language_profiles = {{
        {"generic-java", result.java_files},
        {"generic-kotlin", result.kotlin_files},
        {"generic-node", result.ts_files},
        {"generic-cpp", result.cpp_files},
        {"generic-dotnet", result.csharp_files}
    }};
    const auto dominant = std::max_element(language_profiles.begin(), language_profiles.end(),
        [](const auto& left, const auto& right) { return left.second < right.second; });
    int total_source_files = 0;
    for (const auto& [profile_id, file_count] : language_profiles) {
        (void)profile_id;
        total_source_files += file_count;
    }
    const int other_source_files = total_source_files - dominant->second;
    const bool has_dominant_language = language_families > 1 && dominant->second >= 10 &&
                                       dominant->second >= other_source_files * 4;

    if (!has_retool && language_families > 1 && has_dominant_language) {
        result.recommended_profiles.push_back(dominant->first);
        result.recommended_profiles.push_back("generic-source");
    } else if (!has_retool && language_families > 1) {
        result.recommended_profiles.push_back("generic-source");
    }
    if (has_java) {
        add_unique(result.recommended_profiles, "generic-java");
    }
    if (has_kotlin) {
        add_unique(result.recommended_profiles, "generic-kotlin");
    }
    if (has_node) {
        add_unique(result.recommended_profiles, "generic-node");
    }
    if (has_cpp) {
        add_unique(result.recommended_profiles, "generic-cpp");
    }
    if (has_dotnet) {
        add_unique(result.recommended_profiles, "generic-dotnet");
    }
    if (!has_sources && has_documents) {
        result.recommended_profiles.push_back("generic-docs");
    }

    result.indexable = has_sources || has_documents;
    result.bootstrap_state = !result.indexable ? "empty" : (has_documents ? "documented" : "source-only");
    if (!result.recommended_profiles.empty()) {
        result.primary_profile = result.recommended_profiles.front();
        result.next_steps = {"profile-check --strict", "build", "check", "doctor", "brief --require-current"};
    } else {
        add_unique(result.warnings, "no-indexable-content");
        result.next_steps = {"add source code or machine documentation", "discover"};
    }
    return result;
}

} // namespace shin::context
