#pragma once
#include "io.h"
#include "local_history.h"
#include <chrono>
#include <deque>
#include <map>
#include <array>
#include <set>

namespace shin::observer {
// Leave room for the RPC envelope and client transport metadata below 64 KiB.
inline constexpr std::size_t kLocalStatusByteLimit = 60 * 1024;
void bound_local_service_status(Json& status);

class LocalTelemetry {
public:
    void record(const fs::path& root, const Json& state);
    void detach(const fs::path& root);
    void sample(std::size_t workers, std::optional<std::uint64_t> child_rss = std::uint64_t{0});
    void request_result(const fs::path& root, const std::string& method, const Json& report);
    // Reports retain bounded wall/active/sleep durations, allowlisted stages,
    // and numeric work counters without exposing source paths or contents.
    void refresh_result(const fs::path& root, const Json& report);
    void open_history(const fs::path& directory);
    void close_history();
    Json snapshot() const;
    std::uint64_t requests = 0;
    std::uint64_t admission_rejections = 0;
private:
    using Clock = std::chrono::steady_clock;
    struct Activity {
        std::array<std::uint64_t, 4> calls{}; // query, inspect, read, status
        std::uint64_t diagnostic = 0, uncorrelated = 0;
        std::set<std::string> sessions;
        bool sessions_truncated = false;
        std::map<std::string, std::uint64_t> models;
        std::uint64_t model_unknown = 0, model_overflow = 0;
        void add(std::size_t method, const Json& report, std::size_t session_limit);
        Json snapshot() const;
    };
    Activity activity;
    struct Project {
        Activity service_activity;
        Json value;
        Clock::time_point completed{}, succeeded{}, activity{};
        std::string event_key, completed_phase, last_error;
    };
    Clock::time_point started = Clock::now(), last_sample{};
    std::map<std::string, Project> projects;
    std::deque<Json> samples, events;
    std::deque<Json> request_results;
    std::deque<Json> refresh_results;
    std::uint64_t responses_ready = 0, response_errors = 0, unobserved = 0;
    std::uint64_t observed_follow_ups = 0, admission_waits = 0, wait_recoveries = 0;
    std::unique_ptr<LocalHistory> history;
    Clock::time_point last_persisted_sample{};
};
} // namespace shin::observer
