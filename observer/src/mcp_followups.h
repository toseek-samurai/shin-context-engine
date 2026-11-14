#pragma once
#include "shin/observer/local_project.h"
#include <chrono>
#include <map>
#include <memory>
#include <mutex>

namespace shin::observer {

// Volatile adapter convenience, never authority. Native reads still validate the
// pinned snapshot, route and selected source. No identity is persisted here.
class McpFollowups {
    using Clock = std::chrono::steady_clock;
    struct Entry {
        std::string snapshot, parent;
        Clock::time_point at;
        std::size_t active = 0;
        bool ambiguous = false;
    };
public:
    struct Binding { std::string snapshot, parent; };
    struct Query {
        McpFollowups& owner;
        std::string key;
        bool done = false;
        Query(McpFollowups& input, std::string identity) : owner(input), key(std::move(identity)) {}
        Query(const Query&) = delete;
        Query& operator=(const Query&) = delete;
        ~Query() { if (!done) owner.finish(key, {}); }
        void complete(Binding binding) { if (!done) { owner.finish(key, std::move(binding)); done = true; } }
    };
    explicit McpFollowups(std::chrono::seconds lifetime = std::chrono::minutes(15)) : lifetime_(lifetime) {}
    std::unique_ptr<Query> begin(const std::string& key) {
        if (key.empty()) return {};
        const std::lock_guard lock(mutex_);
        if (!entries_.contains(key) && entries_.size() >= 128) {
            auto oldest = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it)
                if (!it->second.active && (oldest == entries_.end() || it->second.at < oldest->second.at)) oldest = it;
            if (oldest == entries_.end()) return {};
            entries_.erase(oldest);
        }
        auto& entry = entries_[key];
        if (entry.active == 0) entry.ambiguous = false;
        else entry.ambiguous = true;
        ++entry.active;
        entry.snapshot.clear(); entry.parent.clear(); entry.at = Clock::now();
        return std::unique_ptr<Query>(new Query{*this, key});
    }
    Binding resolve(const std::string& key) {
        const std::lock_guard lock(mutex_);
        const auto found = entries_.find(key);
        if (key.empty() || found == entries_.end() || found->second.active || found->second.ambiguous ||
            found->second.snapshot.empty() || Clock::now() - found->second.at >= lifetime_)
            throw Error("mcp-query-binding-required", "Run one shin_query for this project with the same real session_id, or pass the exact returned snapshot. Concurrent queries require an explicit snapshot; no project or session is inferred.");
        return {found->second.snapshot, found->second.parent};
    }
private:
    void finish(const std::string& key, Binding binding) {
        const std::lock_guard lock(mutex_);
        auto found = entries_.find(key);
        if (found == entries_.end()) return;
        auto& entry = found->second;
        if (entry.active) --entry.active;
        if (entry.active == 0 && !entry.ambiguous) {
            entry.snapshot = std::move(binding.snapshot); entry.parent = std::move(binding.parent); entry.at = Clock::now();
        }
    }
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::chrono::seconds lifetime_;
};
} // namespace shin::observer
