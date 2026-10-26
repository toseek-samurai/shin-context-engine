#pragma once
#include "language.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <optional>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace shin::ui {
// The service exports eight recent sessions with at most eight call events.
// The view merges those with host markers and keeps six recent events per session.
inline constexpr std::size_t session_activity_max_sessions = 8;
inline constexpr std::size_t session_activity_max_call_events = 8;
inline constexpr std::size_t session_activity_max_events = 6;
inline constexpr std::size_t session_activity_max_journal_events = session_activity_max_sessions * session_activity_max_events;
enum class SessionEventTone { neutral, success, error, pending, host };
struct SessionActivityEvent {
    std::int64_t at_ms = 0;
    std::string label, method;
    SessionEventTone tone = SessionEventTone::neutral;
};
struct SessionActivityEntry {
    std::string id, calls, errors, tasks;
    std::vector<std::string> details;
    std::vector<SessionActivityEvent> events;
};
struct SessionActivity {
    bool available = false;
    bool incomplete = false;
    std::vector<SessionActivityEntry> sessions;
    std::vector<std::string> notes;
};
// Structured presentation keeps the desktop summary separate from detailed
// observations. Terminal clients retain the same full text through the adapter below.
inline SessionActivity session_activity(const nlohmann::json& telemetry, const nlohmann::json& usage = {}) {
    using Json = nlohmann::json;
    SessionActivity result;
    if (!telemetry.is_object()) return result;
    const auto activity = telemetry.value("mcp_activity",Json::object());
    if (!activity.is_object() || activity.value("status",Json()) != "ready") return result;
    const auto rows = activity.value("session_rows",Json::array());
    if (!rows.is_array() || rows.size() > session_activity_max_sessions) return result;
    result.available = true;
    result.notes.push_back(tr("Caller-reported recovery and explicit host compaction are different events."));
    const auto events = usage.is_object() ? usage.value("context_events",Json::object()) : Json::object();
    const auto markers = events.is_object() ? events.value("events",Json::array()) : Json::array();
    const auto n = [](const Json& row, const char* key) { const auto value = row.value(key,Json());
        return value.is_number_integer() && value.get<std::int64_t>() >= 0 ? std::to_string(value.get<std::uint64_t>()) : std::string("—"); };
    for (const auto& row : rows) {
        if (!row.is_object()) continue;
        const auto identity = row.value("session_id",Json());
        if (!identity.is_string()) continue;
        const auto id = identity.get<std::string>();
        if (id.size() != 64 || id.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
        result.sessions.push_back({id,n(row,"context_calls"),n(row,"errors"),n(row,"tasks"),{}, {}});
        auto& lines = result.sessions.back().details;
        lines.push_back(std::string(tr("Recovery requests: "))+n(row,"recovery_requests"));
        std::string models;
        const auto reported = row.value("models",Json::object());
        if (reported.is_object()) for (const auto& [name,count] : reported.items()) {
            (void)count;
            if (name.size() <= 64 && name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.:/-") == std::string::npos)
                models += (models.empty() ? "" : ", ")+name;
        }
        lines.push_back(std::string(tr("Models reported in calls: "))+(models.empty() ? tr("unknown") : models)+tr(" · Unknown: ")+n(row,"model_unknown"));
        std::size_t compactions = 0; std::int64_t last_compaction = 0; std::string history_model;
        if (markers.is_array() && markers.size() <= 64) for (const auto& marker : markers) {
            if (!marker.is_object() || marker.value("session_id",Json()) != id) continue;
            if (marker.value("kind",Json()) == "compaction") {
                ++compactions;
                const auto when = marker.value("at_ms",Json());
                if (when.is_number_integer()) last_compaction = std::max(last_compaction,when.get<std::int64_t>());
            }
            if (history_model.empty() && marker.value("kind",Json()) == "turn-model") {
                const auto name = marker.value("model_id",Json());
                if (name.is_string() && name.get_ref<const std::string&>().size() <= 64 &&
                    name.get_ref<const std::string&>().find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.:/-") == std::string::npos)
                    history_model = name.get<std::string>();
            }
        }
        if (compactions) lines.push_back(std::string(tr("Observed host compactions: "))+std::to_string(compactions));
        else lines.push_back(tr("Host compaction markers: not observed in retained data."));
        if (!history_model.empty()) lines.push_back(std::string(tr("Model in client history: "))+history_model);
        const auto times = row.value("query_times",Json::array());
        const auto observed_since = activity.value("window_start_ms",Json());
        if (last_compaction && observed_since.is_number_integer() && last_compaction >= observed_since.get<std::int64_t>() &&
            activity.value("dropped_updates",Json()) == 0 && activity.value("unconfirmed_updates",Json()) == 0 &&
            times.is_array() && !times.empty() && times.size() <= 8) {
            std::optional<std::int64_t> next;
            for (const auto& time : times) if (time.is_number_integer() && time.get<std::int64_t>() >= last_compaction)
                next = next ? std::min(*next,time.get<std::int64_t>()) : time.get<std::int64_t>();
            if (next && (row.value("queries_complete",Json()) == true ||
                (times.back().is_number_integer() && last_compaction >= times.back().get<std::int64_t>()))) {
                std::ostringstream delay; delay << std::fixed << std::setprecision(1) << static_cast<double>(*next-last_compaction)/1000.0;
                lines.push_back(std::string(tr("Next recorded query after compaction marker: "))+decimal(delay.str())+tr(" s"));
            }
        }
        const auto analysis = row.value("analysis",Json::object());
        if (analysis.is_object() && analysis.value("schema_version",Json()) == "shin.session-analysis.v1") {
            const auto reads = analysis.value("reads",Json::object());
            if (reads.is_object()) {
                lines.push_back(std::string(tr("File reads (non-probe, written): "))+n(reads,"reads")+
                    tr(" · Same version again: ")+n(reads,"same_version_repeats"));
                lines.push_back(std::string(tr("Additional file versions: "))+n(reads,"additional_versions")+
                    tr(" · Read identity unknown: ")+n(reads,"unknown_reads"));
            }
            const auto episodes = analysis.value("episodes",Json::array());
            if (episodes.is_array() && episodes.size() <= 3 && !episodes.empty()) {
                lines.push_back(tr("Episodes · registered Codex history (UTC)"));
                for (const auto& episode : episodes) {
                    if (!episode.is_object()) continue;
                    const auto start = episode.value("start_ms",Json());
                    if (!start.is_number_integer() || start.get<std::int64_t>() < 0) continue;
                    const auto epoch = static_cast<std::time_t>(start.get<std::int64_t>()/1000);
                    std::tm utc{}; char time[24]{};
#if defined(_WIN32)
                    const bool valid = ::gmtime_s(&utc,&epoch) == 0;
#else
                    const bool valid = ::gmtime_r(&epoch,&utc) != nullptr;
#endif
                    if (valid) (void)std::strftime(time,sizeof(time),"%m-%d %H:%M",&utc);
                    const char* title = episode.value("kind",Json()) == "compaction" ? "After host compaction" : "Host turn";
                    lines.push_back(std::string("  ")+time+" · "+tr(title)+(episode.value("closed",Json()) == true ? "" : tr(" · open at last collection")));
                    lines.push_back(std::string("    ")+tr("Recorded context calls: ")+n(episode,"context_calls")+tr(" · Errors: ")+n(episode,"errors"));
                    const auto detail = episode.value("reads",Json::object());
                    if (detail.is_object()) lines.push_back(std::string("    ")+tr("Same version again: ")+n(detail,"same_version_repeats")+
                        tr(" · Versions seen before boundary: ")+n(detail,"previously_seen_versions"));
                    if (episode.value("interval_complete",Json()) != true) lines.push_back(std::string("    ")+tr("Partial interval; calls or boundaries may be missing."));
                }
                if (analysis.value("episodes_truncated",Json()) == true) lines.push_back(tr("Only the latest three observed episodes are shown."));
            } else lines.push_back(tr("Episode boundaries are unavailable; no topics are inferred."));
            lines.push_back(std::string(tr("Context calls outside known intervals: "))+n(analysis,"unassigned_context_calls"));
            if (analysis.value("calls_complete",Json()) != true) lines.push_back(tr("Read counts cover retained observations with gaps."));
        }
        std::vector<SessionActivityEvent> recent;
        if (markers.is_array() && markers.size() <= 64) for (const auto& marker : markers) {
            if (!marker.is_object() || marker.value("session_id",Json()) != id || !marker.value("at_ms",Json()).is_number_integer()) continue;
            const auto kind = marker.value("kind",Json());
            const char* title = kind == "compaction" ? "Host compaction" : kind == "turn-start" ? "Client turn started" :
                kind == "turn-finish" ? "Client turn ended" : kind == "turn-abort" ? "Client turn aborted" : nullptr;
            if (title) recent.push_back({marker.at("at_ms").get<std::int64_t>(),tr(title),"",SessionEventTone::host});
        }
        const auto timeline = row.value("timeline",Json::array());
        if (timeline.is_array()) for (std::size_t i = 0; i < std::min(session_activity_max_call_events,timeline.size()); ++i) {
            const auto& call = timeline.at(i);
            if (!call.is_object()) continue;
            std::string text;
            for (const auto* key : {"method","intent","outcome"}) {
                const auto token = call.value(key,Json());
                if (token.is_string() && token.get_ref<const std::string&>().size() <= 32 &&
                    token.get_ref<const std::string&>().find_first_not_of("abcdefghijklmnopqrstuvwxyz-") == std::string::npos)
                    text += (text.empty() ? "" : " · ") + std::string(tr(token.get_ref<const std::string&>().c_str()));
            }
            const auto when = call.value("at_ms",Json());
            const auto outcome = call.value("outcome",Json());
            const auto method = call.value("method",Json());
            const auto tone = outcome == "success" ? SessionEventTone::success : outcome == "tool-error" || outcome == "rpc-error" ?
                SessionEventTone::error : outcome == "unfinished" ? SessionEventTone::pending : SessionEventTone::neutral;
            recent.push_back({when.is_number_integer() ? when.get<std::int64_t>() : 0,std::move(text),
                method.is_string() && (method == "query" || method == "read" || method == "inspect") ? method.get<std::string>() : "",tone});
        }
        std::stable_sort(recent.begin(),recent.end(),[](const auto& a,const auto& b) { return a.at_ms > b.at_ms; });
        result.sessions.back().events.assign(recent.begin(),recent.begin()+std::min(session_activity_max_events,recent.size()));
        if (!recent.empty()) lines.push_back(tr("Timeline (UTC)"));
        for (std::size_t i = 0; i < std::min(session_activity_max_events,recent.size()); ++i) {
            const auto epoch = static_cast<std::time_t>(recent[i].at_ms/1000); std::tm utc{}; char time[24]{};
#if defined(_WIN32)
            const bool valid = recent[i].at_ms > 0 && ::gmtime_s(&utc,&epoch) == 0;
#else
            const bool valid = recent[i].at_ms > 0 && ::gmtime_r(&epoch,&utc) != nullptr;
#endif
            if (valid) (void)std::strftime(time,sizeof(time),"%m-%d %H:%M",&utc);
            lines.push_back(std::string("  ")+time+"  "+recent[i].label);
        }
    }
    auto& lines = result.notes;
    if (activity.value("sessions_truncated",Json()) == true || !events.is_object() || events.value("complete",Json()) != true)
        lines.push_back(tr("Recent bounded observations only; missing markers do not establish absence of compaction."));
    result.incomplete = activity.value("dropped_updates",Json()) != 0 || activity.value("unconfirmed_updates",Json()) != 0;
    if (result.incomplete) lines.push_back(tr("MCP history has unobserved updates; totals may be incomplete."));
    lines.push_back(tr("Read counts cover shin_read only; local and IDE readers are not observed."));
    lines.push_back(tr("Repeated reads can be useful; file versions do not measure wasted work."));
    lines.push_back(tr("These observations do not prove context use, recovery quality or task success."));
    return result;
}

inline std::vector<std::string> session_activity_lines(const nlohmann::json& telemetry, const nlohmann::json& usage = {}) {
    const auto activity = session_activity(telemetry,usage);
    if (!activity.available) return {tr("Session activity is unavailable.")};
    std::vector<std::string> lines{tr("Sessions · retained MCP observations"),activity.notes.front()};
    if (activity.sessions.empty()) lines.push_back(tr("No correlated MCP sessions have been recorded yet."));
    for (const auto& session : activity.sessions) {
        lines.push_back(""); lines.push_back(std::string(tr("Session "))+session.id.substr(0,8));
        lines.push_back(std::string(tr("Context calls: "))+session.calls+tr(" · Errors: ")+session.errors+tr(" · Tasks: ")+session.tasks);
        lines.insert(lines.end(),session.details.begin(),session.details.end());
    }
    lines.insert(lines.end(),activity.notes.begin()+1,activity.notes.end());
    return lines;
}
}
