#include "context_internal.h"
#include "generated_snapshot.h"
#include "source_relations.h"

namespace shin::context {
namespace {

constexpr std::size_t kMaxRelationInputs = 200000;
constexpr std::uintmax_t kMaxGraphBytes = 64U * 1024U * 1024U;

bool relative_source_path(const std::string& path) {
    if (path.empty() || path.size() > 4096 || path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos ||
        std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
        return false;
    }
    const fs::path value(path);
    return !value.is_absolute() && value.lexically_normal().generic_string() == path &&
           std::none_of(value.begin(), value.end(), [](const fs::path& p) { return p == ".." || p == "."; });
}

void require_relation_budget(std::size_t size) {
    if (size > kMaxRelationInputs) {
        throw CliError("source-relations-limit", "resource", "Source relation count exceeds its bounded limit.",
                       1, false, "Narrow the profile source inventory.");
    }
}

void require_relation_fields(const LinkRow& link) {
    if (link.path.size() > 4096 || link.target.size() > 4096 || link.resolved_path.size() > 4096 ||
        link.text.size() > 4096 || link.line < 1) {
        throw CliError("source-relations-limit", "resource", "Source relation fields exceed their bounded contract.",
                       1, false, "Narrow or correct the source relation target before publication.");
    }
}

}  // namespace

bool source_relation_kind(std::string_view kind) noexcept {
    return kind == "source-include" || kind == "source-payload" ||
           kind == "source-reference" || kind == "source-import";
}

std::vector<LinkRow> resolve_source_relations(const std::vector<SourceRelationInputs>& inputs,
                                             const std::set<std::string>& indexed_paths) {
    using Key = std::pair<std::string, std::string>;
    std::map<Key, std::vector<const SourceBinding*>> bindings;
    std::vector<LinkRow> links;
    std::size_t input_count = 0;
    for (const SourceRelationInputs& input : inputs) {
        input_count += input.direct.size() + input.bindings.size() + input.references.size() + input.imports.size();
        require_relation_budget(input_count);
        for (const SourceBinding& binding : input.bindings) {
            if (!binding.scope.empty() && !binding.symbol.empty() &&
                indexed_paths.contains(binding.declaration_path)) {
                bindings[{binding.scope, binding.symbol}].push_back(&binding);
            }
        }
        for (LinkRow link : input.direct) {
            require_relation_fields(link);
            if (!source_relation_kind(link.kind) || !indexed_paths.contains(link.path)) {
                throw std::logic_error("Invalid structural source relation adapter output.");
            }
            link.status = relative_source_path(link.resolved_path) && indexed_paths.contains(link.resolved_path)
                              ? "resolved" : "unresolved";
            if (link.status != "resolved") link.resolved_path.clear();
            links.push_back(std::move(link));
        }
    }
    for (const SourceRelationInputs& input : inputs) {
        for (const SourceReference& reference : input.references) {
            if (!indexed_paths.contains(reference.path)) continue;
            const auto found = bindings.find({reference.scope, reference.target_symbol});
            std::string target;
            std::string status = "unresolved";
            if (found != bindings.end() && found->second.size() == 1) {
                const auto& binding = *found->second.front();
                target = binding.payload_path.empty() ? binding.declaration_path : binding.payload_path;
                if (relative_source_path(target) && indexed_paths.contains(target)) status = "resolved";
            } else if (found != bindings.end()) {
                status = "ambiguous";
            }
            if (status != "resolved") target.clear();
            if (reference.kind != "source-reference" && reference.kind != "source-import") {
                throw std::logic_error("Invalid named source relation kind.");
            }
            links.push_back({reference.path, reference.kind, reference.target_symbol,
                             reference.target_symbol, status, std::move(target), reference.line});
            if (status == "resolved") {
                const auto& binding = *found->second.front();
                if (binding.payload_path.empty() && binding.end_line >= binding.line && binding.line > 0) {
                    if (binding.range_origin != "rsx-opening-tag" && binding.range_origin != "syntax-declaration")
                        throw std::logic_error("Invalid structural declaration range origin.");
                    links.back().target_start_line = binding.line;
                    links.back().target_end_line = binding.end_line;
                    links.back().target_range_origin = binding.range_origin;
                }
            }
        }
        for (const SourceImportCandidates& import : input.imports) {
            if (!indexed_paths.contains(import.path)) continue;
            std::set<std::string> matched;
            for (const std::string& candidate : import.candidates) {
                if (relative_source_path(candidate) && indexed_paths.contains(candidate)) matched.insert(candidate);
            }
            const std::string status = matched.empty() ? "unresolved" : matched.size() == 1 ? "resolved" : "ambiguous";
            links.push_back({import.path, "source-import", import.target, import.target, status,
                             matched.size() == 1 ? *matched.begin() : "", import.line});
        }
    }
    for (const LinkRow& link : links) require_relation_fields(link);
    std::sort(links.begin(), links.end(), [](const LinkRow& a, const LinkRow& b) {
        return std::tie(a.path, a.line, a.kind, a.target, a.status, a.resolved_path) <
               std::tie(b.path, b.line, b.kind, b.target, b.status, b.resolved_path);
    });
    links.erase(std::unique(links.begin(), links.end(), [](const LinkRow& a, const LinkRow& b) {
        return std::tie(a.path, a.line, a.kind, a.target, a.status, a.resolved_path) ==
               std::tie(b.path, b.line, b.kind, b.target, b.status, b.resolved_path);
    }), links.end());
    return links;
}

SourceGraph load_source_graph(const RuntimeOptions& options) {
    const fs::path path = options.out_root / kGeneratedDir / "links.jsonl";
    SourceGraph graph;
    auto append = [&](const std::string& line) {
        require_relation_budget(graph.links.size() + 1);
        LinkRow link = parse_link_row(line);
        if (source_relation_kind(link.kind) &&
            (link.line < 1 || link.path.size() > 4096 || link.target.size() > 4096 ||
             link.text.size() > 4096 || link.resolved_path.size() > 4096)) {
            throw_index_error("index-corrupt", "Generated relation has an invalid path or line bound.");
        }
        const std::size_t index = graph.links.size();
        if (link.line >= 1 && (link.status == "resolved" || link.status == "ok") && relative_source_path(link.path) &&
            relative_source_path(link.resolved_path)) {
            graph.outgoing[link.path].push_back(index);
            graph.incoming[link.resolved_path].push_back(index);
        } else if (source_relation_kind(link.kind)) {
            ++graph.unresolved;
        }
        graph.links.push_back(std::move(link));
    };
    if (options.generated_snapshot) {
        const std::string_view bytes = options.generated_snapshot->file(path);
        if (bytes.size() > kMaxGraphBytes) {
            throw CliError("source-graph-unavailable", "index", "Source graph is unsafe, missing or exceeds 64 MiB.",
                           1, false, "Rebuild a bounded verified index.");
        }
        GeneratedInput input(options, path);
        std::string line;
        while (std::getline(input, line)) {
            const std::string compact = trim(line);
            if (!compact.empty()) append(compact);
        }
        if (input.bad()) {
            throw CliError("source-graph-unavailable", "index", "Source graph is unsafe, missing or exceeds 64 MiB.",
                           1, false, "Rebuild a bounded verified index.");
        }
    } else {
        std::error_code error;
        const auto status = fs::symlink_status(path, error);
        if (error || !fs::is_regular_file(status) || fs::is_symlink(status) ||
            fs::file_size(path, error) > kMaxGraphBytes || error) {
            throw CliError("source-graph-unavailable", "index", "Source graph is unsafe, missing or exceeds 64 MiB.",
                           1, false, "Rebuild a bounded verified index.");
        }
        for_each_jsonl_line_if_exists(path, append);
    }
    return graph;
}

}  // namespace shin::context
