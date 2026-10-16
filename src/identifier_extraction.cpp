#include "context_internal.h"

namespace shin::context {

namespace {

struct SourceIdentifierOccurrence {
    std::string name;
    int line = 1;
};

std::vector<std::string> sanitize_identifier_source_lines(
    const std::vector<std::string>& lines) {
    std::vector<std::string> sanitized;
    sanitized.reserve(lines.size());
    bool block_comment = false;
    char multiline_quote = 0;
    for (const std::string& line : lines) {
        std::string output(line.size(), ' ');
        char quote = multiline_quote;
        bool escaped = false;
        for (std::size_t index = 0; index < line.size(); ++index) {
            const char current = line[index];
            const char next = index + 1 < line.size() ? line[index + 1] : '\0';
            if (block_comment) {
                if (current == '*' && next == '/') {
                    block_comment = false;
                    ++index;
                }
                continue;
            }
            if (quote != 0) {
                if (escaped) {
                    escaped = false;
                } else if (current == '\\') {
                    escaped = true;
                } else if (current == quote) {
                    quote = 0;
                }
                continue;
            }
            if (current == '/' && next == '/') {
                break;
            }
            if (current == '/' && next == '*') {
                block_comment = true;
                ++index;
                continue;
            }
            if (current == '\'' || current == '"' || current == '`') {
                quote = current;
                continue;
            }
            output[index] = current;
        }
        multiline_quote = quote == '`' ? quote : '\0';
        sanitized.push_back(std::move(output));
    }
    return sanitized;
}

bool source_identifier_start(char value) {
    const unsigned char c = static_cast<unsigned char>(value);
    return is_ascii_upper(c) || is_ascii_lower(c) || value == '_' || value == '$';
}

bool source_identifier_body(char value) {
    const unsigned char c = static_cast<unsigned char>(value);
    return is_ascii_alnum(c) || value == '_' || value == '$';
}

bool source_identifier_keyword(std::string_view value) {
    static const std::set<std::string> keywords = {
        "abstract", "as", "async", "await", "base", "bool", "boolean", "break", "byte",
        "case", "catch", "char", "class", "const", "continue", "decimal", "default",
        "delegate", "delete", "do", "double", "else", "enum", "event", "export", "extends",
        "false", "final", "finally", "float", "for", "foreach", "from", "fun", "function",
        "get", "goto", "if", "implements", "import", "in", "inline", "instanceof", "int",
        "interface", "internal", "is", "let", "long", "namespace", "native", "new", "null",
        "object", "operator", "out", "override", "package", "private", "protected", "public",
        "readonly", "record", "ref", "return", "sealed", "set", "short", "signed", "sizeof",
        "static", "string", "struct", "super", "suspend", "switch", "synchronized", "template",
        "this", "throw", "throws", "transient", "true", "try", "type", "typeof", "uint", "ulong",
        "undefined", "union", "unsigned", "using", "var", "virtual", "void", "volatile", "when",
        "while", "with", "yield"
    };
    return keywords.find(to_lower(std::string(value))) != keywords.end();
}

std::vector<SourceIdentifierOccurrence> collect_source_identifiers(
    const std::vector<std::string>& sanitized) {
    std::vector<SourceIdentifierOccurrence> occurrences;
    for (std::size_t line_index = 0; line_index < sanitized.size(); ++line_index) {
        const std::string& line = sanitized[line_index];
        std::size_t index = 0;
        while (index < line.size()) {
            if (!source_identifier_start(line[index])) {
                ++index;
                continue;
            }
            const std::size_t start = index++;
            while (index < line.size() && source_identifier_body(line[index])) {
                ++index;
            }
            std::string identifier = line.substr(start, index - start);
            if (identifier.size() >= 2 && identifier.size() <= 128 &&
                identifier != "__" && !source_identifier_keyword(identifier)) {
                occurrences.push_back({std::move(identifier), static_cast<int>(line_index + 1)});
            }
        }
    }
    return occurrences;
}

int source_fact_scope_end(const std::vector<std::string>& sanitized, int start_line) {
    if (start_line < 1 || static_cast<std::size_t>(start_line) > sanitized.size()) {
        return std::max(1, start_line);
    }
    std::size_t open_line = std::string::npos;
    std::size_t open_column = std::string::npos;
    const std::size_t first = static_cast<std::size_t>(start_line - 1);
    const std::size_t search_end = std::min(sanitized.size(), first + 8);
    bool terminated = false;
    for (std::size_t line_index = first; line_index < search_end && !terminated; ++line_index) {
        const std::string& line = sanitized[line_index];
        for (std::size_t column = 0; column < line.size(); ++column) {
            if (line[column] == '{') {
                open_line = line_index;
                open_column = column;
                terminated = true;
                break;
            }
            if (line[column] == ';') {
                terminated = true;
                break;
            }
        }
    }
    if (open_line == std::string::npos) {
        return start_line;
    }
    int depth = 0;
    for (std::size_t line_index = open_line; line_index < sanitized.size(); ++line_index) {
        const std::string& line = sanitized[line_index];
        const std::size_t begin = line_index == open_line ? open_column : 0;
        for (std::size_t column = begin; column < line.size(); ++column) {
            if (line[column] == '{') {
                ++depth;
            } else if (line[column] == '}' && --depth == 0) {
                return static_cast<int>(line_index + 1);
            }
        }
    }
    return start_line;
}

int source_fact_scope_priority(const FactRow& row) {
    if (contains_any(row.record_type,
                     {"arrow-function", "function", "method", "hook", "component", "test-case"})) {
        return 4;
    }
    if (contains_any(row.record_type, {"property", "delegate", "constructor"})) {
        return 3;
    }
    if (contains_any(row.record_type,
                     {"class", "interface", "record", "struct", "enum", "object"})) {
        return 2;
    }
    return 1;
}

void push_unique_identifier(std::vector<std::string>& identifiers,
                            const std::string& identifier) {
    if (std::find(identifiers.begin(), identifiers.end(), identifier) == identifiers.end()) {
        identifiers.push_back(identifier);
    }
}

void add_identifier_window_fact(const Profile& profile,
                                const std::string& path,
                                const std::string& language,
                                int start_line,
                                int end_line,
                                int part,
                                std::vector<std::string> identifiers,
                                std::vector<FactRow>& rows) {
    std::string summary;
    for (const std::string& identifier : identifiers) {
        if (!summary.empty()) {
            summary.push_back(' ');
        }
        summary += identifier;
    }
    const std::string name = "lines-" + std::to_string(start_line) + "-" +
                             std::to_string(end_line) + "-part-" + std::to_string(part);
    add_source_fact_row(rows, profile, path, start_line, language, "identifier-window",
                        name, "identifier references " + summary);
    FactRow& row = rows.back();
    row.start_line = start_line;
    row.end_line = end_line;
    row.identifier_terms = std::move(identifiers);
    row.range_origin = "lexical-window";
}

}  // namespace

void enrich_language_source_facts(const Profile& profile,
                                  const std::string& path,
                                  const std::string& language,
                                  const std::vector<std::string>& lines,
                                  std::vector<FactRow>& rows) {
    constexpr std::size_t kMaxIdentifiersPerFact = 256;
    constexpr int kFallbackWindowLines = 24;
    constexpr std::size_t kMaxIdentifiersPerWindow = 96;
    const std::vector<std::string> sanitized = sanitize_identifier_source_lines(lines);
    const std::vector<SourceIdentifierOccurrence> occurrences = collect_source_identifiers(sanitized);
    std::vector<std::size_t> scope_rows;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        FactRow& row = rows[index];
        if (row.path != path || row.source != "source-fact:" + language) {
            continue;
        }
        if (row.range_origin.empty()) {
            row.start_line = row.line;
            row.end_line = source_fact_scope_priority(row) > 1
                ? source_fact_scope_end(sanitized, row.line) : row.line;
            row.range_origin = "lexical";
        }
        scope_rows.push_back(index);
    }

