#pragma once
#include "language.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <optional>
#include <string>
#include "shin/observer/validation_diagnostic.h"

namespace shin::ui {
using Json = nlohmann::json;

inline std::string text_field(const Json& row, const char* key, std::size_t limit = 160) {
    const auto value = row.find(key);
    if (value == row.end() || !value->is_string()) return {};
    auto text = value->get<std::string>();
    if (text.size() > limit) {
        auto end = limit;
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        text.resize(end);
        text += "…";
    }
    for (auto& c : text) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    return text;
}

// Codes are identifiers, never raw error messages or arbitrary payloads.
inline std::string error_code(const Json& row, const char* key) {
    const auto value = row.find(key);
    if (value == row.end() || !value->is_string()) return {};
    const auto code = value->get<std::string>();
    if (code.size() > 96 || !std::all_of(code.begin(), code.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    })) return "unclassified";
    return code;
}

inline std::optional<double> age_ms(const Json& row, const char* key, double display_elapsed = 0) {
    const auto value = row.find(key);
    if (value == row.end() || !value->is_number()) return {};
    const auto age = value->get<double>();
    if (!std::isfinite(age) || age < 0 || age > 1e15) return {};
    return age + std::max(0.0, display_elapsed);
}

inline std::string age_text(std::optional<double> age) {
    if (!age) return shin::ui::tr("unknown");
    if (*age < 1000) return shin::ui::tr("just now");
    if (*age < 60000) return std::to_string(static_cast<long long>(*age / 1000)) + shin::ui::tr(" s ago");
    if (*age < 3600000) return std::to_string(static_cast<long long>(*age / 60000)) + shin::ui::tr(" min ago");
    if (*age < 86400000) return std::to_string(static_cast<long long>(*age / 3600000)) + shin::ui::tr(" h ago");
    return std::to_string(static_cast<long long>(*age / 86400000)) + shin::ui::tr(" d ago");
}

inline bool active_issue(const Json& project) {
    const auto value = project.find("active_issue");
    if (value != project.end() && value->is_boolean()) return value->get<bool>();
    return text_field(project, "phase") == "blocked";
}

enum class ProjectState { offline, checking, pending, recovering, blocked, current, standby, stopped, unknown };

struct ProjectTableLayout {
    static constexpr float row_height = 26;
    static constexpr float header_height = 24;
    static constexpr float controls_height = 38;
    int page_size, pages;
    float table_height;
};

inline ProjectTableLayout project_table_layout(float available_height, int count,
        float row_height = ProjectTableLayout::row_height) {
    const auto height = std::isfinite(available_height) ? std::clamp(available_height, 0.0f, 1000.0f) : 0.0f;
    if (!std::isfinite(row_height)) row_height = ProjectTableLayout::row_height;
    row_height = std::clamp(row_height, ProjectTableLayout::row_height, 44.0f);
    const int capacity = std::clamp(static_cast<int>(std::floor(std::max(0.0f,
        height - ProjectTableLayout::header_height - ProjectTableLayout::controls_height) /
        row_height)), 1, 16);
    return {capacity, std::max(1, (std::clamp(count, 0, 16) + capacity - 1) / capacity),
        ProjectTableLayout::header_height + static_cast<float>(capacity) * row_height};
}

inline std::optional<double> resident_memory(const Json& residents, const std::string& root, bool connected) {
    if (!connected || !residents.is_array()) return {};
    for (const auto& row : residents) {
        if (text_field(row, "root", 4096) != root) continue;
        const auto bytes = row.find("rss_bytes");
        if (bytes != row.end() && bytes->is_number_unsigned()) return bytes->get<double>() / 1048576;
        if (bytes != row.end() && bytes->is_number_integer() && bytes->get<std::int64_t>() >= 0)
            return bytes->get<double>() / 1048576;
    }
    return {};
}

inline ProjectState project_state(const Json& project, bool connected) {
    if (!connected) return ProjectState::offline;
    if (active_issue(project)) return ProjectState::blocked;
    if (project.contains("recovering") && project.at("recovering") == true) return ProjectState::recovering;
    if (text_field(project, "query_phase") == "standby") return ProjectState::standby;
    if (project.contains("query_ready") && project.at("query_ready").is_boolean())
        return project.at("query_ready").get<bool>() ? ProjectState::current : ProjectState::pending;
    const auto phase = text_field(project, "phase");
    if (phase == "coalescing" || (phase == "checking" && text_field(project, "last_completed_phase") == "coalescing"))
        return ProjectState::pending;
    if (phase == "checking") return ProjectState::checking;
    if (phase == "current") return ProjectState::current;
    if (phase == "stopped") return ProjectState::stopped;
    return ProjectState::unknown;
}

