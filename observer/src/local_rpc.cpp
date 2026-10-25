#include "shin/observer/local_rpc.h"
#include <set>
#include "shin/observer/local_project.h"
#include "local_sessions.h"
#include <algorithm>

namespace shin::observer::local_rpc {
Json reply(const std::string& id) {
    return {{"schema_version", "shin.local-rpc.v1"}, {"request_id", id},
        {"observer_version", SHIN_OBSERVER_VERSION}, {"core_version", SHIN_VERSION}};
}

Json failure(const std::string& id, const std::string& code, const std::string& message, const Json& diagnostic) {
    auto value = reply(id);
    value["ok"] = false;
    value["error"] = error_json(Error(code, message, diagnostic));
    return value;
}

namespace {
Json response_for_version(const Json& response, const std::string& id, const std::string& observer_version,
                          const std::string& core_version, bool diagnose_mismatch = false) {
    if (!response.is_object() || response.value("schema_version", Json()) != "shin.local-rpc.v1" ||
        response.value("request_id", Json()) != id || validation_identifier(response.value("observer_version", Json())).empty() ||
        validation_identifier(response.value("core_version", Json())).empty() || !response.contains("ok") || !response.at("ok").is_boolean())
        throw Error("service-response-invalid", "Local response identity/version is invalid.");
    const auto error = response.value("error", Json());
    const bool valid_error = error.is_object() && !response.at("ok").get<bool>() && !response.contains("result") &&
        !validation_identifier(error.value("code", Json())).empty() && error.value("message", Json()).is_string() &&
        error.at("message").get_ref<const std::string&>().size() <= 4096;
    if (response.at("observer_version") != observer_version || response.at("core_version") != core_version) {
        // Only an explicit refusal may cross the version fence. Success payloads
        // and installer control responses still require the exact verified kit.
        if (diagnose_mismatch && valid_error && error.at("code") == "service-version-mismatch")
            throw Error("service-version-mismatch", "Client Observer " + observer_version + ", Core " + core_version +
                "; service Observer " + response.at("observer_version").get<std::string>() + ", Core " +
                response.at("core_version").get<std::string>() + ". Reconnect the MCP client after updating the kit; verify both components use the current installation.");
        throw Error("service-response-invalid", "Local response identity/version is invalid.");
    }
    if (response.contains("error")) {
        if (!valid_error) throw Error("service-response-invalid", "Local error response is invalid.");
        throw Error(error.at("code"), error.at("message"), error.value("diagnostic", Json()));
    }
    auto result = response.value("result", Json());
    if (!result.is_object() || !result.contains("ok") || result.at("ok") != response.at("ok"))
        throw Error("service-response-invalid", "Local response success is inconsistent.");
    return result;
}

} // namespace
Json project_response(const Json& response, const std::string& id) {
    return response_for_version(response, id, SHIN_OBSERVER_VERSION, SHIN_VERSION, true);
}
Json control_response(const Json& response, const std::string& id, const std::string& observer_version, const std::string& core_version) {
    if (validation_identifier(observer_version).empty() || validation_identifier(core_version).empty())
        throw Error("service-version-mismatch", "A verified kit version is required for installation control.");
    return response_for_version(response, id, observer_version, core_version);
}

std::string string_field(const Json& value, const char* key, std::size_t limit) {
    if (!value.contains(key) || !value.at(key).is_string())
        throw Error("service-request-invalid", "Required string field is missing.");
    const auto result = value.at(key).get<std::string>();
    if (result.size() > limit || result.find('\0') != std::string::npos)
        throw Error("service-request-invalid", "Request string exceeds its bounds.");
    return result;
}

void validate_request(const Json& value) {
    const std::set<std::string> allowed{"schema_version", "request_id", "observer_version", "core_version",
                                        "command", "root", "query", "path", "record", "snapshot"};
    const std::set<std::string> optional{"diagnostic", "session_id", "parent_request_id", "model_id", "wait_for_ready", "readiness_wait_ms", "session"};
    if (!value.is_object())
        throw Error("service-request-invalid", "Unexpected local request fields.");
    for (const auto& key : allowed)
        if (!value.contains(key)) throw Error("service-request-invalid", "Required local request field is missing.");
    for (const auto& [key, entry] : value.items()) {
        (void)entry;
        if (!allowed.contains(key) && !optional.contains(key)) throw Error("service-request-invalid", "Unknown local request field.");
    }
    for (const auto* key : {"session_id", "parent_request_id"}) if (value.contains(key)) {
        const auto text = string_field(value, key, 64);
        if (text.size() != 64 || !std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) throw Error("service-request-invalid", "Correlation must be an opaque digest.");
    }
    if (value.contains("model_id") && (!value.contains("session_id") ||
        !valid_reported_model(string_field(value, "model_id", 64))))
        throw Error("service-request-invalid", "A reported model requires a session and a bounded model identifier.");
    if (value.at("schema_version") != "shin.local-rpc.v1" || value.at("observer_version") != SHIN_OBSERVER_VERSION ||
        value.at("core_version") != SHIN_VERSION)
        throw Error("service-version-mismatch", "Restart the service with the same installed kit as its client.");
    if (string_field(value, "request_id", 96).empty())
        throw Error("service-request-invalid", "A request identity is required.");
    const auto command = string_field(value, "command", 32);
    if (value.contains("readiness_wait_ms") && (command != "query" ||
        !value.at("readiness_wait_ms").is_number_integer() || value.at("readiness_wait_ms") < 5000 ||
        value.at("readiness_wait_ms") > 20000))
        throw Error("service-request-invalid", "Query readiness waiting must be between 5000 and 20000 milliseconds.");
    if (value.contains("wait_for_ready") && (command != "status" || !value.at("wait_for_ready").is_boolean()))
        throw Error("service-request-invalid", "Readiness waiting is a boolean for project status only.");
    if (value.contains("diagnostic") && (!value.at("diagnostic").is_boolean() ||
        !std::set<std::string>{"query", "inspect", "read"}.contains(command)))
        throw Error("service-request-invalid", "Diagnostic labeling is a boolean for project reads only.");
    if (!std::set<std::string>{"status", "query", "inspect", "read", "checkpoint", "resume", "service-status", "service-stop"}.contains(command))
        throw Error("service-method-denied", "The local request surface is read-only except explicit service stop.");
    if (command == "checkpoint" || command == "resume") {
        LocalProjectRequest session; session.command = command; local_session_fields(value,session);
    } else if (value.contains("session")) throw Error("service-request-invalid","Task state belongs only to explicit session operations.");
    (void)string_field(value, "root", 4096);
    (void)string_field(value, "query", 4096);
    (void)string_field(value, "path", 4096);
    (void)string_field(value, "record", 1024);
    (void)string_field(value, "snapshot", 32);
}

}
