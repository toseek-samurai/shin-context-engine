#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <cstddef>
#include <vector>

namespace shin::server {

using Json = nlohmann::json;

class ServiceError : public std::runtime_error {
public:
    ServiceError(int status, std::string code, const std::string& message)
        : std::runtime_error(message), status(status), code(std::move(code)) {}

    int status;
    std::string code;
};

struct Principal {
    std::string id;
    bool admin = false;
    std::vector<std::string> project_ids;
    bool receipt_producer = false;
};

struct ProjectInput {
    std::string id;
    std::string display_name;
};

// Requested coordinates, never an authorization token. Store resolves them anew.
struct WorkspaceReadRequest {
    std::string tenant_id;
    std::string workspace_id;
};

struct ObserverScope {
    std::string project_id;
    WorkspaceReadRequest workspace;
};

// Native request-local observation, never a bearer grant or HTTP payload.
struct WorkspaceWriteScope {
    std::string project_id;
    WorkspaceReadRequest workspace;
    Json admission;
    std::string route_id;
};

struct WorkspaceInput {
    std::string id;
    std::string display_name;
    std::string source_host_id;
    std::string root;
    std::string root_identity;
    bool observer_source = false;
};

struct StoreConfig {
    std::string connection_string;
    std::string expected_database;
    std::string expected_server_id;
    std::string runtime_role;
    std::size_t pool_size = 4;
    std::string source_host_id;
};

struct RegistrationResult {
    Json project;
    bool replayed = false;
};

} // namespace shin::server
