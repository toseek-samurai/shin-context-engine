#pragma once

#include "context_types.h"

namespace shin::context {

struct SourceBinding {
    std::string scope;
    std::string symbol;
    std::string declaration_path;
    std::string payload_path;
    int line = 1;
    int end_line = 0;
    std::string range_origin{};
};

struct SourceReference {
    std::string scope;
    std::string path;
    std::string target_symbol;
    int line = 1;
    std::string kind = "source-reference";
};

struct SourceImportCandidates {
    std::string path;
    std::string target;
    std::vector<std::string> candidates;
    int line = 1;
};

struct SourceRelationInputs {
    std::vector<LinkRow> direct;
    std::vector<SourceBinding> bindings;
    std::vector<SourceReference> references;
    std::vector<SourceImportCandidates> imports;
};

// Adapter inputs are structural observations, never compiler or runtime proof.
std::vector<LinkRow> resolve_source_relations(
    const std::vector<SourceRelationInputs>& inputs,
    const std::set<std::string>& indexed_paths);

struct SourceGraph {
    std::vector<LinkRow> links;
    std::map<std::string, std::vector<std::size_t>> incoming;
    std::map<std::string, std::vector<std::size_t>> outgoing;
    std::size_t unresolved = 0;
};

SourceGraph load_source_graph(const RuntimeOptions& options);
bool source_relation_kind(std::string_view kind) noexcept;

}  // namespace shin::context