inline const char* state_text(ProjectState state) {
    switch (state) {
    case ProjectState::offline: return shin::ui::tr("Service offline");
    case ProjectState::checking: return shin::ui::tr("Checking files");
    case ProjectState::pending: return shin::ui::tr("Waiting for update");
    case ProjectState::recovering: return shin::ui::tr("Recovering index");
    case ProjectState::blocked: return shin::ui::tr("Update blocked");
    case ProjectState::current: return shin::ui::tr("Current");
    case ProjectState::standby: return shin::ui::tr("On demand");
    case ProjectState::stopped: return shin::ui::tr("Observation stopped");
    default: return shin::ui::tr("Unknown state");
    }
}

struct Explanation { std::string cause, next; };

inline Explanation explain(const std::string& code) {
    if (code == "storage-quota-exceeded") return {
        shin::ui::tr("Shin reached its service storage budget and could not save a new context version."),
        shin::ui::tr("Open Shin → Storage to review search caches outside projects and safe cleanup options.")};
    if (code == "storage-free-space-low") return {
        shin::ui::tr("There is not enough free disk space after reserving space for Shin. New context was not saved."),
        shin::ui::tr("Open Shin → Storage to review free space and search caches outside projects.")};
    if (code == "storage-full") return {
        shin::ui::tr("Shin cannot save data: disk space or a storage quota is exhausted."),
        shin::ui::tr("Free some disk space. The service will retry automatically.")};
    if (code == "storage-read-only") return {
        shin::ui::tr("The disk holding Shin data is read-only."),
        shin::ui::tr("Restore write access to the disk. The service will retry automatically.")};
    if (code == "access-denied") return {
        shin::ui::tr("The operating system denied Shin access to required files."),
        shin::ui::tr("Allow Shin to access its data and the project folder in system settings. The service will retry.")};
    if (code == "root-unavailable") return {
        shin::ui::tr("The project folder is unavailable."),
        shin::ui::tr("Connect the project disk or restore the folder to its original location. Shin will resume checking automatically.")};
    if (code == "incomplete-source-observation" || code == "refresh-incomplete-source" || code == "refresh-source-budget") return {
        shin::ui::tr("Shin could not process all source files within its limits. Incomplete context was not published."),
        shin::ui::tr("Shin will retry the check. If this persists, save a report for Shin support.")};
    if (code == "refresh-validation-failed" || code == "project-check-failed") return {
        shin::ui::tr("New context failed required checks. Previous data was preserved."),
        shin::ui::tr("Shin will check the project again after changes. If this persists, save a report for Shin support; check settings do not need changing.")};
    if (code == "writer-busy" || code == "project-busy" || code == "service-busy") return {
        shin::ui::tr("Another operation is using the resource; this does not imply index corruption."),
        shin::ui::tr("Shin will retry automatically after the current operation finishes.")};
    if (code.find("authority") != std::string::npos || code.find("grant") != std::string::npos ||
        code == "binding-invalid" || code == "binding-identity-changed" || code == "refresh-policy-invalid" ||
        code == "route-revision-conflict" || code == "refresh-engine-changed") return {
        shin::ui::tr("Project connection settings changed or could not be verified safely. Updates stopped."),
        shin::ui::tr("Save a diagnostic report for Shin support. Project data and settings are not replaced automatically.")};
    if (code.find("timeout") != std::string::npos || code == "service-response-expired") return {
        shin::ui::tr("No response arrived within the deadline. This does not imply damaged sources."),
        shin::ui::tr("Shin will retry the background check. Retry the query later; if delays persist, save a report for Shin support.")};
    if (code == "service-status-unavailable" || code == "service-unavailable" || code == "client-disconnected" ||
        code == "service-disconnected" || code == "service-connect-failed") return {
        shin::ui::tr("Monitor has no fresh service response. Retained data does not confirm current project state."),
        shin::ui::tr("Wait for reconnection. If this persists, check that Shin can run in the background in your operating system settings.")};
    if (code.find("stale") != std::string::npos || code == "refresh-not-current" || code == "refresh-superseded" ||
        code == "query-snapshot-changed" || code == "refresh-base-changed") return {
        shin::ui::tr("Sources changed; the index is not yet verified against the new files."),
        shin::ui::tr("Shin will update context automatically. Wait for the update to finish.")};
    return {shin::ui::tr("Shin could not complete the operation safely; the precise cause is unknown."),
        shin::ui::tr("Save a diagnostic report for Shin support. Manual changes to the index or service files are not needed.")};
}

