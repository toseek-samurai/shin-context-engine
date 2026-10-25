#include "shin/observer/local_status.h"
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
        const auto& coverage = field(status, "source_coverage");
        if (field(coverage, "state") == "partial")
            out << "Coverage: partial | Quarantined files: " << number(field(coverage, "quarantined_files")) << '\n';
        return out.str();
    }
    const auto& telemetry = field(status, "telemetry");
    out << "Shin service: " << (field(status, "service_running") == true ? "running" : "not running")
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
    for (const auto* key : {"projects", "preparing_projects"}) {
        const auto& rows = field(status, key);
        if (rows.is_array()) for (const auto& root : rows)
            if (root.is_string()) projects.try_emplace(root.get<std::string>(), Json::object());
    }
    const auto& states = field(telemetry, "project_states");
    if (states.is_array()) for (const auto& row : states)
        if (field(row, "root").is_string()) projects[row.at("root").get<std::string>()] = row;
    for (const auto* key : {"enrollment_errors", "resident_errors"}) {
        const auto& errors = field(status, key);
        if (errors.is_object()) for (const auto& item : errors.items()) {
            auto& project = projects[item.key()];
            // Resident refresh/disconnect is transient; admission failures are not.
            if (std::string_view(key) == "resident_errors" &&
                (item.value() == "project-updating" || item.value() == "resident-disconnected")) continue;
            project["active_issue"] = true;
            project["error_code"] = item.value();
        }
    }
    const auto& registry_error = field(status, "registry_error");
    if (registry_error.is_string() && !registry_error.get<std::string>().empty())
        out << "Registry issue: " << label(registry_error) << '\n';
    out << "Projects: " << projects.size() << "\nSEARCH     OBSERVER       LAST CHECK   ROOT\n";
    for (const auto& [root, row] : projects) {
        const bool issue = field(row, "active_issue") == true;
        const auto phase = field(row, "query_phase");
        const std::string search = issue ? "blocked" : field(row, "query_ready") == true ? "ready" :
            phase == "standby" ? "standby" : "preparing";
        const auto observer = field(row, "phase");
        const std::string observed = observer == "current" ? "current" : observer == "checking" ? "checking" :
            observer == "coalescing" ? "coalescing" : observer == "blocked" ? "blocked" : "unknown";
        out << std::left << std::setw(11) << search << std::setw(15) << observed
            << std::setw(13) << (number(field(row, "last_check_age_ms"), 1000) + " s") << label(root) << '\n';
        if (issue) out << "  Issue: " << label(field(row, "error_code")) << '\n';
    }
    if (field(field(telemetry, "projection"), "truncated") == true)
        out << "Historical detail is truncated; current project status is retained.\n";
    out << "Standby projects are observed; their search reader warms on demand.\n"
           "This is a service observation, not a new source audit or an answer-quality score.\n";
    return out.str();
}
}
