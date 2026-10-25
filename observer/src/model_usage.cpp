#include "shin/observer/model_usage.h"
#include "shin/observer/local_project.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <set>

namespace shin::observer {
namespace {
constexpr std::uint64_t maximum = 9007199254740991ULL;
const std::vector<std::string> counters{"input_tokens", "output_tokens", "cache_read_tokens", "cache_write_tokens", "reasoning_tokens", "records"};
void require(bool valid) { if (!valid) throw Error("usage-invalid", "Normalized model usage is invalid or unsupported."); }
void keys(const Json& object, std::set<std::string> expected) {
    require(object.is_object() && object.size() == expected.size());
    for (const auto& item : object.items()) require(expected.contains(item.key()));
}
std::uint64_t count(const Json& value) {
    require(value.is_number_integer() && (!value.is_number_unsigned() ? value.get<std::int64_t>() >= 0 : true));
    const auto number = value.get<std::uint64_t>(); require(number <= maximum); return number;
}
std::string label(const Json& value, std::size_t max, bool identifier) {
    require(value.is_string()); const auto text = value.get<std::string>();
    require(!text.empty() && text.size() <= max && std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }));
    if (identifier) require(std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            std::string("_.:/@+[]()-").find(static_cast<char>(c)) != std::string::npos;
    }));
    return text;
}
std::int64_t date_ms(const Json& value) {
    const auto s = label(value, 10, false);
    require(s.size() == 10 && s[4] == '-' && s[7] == '-');
    for (std::size_t i = 0; i < s.size(); ++i) if (i != 4 && i != 7) require(s[i] >= '0' && s[i] <= '9');
    const std::chrono::year_month_day date{std::chrono::year{std::stoi(s.substr(0, 4))},
        std::chrono::month{static_cast<unsigned>(std::stoi(s.substr(5, 2)))}, std::chrono::day{static_cast<unsigned>(std::stoi(s.substr(8, 2)))}};
    require(date.ok());
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::sys_days(date).time_since_epoch()).count();
}
Json totals() { Json result = Json::object(); for (const auto& key : counters) result[key] = 0ULL; return result; }
void add(Json& target, const Json& row) {
    for (const auto& key : counters) {
        const auto old = count(target.at(key)), value = count(row.at(key));
        require(old <= maximum - value); target[key] = old + value;
    }
}
} // namespace

