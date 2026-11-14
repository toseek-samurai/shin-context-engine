#include "shin/observer/local_status.h"
#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <ctime>

namespace shin::observer {
namespace {
const Json& field(const Json& object, const char* key) {
    static const Json absent;
    const auto found = object.is_object() ? object.find(key) : object.end();
    return found == object.end() ? absent : *found;
}

std::string label(const Json& value) {
    // Paths and error text may contain terminal control characters.
    return value.is_string() ? value.dump(-1, ' ', false, Json::error_handler_t::replace) : "unknown";
}

std::string number(const Json& value, double divisor = 1.0) {
    if (!value.is_number() || value.get<double>() < 0) return "unknown";
    std::ostringstream out;
    out << std::fixed << std::setprecision(divisor == 1.0 ? 0 : 1) << value.get<double>() / divisor;
    return out.str();
}

std::string timestamp(const Json& value) {
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0) return "unknown";
    const auto time = static_cast<std::time_t>(value.get<std::int64_t>() / 1000);
    std::tm utc{};
#ifdef _WIN32
    if (::gmtime_s(&utc, &time) != 0) return "unknown";
#else
    if (!::gmtime_r(&time, &utc)) return "unknown";
#endif
    std::ostringstream out; out << std::put_time(&utc, "%Y-%m-%d %H:%M UTC"); return out.str();
}

const char* observation_name(const Json& project) {
    const auto& phase = field(project, "phase");
    for (const auto* name : {"starting", "checking", "coalescing", "current", "partial", "blocked", "stopped"})
        if (phase == name) return name;
    return "unknown";
}

bool enrollment_retry(const Json& code) {
    return code == "writer-busy" || code == "project-busy";
}
}

bool active_issue(const Json& project) {
    const auto& issue = field(project, "active_issue");
    return issue.is_boolean() ? issue.get<bool>() : field(project, "phase") == "blocked";
}

ProjectState project_state(const Json& project, bool connected) {
    if (!connected) return ProjectState::offline;
    if (active_issue(project)) return ProjectState::blocked;
    if (field(project, "recovering") == true) return ProjectState::recovering;
    const auto& query_phase = field(project, "query_phase");
    if (query_phase == "standby") return ProjectState::standby;
    const auto& ready = field(project, "query_ready");
    if (ready.is_boolean()) return ready.get<bool>() ? ProjectState::current : ProjectState::pending;
    const auto& phase = field(project, "phase");
    if (query_phase == "preparing" || phase == "starting" || phase == "coalescing" ||
        (phase == "checking" && field(project, "last_completed_phase") == "coalescing"))
        return ProjectState::pending;
    if (phase == "checking") return ProjectState::checking;
    if (phase == "current") return ProjectState::current;
    if (phase == "stopped") return ProjectState::stopped;
    return ProjectState::unknown;
}

const char* project_state_name(ProjectState state) {
    switch (state) {
    case ProjectState::offline: return "offline";
    case ProjectState::checking: return "checking";
    case ProjectState::pending: return "preparing";
    case ProjectState::recovering: return "recovering";
    case ProjectState::blocked: return "blocked";
    case ProjectState::current: return "ready";
    case ProjectState::standby: return "standby";
    case ProjectState::stopped: return "stopped";
    default: return "unknown";
    }
}

bool quality_warning(const Json& project) {
    return field(field(project, "quality_warning"), "state") == "degraded";
}

bool partial_coverage(const Json& project) {
    const auto& coverage = field(project, "source_coverage");
    const auto& reason = field(coverage, "reason");
    const auto& quarantined = field(coverage, "quarantined_files");
    const bool has_quarantine = quarantined.is_number_unsigned() ? quarantined.get<std::uint64_t>() > 0 :
        quarantined.is_number_integer() && quarantined.get<std::int64_t>() > 0;
    return field(coverage, "state") == "partial" || has_quarantine ||
        reason == "source-observation-limited" || reason == "profile-revision-pending";
}

bool project_connection_problem(const Json& status) {
    const auto& registry_error = field(status, "registry_error");
    if (registry_error.is_string() && registry_error != "") return true;
    const auto& errors = field(status, "enrollment_errors");
    return errors.is_object() && std::any_of(errors.begin(), errors.end(),
        [](const Json& error) { return !enrollment_retry(error); });
}

