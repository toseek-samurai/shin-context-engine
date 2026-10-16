#include "context_internal.h"
#include "source_relations.h"

namespace shin::context {
namespace {

struct RetoolAuthority {
    std::string status = "active";
    std::string level = "source-fact";
};

struct RsxAttributeSource {
    std::string_view raw;
    int line = 1;
    bool unique = true;
};

struct RsxTag {
    std::string name;
    std::map<std::string, std::string> attributes;
    std::map<std::string, RsxAttributeSource> attribute_sources;
    int line = 1;
    int end_line = 1;
    bool closing = false;
    bool self_closing = false;
};

struct RsxOwner {
    std::string tag;
    std::string id;
};

struct RelationWriter {
    SourceRelationInputs& inputs;

    void require_capacity() const {
        if (inputs.direct.size() + inputs.bindings.size() + inputs.references.size() >= 4096) {
            throw CliError("source-relations-limit", "resource",
                           "Retool source relation input exceeds 4096 entries in one file.",
                           1, false, "Split the source file or narrow the profile source inventory.");
        }
    }

    void direct(const std::string& path, const std::string& kind,
                const std::string& target, int line) {
        require_capacity();
        const std::string resolved = (fs::path(path).parent_path() / fs::path(target))
                                         .lexically_normal().generic_string();
        inputs.direct.push_back({path, kind, target, target, "unresolved", resolved, line});
    }

    void binding(const std::string& scope, const std::string& symbol,
                 const std::string& path, const std::string& payload, int line, int end_line = 0) {
        require_capacity();
        inputs.bindings.push_back({scope, symbol, path, payload, line, end_line,
                                  end_line > 0 ? "rsx-opening-tag" : ""});
    }

