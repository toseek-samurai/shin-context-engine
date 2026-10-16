#pragma once
#include "io.h"
#include <chrono>
#include <functional>

namespace shin::observer {


// Bounds on the cumulative time spent suspended, in milliseconds.
struct SleepSample {
    double lower_ms{};
    double upper_ms{};
};

// Returns a conservative native estimate when the platform exposes paired
// clocks that differ in whether system sleep advances them.
std::optional<SleepSample> native_suspend_sample() noexcept;

class LocalSuspendObservation {
public:
    using Sampler = std::function<std::optional<SleepSample>()>;

    explicit LocalSuspendObservation(Sampler sampler = native_suspend_sample);

    // Lower bound on newly observed suspend time since construction. Invalid,
    // unavailable, or regressive samples do not increase the result.
    double suspended_ms() noexcept;
    bool available() const noexcept { return baseline_.has_value(); }

    static constexpr double meaningful_sleep_threshold_ms = 100.0;

private:
    Sampler sampler_;
    std::optional<SleepSample> baseline_;
    std::optional<SleepSample> last_;
    double observed_ms_{};
};


enum class LocalWorkerRole { resident, refresh, read };
struct LocalWorkerOutput {
    std::optional<std::string> frame;
    bool ended = false;
};
// Owned child only; no saved PID, shell, public endpoint or retrieval policy.
class LocalWorkerProcess {
public:
    using Clock = std::chrono::steady_clock;
    LocalWorkerProcess(const fs::path& executable, LocalWorkerRole role,
                       LocalSuspendObservation::Sampler sampler = native_suspend_sample);
    ~LocalWorkerProcess();
    LocalWorkerProcess(const LocalWorkerProcess&) = delete;
    LocalWorkerProcess& operator=(const LocalWorkerProcess&) = delete;
    void send(const Json& payload, Clock::time_point deadline, const std::function<bool()>& cancelled = {});
    LocalWorkerOutput poll();
    Json receive(Clock::time_point deadline, const std::function<bool()>& cancelled = {},
                 const std::function<void(const Json&)>& progress = {});
    std::uint64_t process_id() const;
    std::optional<std::uint64_t> rss_bytes() const;
    void stop() noexcept;
private:
    LocalWorkerOutput read(Clock::time_point deadline);
    bool cancelled_or_suspended(const std::function<bool()>& cancelled);
    class Impl;
    const LocalWorkerRole role_;
    LocalSuspendObservation suspension_;
    std::unique_ptr<Impl> impl_;
    std::string buffered_;
    bool ended_ = false;
};
}
