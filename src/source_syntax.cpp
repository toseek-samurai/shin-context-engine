#include "source_syntax.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>

extern "C" const TSLanguage *tree_sitter_java(void);
extern "C" const TSLanguage *tree_sitter_typescript(void);
extern "C" const TSLanguage *tree_sitter_tsx(void);
extern "C" const TSLanguage *tree_sitter_cpp(void);

namespace shin::context {
namespace {

constexpr std::size_t kMaxSourceBytes = 1024U * 1024U;
constexpr std::size_t kMaxSyntaxNodes = 200000;
constexpr std::size_t kMaxProgressCallbacks = 100000;
constexpr uint32_t kInputChunkBytes = 4096;

enum class SyntaxLanguage {
    Unsupported,
    Java,
    TypeScript,
    Tsx,
    Cpp,
};

struct ParseInput {
    std::string_view content;
    std::size_t progress_callbacks = 0;
    bool cancelled = false;
};

using ParserPtr = std::unique_ptr<TSParser, decltype(&ts_parser_delete)>;
using TreePtr = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

class TreeCursor {
public:
    explicit TreeCursor(TSNode node) : cursor_(ts_tree_cursor_new(node)) {}
    ~TreeCursor() { ts_tree_cursor_delete(&cursor_); }

    TreeCursor(const TreeCursor &) = delete;
    TreeCursor &operator=(const TreeCursor &) = delete;

