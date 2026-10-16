#pragma once
#include "context_types.h"

namespace shin::context {
// Retain raw text/identifiers and append CamelCase components for lexical recall.
std::string source_search_projection(std::string_view text);
// One private, generation-bound index. Concurrent requests fail busy rather
// than queue; the owner must keep this object alive until callers finish.
class SourceDiscoveryIndex {
public:
    explicit SourceDiscoveryIndex(RuntimeOptions options, const fs::path& cache_directory = {});
    ~SourceDiscoveryIndex();
    SourceDiscoveryIndex(const SourceDiscoveryIndex&) = delete;
    SourceDiscoveryIndex& operator=(const SourceDiscoveryIndex&) = delete;
    SourceDiscoveryIndex(SourceDiscoveryIndex&&) = delete;
    SourceDiscoveryIndex& operator=(SourceDiscoveryIndex&&) = delete;

    Json query(const RuntimeOptions& binding, const std::vector<std::string>& queries);
    Json context(const RuntimeOptions& binding, const std::string& query);
    // Persist this prepared candidate under the identity of an identical
    // published generation. Caller holds both publication locks and authority.
    void persist_published_cache(const RuntimeOptions& destination, const fs::path& cache_directory);
    Json preparation_metrics() const;
    // Require the exact retained immutable snapshot; verify returned sources only.
    Json query_at_snapshot(const RuntimeOptions& binding, const std::vector<std::string>& queries);
    Json context_at_snapshot(const RuntimeOptions& binding, const std::string& query);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Explicit offline preparation/query batch. Never replaces ordinary Brief.
Json discover_source_candidates(const RuntimeOptions& options, const std::vector<std::string>& queries);
}
