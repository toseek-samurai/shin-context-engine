#pragma once

#include "config.h"
#include "generated_snapshot.h"

#include <condition_variable>
#include <functional>
#include <mutex>

namespace shin::server {

class SnapshotCache {
public:
    using Snapshot = std::shared_ptr<const context::GeneratedSnapshot>;
    using Loader = std::function<Snapshot()>;

    explicit SnapshotCache(const RetrievalConfig& limits);
    Snapshot acquire(const std::string& route, const std::string& snapshot_id, const Loader& loader);
    Json statistics() const;

private:
    struct Entry {
        std::string route;
        Snapshot snapshot;
        std::uint64_t last_used;
    };
    bool reserve_load();
    std::uintmax_t payload_bytes() const;

    const std::uintmax_t maximum_payload_bytes_;
    const std::uintmax_t maximum_generation_bytes_;
    const std::size_t maximum_generations_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Entry> entries_;
    bool loading_ = false;
    std::uint64_t clock_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t loads_ = 0;
    std::uint64_t evictions_ = 0;
};

} // namespace shin::server
