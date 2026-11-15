#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace shin::observer {
// Borrowed readiness only, never permission to read, accept or qualify data.
// POSIX: a nonblocking descriptor. Windows: an overlapped-operation event.
struct LocalReadySource {
    std::intptr_t native = -1;
    bool writable = false;
    bool immediate = false;
};

// One owner thread runs the loop. Native producers may retain notifier copies;
// they become inert after teardown. Wait registrations are retired before the
// owner can close or reuse a borrowed descriptor/event.
class LocalCoordinator {
public:
    using Clock = std::chrono::steady_clock;
    LocalCoordinator();
    ~LocalCoordinator();
    LocalCoordinator(const LocalCoordinator&) = delete;
    LocalCoordinator& operator=(const LocalCoordinator&) = delete;
    std::function<void()> notifier() const;
    void wait(Clock::time_point deadline, std::span<const LocalReadySource> sources = {});
    // CLI/SCM explicitly supply observable stop sources. Other native callers
    // retain the existing 20 ms cancellation-probe bound for opaque callbacks.
    void stop_on_signals();
    // A native stop event wakes the owner, which retains its cancellation check.
    void stop_on(LocalReadySource source);
    bool stopped() const noexcept;
    void poll_signals();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace shin::observer
