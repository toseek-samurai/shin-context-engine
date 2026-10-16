#include "context_internal.h"
#include "source_relations.h"
#include "source_syntax.h"

namespace shin::context {
namespace {

bool primary_ts_declaration(const FactRow& row) {
    static const std::set<std::string> kinds = {
        "ts-class", "ts-interface", "ts-enum", "ts-type", "ts-function", "ts-arrow-function"
    };
    return kinds.contains(row.record_type);
}

bool primary_cpp_declaration(const FactRow& row) {
    return row.record_type == "cpp-function" || row.record_type == "cpp-class" ||
        row.record_type == "cpp-struct" || row.record_type == "cpp-enum" || row.record_type == "cpp-namespace";
}

std::vector<std::string> import_candidates(const std::string& path, const std::string& target) {
    const fs::path base = (fs::path(path).parent_path() / target).lexically_normal();
    std::vector<std::string> result;
    const std::string extension = base.extension().generic_string();
    if (extension.empty()) {
        for (const char* suffix : {".ts", ".tsx", ".js", ".jsx", ".mts", ".cts", ".mjs", ".cjs"}) {
            result.push_back(base.generic_string() + suffix);
            result.push_back((base / (std::string("index") + suffix)).generic_string());
        }
    } else {
        result.push_back(base.generic_string());
        // NodeNext source may import the emitted .js name. Ambiguity is not resolved by preference.
        fs::path source = base;
        if (extension == ".js") {
            result.push_back(source.replace_extension(".ts").generic_string());
            result.push_back(source.replace_extension(".tsx").generic_string());
        } else if (extension == ".mjs" || extension == ".cjs") {
            result.push_back(source.replace_extension(extension == ".mjs" ? ".mts" : ".cts").generic_string());
        }
    }
    return result;
}

void set_syntax_range(FactRow& row, const SyntaxDeclaration& declaration) {
    row.start_line = declaration.start_line;
    row.end_line = declaration.end_line;
    row.range_origin = "syntax";
}

}  // namespace

void apply_syntax_source_facts(const Profile& profile, const std::string& path,
                               const std::string& content, const std::vector<std::string>& lines,
                               const std::string& language, std::vector<FactRow>& rows,
                               SourceRelationInputs& relations) {
    const SyntaxFacts syntax = inspect_source_syntax(content, extension_of(path));
    if (!syntax.supported) return;
    const bool lexical_only = !syntax.complete && syntax.error_nodes == 0 && syntax.declarations.empty();
    if (!lexical_only) {
        std::erase_if(rows, [&](const FactRow& row) { return row.path == path &&
            (primary_ts_declaration(row) || (language == "cpp" && primary_cpp_declaration(row))); });
    }
    // Retain export/test retrieval metadata, but never infer its body by looking for the next brace.
    for (FactRow& row : rows) {
        if (row.path != path || row.source != "source-fact:" + language) continue;
        row.range_origin = "lexical";
        for (const SyntaxDeclaration& declaration : syntax.declarations) {
            if (language == "cpp") break;
            if (row.symbol_name == declaration.name && row.line >= declaration.start_line &&
                row.line <= declaration.line) {
                set_syntax_range(row, declaration);
                break;
            }
        }
    }
    for (const SyntaxDeclaration& declaration : syntax.declarations) {
        const auto existing = std::find_if(rows.begin(), rows.end(), [&](const FactRow& row) {
            return row.path == path && row.source == "source-fact:" + language &&
                row.symbol_name == declaration.name && row.range_origin == "syntax" &&
                row.start_line == declaration.start_line &&
                (row.record_type == "ts-hook" || row.record_type == "ts-component");
        });
        if (declaration.kind != "arrow-function" || existing == rows.end()) {
            const std::string header = language == "cpp" ? declaration.header : preview_from_line_range(lines,
                static_cast<std::size_t>(declaration.start_line - 1),
                static_cast<std::size_t>(declaration.line), 420);
            add_source_fact_row(rows, profile, path, declaration.line, language, declaration.kind,
                                declaration.name, header);
            set_syntax_range(rows.back(), declaration);
            if (language == "cpp") {
                rows.back().id += "-b" + std::to_string(declaration.byte_offset);
                const auto& qualified = declaration.qualified_name;
                if (!qualified.empty() && qualified.size() <= 128 &&
                    std::all_of(qualified.begin(), qualified.end(), [](unsigned char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == ':';
                    }))
                    rows.back().identifier_terms.push_back(declaration.qualified_name);
            }
        }
        if (language == "java" && declaration.top_level) {
            const std::string qualified = syntax.package_name.empty() ? declaration.name
                : syntax.package_name + "." + declaration.name;
            relations.bindings.push_back({"java-qualified-type", qualified, path, "", declaration.start_line,
                                          declaration.end_line, "syntax-declaration"});
        }
    }
    if (!syntax.package_name.empty()) {
        add_source_fact_row(rows, profile, path, syntax.package_line, language, "package",
                            syntax.package_name, "package " + syntax.package_name);
        rows.back().range_origin = "syntax-header";
    }
    for (const SyntaxImport& import : syntax.imports) {
        if (import.relative) {
            relations.imports.push_back({path, import.target, import_candidates(path, import.target), import.line});
        } else if (language == "java") {
            relations.references.push_back({"java-qualified-type", path, import.target, import.line, "source-import"});
        }
    }
    if (!syntax.complete) {
        add_source_fact_row(rows, profile, path, 1, language, "syntax-incomplete", "incomplete-syntax",
                            "Partial syntax observation: malformed source or bounded parser limit; ranges cover only observed valid declarations.");
        rows.back().range_origin = "syntax-header";
    }
}

}  // namespace shin::context
