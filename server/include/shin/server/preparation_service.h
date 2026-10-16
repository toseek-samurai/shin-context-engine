#pragma once

#include "route_catalog.h"
#include "store.h"
#include <condition_variable>
#include <atomic>
#include <thread>

namespace shin::server {

class PreparationService {
public:
    PreparationService(const Config& config, const RouteCatalog& catalog, Store& store);
    ~PreparationService();
    void start();
    void request_stop();
    Json status() const;
    Json inspect(const Principal& principal, std::uint64_t id, const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json preflight(const Principal& principal, std::uint64_t id, const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json active_generation(const Principal& principal, const std::string& project, const std::string& route,
                           const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    Json activate(const Principal& principal, const std::string& project, const std::string& route, const Json& request,
                  const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    // Native internal continuation; verified bindings are never supplied by an HTTP client.
    Json with_verified_candidate(const Principal& principal, std::uint64_t id,
                                 const std::function<Json(const Json&)>& commit,
                                 const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json retention_plan(const Principal& principal, std::uint64_t id,
                        const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json apply_retention(const Principal& principal, std::uint64_t id, const Json& request,
                         const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json enqueue(const Principal& principal, const std::string& project, const std::string& route,
                 const Json& request, const std::string& idempotency_key,
                 const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
private:
    enum class Observation { inspection, preflight, activation };
    Json observe_candidate(const Principal& principal, std::uint64_t id, Observation mode,
                           const std::function<Json(const Json&)>& on_verified = {},
                           const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    std::string fingerprint(const BoundRoute& binding) const;
    std::string artifact_namespace() const;
    void verify_state() const;
    void work(std::stop_token stop);
    void prepare(const Json& job, std::stop_token stop);
    void finish_failure(const Json& job, const std::string& reason) noexcept;
    void observe(const std::string& state, const std::string& error = "", std::uint64_t job_id = 0);

    const Config& config_;
    const RouteCatalog& catalog_;
    Store& store_;
    std::string state_identity_;
    std::string worker_id_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    bool requested_ = false;
    std::string state_ = "disabled";
    std::string last_error_;
    std::uint64_t active_job_ = 0;
    std::atomic<std::uint64_t> uncertain_completions_{0};
    std::mutex inspection_mutex_;
    std::mutex retention_mutex_;
    std::jthread worker_;
};

} // namespace shin::server
