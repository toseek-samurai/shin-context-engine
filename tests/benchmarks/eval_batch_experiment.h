#pragma once

#include "context_internal.h"

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace shin::context::experiment {

class StageAwaiter;
using PendingStage = StageAwaiter;

class BriefTask {
public:
    struct promise_type {
        std::optional<BriefResult> result;
        std::exception_ptr error;
        PendingStage* pending = nullptr;

        BriefTask get_return_object() noexcept;
        std::suspend_always initial_suspend() const noexcept { return {}; }
        std::suspend_always final_suspend() const noexcept { return {}; }
        void return_value(BriefResult value) { result.emplace(std::move(value)); }
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };

    using Handle = std::coroutine_handle<promise_type>;

    BriefTask() noexcept = default;
    explicit BriefTask(Handle handle) noexcept : handle_(handle) {}
    BriefTask(const BriefTask&) = delete;
    BriefTask& operator=(const BriefTask&) = delete;
    BriefTask(BriefTask&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    BriefTask& operator=(BriefTask&& other) noexcept;
    ~BriefTask() noexcept;

    bool done() const noexcept { return !handle_ || handle_.done(); }
    Handle handle() const noexcept { return handle_; }
    BriefResult take_result();

private:
    Handle handle_{};
};

class StageAwaiter {
public:
    StageAwaiter(const RuntimeOptions& options,
                 const QueryRequest& request,
                 CandidateRows candidates,
                 int required_group_matches,
                 bool fallback_used);
    bool await_ready() const noexcept { return false; }
    template <class Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        handle.promise().pending = this;
    }
    BriefResult await_resume();

    const RuntimeOptions& options() const noexcept { return options_; }
    const QueryRequest& request() const noexcept { return request_; }
    const CandidateRows& candidates() const noexcept { return candidates_; }
    int required_group_matches() const noexcept { return required_group_matches_; }
    bool fallback_used() const noexcept { return fallback_used_; }
    void set_result(BriefResult result) { result_.emplace(std::move(result)); }
    BriefResult& scoring_result() noexcept { return scoring_result_; }
    BriefResult take_scoring_result() { return std::move(scoring_result_); }

private:
    const RuntimeOptions& options_;
    const QueryRequest& request_;
    CandidateRows candidates_;
    int required_group_matches_ = 0;
    bool fallback_used_ = false;
    BriefResult scoring_result_;
    std::optional<BriefResult> result_;
};

BriefTask collect_brief_task(const RuntimeOptions& options,
                             const QueryRequest& request,
                             const Json* manifest);
BriefResult direct_brief_stage(const RuntimeOptions& options,
                               const QueryRequest& request,
                               const CandidateRows& candidates,
                               int required_group_matches,
                               bool fallback_used);

std::vector<BriefResult> run_brief_tasks(const RuntimeOptions& options,
                                         const std::vector<QueryRequest>& requests,
                                         const Json* manifest,
                                         bool batch);

void configure_eval_batch(bool enabled, std::size_t batch_size = 4);
bool eval_batch_enabled() noexcept;
std::size_t eval_batch_size() noexcept;
Json batch_metrics();

std::vector<BriefExecution> batch_brief_executions(
    const RetrievalSession& session,
    const std::vector<QueryRequest>& requests);
std::vector<Json> batch_brief_documents(
    const RetrievalSession& session,
    const std::vector<QueryRequest>& requests);
Json eval_details(const RetrievalSession& session);

} // namespace shin::context::experiment
