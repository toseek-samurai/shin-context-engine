#pragma once

#include "config.h"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace shin::server {

class JournalWriter {
public:
    using Snapshotter = std::function<Json(std::uint64_t)>;
    using Exporter = std::function<Json(const Json&)>;
    static constexpr std::size_t maximum_records = 256;
    static constexpr std::size_t maximum_batch_bytes = 4U * 1024U * 1024U;

    JournalWriter(JournalWriterConfig policy, Snapshotter snapshot, Exporter export_batch);
    ~JournalWriter();
    void start();
    void shutdown();
    Json status() const;

private:
    using Clock = std::chrono::steady_clock;
    enum class Step { idle, exported, failed, blocked };
    bool load_pending();
    Step step();
    void failed(const ServiceError& error, std::string_view stage);
    void work(std::stop_token stop);
    JournalWriterConfig policy_;
    Snapshotter snapshot_;
    Exporter export_;
    std::mutex lifecycle_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    bool started_ = false, stopped_ = false, in_flight_ = false, blocked_ = false;
    bool shutdown_prefix_acknowledged_ = false;
    std::string instance_, failure_code_, failure_stage_;
    std::uint64_t sampled_through_ = 0, acknowledged_through_ = 0;
    std::uint64_t polls_ = 0, attempts_ = 0, successes_ = 0, failures_ = 0, consecutive_failures_ = 0;
    std::uint64_t inserted_ = 0, new_missing_ = 0, recovered_missing_ = 0;
    std::int64_t producer_epoch_ = 0;
    std::size_t pending_records_ = 0, pending_bytes_ = 0;
    std::uint64_t pending_through_ = 0;
    Clock::time_point sampled_at_{}, next_attempt_{};
    Json pending_ = nullptr;
    std::jthread worker_;
};

} // namespace shin::server
