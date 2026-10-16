#include "store_deadlines.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace shin::server {
StoreDeadlines::StoreDeadlines(std::size_t slots) : entries_(slots),
    worker_([this](std::stop_token stop) { watch(stop); }) {}

StoreDeadlines::~StoreDeadlines() {
    {
        std::lock_guard lock(mutex_);
        worker_.request_stop();
    }
    changed_.notify_all();
    worker_.join();
    for (auto& entry : entries_) {
        if (entry.descriptor >= 0) ::close(entry.descriptor);
    }
}

void StoreDeadlines::arm(std::size_t slot, int socket, std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex_);
    auto& entry = entries_.at(slot);
    if (entry.descriptor >= 0 || timeout.count() <= 0)
        throw std::logic_error("Invalid database lease deadline.");
    // The driver may close its descriptor on failure. Own a duplicate so an
    // expired lease can never interrupt an unrelated socket reusing that number.
    const int duplicate = ::fcntl(socket, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) throw std::system_error(errno, std::generic_category(), "Database deadline socket");
    entry = {duplicate, Clock::now() + timeout, false};
    changed_.notify_all();
}

bool StoreDeadlines::disarm(std::size_t slot) noexcept {
    std::lock_guard lock(mutex_);
    auto& entry = entries_[slot];
    const bool expired = entry.descriptor >= 0 && entry.expired;
    if (entry.descriptor >= 0) ::close(std::exchange(entry.descriptor, -1));
    changed_.notify_all();
    return expired;
}

void StoreDeadlines::watch(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        const auto now = Clock::now();
        auto next = Clock::time_point::max();
        for (auto& entry : entries_) {
            if (entry.descriptor < 0 || entry.expired) continue;
            if (entry.deadline <= now) {
                // Do not call libpq/libpqxx from this thread or close its descriptor.
                ::shutdown(entry.descriptor, SHUT_RDWR);
                entry.expired = true;
            } else {
                next = std::min(next, entry.deadline);
            }
        }
        if (next == Clock::time_point::max()) changed_.wait(lock);
        else changed_.wait_until(lock, next);
    }
}
} // namespace shin::server