    TSTreeCursor *get() noexcept { return &cursor_; }

private:
    TSTreeCursor cursor_;
};

SyntaxLanguage syntax_language(std::string_view extension) noexcept {
    if (extension == ".java") return SyntaxLanguage::Java;
    if (extension == ".ts" || extension == ".cts" || extension == ".mts" ||
        extension == ".js" || extension == ".cjs" || extension == ".mjs") {
        return SyntaxLanguage::TypeScript;
    }
    if (extension == ".tsx" || extension == ".jsx") return SyntaxLanguage::Tsx;
    if (extension == ".cpp" || extension == ".cc" || extension == ".cxx" ||
        extension == ".h" || extension == ".hh" || extension == ".hpp" ||
        extension == ".hxx" || extension == ".ipp" || extension == ".inl") return SyntaxLanguage::Cpp;
    return SyntaxLanguage::Unsupported;
}

const TSLanguage *tree_sitter_language(SyntaxLanguage language) noexcept {
    switch (language) {
        case SyntaxLanguage::Java: return tree_sitter_java();
        case SyntaxLanguage::TypeScript: return tree_sitter_typescript();
        case SyntaxLanguage::Tsx: return tree_sitter_tsx();
        case SyntaxLanguage::Cpp: return tree_sitter_cpp();
        case SyntaxLanguage::Unsupported: return nullptr;
    }
    return nullptr;
}

const char *read_parse_input(void *payload,
                             uint32_t byte_index,
                             TSPoint,
                             uint32_t *bytes_read) noexcept {
    auto *input = static_cast<ParseInput *>(payload);
    if (byte_index >= input->content.size()) {
        *bytes_read = 0;
        return nullptr;
    }
    const std::size_t available = input->content.size() - byte_index;
    *bytes_read = static_cast<uint32_t>(std::min<std::size_t>(available, kInputChunkBytes));
    return input->content.data() + byte_index;
}

bool stop_at_progress_budget(TSParseState *state) noexcept {
    auto *input = static_cast<ParseInput *>(state->payload);
    ++input->progress_callbacks;
    if (input->progress_callbacks > kMaxProgressCallbacks) {
        input->cancelled = true;
        return true;
    }
    return false;
}

TreePtr parse_syntax_tree(ParseInput& input, SyntaxLanguage language) {
    ParserPtr parser(ts_parser_new(), ts_parser_delete);
    const TSLanguage* grammar = tree_sitter_language(language);
    if (!parser || grammar == nullptr || !ts_parser_set_language(parser.get(), grammar))
        return TreePtr(nullptr, ts_tree_delete);
    TSInput source{&input, read_parse_input, TSInputEncodingUTF8, nullptr};
    TSParseOptions options{&input, stop_at_progress_budget};
    return TreePtr(ts_parser_parse_with_options(parser.get(), nullptr, source, options), ts_tree_delete);
}

int source_line(TSPoint point) noexcept {
    return static_cast<int>(point.row) + 1;
}

int source_end_line(TSPoint point) noexcept {
    return std::max(1, static_cast<int>(point.row) + (point.column == 0 ? 0 : 1));
}

bool byte_range_valid(TSNode node, std::string_view content) noexcept {
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    return start <= end && end <= content.size();
}

std::string node_text(TSNode node, std::string_view content) {
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    return std::string(content.substr(start, end - start));
}

bool is_safe_declaration_name(TSNode node) noexcept {
    const std::string_view type(ts_node_type(node));
    return type == "identifier" || type == "type_identifier" ||
           type == "property_identifier" || type == "private_property_identifier";
}

TSNode declaration_name(TSNode node) noexcept {
    return ts_node_child_by_field_name(node, "name", 4);
}

std::string_view java_declaration_kind(std::string_view type) noexcept {
    if (type == "class_declaration") return "class";
    if (type == "interface_declaration") return "interface";
    if (type == "enum_declaration") return "enum";
    if (type == "record_declaration") return "record";
    if (type == "method_declaration") return "method";
    if (type == "constructor_declaration" || type == "compact_constructor_declaration") {
        return "constructor";
    }
    return {};
}

std::string_view typescript_declaration_kind(TSNode node) noexcept {
    const std::string_view type(ts_node_type(node));
    if (type == "class_declaration") return "class";
    if (type == "interface_declaration") return "interface";
    if (type == "enum_declaration") return "enum";
    if (type == "type_alias_declaration") return "type";
    if (type == "function_declaration") return "function";
    if (type == "method_definition" || type == "method_signature" ||
        type == "abstract_method_signature") {
        return "method";
    }
    if (type != "variable_declarator") return {};
    const TSNode value = ts_node_child_by_field_name(node, "value", 5);
    return !ts_node_is_null(value) && std::string_view(ts_node_type(value)) == "arrow_function"
               ? "arrow-function"
               : std::string_view{};
}

void append_declaration(SyntaxFacts &facts,
                        TSNode node,
                        std::string_view kind,
                        std::string_view content,
                        SyntaxLanguage language) {
    if (kind.empty() || ts_node_has_error(node) || !byte_range_valid(node, content)) return;
    const TSNode name = declaration_name(node);
    if (ts_node_is_null(name) || !is_safe_declaration_name(name) || !byte_range_valid(name, content)) return;
    TSNode range_start = node;
    if (language != SyntaxLanguage::Java) {
        for (TSNode previous = ts_node_prev_named_sibling(range_start);
             !ts_node_is_null(previous) && std::string_view(ts_node_type(previous)) == "decorator";
             previous = ts_node_prev_named_sibling(range_start)) {
            range_start = previous;
        }
    }
    const TSPoint start = ts_node_start_point(range_start);
    const TSPoint end = ts_node_end_point(node);
    const TSNode parent = ts_node_parent(node);
    const bool top_level = language == SyntaxLanguage::Java && !ts_node_is_null(parent) &&
                           std::string_view(ts_node_type(parent)) == "program";
    facts.declarations.push_back({node_text(name, content), std::string(kind), top_level,
                                  source_line(ts_node_start_point(name)), source_line(start),
                                  source_end_line(end)});
}

TSNode java_qualified_child(TSNode node) noexcept {
    const uint32_t child_count = ts_node_named_child_count(node);
    for (uint32_t index = 0; index < child_count; ++index) {
        const TSNode child = ts_node_named_child(node, index);
        const std::string_view type(ts_node_type(child));
        if (type == "identifier" || type == "scoped_identifier") return child;
    }
    return {};
}

void inspect_java_package(SyntaxFacts &facts, TSNode node, std::string_view content) {
    if (!facts.package_name.empty() || ts_node_has_error(node)) return;
    const TSNode name = java_qualified_child(node);
    if (!ts_node_is_null(name) && byte_range_valid(name, content)) {
        facts.package_name = node_text(name, content);
        facts.package_line = source_line(ts_node_start_point(name));
    }
}

bool java_import_is_explicit(TSNode node) noexcept {
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t index = 0; index < child_count; ++index) {
        const std::string_view type(ts_node_type(ts_node_child(node, index)));
        if (type == "static" || type == "asterisk") return false;
    }
    return true;
}

void inspect_java_import(SyntaxFacts &facts, TSNode node, std::string_view content) {
    if (ts_node_has_error(node) || !java_import_is_explicit(node)) return;
    const TSNode target = java_qualified_child(node);
    if (ts_node_is_null(target) || !byte_range_valid(target, content)) return;
    facts.imports.push_back({node_text(target, content), false, source_line(ts_node_start_point(target))});
}

bool plain_relative_literal(TSNode node, std::string_view content, std::string &target) {
    if (!byte_range_valid(node, content)) return false;
    const std::string_view literal = content.substr(ts_node_start_byte(node), ts_node_end_byte(node) - ts_node_start_byte(node));
    if (literal.size() < 3 || (literal.front() != '\'' && literal.front() != '"') ||
        literal.back() != literal.front()) {
        return false;
    }
    const std::string_view value = literal.substr(1, literal.size() - 2);
    if (value.find_first_of("\\\r\n") != std::string_view::npos ||
        !(value.starts_with("./") || value.starts_with("../"))) {
        return false;
    }
    target.assign(value);
    return true;
}

void inspect_typescript_import(SyntaxFacts &facts, TSNode node, std::string_view content) {
    if (ts_node_has_error(node)) return;
    const TSNode source = ts_node_child_by_field_name(node, "source", 6);
    if (ts_node_is_null(source) || std::string_view(ts_node_type(source)) != "string") return;
    std::string target;
    if (plain_relative_literal(source, content, target)) {
        facts.imports.push_back({std::move(target), true, source_line(ts_node_start_point(source))});
    }
}

TSNode cpp_declarator_child(TSNode node) noexcept {
    const TSNode child = ts_node_child_by_field_name(node, "declarator", 10);
    if (!ts_node_is_null(child)) return child;
    const std::string_view type(ts_node_type(node));
    if (type == "parenthesized_declarator" || type == "reference_declarator") {
        for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
            const TSNode nested = ts_node_named_child(node, i);
            if (!ts_node_is_extra(nested)) return nested;
        }
    }
    return {};
}

