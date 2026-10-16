#pragma once
#include "local_project.h"
#include <chrono>

namespace shin::observer {
// Internal worker envelopes only; never a public enrollment or authorization API.
Json execute_local_worker_read(const Json& payload);
Json execute_local_worker_refresh(const Json& payload, const std::function<bool()>& stopped,
                                  const std::function<void(const Json&)>& progress = {});
Json local_worker_metric(const Json& response, double duration_ms);
// One serial reader; transport and process lifetime remain platform-owned.
class LocalResidentWorker {
public:
    explicit LocalResidentWorker(const Json& preparation);
    bool observed_unchanged() const;
    Json ready() const;
    Json execute(const std::string& frame, std::chrono::steady_clock::time_point started);
private:
    LocalProjectRequest request_;
    std::shared_ptr<LocalQuerySession> session_;
};
// Cold reads and qualification use the same owned process adapter as the service.
Json request_local_worker(const LocalProjectRequest& request, const fs::path& executable,
                          const std::function<bool()>& cancelled = {});
}
