#pragma once
#include "local_project.h"
#include <chrono>
#include <map>

namespace shin::observer {
// Internal worker envelopes only; never a public enrollment or authorization API.
Json execute_local_worker_read(const Json& payload);
Json execute_local_worker_refresh(const Json& payload, const std::function<bool()>& stopped,
                                  const std::function<void(const Json&)>& progress = {},
                                  std::shared_ptr<LocalRefreshSession>* session = nullptr);
// One owned serial background process, bounded independently of resident readers.
class LocalRefreshWorker {
public:
    Json execute(const Json& payload, const std::function<bool()>& stopped,
                 const std::function<void(const Json&)>& progress = {});
private:
    struct Entry { std::shared_ptr<LocalRefreshSession> session; std::uint64_t used = 0; };
    std::map<std::string, Entry> sessions_;
    std::uint64_t sequence_ = 0;
};
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
