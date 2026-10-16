#pragma once

#include "config.h"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace shin::server {

struct KnowledgeTarget { std::string project_id; std::string route_id; };

class KnowledgeScheduler {
public:
    using Inspector = std::function<Json(const KnowledgeTarget&)>;
    KnowledgeScheduler(KnowledgeConfig policy, std::vector<KnowledgeTarget> targets, Inspector inspect);
    ~KnowledgeScheduler();
    void start();
    void request_stop();
    Json refresh();
    Json status() const;
    Json snapshot(const std::string& route) const;
private:
    using Clock = std::chrono::steady_clock;
    struct Entry {
        KnowledgeTarget target;
        Clock::time_point due{};
        Clock::time_point observed{};
        Json report = nullptr;
        std::int64_t started_at = 0;
        std::int64_t finished_at = 0;
        double elapsed_ms = 0;
        bool checking = false;
        bool recheck = false;
    };
    void work(std::stop_token stop);
    KnowledgeConfig policy_;
    Inspector inspect_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::vector<Entry> entries_;
    std::uint64_t completed_ = 0;
    std::uint64_t failed_ = 0;
    std::uint64_t deferred_ = 0;
    std::uint64_t refreshes_ = 0;
    std::uint64_t coalesced_ = 0;
    std::jthread worker_;
};

} // namespace shin::server
