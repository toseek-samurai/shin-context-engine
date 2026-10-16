#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace shin::server {
class StoreDeadlines {
public:
    explicit StoreDeadlines(std::size_t slots);
    ~StoreDeadlines();
    StoreDeadlines(const StoreDeadlines&) = delete;
    StoreDeadlines& operator=(const StoreDeadlines&) = delete;

    void arm(std::size_t slot, int socket, std::chrono::milliseconds timeout);
    bool disarm(std::size_t slot) noexcept;

private:
    using Clock = std::chrono::steady_clock;
    struct Entry {
        int descriptor = -1;
        Clock::time_point deadline;
        bool expired = false;
    };
    void watch(std::stop_token stop);

    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Entry> entries_;
    std::jthread worker_;
};
} // namespace shin::server
