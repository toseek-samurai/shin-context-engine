#pragma once

#include "config.h"

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>

namespace shin::server {

enum class RetrievalOperation { search, brief, pack };
enum class RetrievalSelector { route, publication };
enum class RetrievalPhase { validation, selection, admission, execution, finalization };

class RetrievalTelemetry {
public:
    struct Attribution { std::string principal_id, tenant_id, workspace_id; };
    using Clock = std::chrono::steady_clock;
    struct Attempt {
        Attempt(RetrievalOperation op, RetrievalSelector kind, Clock::time_point now, bool counted)
            : operation(op), selector(kind), started(now), last_mark(now), tracked(counted) {}
        Attempt(const Attempt&) = delete;
        Attempt& operator=(const Attempt&) = delete;
        RetrievalOperation operation;
        RetrievalSelector selector;
        Clock::time_point started;
        Clock::time_point last_mark;
        RetrievalPhase phase = RetrievalPhase::validation;
        std::array<std::uint64_t, 5> phase_ns{};
        std::array<bool, 5> measured{true, false, false, false, false};
        bool tracked = false;
        bool request_validated = false;
        std::optional<std::uint64_t> result_bytes;
        std::optional<Attribution> attribution;
        void mark(RetrievalPhase next, Clock::time_point now = Clock::now()) noexcept;
    };

    explicit RetrievalTelemetry(const ObservabilityConfig& config);
    Attempt begin(RetrievalOperation operation, RetrievalSelector selector,
                  Clock::time_point now = Clock::now()) noexcept;
    void finish(Attempt& attempt, std::string_view project, std::string_view name,
                const Json& request, const Json* result, int status, std::string_view reason,
                Clock::time_point now = Clock::now()) noexcept;
    Json metrics() const;
    Json journal_snapshot(std::uint64_t after = 0) const;
    Json requests(std::uint64_t after, std::size_t limit, std::string_view instance) const;

private:
    struct Cell {
        std::uint64_t started = 0, completed = 0, active = 0, succeeded = 0, failed = 0;
        std::uint64_t total_ns = 0, max_ns = 0;
        std::array<std::uint64_t, 11> histogram{};
    };
    struct Record { Json value; std::size_t bytes; };
    Json sample(const Attempt& attempt, std::string_view project, std::string_view name,
                const Json& request, const Json* result, int status, std::string_view reason,
                std::uint64_t elapsed_ns) const;
    ObservabilityConfig config_;
    std::string instance_;
    mutable std::mutex mutex_;
    std::array<Cell, 6> cells_{};
    std::deque<Record> records_;
    std::size_t bytes_ = 0;
    std::uint64_t sequence_ = 0, evicted_ = 0, dropped_ = 0, disabled_ = 0;
    std::atomic<std::uint64_t> collection_errors_{0};
};
} // namespace shin::server