TSNode cpp_function_name(SyntaxFacts& facts, TSNode node) noexcept {
    bool nearest_function = false;
    for (int depth = 0; depth < 64 && !ts_node_is_null(node); ++depth) {
        const std::string_view type(ts_node_type(node));
        if (type == "qualified_identifier" && !nearest_function) {
            TSNode leaf = node;
            for (int nested = 0; nested < 64 && !ts_node_is_null(leaf); ++nested) {
                const std::string_view leaf_type(ts_node_type(leaf));
                if (leaf_type == "operator_cast") return node;
                if (leaf_type != "qualified_identifier") return {};
                leaf = declaration_name(leaf);
            }
            if (!ts_node_is_null(leaf)) facts.complete = false;
            return {};
        }
        if (type == "identifier" || type == "field_identifier" || type == "qualified_identifier" ||
            type == "operator_name" || type == "destructor_name" || type == "template_function" ||
            type == "template_method") return nearest_function ? node : TSNode{};
        if (type == "operator_cast") return node;
        if (type == "function_declarator") nearest_function = true;
        else if (type == "pointer_declarator" || type == "reference_declarator" ||
                 type == "array_declarator") nearest_function = false;
        else if (type != "parenthesized_declarator") return {};
        node = cpp_declarator_child(node);
    }
    if (!ts_node_is_null(node)) facts.complete = false;
    return {};
}

void append_cpp_declaration(SyntaxFacts& facts, TSNode node, TSNode name,
                            TSNode header_end, std::string_view kind, std::string_view content) {
    if (ts_node_is_null(name) || ts_node_has_error(node) || ts_node_is_missing(name) ||
        !byte_range_valid(node, content) || !byte_range_valid(name, content)) return;
    const TSNode original_name = name;
    for (int depth = 0;; ++depth) {
        const std::string_view type(ts_node_type(name));
        if (type != "qualified_identifier" && type != "template_function" &&
            type != "template_method" && type != "template_type") break;
        if (depth == 64) { facts.complete = false; return; }
        name = declaration_name(name);
        if (ts_node_is_null(name)) return;
    }
    std::string symbol = node_text(name, content);
    if (std::string_view(ts_node_type(name)) == "operator_cast") {
        TSNode part = cpp_declarator_child(name);
        TSNode parameters{};
        for (int depth = 0; depth < 64 && !ts_node_is_null(part); ++depth) {
            parameters = ts_node_child_by_field_name(part, "parameters", 10);
            if (!ts_node_is_null(parameters)) break;
            part = cpp_declarator_child(part);
        }
        if (ts_node_is_null(parameters)) return;
        symbol = std::string(content.substr(ts_node_start_byte(name),
            ts_node_start_byte(parameters) - ts_node_start_byte(name)));
    }
    if (symbol.empty() || symbol.size() > 1024) { facts.complete = false; return; }
    if (symbol.find_first_of("\"'\\\r\n") != std::string::npos) return;
    TSNode range = node;
    for (int depth = 0; depth < 64; ++depth) {
        const TSNode parent = ts_node_parent(range);
        if (ts_node_is_null(parent)) break;
        const std::string_view type(ts_node_type(parent));
        if (type != "template_declaration" && type != "friend_declaration") break;
        if (ts_node_has_error(parent)) return;
        range = parent;
        if (depth == 63) { facts.complete = false; return; }
    }
    // Names and owners are syntactic observations, not resolved C++ symbols.
    std::string qualified = node_text(original_name, content);
    TSNode parent = ts_node_parent(node);
    for (int depth = 0; !ts_node_is_null(parent); ++depth) {
        if (depth == 64) { facts.complete = false; return; }
        const std::string_view type(ts_node_type(parent));
        if (type == "namespace_definition" || type == "class_specifier" || type == "struct_specifier") {
            const TSNode owner = declaration_name(parent);
            if (!ts_node_is_null(owner) && byte_range_valid(owner, content))
                qualified = node_text(owner, content) + "::" + qualified;
        }
        if (qualified.size() > 4096) { facts.complete = false; return; }
        parent = ts_node_parent(parent);
    }
    const uint32_t end = ts_node_end_byte(header_end);
    facts.declarations.push_back({symbol, std::string(kind), false,
        source_line(ts_node_start_point(name)), source_line(ts_node_start_point(range)),
        source_end_line(ts_node_end_point(node)), ts_node_start_byte(original_name),
        std::string(content.substr(ts_node_start_byte(range), end - ts_node_start_byte(range))), qualified});
}

