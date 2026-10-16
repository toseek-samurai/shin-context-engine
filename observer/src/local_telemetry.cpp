#include "shin/observer/local_telemetry.h"
#include "shin/observer/observer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__linux__)
#include <fstream>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace shin::observer {
namespace {
std::string diagnostic_code(const Json& value) {
    if (!value.is_string()) return {};
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() > 96 || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    })) return "unclassified";
    return text;
}

Json resident_bytes() {
#ifdef __APPLE__
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<std::uint64_t>(info.resident_size);
#elif defined(__linux__)
    std::ifstream stat("/proc/self/statm");
    std::uint64_t virtual_pages = 0, pages = 0;
    const auto page = ::sysconf(_SC_PAGESIZE);
    if (page > 0 && stat >> virtual_pages >> pages) return pages * static_cast<std::uint64_t>(page);
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS usage{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &usage, static_cast<DWORD>(sizeof(usage))))
        return static_cast<std::uint64_t>(usage.WorkingSetSize);
#endif
    return nullptr;
}
} // namespace

void bound_local_service_status(Json& status) {
    auto& telemetry = status.at("telemetry");
    if (!telemetry.contains("projection"))
        telemetry["projection"] = {{"byte_limit", kLocalStatusByteLimit}, {"truncated", false},
                                   {"omitted_rows", Json::object()}};
    auto& projection = telemetry.at("projection");
    auto bytes = status.dump().size();
    // Trim copies, never the live rings or SQLite. Preserve current health and
    // summary counters; discard duplicate current-run detail before history.
    struct Detail { const char* path; bool newest_first; const char* flag; };
    for (const auto& detail : {
            Detail{"/request_metrics/recent", false, "/request_metrics/recent_truncated"},
            Detail{"/refresh_diagnostics", false, "/refresh_diagnostics_truncated"},
            Detail{"/events", false, nullptr}, Detail{"/samples", false, nullptr},
            Detail{"/history/events", true, nullptr},
            Detail{"/history/refresh_diagnostics", true, "/history/refresh_diagnostics_truncated"},
            Detail{"/history/refresh_aggregates", false, "/history/refresh_aggregates_truncated"},
            Detail{"/history/admission_cohorts", true, "/history/admission_cohorts_truncated"},
            Detail{"/history/request_cohorts", true, "/history/request_cohorts_truncated"},
            Detail{"/history/timeline", false, nullptr},
            Detail{"/history/minute_timeline", false, nullptr}}) {
        const Json::json_pointer path(detail.path);
        if (!telemetry.contains(path)) continue;
        auto& rows = telemetry.at(path);
        while (bytes > kLocalStatusByteLimit && !rows.empty()) {
            // Reserve metadata growth once per batch, not a full JSON dump per row.
            const auto excess = bytes - kLocalStatusByteLimit + 256;
            std::size_t freed = 0, removed = 0;
            while (!rows.empty() && freed < excess) {
                const auto index = detail.newest_first ? rows.size() - 1 : 0;
                freed += rows.at(index).dump().size() + (rows.size() > 1 ? 1 : 0);
                rows.erase(index);
                ++removed;
            }
            auto& omissions = projection.at("omitted_rows");
            omissions[detail.path] = omissions.value(detail.path, std::size_t{0}) + removed;
            projection["truncated"] = true;
            if (detail.flag) telemetry[Json::json_pointer(detail.flag)] = true;
            bytes = status.dump().size();
        }
    }
    if (bytes > kLocalStatusByteLimit)
        throw Error("service-status-too-large", "Essential local service status exceeds its byte budget.");
}

void LocalTelemetry::open_history(const fs::path& directory) { history = std::make_unique<LocalHistory>(directory); }
void LocalTelemetry::close_history() { history.reset(); }

void LocalTelemetry::detach(const fs::path& root) {
    if (projects.erase(root.string()) == 0) return;
    events.push_back({{"project_id", digest(root.string())}, {"name", root.filename().string()},
        {"phase", "detached"}, {"at_ms", now_ms()},
        {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()},
        {"error_code", ""}, {"recoveries", 0}, {"last_recovery", ""}});
    if (events.size() > 32) events.pop_front();
    if (history) history->append("event", events.back());
}

