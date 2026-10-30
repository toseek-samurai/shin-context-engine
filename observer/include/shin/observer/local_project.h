#pragma once
#include "observer.h"
#include <vector>
#include <algorithm>

namespace shin::observer {
// Caller-reported identifier, never inferred from a shared process or used for authority.
inline bool valid_reported_model(const std::string& value) {
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_' || c == ':' || c == '/';
    });
}
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
    bool wait_for_ready = true; // MCP status probes observe cold preparation without waiting.
    std::uint64_t readiness_wait_ms = 0; // Optional query admission budget; bounded by the native service.
    // Optional opaque telemetry only; never used for project authority.
    std::string session_id, parent_request_id, model_id;
    std::string task_id, intent;
    Json session_draft;
    std::string session_task;
    std::string expected_revision;
    // Local adapter telemetry callback; no authority and never accepted from wire/CLI.
    std::function<void(const std::string&)> observe_request_id;
    // Internal reconciliation token, never accepted from a CLI argument.
    std::string maintenance_worktree;
    std::string maintenance_root_identity;
    std::string maintenance_workspace_identity;
    std::function<bool()> maintenance_stop;
    // Optional bounded stage observation (refresh and final reader verification); never authority.
    std::function<void(const std::string&)> maintenance_progress;
    // Private resident preparation hook, never accepted from CLI or RPC JSON.
    std::shared_ptr<LocalQuerySession>* query_session = nullptr;
};

// Single-threaded CLI adapter over native Core/Observer commands.
Json local_project(const LocalProjectRequest& request);
// Installer rollback only: atomically retire an owned candidate publication so
// the restored CLI can rebuild. Never rewrites bindings or accepts old retrieval.
bool retire_bootstrap_update_index(const LocalProjectRequest& request, const Json& expected_binding,
                                 const std::string& restored_generator, const std::string& published_profile_hash = {});
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
// Validated pending reads may wake the ordinary background publisher. This is
// scheduling only; no query executes refresh or grants publication authority.
using LocalReaderDemand = std::function<std::vector<fs::path>()>;
Json watch_local_projects(const LocalProjectRequest& request, const std::vector<fs::path>& roots,
                          std::uint64_t samples, const std::function<bool()>& stopped,
                          const std::function<void()>& service_poll = {}, const LocalEnrollmentHooks& enrollment = {},
                          const std::function<void(const fs::path&, const Json&)>& report = {},
                          const LocalRefresh& refresh = {}, const LocalReaderDemand& reader_demand = {});
} // namespace shin::observer