    void reference(const std::string& scope, const std::string& path,
                   const std::string& target, int line) {
        require_capacity();
        inputs.references.push_back({scope, path, target, line});
    }
};

void scan_runtime_references(const Profile& profile,
                             const std::string& path,
                             const std::vector<std::string>& lines,
                             const std::string& owner,
                             std::vector<FactRow>& rows,
                             int first_line);
void scan_runtime_relations(std::string_view source, const std::string& path,
                            const std::string& scope, int first_line, RelationWriter& relations);
std::string runtime_code(std::string_view source);

// Preserve byte offsets and line numbers while excluding non-code lexical regions.
std::size_t noncode_end(std::string_view source, std::size_t cursor, bool sql = false) {
    const char ch = source[cursor];
    if (ch == '\'' || ch == '"' || ch == '`') {
        for (std::size_t end = cursor + 1; end < source.size(); ++end) {
            if (source[end] == '\\') {
                ++end;
            } else if (source[end] == ch) {
                if (sql && end + 1 < source.size() && source[end + 1] == ch) {
                    ++end;
                } else {
                    return end + 1;
                }
            }
        }
        return source.size();
    }
    if (source.substr(cursor, 2) == "/*") {
        int depth = 1;
        for (std::size_t end = cursor + 2; end + 1 < source.size(); ++end) {
            if (sql && source.substr(end, 2) == "/*") {
                ++depth;
                ++end;
            } else if (source.substr(end, 2) == "*/") {
                if (--depth == 0) return end + 2;
                ++end;
            }
        }
        return source.size();
    }
    if (source.substr(cursor, 2) == "//" || (sql && (source.substr(cursor, 2) == "--" || ch == '#'))) {
        const std::size_t end = source.find('\n', cursor + 2);
        return end == std::string_view::npos ? source.size() : end;
    }
    if (source.substr(cursor, 4) == "<!--") {
        const std::size_t end = source.find("-->", cursor + 4);
        return end == std::string_view::npos ? source.size() : end + 3;
    }
    if (sql && ch == '$') {
        std::size_t delimiter_end = cursor + 1;
        while (delimiter_end < source.size() &&
               (is_ascii_alnum(static_cast<unsigned char>(source[delimiter_end])) || source[delimiter_end] == '_')) {
            ++delimiter_end;
        }
        if (delimiter_end < source.size() && source[delimiter_end] == '$') {
            const auto delimiter = source.substr(cursor, delimiter_end - cursor + 1);
            const std::size_t end = source.find(delimiter, delimiter_end + 1);
            return end == std::string_view::npos ? source.size() : end + delimiter.size();
        }
    }
    if (!sql && ch == '/') {
        std::size_t previous = cursor;
        while (previous > 0 && is_ascii_space(static_cast<unsigned char>(source[previous - 1]))) --previous;
        std::size_t word_start = previous;
        while (word_start > 0 && is_ascii_alnum(static_cast<unsigned char>(source[word_start - 1]))) --word_start;
        const std::string_view word = source.substr(word_start, previous - word_start);
        const bool expression_start = previous == 0 ||
            std::string_view("=(:,[!&|?;{}>+*-~%^").find(source[previous - 1]) != std::string_view::npos ||
            word == "return" || word == "throw" || word == "case" || word == "yield" ||
            word == "await" || word == "void" || word == "typeof" || word == "delete";
        if (expression_start) {
            bool character_class = false;
            for (std::size_t end = cursor + 1; end < source.size(); ++end) {
                if (source[end] == '\n') return end;
                if (source[end] == '\\') {
                    ++end;
                } else if (source[end] == '[') {
                    character_class = true;
                } else if (source[end] == ']') {
                    character_class = false;
                } else if (source[end] == '/' && !character_class) {
                    return end + 1;
                }
            }
            return source.size();
        }
    }
    return cursor;
}

std::size_t expression_end(std::string_view source, std::size_t cursor) {
    int depth = 0;
    while (cursor < source.size()) {
        const std::size_t skipped = noncode_end(source, cursor);
        if (skipped != cursor) {
            cursor = skipped;
            continue;
        }
        const char ch = source[cursor++];
        if (ch == '{') {
            ++depth;
        } else if (ch == '}' && --depth == 0) {
            return cursor;
        }
    }
    return std::string_view::npos;
}

void skip_space(std::string_view source, std::size_t& cursor) {
    while (cursor < source.size() && is_ascii_space(static_cast<unsigned char>(source[cursor]))) {
        ++cursor;
    }
}

void skip_expression_trivia(std::string_view source, std::size_t& cursor) {
    skip_space(source, cursor);
    while (cursor < source.size() &&
           (source.substr(cursor, 2) == "/*" || source.substr(cursor, 2) == "//")) {
        cursor = noncode_end(source, cursor);
        skip_space(source, cursor);
    }
}

std::string static_string(std::string_view source, std::size_t& cursor) {
    skip_space(source, cursor);
    if (cursor == source.size() || (source[cursor] != '\'' && source[cursor] != '"')) {
        return "";
    }
    const char quote = source[cursor++];
    const std::size_t start = cursor;
    while (cursor < source.size()) {
        const char ch = source[cursor++];
        if (ch == '\\' || ch == '\n' || ch == '\r') {
            return "";
        }
        if (ch == quote) {
            const std::string value(source.substr(start, cursor - start - 1));
            return value.find("{{") == std::string::npos ? value : "";
        }
    }
    return "";
}

std::string static_attribute(const RsxAttributeSource& attribute) {
    if (!attribute.unique) return "";
    std::string_view raw = attribute.raw;
    if (raw.size() >= 2 && raw.front() == '{' && raw.back() == '}') {
        raw = raw.substr(1, raw.size() - 2);
    }
    std::size_t cursor = 0;
    const std::string value = static_string(raw, cursor);
    skip_space(raw, cursor);
    return cursor == raw.size() ? value : "";
}

bool runtime_identifier_char(char ch) {
    return is_ascii_alnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '$';
}

std::string static_include(std::string_view source, std::size_t& cursor) {
    if (source.substr(cursor, 7) != "include") return "";
    cursor += 7;
    skip_expression_trivia(source, cursor);
    if (cursor == source.size() || source[cursor] != '(') return "";
    ++cursor;
    skip_expression_trivia(source, cursor);
    const std::string target = static_string(source, cursor);
    if (target.empty()) return "";
    skip_expression_trivia(source, cursor);
    if (cursor < source.size() && source[cursor] == ',') {
        ++cursor;
        skip_expression_trivia(source, cursor);
        if (static_string(source, cursor).empty()) return "";
        skip_expression_trivia(source, cursor);
    }
    if (cursor == source.size() || source[cursor] != ')') return "";
    ++cursor;
    return target;
}

void scan_template_relations(std::string_view source, const std::string& path,
                             const std::string& scope, int first_line, bool sql,
                             RelationWriter& relations) {
    int line = first_line;
    for (std::size_t cursor = 0; cursor < source.size();) {
        std::size_t stop = noncode_end(source, cursor, sql);
        if (source.substr(cursor, 2) == "{{") {
            stop = expression_end(source, cursor);
            if (stop == std::string_view::npos) return;
            if (stop >= cursor + 4 && source.substr(stop - 2, 2) == "}}") {
                scan_runtime_relations(source.substr(cursor + 2, stop - cursor - 4),
                                       path, scope, line, relations);
            }
        }
        if (stop == cursor) stop = cursor + 1;
        line += static_cast<int>(std::count(source.begin() + static_cast<std::ptrdiff_t>(cursor),
                                           source.begin() + static_cast<std::ptrdiff_t>(stop), '\n'));
        cursor = stop;
    }
}

bool is_name_char(char value) {
    const unsigned char ch = static_cast<unsigned char>(value);
    return is_ascii_alnum(ch) || value == '_' || value == '-' || value == ':' || value == '$';
}

RetoolAuthority authority_for_path(const std::string& path) {
    if (starts_with(path, "apps/archive-apps/")) {
        return {"superseded", "history"};
    }
    if (starts_with(path, "apps/templates/")) {
        return {"active", "advisory"};
    }
    return {};
}

void add_retool_fact(std::vector<FactRow>& rows,
                     const Profile& profile,
                     const std::string& path,
                     int line,
                     const std::string& kind,
                     const std::string& name,
                     const std::string& summary,
                     int end_line = -1) {
    const RetoolAuthority authority = authority_for_path(path);
    add_source_fact_row(rows, profile, path, line, "retool", kind,
                        truncate_text(compact_whitespace(name), 220), summary,
                        authority.status, authority.level, end_line);
}

std::string attr(const RsxTag& tag, const std::string& key) {
    const auto it = tag.attributes.find(key);
    return it == tag.attributes.end() ? "" : trim(it->second);
}

std::string first_quoted_argument(const std::string& value, std::string_view function_name) {
    const std::size_t call = value.find(function_name);
    if (call == std::string::npos) {
        return "";
    }
    std::size_t cursor = call + function_name.size();
    while (cursor < value.size() && is_ascii_space(static_cast<unsigned char>(value[cursor]))) {
        ++cursor;
    }
    if (cursor >= value.size() || value[cursor] != '(') {
        return "";
    }
    ++cursor;
    while (cursor < value.size() && is_ascii_space(static_cast<unsigned char>(value[cursor]))) {
        ++cursor;
    }
    if (cursor >= value.size() || (value[cursor] != '"' && value[cursor] != '\'')) {
        return "";
    }
    const char quote = value[cursor++];
    std::string result;
    bool escaped = false;
    while (cursor < value.size()) {
        const char ch = value[cursor++];
        if (escaped) {
            result.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == quote) {
            return result;
        } else {
            result.push_back(ch);
        }
    }
    return "";
}

std::map<std::string, std::string> parse_rsx_attributes(
    std::string_view source, int first_line,
    std::map<std::string, RsxAttributeSource>& attribute_sources) {
    std::map<std::string, std::string> attributes;
    std::size_t cursor = 0;
    std::size_t line_cursor = 0;
    int line = first_line;
    const auto remember_source = [&](const std::string& key, std::size_t start, std::size_t end) {
        line += static_cast<int>(std::count(source.begin() + static_cast<std::ptrdiff_t>(line_cursor),
                                           source.begin() + static_cast<std::ptrdiff_t>(start), '\n'));
        line_cursor = start;
        const auto [it, inserted] = attribute_sources.emplace(
            key, RsxAttributeSource{source.substr(start, end - start), line, true});
        if (!inserted) it->second.unique = false;
    };
    while (cursor < source.size()) {
        while (cursor < source.size() &&
               (is_ascii_space(static_cast<unsigned char>(source[cursor])) || source[cursor] == '/')) {
            ++cursor;
        }
        const std::size_t key_start = cursor;
        while (cursor < source.size() && is_name_char(source[cursor])) {
            ++cursor;
        }
        if (cursor == key_start) {
            ++cursor;
            continue;
        }
        const std::string key(source.substr(key_start, cursor - key_start));
        while (cursor < source.size() && is_ascii_space(static_cast<unsigned char>(source[cursor]))) {
            ++cursor;
        }
        if (cursor >= source.size() || source[cursor] != '=') {
            attributes.emplace(key, "true");
            remember_source(key, cursor, cursor);
            continue;
        }
        ++cursor;
        while (cursor < source.size() && is_ascii_space(static_cast<unsigned char>(source[cursor]))) {
            ++cursor;
        }
        const std::size_t value_start = cursor;
        if (cursor < source.size() &&
            (source[cursor] == '"' || source[cursor] == '\'' || source[cursor] == '`')) {
            const char quote = source[cursor++];
            const std::size_t content_start = cursor;
            std::size_t content_end = source.size();
            bool escaped = false;
            while (cursor < source.size()) {
                const char ch = source[cursor++];
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == quote) {
                    content_end = cursor - 1;
                    break;
                }
            }
            attributes.emplace(key, std::string(source.substr(content_start, content_end - content_start)));
            remember_source(key, value_start, cursor);
            continue;
        }
        if (cursor < source.size() && source[cursor] == '{') {
            const std::size_t end = expression_end(source, cursor);
            cursor = end == std::string_view::npos ? source.size() : end;
            const std::size_t inner_start = value_start + 1;
            const std::size_t inner_end = cursor > 0 ? cursor - 1 : cursor;
            attributes.emplace(key, std::string(source.substr(inner_start, inner_end - inner_start)));
            if (end != std::string_view::npos) remember_source(key, value_start, cursor);
            continue;
        }
        while (cursor < source.size() && !is_ascii_space(static_cast<unsigned char>(source[cursor])) &&
               source[cursor] != '/') {
            ++cursor;
        }
        attributes.emplace(key, std::string(source.substr(value_start, cursor - value_start)));
        remember_source(key, value_start, cursor);
    }
    return attributes;
}

std::vector<RsxTag> scan_rsx_tags(const std::string& content) {
    std::vector<RsxTag> tags;
    std::size_t cursor = 0;
    int line = 1;
    while (cursor < content.size()) {
        if (content[cursor] == '{') {
            const std::size_t end = expression_end(content, cursor);
            const std::size_t stop = end == std::string_view::npos ? content.size() : end;
            line += static_cast<int>(std::count(content.begin() + static_cast<std::ptrdiff_t>(cursor),
                                               content.begin() + static_cast<std::ptrdiff_t>(stop), '\n'));
            cursor = stop;
            continue;
        }
        if (content[cursor] != '<') {
            line += content[cursor] == '\n' ? 1 : 0;
            ++cursor;
            continue;
        }
        if (content.compare(cursor, 4, "<!--") == 0) {
            const std::size_t end = content.find("-->", cursor + 4);
            const std::size_t stop = end == std::string::npos ? content.size() : end + 3;
            line += static_cast<int>(std::count(content.begin() + static_cast<std::ptrdiff_t>(cursor),
                                                content.begin() + static_cast<std::ptrdiff_t>(stop), '\n'));
            cursor = stop;
            continue;
        }
        std::size_t name_start = cursor + 1;
        bool closing = false;
        if (name_start < content.size() && content[name_start] == '/') {
            closing = true;
            ++name_start;
        }
        if (name_start >= content.size() ||
            (!is_ascii_upper(static_cast<unsigned char>(content[name_start])) && content[name_start] != '$')) {
            ++cursor;
            continue;
        }
        std::size_t name_end = name_start;
        while (name_end < content.size() && is_name_char(content[name_end])) {
            ++name_end;
        }
        std::size_t end = name_end;
        for (; end < content.size(); ++end) {
            const char ch = content[end];
            const std::size_t skipped = ch == '\'' || ch == '"' || ch == '`' ? noncode_end(content, end) : end;
            if (skipped != end) {
                end = skipped - 1;
                continue;
            }
            if (ch == '{') {
                const std::size_t stop = expression_end(content, end);
                if (stop == std::string_view::npos) {
                    end = content.size();
                    break;
                }
                end = stop - 1;
            } else if (ch == '>') {
                break;
            }
        }
        if (end >= content.size()) {
            break;
        }
        std::size_t before_end = end;
        while (before_end > name_end && is_ascii_space(static_cast<unsigned char>(content[before_end - 1]))) {
            --before_end;
        }
        RsxTag tag;
        tag.name = content.substr(name_start, name_end - name_start);
        tag.line = line;
        tag.end_line = line + static_cast<int>(std::count(
            content.begin() + static_cast<std::ptrdiff_t>(cursor),
            content.begin() + static_cast<std::ptrdiff_t>(end + 1), '\n'));
        tag.closing = closing;
        tag.self_closing = !closing && before_end > name_end && content[before_end - 1] == '/';
        if (!closing) {
            tag.attributes = parse_rsx_attributes(
                std::string_view(content).substr(name_end, end - name_end), line,
                tag.attribute_sources);
        }
        tags.push_back(std::move(tag));
        const std::size_t stop = end + 1;
        line += static_cast<int>(std::count(content.begin() + static_cast<std::ptrdiff_t>(cursor),
                                            content.begin() + static_cast<std::ptrdiff_t>(stop), '\n'));
        cursor = stop;
    }
    return tags;
}

fs::path nearest_retool_owner(const fs::path& root,
                              const std::string& path,
                              const std::string& marker_file) {
    fs::path current = fs::path(path).parent_path();
    while (!current.empty() && current != current.root_path()) {
        std::error_code ec;
        if (fs::is_regular_file(root / current / marker_file, ec)) {
            return current;
        }
        const fs::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return fs::path(path).parent_path();
}

std::string app_owner(const fs::path& root, const std::string& path) {
    return nearest_retool_owner(root, path, "metadata.json").generic_string();
}

std::string workflow_owner(const fs::path& root, const std::string& path) {
    return nearest_retool_owner(root, path, "workflow.yml").generic_string();
}

std::string physical_owner(const std::string& path) {
    return fs::path(path).stem().generic_string();
}

std::string classify_rsx_symbol(const std::string& tag) {
    static const std::set<std::string> queries = {
        "FirebaseQuery", "GCSQuery", "GlobalWidgetQuery", "JavascriptQuery", "OpenAPIQuery",
        "RESTQuery", "SqlQuery", "SqlQueryUnified", "SqlTransformQuery", "WorkflowRun"
    };
    if (queries.contains(tag)) {
        return "query";
    }
    if (tag == "Screen") {
        return "screen";
    }
    if (tag == "State") {
        return "state";
    }
    if (tag == "Function") {
        return "function";
    }
    if (tag == "Folder") {
        return "folder";
    }
    if (tag == "Module" || starts_with(tag, "GlobalWidget")) {
        return "module";
    }
    return "component";
}

bool intrinsic_retool_resource(const std::string& resource_name, const std::string& resource_display) {
    static const std::set<std::string> intrinsic = {
        "FirebaseQuery", "GCSQuery", "GlobalWidgetQuery", "JavascriptQuery", "OpenAPIQuery",
        "RESTQuery", "SqlQuery", "SqlQueryUnified", "SqlTransformQuery", "WorkflowRun"
    };
    return resource_display.empty() && intrinsic.contains(resource_name);
}

std::string compact_endpoint(const std::string& endpoint) {
    const std::string payload = first_quoted_argument(endpoint, "include");
    return payload.empty() ? truncate_text(compact_whitespace(endpoint), 120) : payload;
}

std::string nearest_owner_id(const std::vector<RsxOwner>& stack) {
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if (!it->id.empty()) {
            return it->id;
        }
    }
    return "app-root";
}

std::string selected_attribute_summary(const Profile& profile, const RsxTag& tag) {
    std::string summary;
    for (const std::string& key : profile.source_fact_attribute_keys) {
        const auto value = tag.attributes.find(key);
        if (value == tag.attributes.end()) {
            continue;
        }
        summary += " " + key + "=" +
                   truncate_text(compact_whitespace(value->second), 160);
    }
    return summary;
}

void close_rsx_owner(std::vector<RsxOwner>& stack, const std::string& name) {
    while (!stack.empty()) {
        const std::string current = stack.back().tag;
        stack.pop_back();
        if (current == name) {
            return;
        }
    }
}

void add_rsx_payload_fact(std::vector<FactRow>& rows,
                          const Profile& profile,
                          const std::string& path,
                          const RsxTag& tag,
                          const std::string& owner_id,
                          const std::string& attribute_name,
                          const std::string& value) {
    const std::string payload = first_quoted_argument(value, "include");
    if (payload.empty()) {
        return;
    }
    const std::string source_id = attr(tag, "id").empty() ? owner_id : attr(tag, "id");
    add_retool_fact(rows, profile, path, tag.line, "payload-owner",
                    source_id + " owns " + payload,
                    "owner=" + source_id + " attribute=" + attribute_name + " payload=" + payload,
                    tag.end_line);
}

void scan_rsx_relations(const RsxTag& tag, const std::string& path,
                        const std::string& scope, RelationWriter& relations) {
    const auto static_value = [&](const std::string& key) {
        const auto found = tag.attribute_sources.find(key);
        return found == tag.attribute_sources.end() ? "" : static_attribute(found->second);
    };
    if (tag.name == "Include") {
        const std::string target = static_value("src");
        if (!target.empty()) {
            relations.direct(path, "source-include", target, tag.attribute_sources.at("src").line);
        }
    } else if (tag.name == "Event") {
        const std::string target = static_value("pluginId");
        if (!target.empty()) {
            relations.reference(scope, path, target, tag.attribute_sources.at("pluginId").line);
        }
    }

    std::string principal_payload;
    for (const auto& [key, attribute] : tag.attribute_sources) {
        if (!attribute.unique || attribute.raw.size() < 2) continue;
        const std::string_view raw = attribute.raw;
        if (raw.front() == '{' && raw.back() == '}') {
            const std::string_view expression = raw.substr(1, raw.size() - 2);
            const std::string code = runtime_code(expression);
            int line = attribute.line;
            for (std::size_t cursor = 0; cursor < code.size();) {
                const std::size_t start = cursor;
                if (runtime_identifier_char(code[cursor])) {
                    while (cursor < code.size() && runtime_identifier_char(code[cursor])) ++cursor;
                    std::size_t previous = start;
                    while (previous > 0 && is_ascii_space(static_cast<unsigned char>(code[previous - 1]))) {
                        --previous;
                    }
                    if (code.substr(start, cursor - start) == "include" &&
                        (previous == 0 || code[previous - 1] != '.')) {
                        std::size_t end = start;
                        const std::string target = static_include(expression, end);
                        if (!target.empty()) {
                            relations.direct(path, "source-payload", target, line);
                            std::size_t tail = end;
                            skip_expression_trivia(expression, tail);
                            const bool script_query = tag.name == "JavascriptQuery" || tag.name == "SqlQuery" ||
                                tag.name == "SqlQueryUnified" || tag.name == "SqlTransformQuery";
                            const bool principal_attribute = (key == "query" && script_query) ||
                                (key == "funcBody" && tag.name == "Function");
                            if (principal_attribute &&
                                previous == 0 && tail == expression.size()) {
                                principal_payload = (fs::path(path).parent_path() / fs::path(target))
                                                        .lexically_normal().generic_string();
                            }
                            cursor = end;
                        }
                    }
                } else {
                    ++cursor;
                }
                line += static_cast<int>(std::count(expression.begin() + static_cast<std::ptrdiff_t>(start),
                                                   expression.begin() + static_cast<std::ptrdiff_t>(cursor), '\n'));
            }
            scan_runtime_relations(code, path, scope, attribute.line, relations);
        } else if ((raw.front() == '"' || raw.front() == '\'') && raw.back() == raw.front()) {
            scan_template_relations(raw.substr(1, raw.size() - 2), path, scope,
                                    attribute.line, false, relations);
        }
    }
    const std::string id = static_value("id");
    if (!id.empty() && tag.name != "Include" && tag.name != "Event") {
        relations.binding(scope, id, path, principal_payload, tag.line, tag.end_line);
    }
}

void scan_rsx(const Profile& profile,
              const fs::path& root,
              const std::string& path,
              const std::string& content,
              std::vector<FactRow>& rows,
              RelationWriter& relations) {
    const std::string app = app_owner(root, path);
    if (fs::path(path).filename() == "main.rsx") {
        std::error_code ec;
        const bool multi_page = fs::is_directory(root / app / ".positions", ec);
        ec.clear();
        const bool single_page = fs::is_regular_file(root / app / ".positions.json", ec);
        const std::string architecture = multi_page ? "multi-page" : (single_page ? "single-page" : "unknown-layout");
        add_retool_fact(rows, profile, path, 1, "app", app + " " + architecture,
                        "app=" + app + " architecture=" + architecture);
    }

    std::vector<RsxOwner> stack;
    for (const RsxTag& tag : scan_rsx_tags(content)) {
        if (tag.closing) {
            close_rsx_owner(stack, tag.name);
            continue;
        }
        scan_rsx_relations(tag, path, app, relations);
        const std::string parent_id = nearest_owner_id(stack);
        const std::string id = attr(tag, "id");
        const std::string resource_name = attr(tag, "resourceName");
        const std::string resource_display = attr(tag, "resourceDisplayName");
        const std::string method = attr(tag, "method").empty() ? attr(tag, "type") : attr(tag, "method");
        const std::string endpoint = attr(tag, "path").empty() ? attr(tag, "query") : attr(tag, "path");
        const bool external_resource = (!resource_name.empty() || !resource_display.empty()) &&
                                       !intrinsic_retool_resource(resource_name, resource_display);

        if (tag.name == "Include") {
            const std::string target = attr(tag, "src");
            if (!target.empty()) {
                add_retool_fact(rows, profile, path, tag.line, "include",
                                parent_id + " includes " + target,
                                "app=" + app + " owner=" + parent_id + " target=" + target +
                                    selected_attribute_summary(profile, tag),
                                tag.end_line);
            }
        } else if (tag.name == "Event") {
            const std::string event = attr(tag, "event");
            const std::string action = attr(tag, "method");
            const std::string target = attr(tag, "pluginId").empty() ? "inline-script" : attr(tag, "pluginId");
            const std::string event_type = attr(tag, "type");
            add_retool_fact(rows, profile, path, tag.line, "event",
                            parent_id + " " + event + " " + action + " " + target + " " + event_type,
                            "app=" + app + " owner=" + parent_id + " event=" + event + " action=" + action +
                                " target=" + target + " type=" + event_type +
                                selected_attribute_summary(profile, tag),
                            tag.end_line);
        } else if (!id.empty()) {
            const std::string kind = classify_rsx_symbol(tag.name);
            std::string name = id + " " + tag.name + " " + physical_owner(path);
            if (!resource_display.empty()) {
                name += " " + resource_display;
            }
            if (!method.empty() && kind == "query") {
                name += " " + method;
            }
            std::string summary = "app=" + app + " physical_owner=" + physical_owner(path) + " id=" + id +
                                  " tag=" + tag.name;
            if (external_resource) {
                summary += " resource=" + (resource_display.empty() ? resource_name : resource_display);
            }
            if (!endpoint.empty() && kind == "query") {
                summary += " endpoint=" + compact_endpoint(endpoint);
            }
            summary += selected_attribute_summary(profile, tag);
            add_retool_fact(rows, profile, path, tag.line, kind, name,
                            summary, tag.end_line);
        }

        if (!id.empty() && external_resource) {
            const std::string resource = resource_display.empty() ? resource_name : resource_display;
            add_retool_fact(rows, profile, path, tag.line, "resource-binding",
                            id + " " + tag.name + " uses " + resource + " " + method + " " + compact_endpoint(endpoint),
                            "app=" + app + " query=" + id + " resource_name=" + resource_name +
                                " resource_display=" + resource_display + " method=" + method +
                                " endpoint=" + compact_endpoint(endpoint) +
                                selected_attribute_summary(profile, tag),
                            tag.end_line);
            if (tag.name.size() <= 128) rows.back().identifier_terms.push_back(tag.name);
        }
        for (const auto& [key, value] : tag.attributes) {
            add_rsx_payload_fact(rows, profile, path, tag, parent_id, key, value);
        }
        std::string expressions;
        for (const auto& [key, value] : tag.attributes) {
            (void)key;
            expressions += value;
            expressions.push_back('\n');
        }
        const std::string runtime_owner = tag.name == "Event" ? parent_id : (id.empty() ? parent_id : id);
        scan_runtime_references(profile, path, split_lines(expressions), runtime_owner, rows, tag.line);
        if (!tag.self_closing) {
            stack.push_back({tag.name, id});
        }
    }
}

bool likely_retool_identifier(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(value.front());
    return is_ascii_upper(first) || value.front() == '$' || value == "current_user" ||
           value == "retoolContext" || value == "workflowContext" || value == "utils";
}

bool ignored_runtime_target(const std::string& value) {
    static const std::set<std::string> ignored = {
        "Array", "Date", "JSON", "Math", "Number", "Object", "Promise", "String", "console"
    };
    return ignored.contains(value);
}

bool action_member(const std::string& value) {
    static const std::set<std::string> actions = {
        "clearValue", "hide", "openApp", "openPage", "reset", "resetInstanceValues", "setData",
        "setValue", "setCurrentView", "setCurrentViewIndex", "show", "showNotification", "trigger",
        "validate"
    };
    return actions.contains(value);
}

bool data_member(const std::string& value) {
    static const std::set<std::string> members = {
        "data", "error", "initialData", "instanceValues", "metadata", "selectedRow",
        "selectedSourceRows", "value", "values"
    };
    return members.contains(value);
}

std::string runtime_code(std::string_view source) {
    std::string code(source);
    for (std::size_t cursor = 0; cursor < source.size();) {
        const std::size_t end = noncode_end(source, cursor);
        if (end == cursor) {
            ++cursor;
            continue;
        }
        const bool literal = source[cursor] == '\'' || source[cursor] == '"' || source[cursor] == '`' ||
            (source[cursor] == '/' && source.substr(cursor, 2) != "//" && source.substr(cursor, 2) != "/*");
        for (std::size_t offset = cursor; offset < end; ++offset) {
            if (code[offset] != '\n') code[offset] = ' ';
        }
        if (literal) code[cursor] = '\x01';
        cursor = end;
    }
    return code;
}

void scan_runtime_relations(std::string_view source, const std::string& path,
                            const std::string& scope, int first_line, RelationWriter& relations) {
    const std::string code = runtime_code(source);
    int line = first_line;
    for (std::size_t cursor = 0; cursor < code.size();) {
        if (!runtime_identifier_char(code[cursor])) {
            line += code[cursor++] == '\n' ? 1 : 0;
            continue;
        }
        const std::size_t start = cursor;
        while (cursor < code.size() && runtime_identifier_char(code[cursor])) ++cursor;
        std::size_t previous = start;
        while (previous > 0 && is_ascii_space(static_cast<unsigned char>(code[previous - 1]))) --previous;
        if (is_ascii_digit(static_cast<unsigned char>(code[start])) ||
            (previous > 0 && code[previous - 1] == '.')) continue;
        const std::string target = code.substr(start, cursor - start);
        std::size_t member_cursor = cursor;
        skip_space(code, member_cursor);
        if (member_cursor == code.size() || code[member_cursor] != '.') continue;
        ++member_cursor;
        skip_space(code, member_cursor);
        const std::size_t member_start = member_cursor;
        while (member_cursor < code.size() && runtime_identifier_char(code[member_cursor])) ++member_cursor;
        const std::string member = code.substr(member_start, member_cursor - member_start);
        if (!ignored_runtime_target(target) &&
            (action_member(member) || (data_member(member) && likely_retool_identifier(target)))) {
            relations.reference(scope, path, target, line);
        }
    }
}

void scan_runtime_references(const Profile& profile,
                             const std::string& path,
                             const std::vector<std::string>& lines,
                             const std::string& owner,
                             std::vector<FactRow>& rows,
                             int first_line) {
    std::set<std::tuple<int, std::string, std::string>> emitted;
    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
        const std::string& line = lines[line_index];
        std::size_t cursor = 0;
        while (cursor < line.size()) {
            const unsigned char first = static_cast<unsigned char>(line[cursor]);
            if (!is_ascii_alnum(first) && line[cursor] != '_' && line[cursor] != '$') {
                ++cursor;
                continue;
            }
            const std::size_t target_start = cursor++;
            while (cursor < line.size() &&
                   (is_ascii_alnum(static_cast<unsigned char>(line[cursor])) || line[cursor] == '_' ||
                    line[cursor] == '$')) {
                ++cursor;
            }
            const std::string target = line.substr(target_start, cursor - target_start);
            while (cursor < line.size() && is_ascii_space(static_cast<unsigned char>(line[cursor]))) {
                ++cursor;
            }
            if (cursor >= line.size() || line[cursor] != '.') {
                continue;
            }
            ++cursor;
            while (cursor < line.size() && is_ascii_space(static_cast<unsigned char>(line[cursor]))) {
                ++cursor;
            }
            const std::size_t member_start = cursor;
            while (cursor < line.size() &&
                   (is_ascii_alnum(static_cast<unsigned char>(line[cursor])) || line[cursor] == '_')) {
                ++cursor;
            }
            const std::string member = line.substr(member_start, cursor - member_start);
            const bool action = action_member(member);
            if (member.empty() || ignored_runtime_target(target) ||
                (!action && (!data_member(member) || !likely_retool_identifier(target)))) {
                continue;
            }
            const int source_line = first_line + static_cast<int>(line_index);
            if (!emitted.emplace(source_line, target, member).second) {
                continue;
            }
            const std::string kind = action ? "call" : "reference";
            add_retool_fact(rows, profile, path, source_line, kind,
                            owner + " " + member + " " + target,
                            "owner=" + owner + " target=" + target + " member=" + member);
        }
    }
}