void LocalTelemetry::record(const fs::path& root, const Json& state) {
    static const std::set<std::string> retry_reasons_allowed{"refresh-suspended", "refresh-superseded",
        "file-changed-during-verification"};
    const auto key = root.string();
    if (!projects.contains(key) && projects.size() >= 16) return;
    auto& project = projects[key];
    const auto phase = state.at("phase").get<std::string>();
    const auto ticks = state.at("ticks").get<std::uint64_t>();
    if (phase == "checking" && (project.value.is_null() || project.value.value("phase", "") != "checking"))
        project.activity = Clock::now();
    if (phase != "checking" && (project.value.is_null() || project.value.value("ticks", std::uint64_t{0}) != ticks))
        project.completed = Clock::now();
    if (phase != "checking") {
        project.completed_phase = phase;
        project.activity = {};
        if (phase == "current") project.succeeded = Clock::now();
    }
    Json value{{"id", digest(key)}, {"name", root.filename().string()}, {"root", key}, {"phase", phase},
        {"ticks", ticks}, {"publications", state.at("publications")},
        {"preparations", state.value("preparations", std::uint64_t{0})}, {"failures", state.at("failures")},
        {"superseded_refreshes", state.value("superseded_refreshes", std::uint64_t{0})},
        {"suspended_refreshes", state.value("suspended_refreshes", std::uint64_t{0})},
        {"retry_reason", retry_reasons_allowed.contains(state.value("retry_reason", ""))
            ? state.value("retry_reason", "") : ""},
        {"recoveries", state.value("recoveries", std::uint64_t{0})},
        {"last_recovery", state.value("last_recovery", "")},
        {"retry_after_ms", state.value("retry_after_ms", 1000)},
        {"idle_checks", state.value("idle_checks", 0)},
        {"change_monitor_healthy", state.value("change_monitor_healthy", false)},
        {"change_monitor_restarts", state.value("change_monitor_restarts", std::uint64_t{0})},
        {"change_monitor_recoveries", state.value("change_monitor_recoveries", std::uint64_t{0})},
        {"last_change_monitor_error", diagnostic_code(state.value("last_change_monitor_error", Json()))},
        {"authority", state.at("refresh_authority")}, {"last_tick_ms", state.value("last_tick_duration_ms", 0.0)},
        {"error_code", ""}, {"next_action", ""}};
    const auto result = state.value("last_result", Json(nullptr));
    if (result.is_object()) {
        if (result.contains("source_coverage") && result.at("source_coverage").is_object())
            value["source_coverage"] = {{"state", "partial"}, {"reason", "profile-revision-pending"}};
        if (result.contains("candidate_diagnostic")) {
            const auto diagnostic = bounded_failure_diagnostic(result.at("candidate_diagnostic"));
            if (!diagnostic.is_null()) value["validation_diagnostic"] = diagnostic;
        }
        if (result.contains("error") && result.at("error").is_object()) {
            value["error_code"] = diagnostic_code(result.at("error").value("code", Json("refresh-failed")));
            const auto diagnostic = bounded_failure_diagnostic(result.at("error").value("diagnostic", Json()));
            if (!diagnostic.is_null()) value["validation_diagnostic"] = diagnostic;
        }
        if (value.at("error_code") == "" && result.value("ok", true) == false &&
            result.value("phase", "") != "coalescing") {
            const auto observation = result.value("observation", Json(nullptr));
            if (observation.is_object()) value["error_code"] = diagnostic_code(observation.value("reason_code", Json(nullptr)));
            if (value.at("error_code") == "" && result.contains("diagnostic_ids") &&
                result.at("diagnostic_ids").is_array() && !result.at("diagnostic_ids").empty())
                value["error_code"] = "project-check-failed";
        }
        value["next_action"] = result.value("next_action", "");
    }
    const bool active_issue = phase == "blocked" || (phase == "checking" && project.completed_phase == "blocked");
    value["recovering"] = !active_issue && (phase == "checking" || phase == "coalescing") &&
        result.is_object() && result.value("recovery_pending", false);
    if (active_issue && !value.at("error_code").get<std::string>().empty())
        project.last_error = value.at("error_code").get<std::string>();
    value["active_issue"] = active_issue;
    value["last_completed_phase"] = project.completed_phase;
    value["last_error_code"] = project.last_error;
    if (phase != "checking") {
        const auto event_key = phase + ":" + value.at("publications").dump() + ":" +
            value.at("preparations").dump() + ":" + value.at("recoveries").dump() + ":" +
            value.at("suspended_refreshes").dump() + ":" + value.at("retry_reason").dump() + ":" +
            value.at("change_monitor_restarts").dump() + ":" + value.at("change_monitor_recoveries").dump() + ":" +
            value.at("error_code").get<std::string>();
        if (event_key != project.event_key) {
            events.push_back({{"project_id", value.at("id")}, {"name", value.at("name")}, {"phase", phase},
                {"at_ms", now_ms()}, {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()},
                {"error_code", value.at("error_code")}, {"recoveries", value.at("recoveries")},
                {"last_recovery", value.at("last_recovery")}});
            if (events.size() > 32) events.pop_front();
            if (history) history->append("event", events.back());
            project.event_key = event_key;
        }
    }
    project.value = std::move(value);
}

