#pragma once
#include "local_registry.h"
#include <string_view>
#include <mutex>

namespace shin::observer {
// Advisory only: never changes an error, admission, snapshot or project scope.
Json local_mcp_recovery(const std::string& code, const Json& diagnostic = nullptr, std::string_view method = {});
// Static, project-independent usage and recovery guidance. Performs no IO/RPC.
Json local_mcp_help(const std::string& code = {});

// Explicit host allowlist, optionally refreshed from this agent's own settings.
// Enrollment and OS-user service access remain independently enforced.
class LocalMcpScope {
public:
    LocalMcpScope(const fs::path& home, const std::vector<fs::path>& roots,
                  const fs::path& executable = {}, const fs::path& config = {}, const std::string& client = {});
    Json catalog() const;
    LocalProjectRequest select(const std::string& project) const;
    void verify(const std::string& project) const;
    std::string snapshot(const std::string& project, const std::string& native) const;
    std::string native_snapshot(const std::string& project, const std::string& scoped) const;
private:
    struct Entry {
        LocalProjectRequest request;
        fs::path configured_root;
        std::string handle, root_identity, workspace_identity, startup_error;
        LocalProjectLifecycle lifecycle;
    };
    Entry entry(const std::string& project) const;
    void verify(const Entry& value) const;
    void refresh_locked() const;
    fs::path home_;
    std::string home_identity_;
    fs::path executable_, config_;
    std::string client_, config_parent_identity_;
    mutable std::mutex mutex_;
    mutable std::uint64_t grant_sequence_ = 0;
    mutable std::vector<fs::path> configured_roots_;
    mutable std::vector<Entry> entries_;
};
} // namespace shin::observer
