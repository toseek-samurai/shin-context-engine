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
inline const char* client_skill_label(std::string_view state, bool managed = true) {
    if (state == "current") return managed ? "Instructions are current" : "Instructions are current · updates not enabled";
    if (state == "update-available") return "Instruction update available";
    if (state == "not-installed") return "Shin instructions are not installed";
    if (state == "customized") return "Custom instructions · review changes manually";
    if (state == "recovery-required") return "Instruction update needs recovery";
    return "Instructions could not be checked";
}
inline const char* client_skill_action(std::string_view state) {
    if (state == "not-installed") return "Install instructions";
    if (state == "update-available") return "Update instructions";
    if (state == "recovery-required") return "Recover instructions";
    return "Enable instruction updates";
}
}
