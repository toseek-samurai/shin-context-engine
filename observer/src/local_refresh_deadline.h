#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <string>
#include <string_view>

namespace shin::observer {
// Stage changes prove forward work; repeated heartbeats do not extend a stall.
// The independent operation cap also bounds a worker cycling through stages.
class LocalRefreshDeadline {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto operation_limit = std::chrono::seconds(180);
    static constexpr auto stage_limit = std::chrono::seconds(60);
    explicit LocalRefreshDeadline(Clock::time_point now = Clock::now())
        : operation_(now + operation_limit), stage_(now + stage_limit) {}
    Clock::time_point operation_deadline() const { return operation_; }
    bool expired(Clock::time_point now = Clock::now()) const { return now >= std::min(stage_, operation_); }
    bool observe(std::string_view stage, double elapsed_ms, Clock::time_point now = Clock::now()) {
        if (expired(now) || !std::isfinite(elapsed_ms) || elapsed_ms < elapsed_ms_ ||
            elapsed_ms > std::chrono::duration<double, std::milli>(operation_limit).count())
            return false;
        constexpr std::string_view stages[]{"admission", "observation", "candidate-build", "candidate-validation",
            "publication", "qualification", "source-cache", "candidate-source-cache", "source-cache-transfer", "final-verification"};
        if (std::find(std::begin(stages), std::end(stages), stage) == std::end(stages)) return false;
        if (stage != last_stage_) {
            last_stage_ = stage;
            stage_ = std::min(operation_, now + stage_limit);
        }
        elapsed_ms_ = elapsed_ms;
        return true;
    }
private:
    Clock::time_point operation_, stage_;
    std::string last_stage_;
    double elapsed_ms_ = 0;
};
} // namespace shin::observer
