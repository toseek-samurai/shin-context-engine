#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <initializer_list>
#include <string>
#include <string_view>

namespace shin::observer_protocol {
using Json = nlohmann::json;

// A finite vocabulary prevents exception messages, source paths, or content
// from crossing the persistent observation and model diagnostic boundary.
inline nlohmann::json bounded_observation_diagnostic(const nlohmann::json& value) {
    using Json = nlohmann::json;
    if (!value.is_object() || value.size() < 4 || value.size() > 7 ||
        value.value("schema_version", Json()) != "shin.source-observation-diagnostic.v1") return nullptr;
    const auto member = [&](const char* key, std::initializer_list<std::string_view> allowed) {
        const auto it = value.find(key);
        if (it == value.end() || !it->is_string()) return false;
        const auto& text = it->get_ref<const std::string&>();
        for (auto token : allowed) if (text == token) return true;
        return false;
    };
    if (!member("stage", {"source-inventory", "source-identity", "snapshot-comparison", "source-stability", "profile-verification"}) ||
        !member("operation", {"enumerate-source-root", "enumerate-source-entry", "read-source-metadata", "read-source-content",
            "verify-source-metadata", "observe-source", "build-identity", "compare-snapshot", "verify-inventory", "verify-profile"}) ||
        !member("category", {"entry-disappeared", "entry-changed", "access-denied", "io-error", "resource-exhausted", "unexpected-error", "invalid-data"})) return nullptr;
    if (value.size() != 4) {
        if (value.at("category") != "resource-exhausted" ||
            !member("resource", {"process-file-descriptors", "system-file-descriptors", "memory"}) ||
            !value.contains("native_error") || !value.at("native_error").is_number_integer() ||
            value.at("native_error") < 0 || value.at("native_error") > 2147483647) return nullptr;
        const bool limit = value.contains("descriptor_soft_limit");
        if (value.size() != (limit ? 7U : 6U)) return nullptr;
        if (limit && (value.at("resource") != "process-file-descriptors" ||
            !value.at("descriptor_soft_limit").is_number_unsigned())) return nullptr;
    }
    return value;
}

inline constexpr char kIdentitySchema[] = "shin.observer-server-identity.v3";
inline constexpr char kRequestIdentityHeader[] = "X-Shin-Server-ID";

class ValidationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string sha256_hex(std::string_view value);
void verify_observation(const Json& observation);
void verify_submission(const Json& submission);
void verify_acknowledgement_shape(const Json& receipt);
void verify_acknowledgement(const Json& receipt, const Json& submission, const std::string& server_id);
void verify_reconciliation_request(const Json& request);
void verify_reconciliation_result(const Json& result, const Json& request);
void verify_confirmation(const Json& response, const Json& receipt);
} // namespace shin::observer_protocol