Json summarize_model_usage(const Json& input, std::int64_t now) {
    auto expected = std::set<std::string>{"schema_version", "config_sha256", "projects", "buckets", "sources", "updated_at_unix_ms"};
    if (input.contains("context_events")) expected.insert("context_events");
    keys(input, expected);
    const bool bound_workspaces = input.at("schema_version") == "shin.model-usage.v3";
    require(bound_workspaces || input.at("schema_version") == "shin.model-usage.v2");
    const auto pin = label(input.at("config_sha256"), 64, true);
    require(pin.size() == 64 && std::all_of(pin.begin(), pin.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    const auto updated = count(input.at("updated_at_unix_ms"));
    require(updated <= static_cast<std::uint64_t>(now));
    require(input.at("projects").is_array() && !input.at("projects").empty() && input.at("projects").size() <= 24);
    Json result{{"status", "available"}, {"provenance", "shin-native-client-counters"}, {"updated_at_ms", updated},
        {"fresh", static_cast<std::uint64_t>(now) - updated <= 900000}, {"unassigned_sessions", 0ULL}, {"skipped_sessions", 0ULL}, {"projects", Json::array()}};
    struct Group { Json total = totals(), workspaces = Json::array(); std::map<std::string, Json> models, days; std::string name; };
    std::map<std::string, Group> groups;
    std::set<std::string> workspace_keys;
    groups[""].name = "Все проекты";
    for (const auto& p : input.at("projects")) {
        keys(p, bound_workspaces ? std::set<std::string>{"id", "name", "workspace_keys"} : std::set<std::string>{"id", "name"});
        const auto id = label(p.at("id"), 80, true);
        require(!groups.contains(id)); groups[id].name = label(p.at("name"), 80, false);
        if (p.contains("workspace_keys")) {
            require(p.at("workspace_keys").is_array() && !p.at("workspace_keys").empty() && p.at("workspace_keys").size() <= 32);
            for (const auto& item : p.at("workspace_keys")) {
                const auto key = label(item, 64, true);
                require(key.size() == 64 && key.find_first_not_of("0123456789abcdef") == std::string::npos && workspace_keys.insert(key).second);
                groups[id].workspaces.push_back(key);
            }
        }
    }
    require(input.at("sources").is_object() && input.at("sources").size() <= 3);
    bool complete = input.at("sources").size() == 3;
    bool sources_fresh = complete;
    for (const auto& item : input.at("sources").items()) {
        require(item.key() == "codex" || item.key() == "hermes" || item.key() == "opencode");
        const auto& s = item.value();
        keys(s, {"state", "attempted_at_unix_ms", "last_success_unix_ms", "from", "to", "source_records", "unassigned_sessions", "skipped_sessions", "skipped_reasons", "adapter_version"});
        require(s.at("skipped_reasons").is_object() && s.at("skipped_reasons").size() <= 8);
        for (const auto& reason : s.at("skipped_reasons").items()) { (void)label(reason.key(),80,true); (void)count(reason.value()); }
        require(s.at("adapter_version") == "shin-native-v1" && (s.at("state") == "available" || s.at("state") == "partial" || s.at("state") == "unavailable"));
        require(count(s.at("attempted_at_unix_ms")) <= static_cast<std::uint64_t>(now) && date_ms(s.at("from")) <= date_ms(s.at("to")));
        for (const auto* key : {"last_success_unix_ms", "source_records", "unassigned_sessions", "skipped_sessions"})
            if (!s.at(key).is_null()) (void)count(s.at(key));
        complete = complete && s.at("state") == "available";
        sources_fresh = sources_fresh && s.at("state") != "unavailable" && !s.at("last_success_unix_ms").is_null() &&
            count(s.at("last_success_unix_ms")) <= static_cast<std::uint64_t>(now) &&
            static_cast<std::uint64_t>(now) - count(s.at("last_success_unix_ms")) <= 900000;
        for (const auto* key : {"unassigned_sessions", "skipped_sessions"}) {
            const auto old = count(result.at(key)), n = count(s.at(key));
            require(old <= maximum - n); result[key] = old + n;
        }
    }
    result["complete"] = complete;
    result["sources"] = input.at("sources");
    result["fresh"] = result.at("fresh").get<bool>() && sources_fresh;
    require(input.at("buckets").is_array() && input.at("buckets").size() <= 4000);
    std::set<std::tuple<std::string, std::string, std::string, std::string>> seen;
    for (const auto& row : input.at("buckets")) {
        std::set<std::string> expected{"date", "project", "client", "model", "measurement"}; expected.insert(counters.begin(), counters.end()); keys(row, std::move(expected));
        const auto project = label(row.at("project"), 80, true), client = label(row.at("client"), 16, true), model = label(row.at("model"), 120, true);
        require(groups.contains(project) && (client == "codex" || client == "hermes" || client == "opencode"));
        const auto day = row.at("date").get<std::string>(); const auto timestamp = date_ms(row.at("date"));
        require(timestamp >= 0 && timestamp <= now && seen.emplace(day, project, client, model).second);
        for (const auto& key : counters) (void)count(row.at(key));
        require(row.at("measurement") == "usage-event" || row.at("measurement") == "session-total");
        for (const auto& id : {std::string{}, project}) {
            auto& group = groups.at(id); add(group.total, row);
            const auto key = client + ":" + model;
            if (!group.models.contains(key)) {
                require(group.models.size() < 128);
                auto value = totals(); value["model"] = model; value["client"] = client; group.models[key] = std::move(value);
            }
            add(group.models.at(key), row);
            if (row.at("measurement") == "session-total") continue;
            if (!group.days.contains(day)) {
                require(group.days.size() < 100);
                auto value = totals(); value["at_ms"] = timestamp; group.days[day] = std::move(value);
            }
            add(group.days.at(day), row);
        }
    }
    for (auto& [id, group] : groups) {
        Json models = Json::array(), days = Json::array();
        for (auto& [key, value] : group.models) { (void)key; models.push_back(std::move(value)); }
        std::sort(models.begin(), models.end(), [](const auto& a, const auto& b) { return a.at("records").template get<std::uint64_t>() > b.at("records").template get<std::uint64_t>(); });
        for (auto& [key, value] : group.days) { (void)key; days.push_back(std::move(value)); }
        result["projects"].push_back({{"id", id}, {"name", group.name}, {"workspace_keys", group.workspaces},
            {"totals", group.total}, {"models", models}, {"days", days}});
    }
    if (input.contains("context_events")) try {
        const auto& events = input.at("context_events");
        keys(events, {"schema_version","source","events","omitted","complete"});
        require(events.at("schema_version") == "shin.context-events.v1" && events.at("source") == "codex" &&
            events.at("complete").is_boolean() && events.at("events").is_array() && events.at("events").size() <= 64);
        (void)count(events.at("omitted"));
        const auto opaque = [&](const Json& id) {
            const auto text = label(id,64,true); require(text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos);
        };
        for (const auto& marker : events.at("events")) {
            auto fields = std::set<std::string>{"event_id","session_id","at_ms","kind"};
            for (const auto* key : {"turn_id","window_id","model_id"}) if (marker.contains(key)) fields.insert(key);
            keys(marker,fields); opaque(marker.at("event_id")); opaque(marker.at("session_id"));
            require(count(marker.at("at_ms")) <= static_cast<std::uint64_t>(now));
            require(std::set<Json>{"compaction","turn-start","turn-finish","turn-abort","turn-model"}.contains(marker.at("kind")));
            for (const auto* key : {"turn_id","window_id"}) if (marker.contains(key)) opaque(marker.at(key));
            if (marker.contains("model_id")) { require(marker.at("kind") == "turn-model" && marker.at("model_id").is_string() && valid_reported_model(marker.at("model_id").get<std::string>())); }
        }
        result["context_events"] = events;
    } catch (const std::exception&) { result["context_events"] = {{"status","invalid"}}; }
    return result;
}
Json read_model_usage(const fs::path& path, std::int64_t now) {
    (void)directory_identity(path.parent_path(), true);
    const auto input = parse_json(read_text(path, 2 * 1024 * 1024, true));
    if (input.value("schema_version", "") == "shin.registered-usage.v1") {
        auto snapshot = input.at("snapshot");
        if (input.contains("context_events")) snapshot["context_events"] = input.at("context_events");
        auto result = summarize_model_usage(snapshot, now);
        const auto health_dir = path.parent_path().parent_path()/"usage-health";
        if (fs::exists(fs::symlink_status(health_dir))) {
            StateStore health(health_dir,false);
            if (const auto state = health.load(); state && !state->value("error", "").empty()) {
                result["fresh"] = false; result["complete"] = false;
                result["collection_error"] = state->at("error");
            }
        }
        return result;
    }
    return summarize_model_usage(input, now);
}
} // namespace shin::observer