std::string local_status_text(const Json& status) {
    std::ostringstream out;
    if (field(status, "error").is_object()) {
        const auto& error = status.at("error");
        out << "Shin status unavailable: " << label(field(error, "code")) << '\n'
            << label(field(error, "message")) << '\n';
        return out.str();
    }
    if (field(status, "command") != "service-status") {
        out << "Shin project: " << label(field(status, "root")) << '\n'
            << "Search: " << (field(status, "local_ready") == true ? "ready" : "not ready")
            << " | State: " << label(field(status, "state")) << '\n';
        const auto& quality = field(status, "project_quality");
        if (quality.is_string()) out << "Project checks: " << label(quality) << '\n';
        if (quality_warning(status))
            out << "Search warning: some checks missed expected evidence; verified current context remains available.\n";
        const auto& coverage = field(status, "source_coverage");
        if (field(coverage, "state") == "partial")
            out << "Coverage: partial | Quarantined files: " << number(field(coverage, "quarantined_files")) << '\n';
        return out.str();
    }
    const auto& telemetry = field(status, "telemetry");
    const bool connected = field(status, "service_running") == true;
    out << "Shin service: " << (connected ? "running" : "not running")
        << " | Supervision: " << label(field(status, "supervision")) << '\n'
        << "Uptime: " << number(field(telemetry, "uptime_ms"), 1000) << " s"
        << " | Active queries: " << number(field(status, "query_workers")) << '\n';
    const auto& samples = field(telemetry, "samples");
    const auto rss = samples.is_array() && !samples.empty() ? field(samples.back(), "rss_bytes") : Json();
    out << "RAM (service + owned workers, last sample): " << number(rss, 1024 * 1024) << " MiB\n";
    const auto& metrics = field(telemetry, "request_metrics");
    const auto& mcp = field(telemetry, "mcp_activity");
    if (field(mcp, "status") == "ready")
        out << "MCP tool calls (retained): " << number(field(mcp, "attempts"))
            << " | Context calls: " << number(field(mcp, "context_calls"))
            << " | Recorded since: " << timestamp(field(mcp, "coverage_start_ms")) << '\n';
    else if (mcp.is_object()) out << "MCP activity unavailable; counts are unknown.\n";
    out << "Requests (current run): " << number(field(metrics, "responses_ready")) << " ready, "
        << number(field(metrics, "response_errors")) << " failed, "
        << number(field(metrics, "unobserved")) << " unobserved\n";

    std::map<std::string, Json> projects;
    const Json pending{{"query_phase", "preparing"}};
    for (const auto* key : {"projects", "preparing_projects"}) {
        const auto& rows = field(status, key);
        if (rows.is_array()) for (const auto& root : rows)
            if (root.is_string()) projects.try_emplace(root.get<std::string>(), pending);
    }
    const auto& states = field(telemetry, "project_states");
    if (states.is_array()) for (const auto& row : states)
        if (field(row, "root").is_string()) projects[row.at("root").get<std::string>()] = row;
    for (const auto* key : {"enrollment_errors", "resident_errors"}) {
        const bool enrollment = std::string_view(key) == "enrollment_errors";
        const auto& errors = field(status, key);
        if (errors.is_object()) for (const auto& item : errors.items()) {
            auto& project = projects.try_emplace(item.key(), pending).first->second;
            // Live rows already carry the service's decision, including retained blockers.
            if (field(project, "active_issue").is_boolean()) continue;
            const auto& code = item.value();
            const bool transient = enrollment ? enrollment_retry(code) :
                code == "project-updating" || code == "resident-disconnected";
            if (transient) continue;
            project["active_issue"] = true;
            project["error_code"] = code;
        }
    }
    const auto& registry_error = field(status, "registry_error");
    if (registry_error.is_string() && !registry_error.get<std::string>().empty())
        out << "Registry issue: " << label(registry_error) << '\n';
    out << "Projects: " << projects.size() << "\nSEARCH     OBSERVER       LAST CHECK   ROOT\n";
    for (const auto& [root, row] : projects) {
        const auto state = project_state(row, connected);
        out << std::left << std::setw(11) << project_state_name(state) << std::setw(15) << observation_name(row)
            << std::setw(13) << (number(field(row, "last_check_age_ms"), 1000) + " s") << label(root) << '\n';
        if (state == ProjectState::blocked) out << "  Issue: " << label(field(row, "error_code")) << '\n';
        if (state == ProjectState::current || state == ProjectState::standby) {
            if (partial_coverage(row)) out << "  Coverage: partial\n";
            if (quality_warning(row))
                out << "  Search warning: some checks missed expected evidence; verified current context remains available.\n";
        }
    }
    if (field(field(telemetry, "projection"), "truncated") == true)
        out << "Historical detail is truncated; current project status is retained.\n";
    out << "Standby projects are observed; their search reader warms on demand.\n"
           "This is a service observation, not a new source audit or an answer-quality score.\n";
    return out.str();
}
}
