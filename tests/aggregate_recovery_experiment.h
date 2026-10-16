#pragma once
#include "context_internal.h"
#include <atomic>
#include <mutex>

namespace shin::context::experiment {
// Configuration changes only between completed benchmark lanes. Pack workers
// see the same mode; metrics include their scoring rather than only the caller.
struct AggregateRecoveryState {
    std::atomic<bool> enabled{false};
    std::atomic<std::size_t> attempts{0}, recovered{0}, still_scan{0}, score_calls{0}, dropped_retries{0};
    std::mutex trace_mutex;
    Json retries = Json::array();
};
inline AggregateRecoveryState aggregate_state;
inline void configure_aggregate_recovery(bool enabled) {
    std::lock_guard lock(aggregate_state.trace_mutex);
    aggregate_state.attempts.store(0);
    aggregate_state.recovered.store(0);
    aggregate_state.still_scan.store(0);
    aggregate_state.score_calls.store(0);
    aggregate_state.dropped_retries.store(0);
    aggregate_state.retries = Json::array();
    aggregate_state.enabled.store(enabled);
}
inline bool aggregate_recovery_enabled() { return aggregate_state.enabled.load(std::memory_order_relaxed); }
inline void record_aggregate_score() { aggregate_state.score_calls.fetch_add(1, std::memory_order_relaxed); }
inline void record_aggregate_retry(const QueryRequest& request, int required,
                                   std::size_t before_rows, bool before_indexed,
                                   const CandidateRows& result) {
    aggregate_state.attempts.fetch_add(1, std::memory_order_relaxed);
    if (result.from_terms_index) aggregate_state.recovered.fetch_add(1, std::memory_order_relaxed);
    else aggregate_state.still_scan.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(aggregate_state.trace_mutex);
    if (aggregate_state.retries.size() >= 256) {
        aggregate_state.dropped_retries.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    aggregate_state.retries.push_back({{"query", request.query}, {"required", required},
        {"before_selected_rows", before_rows}, {"before_indexed", before_indexed},
        {"after_rows", result.discovered_row_count}, {"after_indexed", result.from_terms_index},
        {"coverage_complete", result.term_index_coverage_complete}});
}
inline Json aggregate_metrics() {
    std::lock_guard lock(aggregate_state.trace_mutex);
    return {{"attempts", aggregate_state.attempts.load()}, {"recovered", aggregate_state.recovered.load()},
        {"still_scan", aggregate_state.still_scan.load()}, {"score_calls", aggregate_state.score_calls.load()},
        {"dropped_retries", aggregate_state.dropped_retries.load()}, {"retries", aggregate_state.retries}};
}
} // namespace shin::context::experiment
