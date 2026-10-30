#include "objc_syntax.h"

#include <algorithm>
#include <cstring>

namespace shin::context {
namespace {

std::string_view type_of(TSNode node) { return ts_node_type(node); }
TSNode field(TSNode node, const char* name) {
    return ts_node_child_by_field_name(node, name, static_cast<uint32_t>(std::strlen(name)));
}
bool valid(TSNode node, std::string_view content) {
    return !ts_node_is_null(node) && !ts_node_is_missing(node) &&
        ts_node_start_byte(node) <= ts_node_end_byte(node) && ts_node_end_byte(node) <= content.size();
}
std::string text(TSNode node, std::string_view content) {
    return valid(node, content) ? std::string(content.substr(ts_node_start_byte(node),
        ts_node_end_byte(node) - ts_node_start_byte(node))) : std::string{};
}
bool owner_node(TSNode node) {
    const auto type = type_of(node);
    return type == "objc_class_interface" || type == "objc_class_implementation" ||
        type == "objc_protocol_declaration";
}
std::string owner_name(TSNode node, std::string_view content) {
    auto result = text(field(node, "name"), content);
    const auto category = field(node, "category");
    if (!ts_node_is_null(category)) result += "(" + text(field(category, "name"), content) + ")";
    return result;
}
std::string qualify(SyntaxFacts& facts, TSNode node, std::string name, std::string_view content) {
    auto parent = ts_node_parent(node);
    for (int depth = 0; !ts_node_is_null(parent); ++depth) {
        if (depth == 64 || name.size() > 4096) { facts.complete = false; return {}; }
        if (owner_node(parent)) {
            const auto owner = owner_name(parent, content);
            if (!owner.empty()) name = owner + "::" + name;
        }
        else if (type_of(parent) == "namespace_definition")
            name = text(field(parent, "name"), content) + "::" + name;
        parent = ts_node_parent(parent);
    }
    return name;
}
void append(SyntaxFacts& facts, TSNode node, TSNode name_node, std::string name,
            std::string_view kind, uint32_t header_end, std::string_view content,
            std::string qualified = {}) {
    if (!valid(node, content) || !valid(name_node, content) || ts_node_has_error(node)) return;
    if (name.empty() || name.size() > 1024 || name.find_first_of("\"'\\\r\n") != std::string::npos) return;
    const auto begin = ts_node_start_byte(node);
    if (header_end < begin || header_end > ts_node_end_byte(node)) return;
    if (qualified.empty()) qualified = qualify(facts, node, name, content);
    const auto start = ts_node_start_point(node), end = ts_node_end_point(node);
    const auto name_at = ts_node_start_point(name_node);
    facts.declarations.push_back({name, std::string(kind), type_of(ts_node_parent(node)) == "translation_unit",
        static_cast<int>(name_at.row + 1), static_cast<int>(start.row + 1),
        std::max(1, static_cast<int>(end.row) + (end.column == 0 ? 0 : 1)),
        ts_node_start_byte(name_node), std::string(content.substr(begin, header_end - begin)), qualified});
}
TSNode property_name(SyntaxFacts& facts, TSNode node) {
    for (int depth = 0; !ts_node_is_null(node); ++depth) {
        if (depth == 64) { facts.complete = false; return {}; }
        const auto type = type_of(node);
        if (type == "identifier" || type == "field_identifier") return node;
        auto next = field(node, "declarator");
        if (ts_node_is_null(next) && type == "parenthesized_declarator") {
            for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
                const auto child = ts_node_named_child(node, i);
                if (type_of(child) == "comment") continue;
                if (!ts_node_is_null(next)) return {};
                next = child;
            }
        }
        node = next;
    }
    return {};
}

} // namespace

bool is_objective_c_node(TSNode node) noexcept {
    const std::string_view type = ts_node_type(node);
    return type == "objc_class_interface" || type == "objc_class_implementation" ||
        type == "objc_protocol_declaration" || type == "objc_class_forward_declaration" ||
        type == "objc_protocol_forward_declaration" || type == "objc_compatibility_alias" ||
        type == "objc_method_definition" || type == "objc_method_declaration" ||
        type == "objc_property_declaration" || type == "message_expression" ||
        type == "objc_string_literal" || type == "objc_selector_expression" ||
        type == "objc_module_import";
}

void inspect_objective_c_declaration(SyntaxFacts& facts, TSNode node, std::string_view content) {
    const auto type = type_of(node);
    if (owner_node(node)) {
        const auto name = field(node, "name");
        auto kind = type == "objc_protocol_declaration" ? "protocol" :
            type == "objc_class_interface" ? "interface" : "implementation";
        const auto category = field(node, "category");
        if (!ts_node_is_null(category))
            kind = ts_node_is_null(field(category, "name")) ? "extension" : "category";
        const auto body = field(node, "body"), ivars = field(node, "ivars");
        uint32_t header_end = ts_node_end_byte(node);
        if (!ts_node_is_null(body)) header_end = std::min(header_end, ts_node_start_byte(body));
        if (!ts_node_is_null(ivars)) header_end = std::min(header_end, ts_node_start_byte(ivars));
        append(facts, node, name, owner_name(node, content), kind, header_end, content);
    } else if (type == "objc_method_definition" || type == "objc_method_declaration") {
        const auto selector = field(node, "selector");
        if (!valid(selector, content)) return;
        std::string name = text(field(selector, "name"), content);
        if (name.empty()) {
            const auto count = ts_node_named_child_count(selector);
            for (uint32_t i = 0; i < count; ++i) {
                const auto parameter = ts_node_named_child(selector, i);
                if (type_of(parameter) != "objc_method_parameter") continue;
                name += text(field(parameter, "keyword"), content) + ":";
                if (name.size() > 1024) { facts.complete = false; return; }
            }
        }
        const auto body = field(node, "body");
        const auto end = ts_node_is_null(body) ? ts_node_end_byte(node) : ts_node_start_byte(body);
        append(facts, node, selector, name, "method", end, content,
               qualify(facts, node, text(field(node, "scope"), content) + name, content));
    } else if (type == "objc_property_declaration") {
        const auto count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            const char* label = ts_node_field_name_for_child(node, i);
            if (!label || std::string_view(label) != "declarator") continue;
            const auto name = property_name(facts, ts_node_child(node, i));
            append(facts, node, name, text(name, content), "property", ts_node_end_byte(node), content);
        }
    } else if (type == "preproc_include" ||
               (type == "preproc_call" && text(field(node, "directive"), content) == "#import")) {
        auto value = text(field(node, type == "preproc_include" ? "path" : "argument"), content);
        const auto first = value.find_first_not_of(" \t\r\n"), last = value.find_last_not_of(" \t\r\n");
        value = first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
        if (value.size() > 2 && value.front() == '"' && value.back() == '"' && !ts_node_has_error(node))
            facts.imports.push_back({value.substr(1, value.size() - 2), true,
                static_cast<int>(ts_node_start_point(node).row + 1)});
    }
}

} // namespace shin::context