inline std::string historical_code(const Json& project) {
    auto code = error_code(project, "last_error_code");
    if (code.empty() && active_issue(project)) code = error_code(project, "error_code");
    return code;
}

inline std::string validation_expected_path_text(const Json& diagnostic) {
    const auto paths = shin::observer::validation_failure_expected_paths(diagnostic);
    std::string result;
    for (const auto& path : paths) {
        if (!result.empty()) result += ", ";
        result += "«" + path + "»";
    }
    return result;
}

inline bool recovered(const Json& project) {
    return !active_issue(project) && (text_field(project, "phase") == "current" ||
        text_field(project, "last_completed_phase") == "current");
}

inline std::uint64_t count(const Json& row, const char* key) {
    const auto value = row.find(key);
    if (value == row.end() || !value->is_number_integer() ||
        (value->is_number_integer() && !value->is_number_unsigned() && value->get<std::int64_t>() < 0)) return 0;
    return value->get<std::uint64_t>();
}

struct DiagnosticOverview { std::string title, summary, next; };

inline bool quality_warning(const Json& project) {
    const auto warning = project.find("quality_warning");
    return warning != project.end() && warning->is_object() && text_field(*warning, "state") == "degraded";
}

inline DiagnosticOverview diagnostic_overview(const Json& project, bool connected) {
    const auto state = project_state(project, connected);
    if ((state == ProjectState::current || state == ProjectState::standby) && project.contains("source_coverage") &&
        project.at("source_coverage").is_object() &&
        text_field(project.at("source_coverage"), "reason") == "source-observation-limited")
        return {shin::ui::tr("Context partially available"),
            shin::ui::tr("Configured source limits were reached. Only unchanged, verified files from the approved context are available."),
            shin::ui::tr("You can use the available context. Review source scope and limits to restore full coverage.")};
    if ((state == ProjectState::current || state == ProjectState::standby) && project.contains("source_coverage") &&
        project.at("source_coverage").is_object() &&
        text_field(project.at("source_coverage"), "reason") == "profile-revision-pending")
        return {shin::ui::tr("Context partially available"), shin::ui::tr("The new file inventory has not passed checks yet. Verifiable files from the previous version remain available."),
            shin::ui::tr("You can use the available context. See profile diagnostics for the coverage gap.")};
    if ((state == ProjectState::current || state == ProjectState::standby) && project.contains("source_coverage") &&
        project.at("source_coverage").is_object() && count(project.at("source_coverage"), "quarantined_files") > 0)
        return {shin::ui::tr("Context partially available"), shin::ui::tr("Files with possible secrets were excluded. Search over the rest of the project works."),
            shin::ui::tr("Review the excluded files. Shin will restore them automatically after correction.")};
    if ((state == ProjectState::current || state == ProjectState::standby) && quality_warning(project))
        return {shin::ui::tr("Ready with search warnings"),
            shin::ui::tr("Some search checks missed expected evidence. Verified current context remains available."),
            shin::ui::tr("Review the search diagnostics. The warning clears when the checks pass.")};
    if ((state == ProjectState::current || state == ProjectState::standby) && count(project, "documentation_warnings") > 0)
        return {shin::ui::tr("Ready with documentation warnings"),
            std::string(shin::ui::tr("Broken documentation links: ")) + std::to_string(count(project, "documentation_warnings")) +
                ". " + shin::ui::tr("Available context remains usable."),
            shin::ui::tr("Fix the links when convenient. Shin will clear the warning automatically.")};
    switch (state) {
    case ProjectState::offline: return {shin::ui::tr("Service offline"), shin::ui::tr("Previous data was retained but is not currently verified."), shin::ui::tr("Waiting for a fresh service response.")};
    case ProjectState::current: return {shin::ui::tr("Context is current"), shin::ui::tr("Checks passed. Shin is ready to answer project queries."), shin::ui::tr("No action needed.")};
    case ProjectState::standby: return {shin::ui::tr("Ready on demand"), shin::ui::tr("Files are observed. Search is not using memory right now."), shin::ui::tr("Search data will load on request.")};
    case ProjectState::checking: return {shin::ui::tr("Checking changes"), shin::ui::tr("Shin is comparing files with the latest context version."), shin::ui::tr("The check will finish automatically.")};
    case ProjectState::pending:
        if (text_field(project, "phase") == "current" && text_field(project, "query_phase") == "preparing")
            return {shin::ui::tr("Preparing search"), shin::ui::tr("The latest check finished. Search data is loading."), shin::ui::tr("Preparation will finish automatically.")};
        return {shin::ui::tr("Updating context"), shin::ui::tr("The new version is not ready to serve yet."), shin::ui::tr("Shin will verify it before publication.")};
    case ProjectState::recovering: return {shin::ui::tr("Recovering index"), shin::ui::tr("Shin is rebuilding service data. Sources stay unchanged."), shin::ui::tr("Recovery runs automatically.")};
    case ProjectState::stopped: return {shin::ui::tr("Observation stopped"), shin::ui::tr("Context freshness is not being checked right now."), shin::ui::tr("Review the project connection in the full report.")};
    case ProjectState::unknown: return {shin::ui::tr("Waiting for status"), shin::ui::tr("The service has not reported a project check result yet."), shin::ui::tr("Wait for the next service response.")};
    case ProjectState::blocked: break;
    }
    auto code = error_code(project, "error_code");
    if (code.empty()) code = historical_code(project);
    const auto reason = shin::observer::validation_failure_reason(project.value("validation_diagnostic", Json()));
    if (code == "storage-quota-exceeded") return {shin::ui::tr("Shin storage budget reached"),
        shin::ui::tr("The service could not save a new context version."), shin::ui::tr("Open Shin → Storage to review caches outside projects.")};
    if (code == "storage-free-space-low") return {shin::ui::tr("Free disk space needed"),
        shin::ui::tr("Shin preserved the disk reserve and did not publish a new context version."),
        shin::ui::tr("Open Shin → Storage to review free space and caches outside projects.")};
    if ((code == "refresh-validation-failed" || code == "project-check-failed") && reason == "secret-like-source-content")
        return {shin::ui::tr("Possible secret in sources"), shin::ui::tr("Context unavailable: secret detection triggered. The previous version was preserved."),
            shin::ui::tr("Review the scanner finding. Keep protection enabled; Shin will update automatically after correction.")};
    if ((code == "refresh-validation-failed" || code == "project-check-failed") && reason == "missing-required-path") {
        const auto path = validation_expected_path_text(project.value("validation_diagnostic", Json()));
        const auto summary = path.empty()
            ? shin::ui::tr("Required material was not found in search results. This does not mean the file is missing on disk.")
            : shin::ui::tr("Required material ") + path + shin::ui::tr(" was not found in search results. This does not mean the file is missing on disk.");
        return {shin::ui::tr("Search check failed"), shin::ui::tr("Context unavailable. ") + summary + shin::ui::tr(" The previous version was preserved."),
            shin::ui::tr("Shin will check the project again after changes. If the file is present, save a report for Shin support; check settings do not need changing.")};
    }
    if (code == "storage-full") return {shin::ui::tr("Insufficient space"), shin::ui::tr("Shin cannot save a new context version."), shin::ui::tr("Free some space. Shin will retry.")};
    if (code == "root-unavailable") return {shin::ui::tr("Folder unavailable"), shin::ui::tr("Shin cannot read project files."), shin::ui::tr("Connect the disk or restore the project folder.")};
    if (code == "refresh-validation-failed" || code == "project-check-failed") return {
        shin::ui::tr("Check failed"), shin::ui::tr("Context unavailable. The previous version was preserved.") +
            (reason.empty() ? std::string{} : shin::ui::tr(" Reason: ") + reason + "."),
        shin::ui::tr("Shin will check the project again after changes. If this persists, save a report for Shin support; check settings do not need changing.")};
    if (code.find("timeout") != std::string::npos) return {shin::ui::tr("Check delayed"), shin::ui::tr("The operation exceeded its deadline."), shin::ui::tr("Shin will retry the check automatically.")};
    return {shin::ui::tr("Needs attention"), shin::ui::tr("Shin paused updates to avoid serving unverified context."),
        shin::ui::tr("Open the report for the cause and next step.")};
}