std::string payload_owner(const std::string& path) {
    return fs::path(path).stem().generic_string();
}

void scan_payload(const Profile& profile,
                  const std::string& path,
                  const std::vector<std::string>& lines,
                  std::vector<FactRow>& rows) {
    const std::string owner = payload_owner(path);
    const std::string extension = extension_of(path);
    add_retool_fact(rows, profile, path, 1, "payload", owner + " " + extension,
                    "owner=" + owner + " payload_type=" + extension,
                    static_cast<int>(std::max<std::size_t>(1, lines.size())));
    rows.back().symbol_name = owner;
    scan_runtime_references(profile, path, lines, owner, rows, 1);
}

std::string yaml_scalar(const std::vector<std::string>& lines, const std::string& key) {
    const std::string prefix = key + ":";
    for (const std::string& raw : lines) {
        if (starts_with(raw, prefix)) {
            std::string value = trim(raw.substr(prefix.size()));
            if (value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') ||
                                      (value.front() == '"' && value.back() == '"'))) {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }
    }
    return "";
}

std::string yaml_block_data_scalar(const std::vector<std::string>& lines, const std::string& key) {
    bool in_block_data = false;
    const std::string marker = key + ":";
    for (const std::string& raw : lines) {
        if (raw == "blockData:") {
            in_block_data = true;
            continue;
        }
        if (!in_block_data) {
            continue;
        }
        if (!raw.empty() && !is_ascii_space(static_cast<unsigned char>(raw.front()))) {
            break;
        }
        const std::string compact = trim(raw);
        if (starts_with(compact, marker)) {
            return trim(compact.substr(marker.size()));
        }
    }
    return "";
}

