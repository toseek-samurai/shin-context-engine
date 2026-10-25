#include "local_session_analysis.h"
#include <algorithm>
#include <map>
#include <set>

namespace shin::observer {
namespace {
bool opaque(const Json& value) {
    return value.is_string() && value.get_ref<const std::string&>().size() == 64 &&
        value.get_ref<const std::string&>().find_first_not_of("0123456789abcdef") == std::string::npos;
}
struct Reads {
    std::uint64_t count = 0, unknown = 0, repeats = 0, revisited = 0;
    std::set<std::string> files;
    std::map<std::string,std::int64_t> versions;
    Json summary() const {
        return {{"reads",count},{"unknown_reads",unknown},{"files",files.size()},{"versions",versions.size()},
            {"same_version_repeats",repeats},{"additional_versions",versions.size()-files.size()},{"previously_seen_versions",revisited}};
    }
    void add(const Json& call, const std::map<std::string,std::int64_t>& prior, std::int64_t boundary = 0) {
        ++count;
        if (!opaque(call.value("project_id",Json())) || !opaque(call.value("read_file_id",Json())) ||
            !opaque(call.value("read_version_id",Json()))) { ++unknown; return; }
        const auto file = call.at("project_id").get<std::string>()+":"+call.at("read_file_id").get<std::string>();
        const auto version = file+":"+call.at("read_version_id").get<std::string>();
        files.insert(file);
        if (!versions.emplace(version,call.at("at_ms").get<std::int64_t>()).second) ++repeats;
        else if (const auto previous = prior.find(version); previous != prior.end() && previous->second < boundary) ++revisited;
    }
};
struct Episode {
    std::int64_t start = 0, end = 0;
    std::string kind, end_kind;
    bool closed = false, ambiguous = false, unfinished = false;
    std::uint64_t context = 0, errors = 0, probes = 0;
    Reads reads;
};
}
Json analyze_session_activity(const Json& calls, const Json& usage, const std::string& session,
                              std::int64_t from, std::int64_t until, bool calls_complete) {
    Json result{{"schema_version","shin.session-analysis.v1"},{"episodes",Json::array()},
        {"calls_complete",calls_complete},{"host_markers_complete",false},{"episode_source","registered-codex-events"},
        {"association","explicit-boundaries-and-timestamps"},{"unassigned_context_calls",0}};
    if (!calls.is_array() || calls.size() > 10000 || from < 0 || until < from) return result;
    const auto host = usage.is_object() ? usage.value("context_events",Json::object()) : Json::object();
    auto events = host.is_object() ? host.value("events",Json::array()) : Json::array();
    const auto observed = usage.is_object() ? usage.value("updated_at_ms",Json()) : Json();
    const bool available = host.is_object() && host.value("schema_version",Json()) == "shin.context-events.v1" &&
        host.value("source",Json()) == "codex" && events.is_array() && events.size() <= 64 &&
        observed.is_number_integer() && observed.get<std::int64_t>() >= 0 && observed.get<std::int64_t>() <= until;
    const auto horizon = available ? observed.get<std::int64_t>() : 0;
    bool host_fresh = available && usage.value("fresh",Json()) == true;
    const auto sources = usage.is_object() ? usage.value("sources",Json::object()) : Json::object();
    const auto codex = sources.is_object() ? sources.value("codex",Json::object()) : Json::object();
    if (available && codex.is_object() && !codex.empty()) {
        const auto success = codex.value("last_success_unix_ms",Json());
        host_fresh = codex.value("state",Json()) == "available" && until-horizon <= 900000 &&
            success.is_number_integer() && success.get<std::int64_t>() <= until && until-success.get<std::int64_t>() <= 900000 &&
            !usage.contains("collection_error");
    }
    result["host_markers_complete"] = host_fresh && host.value("complete",Json()) == true;
    std::vector<Json> boundaries;
    std::set<std::string> ids;
    if (available) for (const auto& event : events) {
        if (!event.is_object() || event.value("session_id",Json()) != session || !opaque(event.value("event_id",Json()))) continue;
        const auto at = event.value("at_ms",Json());
        const auto kind = event.value("kind",Json());
        if (!at.is_number_integer() || at.get<std::int64_t>() < 0 || at.get<std::int64_t>() > horizon ||
            (kind != "turn-start" && kind != "turn-finish" && kind != "turn-abort" && kind != "compaction")) continue;
        if (ids.insert(event.at("event_id").get<std::string>()).second) boundaries.push_back(event);
    }
    std::sort(boundaries.begin(),boundaries.end(),[](const Json& a,const Json& b) { return a.at("at_ms") < b.at("at_ms"); });
    std::vector<Episode> episodes;
    std::set<std::int64_t> boundary_times;
    std::string turn;
    for (std::size_t i = 0; i < boundaries.size(); ++i) {
        const auto& event = boundaries[i];
        const auto at = event.at("at_ms").get<std::int64_t>();
        boundary_times.insert(at);
        const bool tied = (i && boundaries[i-1].at("at_ms") == at) || (i+1 < boundaries.size() && boundaries[i+1].at("at_ms") == at);
        const auto identity = event.value("turn_id",Json());
        const auto next_turn = opaque(identity) ? identity.get<std::string>() : std::string();
        if (!episodes.empty() && !episodes.back().closed) {
            auto& previous = episodes.back(); previous.end = at; previous.closed = true;
            previous.end_kind = event.at("kind").get<std::string>();
            previous.ambiguous = tied || (!next_turn.empty() && !turn.empty() && next_turn != turn && event.at("kind") != "turn-start");
        }
        if (tied) { turn.clear(); continue; }
        if (event.at("kind") == "turn-start") turn = next_turn;
        else if (event.at("kind") != "compaction") { turn.clear(); continue; }
        Episode episode; episode.start = at; episode.end = horizon;
        episode.kind = event.at("kind").get<std::string>();
        episodes.push_back(std::move(episode));
    }
    Reads all;
    const bool history_complete = calls_complete;
    std::uint64_t unassigned = 0;
    // Calls arrive in chronological order from the bounded retained journal.
    for (const auto& call : calls) {
        if (!call.is_object() || !call.value("at_ms",Json()).is_number_integer()) continue;
        const auto at = call.at("at_ms").get<std::int64_t>();
        if (at < from || at > until) continue;
        Episode* episode = nullptr;
        if (!boundary_times.contains(at)) for (auto& candidate : episodes)
            if (!candidate.ambiguous && at > candidate.start && at < candidate.end) { episode = &candidate; break; }
        const bool probe = call.value("purpose",Json()) == "diagnostic";
        if (probe) { if (episode) ++episode->probes; continue; }
        const auto method = call.value("method",Json());
        if (call.value("outcome",Json()) == "unfinished" || call.value("write_state",Json()) == "unobserved") {
            calls_complete = false;
            if (episode) episode->unfinished = true;
        }
        const bool context = method == "query" || method == "inspect" || method == "read" || method == "resume";
        if (context) {
            if (!episode) ++unassigned;
            else {
                ++episode->context;
                if (call.value("outcome",Json()) == "tool-error" || call.value("outcome",Json()) == "rpc-error") ++episode->errors;
            }
        }
        if (method == "read" && call.value("outcome",Json()) == "success" && call.value("write_state",Json()) == "written") {
            if (episode) episode->reads.add(call,all.versions,episode->start);
            all.add(call,{});
        }
    }
    result["calls_complete"] = calls_complete;
    result["reads"] = all.summary(); result["unassigned_context_calls"] = unassigned;
    std::size_t eligible = 0;
    for (const auto& episode : episodes) if (!episode.ambiguous && episode.end > from && episode.start < until) ++eligible;
    for (auto it = episodes.rbegin(); it != episodes.rend() && result["episodes"].size() < 3; ++it) {
        if (it->ambiguous || it->end <= from || it->start >= until) continue;
        result["episodes"].push_back({{"start_ms",it->start},{"end_ms",it->end},{"kind",it->kind},
            {"closed",it->closed},{"end_kind",it->end_kind},{"interval_complete",history_complete && !it->unfinished && result.at("host_markers_complete") == true && it->closed && it->start >= from && it->reads.unknown == 0},
            {"context_calls",it->context},{"errors",it->errors},{"diagnostic_calls",it->probes},{"reads",it->reads.summary()}});
    }
    result["episodes_truncated"] = eligible > 3;
    return result;
}
}
