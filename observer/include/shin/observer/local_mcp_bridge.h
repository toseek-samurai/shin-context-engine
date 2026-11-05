#pragma once
#include "local_project.h"
#include <memory>

namespace shin::observer {
// A live MCP connection keeps its tools, admission and allowed roots. Only the
// verified service runtime may change; this is not a caller-selected version.
class VerifiedMcpRuntime {
public:
    const std::string& observer_version() const { return observer_; }
    const std::string& core_version() const { return core_; }
    const Json& binding() const { return binding_; }
private:
    friend class LocalMcpBridge;
    VerifiedMcpRuntime(std::string observer, std::string core, Json binding)
        : observer_(std::move(observer)), core_(std::move(core)), binding_(std::move(binding)) {}
    std::string observer_, core_;
    Json binding_;
};

class LocalMcpBridge {
public:
    LocalMcpBridge(const fs::path& executable, const fs::path& home);
    ~LocalMcpBridge();
    Json request(const LocalProjectRequest& request, const std::function<bool()>& cancelled = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Captured by the current service before accepting requests. Unpackaged builds
// retain the original exact-compiled-version path.
Json local_mcp_runtime_binding(const fs::path& executable);
std::string local_mcp_adapter_contract();
Json request_verified_mcp_service(const LocalProjectRequest& request, const VerifiedMcpRuntime& runtime,
                                  const std::function<bool()>& cancelled);
}