void scan_workflow_yaml(const Profile& profile,
                        const fs::path& root,
                        const std::string& path,
                        const std::vector<std::string>& lines,
                        std::vector<FactRow>& rows,
                        RelationWriter& relations) {
    const std::string workflow = workflow_owner(root, path);
    if (fs::path(path).filename() == "workflow.yml") {
        const std::string name = yaml_scalar(lines, "name");
        const std::string id = yaml_scalar(lines, "workflowId");
        const std::string version = yaml_scalar(lines, "version");
        add_retool_fact(rows, profile, path, 1, "workflow",
                        (name.empty() ? workflow : name) + " " + id,
                        "workflow=" + workflow + " name=" + name + " id=" + id + " version=" + version);
        return;
    }

    const std::string id = yaml_scalar(lines, "id");
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (!starts_with(lines[index], "id:")) continue;
        const std::string raw = trim(lines[index].substr(3));
        std::size_t cursor = 0;
        std::string symbol = static_string(raw, cursor);
        skip_space(raw, cursor);
        if (symbol.empty() && !raw.empty() &&
            std::all_of(raw.begin(), raw.end(), runtime_identifier_char)) {
            symbol = raw;
        } else if (cursor != raw.size()) {
            symbol.clear();
        }
        if (!symbol.empty()) relations.binding(workflow, symbol, path, "", static_cast<int>(index + 1));
    }
    const std::string type = yaml_scalar(lines, "type");
    const std::string subtype = yaml_scalar(lines, "subtype");
    const std::string resource_name = yaml_scalar(lines, "resourceName");
    const std::string resource_display = yaml_scalar(lines, "resourceDisplayName");
    const std::string uuid = yaml_block_data_scalar(lines, "uuid");
    const std::string block_id = id.empty() ? fs::path(path).stem().generic_string() : id;
    const bool external_resource = (!resource_name.empty() || !resource_display.empty()) &&
                                   !intrinsic_retool_resource(resource_name, resource_display);
    std::string summary = "workflow=" + workflow + " id=" + block_id + " uuid=" + uuid +
                          " type=" + type + " subtype=" + subtype;
    if (external_resource) {
        summary += " resource_name=" + resource_name + " resource_display=" + resource_display;
    }
    add_retool_fact(rows, profile, path, 1, "workflow-block",
                    block_id + " " + uuid + " " + subtype + " " + resource_display + " " + workflow,
                    summary);
    if (external_resource) {
        const std::string resource = resource_display.empty() ? resource_name : resource_display;
        add_retool_fact(rows, profile, path, 1, "resource-binding",
                        block_id + " uses " + resource + " workflow",
                        "workflow=" + workflow + " block=" + block_id + " resource_name=" + resource_name +
                            " resource_display=" + resource_display);
    }
    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
        const std::string compact = trim(lines[line_index]);
        if (!starts_with(compact, "blockId:")) {
            continue;
        }
        const std::string predecessor = trim(compact.substr(8));
        if (!predecessor.empty()) {
            add_retool_fact(rows, profile, path, static_cast<int>(line_index + 1), "workflow-edge",
                            predecessor + " to " + block_id + " " + workflow,
                            "workflow=" + workflow + " predecessor_uuid=" + predecessor +
                                " target=" + block_id + " target_uuid=" + uuid);
        }
    }
    scan_runtime_references(profile, path, lines, block_id, rows, 1);
}

