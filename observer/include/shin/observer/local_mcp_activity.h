#pragma once
#include "io.h"

namespace shin::observer {
// A separate, bounded installation journal: MCP calls are not service executions.
// Processes share a short writer lease; the protocol/render threads never use SQLite.
class LocalMcpActivity {
public:
    explicit LocalMcpActivity(fs::path directory, bool collect = false);
    ~LocalMcpActivity();
    LocalMcpActivity(const LocalMcpActivity&) = delete;
    LocalMcpActivity& operator=(const LocalMcpActivity&) = delete;
    void append(const Json& call);
    Json snapshot() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace shin::observer
