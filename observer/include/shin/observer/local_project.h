#pragma once
#include "observer.h"
#include <vector>

namespace shin::observer {
class LocalQuerySession;
struct LocalProjectRequest {
    std::string command;
    fs::path root;
    fs::path home;
    std::optional<fs::path> profile_dir;
    bool profile_dir_explicit = false;
    bool manage_index = false;
    std::optional<fs::path> observer_config;
    std::optional<std::string> marker;
    std::string query;
    std::string path;
    std::string record;
    std::string snapshot;
    std::string profile_revision;
    std::string profile_review_token;
    std::string marker_revision;
    std::string config_revision;
    bool diagnostic = false;
    // Optional opaque telemetry only; never used for project authority.
    std::string session_id, parent_request_id;
    // Internal reconciliation token, never accepted from a CLI argument.
    std::string maintenance_worktree;
    std::string maintenance_root_identity;
    std::string maintenance_workspace_identity;
    std::function<bool()> maintenance_stop;
    // Optional bounded refresh-stage observation; never participates in authority.
    std::function<void(const std::string&)> maintenance_progress;
    // Private resident preparation hook, never accepted from CLI or RPC JSON.
    std::shared_ptr<LocalQuerySession>* query_session = nullptr;
};

// Single-threaded CLI adapter over native Core/Observer commands.
Json local_project(const LocalProjectRequest& request);
// Observational watch state, never project/publication authority.
Json local_watch_status(const fs::path& directory);
Json watch_local_project(const LocalProjectRequest& request, std::uint64_t samples,
                         const std::function<bool()>& stopped);
struct LocalEnrollmentHooks {
    std::function<std::vector<LocalProjectRequest>()> read;
    std::function<void(const LocalProjectRequest&, const std::string&)> report;
    std::function<void(const fs::path&)> removed;
    std::function<void(const fs::path&, bool)> preparing;
};
using LocalRefresh = std::function<Json(const LocalProjectRequest&)>;
Json watch_local_projects(const LocalProjectRequest& request, const std::vector<fs::path>& roots,
                          std::uint64_t samples, const std::function<bool()>& stopped,
                          const std::function<void()>& service_poll = {}, const LocalEnrollmentHooks& enrollment = {},
                          const std::function<void(const fs::path&, const Json&)>& report = {},
                          const LocalRefresh& refresh = {});
} // namespace shin::observer