void scan_retool_json(const Profile& profile,
                      const fs::path& root,
                      const std::string& path,
                      const std::string& content,
                      std::vector<FactRow>& rows) {
    Json json;
    if (!try_parse_json_text(content, json)) {
        return;
    }
    const fs::path file(path);
    if (starts_with(path, "resources/") && file.filename() == "resource.json") {
        const std::string name = file.parent_path().filename().generic_string();
        const std::string type = json_string_value(json, "type");
        const std::string uuid = json_string_value(json, "uuid");
        const std::string environment = json_string_value(json, "environment");
        add_retool_fact(rows, profile, path, 1, "resource",
                        name + " " + type + " " + uuid,
                        "resource=" + name + " type=" + type + " uuid=" + uuid +
                            " environment=" + environment);
        return;
    }
    if (starts_with(path, "apps/") && file.filename() == "metadata.json") {
        const std::string app = app_owner(root, path);
        add_retool_fact(rows, profile, path, 1, "app-metadata", app,
                        "app=" + app + " metadata=metadata.json");
        return;
    }
    if (starts_with(path, "apps/") &&
        (path.find("/.positions/") != std::string::npos || ends_with(path, "/.positions.json"))) {
        const std::string app = app_owner(root, path);
        const std::string layout = path.find("/.positions/") != std::string::npos ? "multi-page" : "single-page";
        add_retool_fact(rows, profile, path, 1, "layout",
                        app + " " + file.stem().generic_string(),
                        "app=" + app + " layout=" + layout + " positions=" + file.filename().generic_string());
    }
}

} // namespace