inline const Json* retained_project(const Json& project, const Json& history) {
    const auto rows = history.find("projects");
    if (history.value("status", "") != "ready" || rows == history.end() || !rows->is_array()) return nullptr;
    for (const auto& row : *rows)
        if (text_field(row, "id", 96) == text_field(project, "id", 96)) return &row;
    return nullptr;
}

inline std::string diagnostic_history_status(const Json& project, bool connected) {
    if (!connected) return shin::ui::tr("Currently offline");
    if (active_issue(project)) return shin::ui::tr("Issue still active");
    if (!historical_code(project).empty()) return recovered(project) ? shin::ui::tr("Checks recovered") : shin::ui::tr("Recovery not confirmed yet");
    if (!project.contains("failures") || !project.at("failures").is_number_integer()) return shin::ui::tr("Previous check details unavailable");
    return count(project, "failures") ? shin::ui::tr("Previous failure reasons were not retained") : shin::ui::tr("No check failures recorded");
}

// Shared geometry for an icon and its adjacent counter. Both use one row center.
inline float centered_item_y(float row_height, float item_height) {
    return std::floor((row_height - item_height) / 2);
}

// Explicit allowlist: never serialize the service reply, events, usage or last_result.
inline std::string diagnostic_summary(const Json& project, const Json& service, bool connected,
                                      const std::string& connection_code, const std::string& monitor_version) {
    const auto telemetry = service.value("telemetry", Json::object());
    Json summary{{"schema_version", "shin.monitor-diagnostic.v1"},
        {"project", text_field(project, "name")}, {"project_id", text_field(project, "id", 96)},
        {"root", text_field(project, "root", 768)}, {"monitor_version", text_field(Json{{"version", monitor_version}}, "version", 64)},
        {"observer_version", text_field(service, "version", 64)}, {"core_version", text_field(service, "core_version", 64)},
        {"connected", connected}, {"phase", text_field(project, "phase", 64)},
        {"last_completed_phase", text_field(project, "last_completed_phase", 64)},
        {"active_issue_last_observed", active_issue(project)}, {"error_code", error_code(project, "error_code")},
        {"last_error_code", historical_code(project)}, {"connection_code", error_code(Json{{"code", connection_code}}, "code")},
        {"authority", text_field(project, "authority", 96)}};
    summary["documentation_warnings"] = count(project, "documentation_warnings");
    if (quality_warning(project)) summary["quality_warning"] = {{"state", "degraded"},
        {"diagnostic", shin::observer::bounded_validation_diagnostic(project.at("quality_warning").value("diagnostic", Json()))}};
    const auto validation = shin::observer::bounded_validation_diagnostic(project.value("validation_diagnostic", Json()));
    if (!validation.is_null()) summary["validation_diagnostic"] = validation;
    if (project.contains("source_coverage") && project.at("source_coverage").is_object()) {
        const auto& coverage = project.at("source_coverage");
        summary["quarantined_files"] = count(coverage, "quarantined_files");
        summary["source_coverage"] = {{"state", text_field(coverage, "state", 64)},
            {"reason", text_field(coverage, "reason", 64)}};
        const auto counts = coverage.find("source_counts");
        if (counts != coverage.end() && counts->is_object())
            for (const auto* key : {"documents", "source_facts", "candidate_files", "capped_files", "skipped_files"})
                summary["source_coverage"]["source_counts"][key] = count(*counts, key);
    }
    for (const auto* key : {"ticks", "publications", "preparations", "failures", "recoveries", "superseded_refreshes"})
        summary[key] = project.contains(key) ? Json(count(project, key)) : Json(nullptr);
    const auto history = telemetry.value("history", Json::object());
    Json retained;
    for (const auto& row : history.value("projects", Json::array()))
        if (text_field(row, "id", 96) == text_field(project, "id", 96)) retained = row;
    for (const auto* key : {"requests", "errors", "unobserved"})
        summary[std::string("retained_") + key] = history.value("status", "") == "ready" && retained.is_object() ? Json(count(retained, key)) : Json(nullptr);
    const auto captured = age_ms(telemetry, "captured_at_ms");
    summary["captured_at_ms"] = captured ? Json(*captured) : Json(nullptr);
    for (const auto* key : {"last_success_age_ms", "activity_age_ms", "last_check_age_ms"}) {
        const auto age = age_ms(project, key);
        summary[key] = age ? Json(*age) : Json(nullptr);
    }
    const auto success = age_ms(project, "last_success_age_ms");
    summary["estimated_last_success_at_ms"] = captured && success ? Json(*captured - *success) : Json(nullptr);
    const auto duration = age_ms(project, "last_tick_ms");
    summary["last_tick_ms"] = duration ? Json(*duration) : Json(nullptr);
    return summary.dump(2);
}
} // namespace shin::ui
