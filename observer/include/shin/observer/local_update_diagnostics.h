#pragma once
#include "observer.h"

namespace shin::observer {
inline bool transient_update_project_error(const std::string& code) {
    return code == "project-busy" || code == "writer-busy" || code == "binding-changed" ||
        code == "project-updating" || code == "refresh-timeout" || code == "refresh-suspended";
}

// Quality refusals can describe an already degraded baseline across a qualified
// Core patch. They never count as target readiness or as transient failures of
// a previously ready project.
inline bool recoverable_update_project_error(const std::string& code) {
    return transient_update_project_error(code) || code == "project-check-failed" ||
        code == "refresh-validation-failed" || code == "project-not-ready";
}
inline bool previous_update_project_unready(const Json& status, const std::string& root) {
    if (!status.is_object() || status.value("ok", Json()) != true ||
        status.value("service_running", Json()) != true || status.value("enrollment_mode", Json()) != "registry" ||
        status.value("registry_error", Json()) != "") return false;
    const auto errors = status.value("enrollment_errors", Json());
    return errors.is_object() && errors.contains(root) &&
        recoverable_update_project_error(validation_identifier(errors.at(root)));
}

// Reuse the bounded diagnostic wire contract. Only codes and opaque root hashes
// leave the updater; service messages, source text and absolute paths do not.
inline Json update_service_diagnostic(const Json& status, const std::string& stage) {
    Json gate{{"id", stage}, {"cases", Json::array()}};
    const auto registry = validation_identifier(status.value("registry_error", Json()));
    if (!registry.empty()) gate["error_code"] = registry;
    const auto errors = status.value("enrollment_errors", Json::object());
    if (errors.is_object()) for (const auto& [root, value] : errors.items()) {
        const auto code = validation_identifier(value);
        if (!code.empty()) gate["cases"].push_back({{"id", digest(root)}, {"reasons", Json::array({code})}});
    }
    return bounded_validation_diagnostic({{"schema_version", "shin.validation-diagnostic.v1"}, {"gates", Json::array({gate})}});
}

inline Json merge_update_diagnostic(const Json& first, const Json& second) {
    auto result = bounded_validation_diagnostic(first);
    const auto added = bounded_validation_diagnostic(second);
    if (result.is_null()) return added;
    if (added.is_null()) return result;
    for (const auto& gate : added.at("gates")) result["gates"].push_back(gate);
    result["truncated"] = result.value("truncated", false) || added.value("truncated", false);
    return bounded_validation_diagnostic(result);
}

inline Json update_project_diagnostic(const std::string& root, const std::string& code, const Json& readiness = Json::object()) {
    const auto detail = !readiness.is_object() ? Json() : readiness.contains("steps") ?
        readiness_diagnostic(readiness) : validation_diagnostic(readiness);
    return merge_update_diagnostic(update_service_diagnostic({{"enrollment_errors", {{root, code}}}}, "project-qualification"),
        detail);
}
} // namespace shin::observer
