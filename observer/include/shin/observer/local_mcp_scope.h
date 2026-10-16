#pragma once
#include "local_registry.h"

namespace shin::observer {
// Advisory only: never changes an error, admission, snapshot or project scope.
Json local_mcp_recovery(const std::string& code, const Json& diagnostic = nullptr);
// Static, project-independent usage and recovery guidance. Performs no IO/RPC.
Json local_mcp_help(const std::string& code = {});

// Explicit host allowlist, fixed for this MCP connection. Enrollment and OS-user
// service access remain independently enforced by the normal read path.
class LocalMcpScope {
public:
    LocalMcpScope(const fs::path& home, const std::vector<fs::path>& roots);
    Json catalog() const;
    LocalProjectRequest select(const std::string& project) const;
    void verify(const std::string& project) const;
    std::string snapshot(const std::string& project, const std::string& native) const;
    std::string native_snapshot(const std::string& project, const std::string& scoped) const;
private:
    struct Entry {
        LocalProjectRequest request;
        std::string handle, root_identity, workspace_identity, startup_error;
        LocalProjectLifecycle lifecycle;
    };
    const Entry& entry(const std::string& project) const;
    void verify(const Entry& value) const;
    fs::path home_;
    std::string home_identity_;
    std::vector<Entry> entries_;
};
} // namespace shin::observer
