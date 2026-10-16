#pragma once
#include "context_internal.h"
#include "shin/observer/io.h"
#include <functional>

namespace shin::observer {
inline void refresh_progress(const std::function<void(const std::string&)>& progress, const char* stage) noexcept {
    if (!progress) return;
    try { progress(stage); } catch (...) { return; /* Diagnostics never grant or revoke authority. */ }
}

template<class Request>
inline void refresh_stage(const Request& request, const char* stage) noexcept {
    if (request.command == "_refresh") refresh_progress(request.maintenance_progress, stage);
}

inline Json envelope(const std::string& command, bool ok) {
    return {{"schema_version", "shin.ce-response.v1"}, {"tool", "shin-ce"},
            {"version", SHIN_OBSERVER_VERSION}, {"core_version", SHIN_VERSION},
            {"command", command}, {"ok", ok}, {"mode", "offline"},
            {"delivery_performed", false}, {"daemon_started", false}, {"consumer_mutated", false}};
}

inline std::string qualification_key(Json binding, const std::string& profile_hash,
                              const std::optional<std::string>& marker_hash = std::nullopt) {
    fs::path executable;
    try { executable = current_executable_path(); }
    catch (const Error& error) {
        if (error.code != "installation-unavailable") throw;
        throw Error("qualification-engine-changed", "The executing image is unavailable; restart with one matched kit.");
    }
    const auto binary = digest(read_text(executable, 128U * 1024U * 1024U, false));
    binding.erase("prepared_snapshot");
    binding["profile_config_hash"] = profile_hash;
    if (marker_hash) binding["marker_hash"] = *marker_hash;
    binding["core_version"] = SHIN_VERSION;
    binding["generator_version"] = shin::context::kGeneratorVersion;
    binding["observer_version"] = SHIN_OBSERVER_VERSION;
    binding["executing_core_version"] = SHIN_VERSION;
    binding["executing_generator"] = shin::context::kGeneratorVersion;
    binding["executable_sha256"] = binary;
    binding["executing_image"] = executing_image_identity();
    binding["qualification_policy"] = "verified-generation-live-safety-v2";
    return digest(binding.dump());
}

struct RefreshMeasurements {
    Json values = Json::object();
    template<class F> auto measure(const char* name, F&& action) {
        struct Finish {
            Json& values; const char* name; shin::context::SteadyClock::time_point start;
            ~Finish() noexcept {
                try { values[name] = values.value(name, 0.0) + shin::context::elapsed_ms_since(start); }
                catch (...) { return; } // Optional diagnostics must not replace the operation's result.
            }
        } finish{values, name, shin::context::SteadyClock::now()};
        return action();
    }
};

inline Json source_observation_error(const Json& observation, const Json& tick_error = Json()) {
    auto reason = validation_identifier(observation.value("reason_code", Json()));
    if (reason.empty() && tick_error.is_object())
        reason = validation_identifier(tick_error.value("code", Json()));
    auto diagnostic = observation.value("diagnostic", Json());
    if (diagnostic.is_null() && tick_error.is_object())
        diagnostic = tick_error.value("diagnostic", Json());
    return error_json(Error(reason.empty() ? "project-observation-failed" : reason,
        "Source observation failed; the previous index is preserved.", diagnostic));
}

inline bool failed_source_observation_tick(const Json& tick) {
    if (!tick.is_object() || tick.value("phase", "") != "blocked") return false;
    const auto ok = tick.find("ok");
    if (ok == tick.end() || !ok->is_boolean() || ok->get<bool>()) return false;
    const auto observation = tick.find("observation");
    if (observation == tick.end() || !observation->is_object()) return false;
    const auto status = observation->value("status", "");
    if (status.empty() || status == "current" || status == "stale" || status == "unpublished") return false;

    // A generated observation error duplicates its typed reason at the tick
    // level. Keep any conflicting top-level error as the hard refusal it is.
    if (tick.contains("error")) {
        const auto error = tick.find("error");
        if (!error->is_object()) return false;
        const auto tick_code = validation_identifier(error->value("code", Json()));
        const auto observation_code = validation_identifier(observation->value("reason_code", Json()));
        if (tick_code.empty() || observation_code.empty() || tick_code != observation_code) return false;
    }
    return true;
}

inline Json classified_source_observation_tick(Json tick, const Json& classification,
                                               const std::string& phase) {
    tick["ok"] = false;
    tick["phase"] = phase;
    if (classification.contains("retry_reason")) tick["retry_reason"] = classification.at("retry_reason");
    else tick.erase("retry_reason");
    if (classification.contains("diagnostic")) tick["diagnostic"] = classification.at("diagnostic");
    else tick.erase("diagnostic");
    if (classification.contains("error")) tick["error"] = classification.at("error");
    else tick.erase("error");
    return tick;
}

// A source-binding error alone does not prove a race: unchanged input must
// retain the original failure, and changed authority always takes precedence.
template<class Authority, class Verify, class Prepare>
auto prepare_with_source_fence(Authority&& authority, Verify&& verify_source,
                               Prepare&& prepare) -> decltype(prepare()) {
    try { return prepare(); }
    catch (const shin::context::CliError& error) {
        if (error.code() == "source-read-hash-mismatch" || error.code() == "stale-index") {
            authority();
            verify_source();
        }
        throw;
    } catch (const Error& error) {
        if (error.code == "qualification-inputs-changed") {
            authority();
            verify_source();
        }
        throw;
    }
}
} // namespace shin::observer
