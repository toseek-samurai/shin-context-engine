#pragma once

#include "model.h"
#include <functional>

namespace shin::server {

enum class BootstrapSelectorKind { route, publication };

struct BootstrapSelector {
    BootstrapSelectorKind kind;
    std::string id;
};

// Trusted bounded service callbacks; the response cap is not a process RSS cap.
struct BootstrapAccess {
    std::function<void(const std::string& project)> authorize;
    std::function<Json(const std::string& project, const BootstrapSelector& selector, const std::string& query)> retrieve;
    std::function<Json(const std::string& project, const BootstrapSelector& selector)> health;
};

Json session_bootstrap(const Json& request, const BootstrapAccess& access);

} // namespace shin::server