void LocalTelemetry::sample(std::size_t workers, std::optional<std::uint64_t> child_rss) {
    const auto now = Clock::now();
    if (last_sample != Clock::time_point{} && now - last_sample < std::chrono::seconds(1)) return;
    last_sample = now;
    const auto own = resident_bytes();
    const Json rss = own.is_number_unsigned() && child_rss ? Json(own.get<std::uint64_t>() + *child_rss) : Json(nullptr);
    samples.push_back({{"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count()},
        {"rss_bytes", rss}, {"workers", workers}, {"requests_started", requests}});
    if (samples.size() > 120) samples.pop_front();
    if (history && (last_persisted_sample == Clock::time_point{} || now - last_persisted_sample >= std::chrono::seconds(30))) {
        history->append("sample", samples.back());
        last_persisted_sample = now;
    }
}

void LocalTelemetry::request_result(const fs::path& root, const std::string& method, const Json& report) {
    const bool rejected = report.is_object() && report.value("stage", Json()) == "admission";
    if (!rejected && method != "query" && method != "inspect" && method != "read") return;
    const bool measured = report.is_object() && report.value("schema_version", "") == "shin.worker-metric.v1" &&
        report.contains("ok") && report.at("ok").is_boolean() &&
        report.contains("duration_ms") && report.at("duration_ms").is_number() &&
        std::isfinite(report.at("duration_ms").get<double>()) && report.at("duration_ms").get<double>() >= 0 &&
        report.at("duration_ms").get<double>() <= 32000 &&
        report.contains("response_bytes") && report.at("response_bytes").is_number_unsigned() &&
        report.at("response_bytes").get<std::uint64_t>() <= 128 * 1024;
    const auto metadata = report.is_object() ? report : Json::object();
    std::string code = diagnostic_code(metadata.value("error_code", Json()));
    if (code.size() > 96 || !std::all_of(code.begin(), code.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    })) code = "unclassified";
    const bool ok = measured && report.at("ok").get<bool>();
    if (rejected) ++admission_rejections;
    else if (!measured) ++unobserved;
    else if (ok) ++responses_ready;
    else ++response_errors;
    const auto project_id = root.empty() ? "" : digest(root.string());
    const auto opaque = [&](const char* key) -> std::string {
        const auto value = metadata.value(key, Json());
        if (!value.is_string()) return "";
        const auto text = value.get<std::string>();
        return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }) ? text : "";
    };
    const auto session = opaque("session_id"), parent = opaque("parent_request_id"), generation = opaque("generation_id");
    const bool followed = !rejected && ok && (method == "read" || method == "inspect") && !parent.empty() && !session.empty() && !generation.empty() &&
        std::any_of(request_results.begin(), request_results.end(), [&](const auto& row) {
            return row.at("request_id") == parent && row.at("session_id") == session &&
                row.at("project_id") == project_id && row.at("generation_id") == generation && row.at("method") == "query" && row.at("outcome") == "response-ready";
        });
    request_results.push_back({{"project_id", project_id}, {"method", method},
        {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()},
        {"outcome", rejected ? "admission-rejected" : !measured ? "unobserved" : ok ? "response-ready" : "response-error"},
        {"duration_ms", measured ? report.at("duration_ms") : Json(nullptr)},
        {"response_bytes", measured ? report.at("response_bytes") : Json(nullptr)}, {"error_code", code}});
    auto& row = request_results.back();
    row["stage"] = rejected ? "admission" : "execution";
    row["request_id"] = opaque("request_id");
    row["session_id"] = session;
    row["generation_id"] = generation;
    row["parent_request_id"] = parent;
    row["follow_up"] = parent.empty() ? "not-reported" : followed ? "observed" : "unverified";
    const auto bounded_ms = [](const Json& value) -> Json {
        if (!value.is_number()) return nullptr;
        const auto number = value.get<double>();
        return std::isfinite(number) && number >= 0 && number <= 60000 ? value : Json(nullptr);
    };
    row["service_duration_ms"] = bounded_ms(metadata.value("service_duration_ms", Json()));
    row["wait_ms"] = bounded_ms(metadata.value("wait_ms", Json()));
    const auto retries = metadata.value("admission_retries", Json());
    row["admission_retries"] = retries.is_number_unsigned() && retries.get<std::uint64_t>() <= 1000 ? retries : Json(0);
    if (followed) ++observed_follow_ups;
    if (row["wait_ms"].is_number() && row["wait_ms"].get<double>() > 0) {
        ++admission_waits;
        if (ok && !rejected) ++wait_recoveries;
    }
    const auto reader = metadata.value("reader_state", Json());
    row["reader_state"] = reader == "cold" || reader == "warm" ? reader : Json("unknown");
    row["purpose"] = metadata.value("purpose", Json()) == "diagnostic" ? "diagnostic" : "unclassified";
    row["phase_ms"] = Json::object();
    const auto phases = metadata.value("phase_ms", Json());
    for (const auto* phase : {"admission", "documents", "source", "source_collection", "final_verification"})
        row["phase_ms"][phase] = measured && phases.is_object() ? bounded_ms(phases.value(phase, Json())) : Json(nullptr);
    if (request_results.size() > 64) request_results.pop_front();
    if (history) history->append(rejected ? "admission" : "request", request_results.back());
}

