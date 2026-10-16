#pragma once

#include "snapshot_cache.h"
#include "store.h"
#include "route_catalog.h"
#include "retrieval_telemetry.h"
#include <atomic>
#include <mutex>
#include <optional>

namespace shin::server {

class RetrievalService {
public:
    RetrievalService(const Config& config, const RouteCatalog& catalog);
    Json retrieve(Store& store, const Principal& principal, const std::string& project,
                  const std::string& route, std::string_view operation, const Json& request);
    Json retrieve_publication(Store& store, const Principal& principal, const std::string& project,
                              const std::string& publication, std::string_view operation, const Json& request);
    // Workspace-scoped retrieval; the personal HTTP route uses this admission path.
    Json retrieve_workspace(Store& store, const Principal& principal, const std::string& project,
                            const std::string& route, const WorkspaceReadRequest& scope,
                            std::string_view operation, const Json& request);
    Json retrieve_workspace_publication(Store& store, const Principal& principal, const std::string& project,
                                        const std::string& publication, const WorkspaceReadRequest& scope,
                                        std::string_view operation, const Json& request);
    Json cache_statistics(const Principal& principal) const;
    Json telemetry_metrics(const Principal& principal) const;
    Json capture_journal(Store& store, const Principal& principal) const;
    Json journal_snapshot(std::uint64_t after = 0) const;
    Json capture_journal_batch(Store& store, const Principal& principal, const Json& snapshot) const;
    Json telemetry_requests(const Principal& principal, std::uint64_t after, std::size_t limit,
                            std::string_view instance) const;
    Json inspect_route_health(Store& store, const Principal& principal,
                              const std::string& project, const std::string& route);
    // Internal native continuity results contain host-private binding; ContinuityService projects them.
    Json evaluate_continuity(Store& store, const Principal& principal, const std::string& project,
                             const Json& selector, const std::string& operation, const std::string& task,
                             const std::string& thread, const std::string& input, bool require_current,
                             const Json& expected_provenance = Json(), const Json& receipt_origins = Json::array(),
                             const std::optional<WorkspaceReadRequest>& scope = std::nullopt);

private:
    struct ForegroundActivity {
        explicit ForegroundActivity(std::atomic<unsigned>& count) : count_(count) { ++count_; }
        ~ForegroundActivity() { --count_; }
        ForegroundActivity(const ForegroundActivity&) = delete;
        ForegroundActivity& operator=(const ForegroundActivity&) = delete;
        std::atomic<unsigned>& count_;
    };
    Json retrieve_selected(Store& store, const Principal& principal, const std::string& project,
                           const std::string& name, std::string_view operation, const Json& request,
                           bool publication, const std::function<Json()>& selection,
                           const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    Json execute_selected(Store& store, const Principal& principal, const std::string& project,
                          const std::string& name, std::string_view operation, const Json& request,
                          bool publication, const std::function<Json()>& selection,
                          RetrievalTelemetry::Attempt& attempt, const std::optional<WorkspaceReadRequest>& scope);
    const Config& config_;
    std::string state_identity_;
    const RouteCatalog& catalog_;
    SnapshotCache cache_;
    RetrievalTelemetry telemetry_;
    mutable std::mutex journal_registration_mutex_;
    mutable std::int64_t journal_epoch_ = 0;
    std::atomic<unsigned> foreground_{0};
};

} // namespace shin::server
