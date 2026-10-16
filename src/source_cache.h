#pragma once
#include "context_types.h"

namespace shin::context {
// A private derived artifact slot with a cross-process writer lock.
class SourceCache {
public:
    // Cleanup callers preserve pending artifacts outside their explicit plan.
    SourceCache(const fs::path& directory, const std::string& key, bool recover_pending = true);
    ~SourceCache();
    SourceCache(const SourceCache&) = delete;
    SourceCache& operator=(const SourceCache&) = delete;
    std::optional<std::string> load() const;
    void save(std::string_view bytes) const;
    static constexpr std::size_t maximum_bytes = 256U * 1024U * 1024U;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
