#pragma once

#include "context_types.h"

namespace shin::generated_io {
class IndexLock;
}

namespace shin::context {

std::unique_ptr<shin::generated_io::IndexLock> lock_index_for_read(const fs::path& out_root);

enum class RetrievalSessionOpenMode {
    fast_freshness,
    require_current,
    verified_snapshot,
    verified_current
};

class RetrievalSession {
public:
    RetrievalSession(RuntimeOptions options, bool require_current);
    RetrievalSession(RuntimeOptions options, RetrievalSessionOpenMode mode);
    RetrievalSession(std::shared_ptr<const GeneratedSnapshot> snapshot, bool require_current);
    RetrievalSession(std::shared_ptr<const GeneratedSnapshot> snapshot, RetrievalSessionOpenMode mode);
    ~RetrievalSession();

    RetrievalSession(const RetrievalSession&) = delete;
    RetrievalSession& operator=(const RetrievalSession&) = delete;
    RetrievalSession(RetrievalSession&&) = delete;
    RetrievalSession& operator=(RetrievalSession&&) = delete;

    const RuntimeOptions& options() const noexcept { return options_; }
    const IndexFreshness& freshness() const noexcept { return freshness_; }
    const Json& manifest() const noexcept;

    Json inspect(const std::string& path) const;
    Json read(std::string_view request) const;
    // Immutable publication only; verifies selected files, not corpus freshness.
    Json read_selected(std::string_view request) const;
    Json search(const QueryRequest& request) const;
    Json context(const QueryRequest& request) const;
    BriefExecution brief_execution(const QueryRequest& request) const;
    Json brief(const QueryRequest& request,
               BriefProjection projection = BriefProjection::full) const;

private:
    Json read_impl(std::string_view request, bool selected_only) const;
    RuntimeOptions options_;
    std::unique_ptr<shin::generated_io::IndexLock> lock_;
    IndexFreshness freshness_;
    Json manifest_;
};

}  // namespace shin::context
