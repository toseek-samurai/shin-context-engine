#include "shin/server/snapshot_cache.h"

#include <algorithm>
#include <chrono>

namespace shin::server {

SnapshotCache::SnapshotCache(const RetrievalConfig& limits)
    : maximum_payload_bytes_(limits.max_payload_bytes),
      maximum_generation_bytes_(limits.max_generation_bytes),
      maximum_generations_(limits.max_generations) {
    if (maximum_generation_bytes_ == 0 || maximum_payload_bytes_ < maximum_generation_bytes_ ||
        maximum_payload_bytes_ > 1024ULL * 1024ULL * 1024ULL ||
        maximum_generations_ == 0 || maximum_generations_ > 32)
        throw ServiceError(400, "invalid-cache-limits", "Resident limits are invalid.");
    entries_.reserve(maximum_generations_);
}

std::uintmax_t SnapshotCache::payload_bytes() const {
    std::uintmax_t result = 0;
    for (const auto& entry : entries_) result += entry.snapshot->payload_bytes();
    return result;
}

bool SnapshotCache::reserve_load() {
    // Reserve the full generation cap before I/O. Active retired owners remain counted.
    while (entries_.size() >= maximum_generations_ ||
           payload_bytes() > maximum_payload_bytes_ - maximum_generation_bytes_) {
        auto victim = entries_.end();
        for (auto entry = entries_.begin(); entry != entries_.end(); ++entry) {
            if (entry->snapshot.use_count() == 1 &&
                (victim == entries_.end() || entry->last_used < victim->last_used)) victim = entry;
        }
        if (victim == entries_.end()) return false;
        entries_.erase(victim);
        ++evictions_;
    }
    loading_ = true;
    return true;
}

SnapshotCache::Snapshot SnapshotCache::acquire(const std::string& route,
                                               const std::string& snapshot_id,
                                               const Loader& loader) {
    std::unique_lock lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (;;) {
        for (auto& entry : entries_) {
            if (entry.route == route && entry.snapshot->id() == snapshot_id) {
                entry.last_used = ++clock_;
                ++hits_;
                return entry.snapshot;
            }
        }
        if (!loading_) break;
        if (changed_.wait_until(lock, deadline) == std::cv_status::timeout)
            throw ServiceError(503, "snapshot-load-busy", "A snapshot load is in progress; retry later.");
    }
    if (!reserve_load())
        throw ServiceError(503, "snapshot-cache-busy", "Resident snapshot budget is occupied by active requests.");
    lock.unlock();
    Snapshot loaded;
    try {
        loaded = loader();
        if (!loaded || loaded->payload_bytes() > maximum_generation_bytes_)
            throw ServiceError(503, "snapshot-memory-budget-exceeded", "Snapshot exceeds its resident budget.");
        if (loaded->id() != snapshot_id)
            throw ServiceError(409, "snapshot-generation-changed", "Publication changed during loading; retry the request.");
        lock.lock();
        entries_.push_back({route, loaded, ++clock_});
        ++loads_;
        loading_ = false;
        changed_.notify_all();
        return loaded;
    } catch (...) {
        // Destroy failed payloads before releasing their reservation to another loader.
        loaded.reset();
        if (!lock.owns_lock()) lock.lock();
        loading_ = false;
        changed_.notify_all();
        throw;
    }
}

Json SnapshotCache::statistics() const {
    std::lock_guard lock(mutex_);
    return {{"resident_generations", entries_.size()}, {"resident_payload_bytes", payload_bytes()},
            {"reserved_payload_bytes", loading_ ? maximum_generation_bytes_ : 0},
            {"load_in_progress", loading_}, {"hits", hits_}, {"loads", loads_}, {"evictions", evictions_},
            {"maximum_payload_bytes", maximum_payload_bytes_}, {"maximum_generations", maximum_generations_},
            {"accounting", "stored-artifact-bytes-not-process-rss"}};
}

} // namespace shin::server