void LocalTelemetry::refresh_result(const fs::path& root, const Json& report) {
    static const std::set<std::string> allowed{"admission", "observation", "candidate-build", "candidate-validation",
        "publication", "qualification", "source-cache", "candidate-source-cache", "source-cache-transfer",
        "final-verification"};
    static const std::set<std::string> phases_allowed{"current", "partial", "coalescing", "blocked", "paused", "unknown"};
    constexpr double max_refresh_ms = 7.0 * 24.0 * 60.0 * 60.0 * 1000.0;
    const auto outcome = report.value("outcome", "error");
    if (outcome != "success" && outcome != "error" && outcome != "paused") return;
    const auto project = root.empty() ? std::string{} : digest(root.string());
    Json phases = Json::object();
    if (report.contains("phases") && report.at("phases").is_object()) {
        for (const auto& [stage, duration] : report.at("phases").items()) {
            if (allowed.contains(stage) && duration.is_number() && std::isfinite(duration.get<double>()) &&
                duration.get<double>() >= 0.0 && duration.get<double>() <= max_refresh_ms)
                phases[stage] = duration.get<double>();
        }
    }
    auto duration = report.value("duration_ms", Json());
    if (!duration.is_number() || !std::isfinite(duration.get<double>()) ||
        duration.get<double>() < 0.0 || duration.get<double>() > max_refresh_ms) duration = nullptr;
    const auto optional_duration = [&](const char* key) -> Json {
        const auto value = report.value(key, Json());
        if (!value.is_number()) return nullptr;
        const auto number = value.get<double>();
        return std::isfinite(number) && number >= 0.0 && number <= max_refresh_ms ? value : Json(nullptr);
    };
    static const std::array<const char*, 29> work_fields{{"source_files", "source_bytes", "source_chunks",
        "source_cache_builds", "source_read_scan_ms", "source_chunking_ms", "source_index_ms", "source_persist_ms",
        "qualification_reused_output_bytes", "qualification_reused_output_files",
        "qualification_snapshot_ms", "qualification_generation_ms", "qualification_profile_ms",
        "qualification_doctor_ms", "qualification_adoption_ms",
        "validation_profile-check_ms", "validation_check_ms", "validation_doctor_ms", "validation_vacuum_ms",
        "validation_eval_ms", "validation_pack-eval_ms", "validation_adoption-check_ms",
        "source_transfer_ms", "source_transfer_verify_ms", "source_transfer_snapshot_ms",
        "validation_generation_runs", "validation_reuses", "validation_verified_source_files", "validation_verified_source_bytes"}};
    Json work = Json::object();
    const auto supplied_work = report.value("work", Json());
    if (supplied_work.is_object()) {
        for (const auto* key : work_fields) {
            const auto value = supplied_work.value(key, Json());
            if (!value.is_number()) continue;
            const auto number = value.get<double>();
            if (std::isfinite(number) && number >= 0.0 && number <= 1.0e15) work[key] = value;
        }
    }
    const auto phase = report.value("phase", "unknown");
    Json row{{"project_id", project}, {"outcome", outcome}, {"phase", phases_allowed.contains(phase) ? phase : "unknown"},
        {"duration_ms", duration},
        {"active_ms", optional_duration("active_ms")}, {"sleep_ms", optional_duration("sleep_ms")},
        {"work", std::move(work)},
        {"error_code", diagnostic_code(report.value("error_code", ""))},
        {"last_stage", allowed.contains(report.value("last_stage", "")) ? report.value("last_stage", "") : ""},
        {"phases", std::move(phases)}};
    if (refresh_results.size() == 32) refresh_results.pop_front();
    refresh_results.push_back(row);
    if (history) history->append("refresh", row);
}