namespace {

std::string summary_field(const std::string& summary, const std::string& key) {
    const std::string marker = key + "=";
    const std::size_t start = summary.find(marker);
    if (start == std::string::npos) {
        return "";
    }
    const std::size_t value_start = start + marker.size();
    const std::size_t end = summary.find(' ', value_start);
    return summary.substr(value_start, end == std::string::npos ? std::string::npos : end - value_start);
}

std::string workflow_field(const std::string& summary) {
    constexpr std::string_view marker = "workflow=";
    if (!starts_with(summary, marker)) {
        return "";
    }
    for (const std::string_view next : {" id=", " predecessor_uuid="}) {
        const std::size_t end = summary.find(next, marker.size());
        if (end != std::string::npos) {
            return summary.substr(marker.size(), end - marker.size());
        }
    }
    return "";
}

} // namespace

Json retool_generation_environment(const fs::path& root, const std::vector<std::string>& paths) {
    Json result = Json::array();
    for (const auto& path : paths) {
        if (!starts_with(path, "apps/") && !starts_with(path, "workflows/") && !starts_with(path, "resources/")) continue;
        const auto app = app_owner(root, path);
        std::error_code error;
        const bool multi = fs::is_directory(root / app / ".positions", error);
        error.clear();
        const bool single = fs::is_regular_file(root / app / ".positions.json", error);
        result.push_back({path, app, workflow_owner(root, path), multi, single});
    }
    return result;
}