    std::map<int, std::vector<std::string>> fallback_windows;
    for (const SourceIdentifierOccurrence& occurrence : occurrences) {
        std::optional<std::size_t> selected;
        int selected_span = std::numeric_limits<int>::max();
        int selected_priority = -1;
        for (std::size_t row_index : scope_rows) {
            const FactRow& row = rows[row_index];
            if (occurrence.line < row.start_line || occurrence.line > row.end_line) {
                continue;
            }
            const int span = row.end_line - row.start_line;
            const int priority = source_fact_scope_priority(row);
            if (span < selected_span || (span == selected_span && priority > selected_priority)) {
                selected = row_index;
                selected_span = span;
                selected_priority = priority;
            }
        }
        if (selected.has_value()) {
            std::vector<std::string>& identifiers = rows[*selected].identifier_terms;
            if (std::find(identifiers.begin(), identifiers.end(), occurrence.name) != identifiers.end()) {
                continue;
            }
            if (identifiers.size() < kMaxIdentifiersPerFact) {
                identifiers.push_back(occurrence.name);
                continue;
            }
        }
        const int window = (occurrence.line - 1) / kFallbackWindowLines;
        push_unique_identifier(fallback_windows[window], occurrence.name);
    }

    for (auto& [window, identifiers] : fallback_windows) {
        const int start_line = window * kFallbackWindowLines + 1;
        const int end_line = std::min(static_cast<int>(lines.size()),
                                      start_line + kFallbackWindowLines - 1);
        int part = 1;
        for (std::size_t offset = 0; offset < identifiers.size();
             offset += kMaxIdentifiersPerWindow, ++part) {
            const std::size_t end = std::min(identifiers.size(), offset + kMaxIdentifiersPerWindow);
            add_identifier_window_fact(
                profile, path, language, start_line, end_line, part,
                std::vector<std::string>(identifiers.begin() + static_cast<std::ptrdiff_t>(offset),
                                         identifiers.begin() + static_cast<std::ptrdiff_t>(end)),
                rows);
        }
    }
}

}  // namespace shin::context