void inspect_cpp_declaration(SyntaxFacts& facts, TSNode node, std::string_view content) {
    const std::string_view type(ts_node_type(node));
    std::string_view kind;
    if (type == "class_specifier") kind = "class";
    else if (type == "struct_specifier") kind = "struct";
    else if (type == "enum_specifier") kind = "enum";
    else if (type == "namespace_definition") kind = "namespace";
    if (!kind.empty()) {
        const TSNode name = declaration_name(node);
        // A type reference such as a `struct stat` parameter is not a definition.
        if (!ts_node_is_null(name) && !ts_node_is_null(ts_node_child_by_field_name(node, "body", 4)))
            append_cpp_declaration(facts, node, name, name, kind, content);
        return;
    }
    if (type != "function_definition" && type != "declaration" && type != "field_declaration") return;
    for (uint32_t i = 0; i < ts_node_child_count(node); ++i) {
        const char* field = ts_node_field_name_for_child(node, i);
        if (field == nullptr || std::string_view(field) != "declarator") continue;
        const TSNode declarator = ts_node_child(node, i);
        const TSNode name = cpp_function_name(facts, declarator);
        if (!ts_node_is_null(name)) append_cpp_declaration(facts, node, name, declarator, "function", content);
    }
}

void visit_tree(SyntaxFacts &facts, TSTree *tree, SyntaxLanguage language, std::string_view content) {
    TreeCursor cursor(ts_tree_root_node(tree));
    std::size_t visited = 0;
    for (;;) {
        if (++visited > kMaxSyntaxNodes) {
            facts.complete = false;
            break;
        }
        const TSNode node = ts_tree_cursor_current_node(cursor.get());
        const std::string_view type(ts_node_type(node));
        if (type == "ERROR" || ts_node_is_missing(node)) ++facts.error_nodes;

        if (language == SyntaxLanguage::Java) {
            append_declaration(facts, node, java_declaration_kind(type), content, language);
            if (type == "package_declaration") inspect_java_package(facts, node, content);
            if (type == "import_declaration") inspect_java_import(facts, node, content);
        } else if (language == SyntaxLanguage::Cpp) {
            inspect_cpp_declaration(facts, node, content);
        } else {
            append_declaration(facts, node, typescript_declaration_kind(node), content, language);
            if (type == "import_statement" || type == "export_statement") {
                inspect_typescript_import(facts, node, content);
            }
        }

        if (ts_tree_cursor_goto_first_child(cursor.get())) continue;
        while (!ts_tree_cursor_goto_next_sibling(cursor.get())) {
            if (!ts_tree_cursor_goto_parent(cursor.get())) return;
        }
    }
}

}  // namespace

SyntaxFacts inspect_source_syntax(std::string_view content, std::string_view extension) {
    SyntaxFacts facts;
    const SyntaxLanguage language = syntax_language(extension);
    if (language == SyntaxLanguage::Unsupported) return facts;
    facts.supported = true;
    if (content.size() > kMaxSourceBytes || content.size() > UINT32_MAX) return facts;

    ParseInput input{content};
    TreePtr tree = parse_syntax_tree(input, language);
    if (!tree || input.cancelled) return facts;

    facts.complete = true;
    visit_tree(facts, tree.get(), language, content);
    if (facts.error_nodes != 0) facts.complete = false;
    return facts;
}