void scan_retool_source_facts(const Profile& profile,
                              const fs::path& root,
                              const std::string& path,
                              const std::string& content,
                              const std::vector<std::string>& lines,
                              std::vector<FactRow>& rows,
                              SourceRelationInputs& relations) {
    if (!starts_with(path, "apps/") && !starts_with(path, "resources/") && !starts_with(path, "workflows/")) {
        return;
    }
    RelationWriter writer{relations};
    const std::string extension = extension_of(path);
    if (extension == ".rsx") {
        scan_rsx(profile, root, path, content, rows, writer);
    } else if (starts_with(path, "workflows/") && (extension == ".yml" || extension == ".yaml")) {
        scan_workflow_yaml(profile, root, path, lines, rows, writer);
    } else if (extension == ".json") {
        scan_retool_json(profile, root, path, content, rows);
    } else if (extension == ".js" || extension == ".sql" || extension == ".query" ||
               extension == ".css" || extension == ".html") {
        scan_payload(profile, path, lines, rows);
        const std::string scope = starts_with(path, "workflows/") ? workflow_owner(root, path) : app_owner(root, path);
        if (extension == ".js") {
            scan_runtime_relations(content, path, scope, 1, writer);
        } else {
            scan_template_relations(content, path, scope, 1, extension == ".sql" || extension == ".query", writer);
        }
    }
}

