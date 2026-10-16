#pragma once

#include "config.h"
#include "context_types.h"

namespace shin::server {

struct BoundRoute {
    RetrievalRoute route;
    context::RuntimeOptions options;
    std::string profile_bytes_hash;
    std::string fingerprint;
};

class RouteCatalog {
public:
    explicit RouteCatalog(const Config& config);
    const BoundRoute& find(const std::string& project, const std::string& route) const;
    void verify(const BoundRoute& binding, const Json& workspace) const;
private:
    std::map<std::string, BoundRoute> bindings_;
};

} // namespace shin::server