SyntaxChunking chunk_source_syntax(std::string_view content, std::string_view extension,
                                  std::size_t max_bytes, std::size_t max_chunks) {
    if (max_bytes < 4 || max_bytes > 65536 || max_chunks == 0 || max_chunks > 16384)
        throw std::invalid_argument("Invalid syntax chunk budget.");
    SyntaxChunking result;
    const auto language = syntax_language(extension);
    result.supported = language != SyntaxLanguage::Unsupported;
    if (content.size() > kMaxSourceBytes) return result;
    ParseInput input{content};
    TreePtr tree = result.supported ? parse_syntax_tree(input, language) : TreePtr(nullptr, ts_tree_delete);
    result.syntax_complete = tree && !input.cancelled && !ts_node_has_error(ts_tree_root_node(tree.get()));
    const auto emit = [&](std::size_t begin, std::size_t end, bool fallback) {
        if (begin == end) return true;
        if (!result.chunks.empty() && result.chunks.back().end == begin &&
            end - result.chunks.back().begin <= max_bytes) {
            result.chunks.back().end = end;
            result.chunks.back().fallback = result.chunks.back().fallback || fallback;
            return true;
        }
        if (result.chunks.size() == max_chunks) return false;
        result.chunks.push_back({begin, end, fallback});
        return true;
    };
    const auto window = [&](std::size_t begin, std::size_t end) {
        while (begin < end) {
            std::size_t cut = std::min(end, begin + max_bytes);
            if (cut < end) {
                while (cut > begin && (static_cast<unsigned char>(content[cut]) & 0xc0) == 0x80) --cut;
                if (cut == begin) return false;
                const auto newline = content.substr(begin, cut - begin).rfind('\n');
                if (newline != std::string_view::npos) cut = begin + newline + 1;
            }
            if (!emit(begin, cut, true)) return false;
            begin = cut;
        }
        return true;
    };
    if (!result.syntax_complete) {
        result.coverage_complete = window(0, content.size());
        return result;
    }

    struct Pending { TSNode node; std::size_t begin; std::size_t end; unsigned depth; };
    std::vector<Pending> pending{{ts_tree_root_node(tree.get()), 0, content.size(), 0}};
    while (!pending.empty()) {
        const auto part = pending.back();
        pending.pop_back();
        if (part.begin == part.end) continue;
        if (part.end - part.begin <= max_bytes) {
            if (!emit(part.begin, part.end, false)) return result;
            continue;
        }
        const auto node_begin = ts_node_start_byte(part.node), node_end = ts_node_end_byte(part.node);
        if (node_begin >= part.begin && node_end <= part.end && node_end > node_begin &&
            node_end - node_begin <= max_bytes) {
            // Large surrounding whitespace must not force a fitting syntax
            // node apart merely because it owns that node's leading gap.
            if (!window(part.begin, node_begin) || !emit(node_begin, node_end, false) ||
                !window(node_end, part.end)) return result;
            continue;
        }
        if (part.depth == 64 || result.visited_nodes >= kMaxSyntaxNodes) {
            result.syntax_complete = false;
            if (!window(part.begin, part.end)) return result;
            continue;
        }
        std::vector<Pending> children;
        std::size_t position = part.begin;
        bool valid = true;
        TreeCursor cursor(part.node);
        if (ts_tree_cursor_goto_first_child(cursor.get())) do {
            if (++result.visited_nodes > kMaxSyntaxNodes) { valid = false; break; }
            const auto child = ts_tree_cursor_current_node(cursor.get());
            if (!ts_node_is_named(child)) continue;
            const auto start = ts_node_start_byte(child), end = ts_node_end_byte(child);
            if (end == start) continue;
            if (start < position || end > part.end || end < start) { valid = false; break; }
            // Leading gaps and the final suffix keep punctuation/comments and
            // whitespace in exactly one piece, without a separate text copy.
            children.push_back({child, position, end, part.depth + 1});
            position = end;
        } while (ts_tree_cursor_goto_next_sibling(cursor.get()));
        if (!valid || children.empty()) {
            if (!valid) result.syntax_complete = false;
            if (!window(part.begin, part.end)) return result;
            continue;
        }
        children.back().end = part.end;
        for (auto it = children.rbegin(); it != children.rend(); ++it) pending.push_back(*it);
    }
    result.coverage_complete = true;
    return result;
}

}  // namespace shin::context