void finalize_retool_source_facts(const Profile& profile, std::vector<FactRow>& rows) {
    std::unordered_map<std::string, std::string> block_by_uuid;
    for (const FactRow& row : rows) {
        if (row.record_type != "retool-workflow-block") {
            continue;
        }
        const std::string uuid = summary_field(row.summary, "uuid");
        const std::string id = summary_field(row.summary, "id");
        const std::string workflow = workflow_field(row.summary);
        if (!workflow.empty() && !uuid.empty() && !id.empty()) {
            block_by_uuid.emplace(workflow + '\n' + uuid, id);
        }
    }
    for (FactRow& row : rows) {
        if (row.record_type != "retool-workflow-edge") {
            continue;
        }
        const std::string predecessor_uuid = summary_field(row.summary, "predecessor_uuid");
        const std::string workflow = workflow_field(row.summary);
        const auto predecessor = block_by_uuid.find(workflow + '\n' + predecessor_uuid);
        if (predecessor == block_by_uuid.end()) {
            continue;
        }
        const std::string target = summary_field(row.summary, "target");
        row.title = "retool workflow-edge " + predecessor->second + " " + predecessor_uuid +
                    " to " + target + " " + workflow;
        row.summary += " predecessor=" + predecessor->second;
        row.topics = detect_topics(profile, row.path, row.title, row.summary);
    }
}

} // namespace shin::context