Json LocalTelemetry::snapshot() const {
    const auto now = Clock::now();
    auto rows = Json::array();
    for (const auto& [key, project] : projects) {
        (void)key;
        auto value = project.value;
        value["last_check_age_ms"] = project.completed == Clock::time_point{} ? Json(nullptr) :
            Json(std::chrono::duration_cast<std::chrono::milliseconds>(now - project.completed).count());
        value["last_success_age_ms"] = project.succeeded == Clock::time_point{} ? Json(nullptr) :
            Json(std::chrono::duration_cast<std::chrono::milliseconds>(now - project.succeeded).count());
        value["activity_age_ms"] = project.activity == Clock::time_point{} ? Json(nullptr) :
            Json(std::chrono::duration_cast<std::chrono::milliseconds>(now - project.activity).count());
        rows.push_back(std::move(value));
    }
    std::vector<double> durations;
    for (const auto& row : request_results)
        if (row.at("method") == "query" && row.at("outcome") == "response-ready" && row.at("duration_ms").is_number())
            durations.push_back(row.at("duration_ms"));
    std::sort(durations.begin(), durations.end());
    const auto percentile = [&](double fraction) -> Json {
        if (durations.empty()) return nullptr;
        return durations[static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(durations.size()))) - 1];
    };
    return {{"schema_version", "shin.local-service-telemetry.v1"}, {"captured_at_ms", now_ms()},
        {"uptime_ms", std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count()},
        {"scope", "service-and-owned-workers-current-run"}, {"durable", false}, {"requests_started", requests},
        {"project_states", std::move(rows)}, {"samples", samples}, {"events", events},
        {"history", history ? history->snapshot() : Json{{"status", "disabled"}}},
        {"refresh_diagnostics", refresh_results},
        {"request_metrics", {{"scope", "worker-execution-until-response-ready"},
            {"service_duration_scope", "complete-request-received-until-response-ready"},
            {"purpose_policy", "explicit-diagnostic-otherwise-unclassified"},
            {"correlation_policy", "optional-opaque-client-session-explicit-parent"},
            {"follow_up_scope", "successful-read-or-inspect-of-recent-same-project-session-generation-query;not-task-success"},
            {"responses_ready", responses_ready}, {"response_errors", response_errors}, {"unobserved", unobserved},
            {"observed_follow_ups", observed_follow_ups}, {"admission_waits", admission_waits}, {"wait_recoveries", wait_recoveries},
            {"rpc_admission_rejections", admission_rejections}, {"recent", request_results},
            {"query_sample_count", durations.size()}, {"query_p50_ms", percentile(.5)}, {"query_p95_ms", percentile(.95)},
            {"model_attribution", "unavailable"}, {"task_outcome", "not-observed"}}}};
}
} // namespace shin::observer
