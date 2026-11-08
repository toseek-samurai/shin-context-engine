#pragma once
#include <string_view>

namespace shin::ui {
// Configuration state, never a claim that a particular model has used MCP.
inline const char* client_access_label(std::string_view state) {
    if (state == "configured") return "Access configured · live project list";
    if (state == "reconnect-required") return "Access configured · reconnect MCP once";
    if (state == "review-required") return "Connection needs review";
    return "Access not configured";
}
}
