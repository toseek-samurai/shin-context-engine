#include "shin/observer/model_usage.h"
#include "shin/observer/local_registry.h"
#include "shin/observer/observer.h"
#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <thread>
#include <array>
#include <pwd.h>
#include <unistd.h>

namespace shin::observer {
namespace {
void require(bool ok, const char* code) {
    if (!ok) throw Error(code, "Automatic model history could not be collected; existing history is preserved.");
}
Json project_metadata(const Json& projects) {
    Json result = Json::array();
    for (const auto& p : projects) {
        Json keys = Json::array();
        for (const auto& path : p.at("paths")) keys.push_back(digest(path.get<std::string>()));
        result.push_back({{"id",p.at("id")},{"name",p.at("name")},{"workspace_keys",keys}});
    }
    return result;
}
}

Json default_model_usage_sources(char** environment) {
    std::map<std::string,std::string> variables;
    for (auto** entry = environment; entry && *entry; ++entry) {
        const std::string_view text(*entry); const auto end = text.find('=');
        const std::string key(text.substr(0,end));
        if (end != std::string_view::npos && (key == "HOME" || key == "CODEX_HOME" || key == "XDG_DATA_HOME" || key == "HERMES_HOME"))
            variables[key] = text.substr(end+1);
    }
    if (!variables.contains("HOME")) {
        struct passwd user_record{}; struct passwd* found = nullptr; std::array<char,16384> buffer{};
        if (::getpwuid_r(::geteuid(), &user_record, buffer.data(), buffer.size(), &found) == 0 && found && found->pw_dir)
            variables["HOME"] = found->pw_dir;
    }
    const fs::path user = variables["HOME"];
    const auto path = [&](const char* key, const fs::path& fallback) {
        const auto found = variables.find(key);
        const fs::path value = found == variables.end() ? fallback : fs::path(found->second);
        require(value.is_absolute() && value.lexically_normal() == value, "usage-source-path-invalid");
        return value;
    };
    require(user.is_absolute(), "usage-source-path-invalid");
    const auto codex = path("CODEX_HOME", user/".codex");
    const auto data = path("XDG_DATA_HOME", user/".local/share");
    const auto hermes = path("HERMES_HOME", user/".hermes");
    fs::path database = codex/"state_5.sqlite";
    // Discover only the versioned metadata database, never recurse through chats.
    std::error_code error; unsigned latest = 0; std::size_t count = 0;
    for (fs::directory_iterator it(codex,error), end; !error && it != end && ++count <= 128; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (!name.starts_with("state_") || !name.ends_with(".sqlite")) continue;
        const auto number = std::string_view(name).substr(6,name.size()-13); unsigned version = 0;
        const auto parsed = std::from_chars(number.data(),number.data()+number.size(),version);
        if (parsed.ec == std::errc{} && parsed.ptr == number.data()+number.size() && version > latest) {
            latest = version; database = it->path();
        }
    }
    return {{"codex",{{"database",database.string()},{"sessions",(codex/"sessions").string()},{"archives",(codex/"archived_sessions").string()}}},
            {"opencode",{{"database",(data/"opencode/opencode.db").string()}}},
            {"hermes",{{"database",(hermes/"state.db").string()}}}};
}

Json collect_registered_model_usage(const fs::path& home, const Json& sources, const Json& seed, std::stop_token stop) {
    const auto registry_stamp = read_text(home/"registry/state.json",kMaximumStateBytes,true);
    const auto registry = registered_local_projects(home);
    const auto directory = home/"usage";
    ensure_private_directory(directory);
    StateStore store(directory,true); store.recover_pending();
    const auto previous = store.load();
    if (previous) require(previous->value("schema_version","") == "shin.registered-usage.v1", "usage-state-invalid");
    require(!previous || seed.is_null(), "usage-already-configured");
    auto config = previous ? previous->at("config") : seed.is_null() ?
        Json{{"schema_version","shin.usage-collector.v1"},{"projects",Json::array()},{"sources",sources}} : seed;
    require(config.at("schema_version") == "shin.usage-collector.v1" && config.at("projects").is_array(), "usage-config-invalid");
    if (previous) {
        const auto& old = previous->at("snapshot");
        (void)summarize_model_usage(old, now_ms());
        require(old.at("config_sha256") == digest(config.dump()) && old.at("projects") == project_metadata(config.at("projects")), "usage-config-changed");
    }
    std::set<std::string> paths, ids, active_roots;
    for (const auto& project : config.at("projects")) {
        require(ids.insert(project.at("id").get<std::string>()).second, "usage-config-invalid");
        for (const auto& path : project.at("paths")) require(paths.insert(path.get<std::string>()).second, "usage-config-invalid");
    }
    for (const auto& project : registry) {
        const auto root = project.root.string(); active_roots.insert(root);
        if (paths.contains(root)) continue;
        require(config.at("projects").size() < 24, "usage-project-capacity");
        const auto id = digest(root);
        require(ids.insert(id).second, "usage-project-conflict");
        auto name = project.root.filename().string();
        // Names have the same bounded display contract as existing usage groups.
        if (name.size() > 80 || std::any_of(name.begin(),name.end(),[](unsigned char c){return c < 32 || c == 127;})) name = id.substr(0,12);
        config["projects"].push_back({{"id",id},{"name",name},{"paths",Json::array({root})}});
        paths.insert(root);
    }
    auto active_config = config; active_config["projects"] = Json::array(); std::set<std::string> active_ids;
    for (const auto& project : config.at("projects")) {
        if (std::any_of(project.at("paths").begin(),project.at("paths").end(),[&](const auto& path){return active_roots.contains(path.template get<std::string>());})) {
            active_config["projects"].push_back(project); active_ids.insert(project.at("id").get<std::string>());
        }
    }
    if (active_ids.empty()) return {{"ok",true},{"collected",false},{"reason","no-active-projects"}};
    auto result = collect_model_usage(active_config,now_ms(),stop,home);
    result["config_sha256"] = digest(config.dump()); result["projects"] = project_metadata(config.at("projects"));
    if (previous) {
        const auto& old = previous->at("snapshot");
        // Preserve disconnected groups and unavailable clients without adding the
        // same cumulative bucket twice. Reconnected groups are recollected once.
        auto retained = Json::array();
        for (const auto& row : result.at("buckets"))
            if (result.at("sources").at(row.at("client").get<std::string>()).at("state") != "unavailable") retained.push_back(row);
        for (const auto& row : old.at("buckets"))
            if (!active_ids.contains(row.at("project").get<std::string>()) || result.at("sources").at(row.at("client").get<std::string>()).at("state") == "unavailable") retained.push_back(row);
        for (auto& source : result.at("sources").items()) if (source.value().at("state") == "unavailable")
            source.value()["last_success_unix_ms"] = old.at("sources").at(source.key()).at("last_success_unix_ms");
        result["buckets"] = std::move(retained);
    }
    require(!stop.stop_requested(), "usage-cancelled");
    require(read_text(home/"registry/state.json",kMaximumStateBytes,true) == registry_stamp, "usage-registry-changed");
    (void)summarize_model_usage(result,now_ms());
    Json saved{{"schema_version","shin.registered-usage.v1"},{"config",config},{"snapshot",result}};
    // Keep the strict token snapshot readable by older kits during rollback.
    if (saved["snapshot"].contains("context_events")) {
        saved["context_events"] = std::move(saved["snapshot"]["context_events"]);
        saved["snapshot"].erase("context_events");
    }
    while (saved.dump().size() > kMaximumStateBytes && saved.contains("context_events") &&
           !saved["context_events"]["events"].empty()) {
        auto& events = saved["context_events"];
        events["events"].erase(events["events"].size()-1);
        events["omitted"] = events.value("omitted",std::uint64_t{0})+1;
        events["complete"] = false;
    }
    if (saved.dump().size() > kMaximumStateBytes) saved.erase("context_events");
    store.save(saved);
    return {{"ok",true},{"collected",true},{"projects",result.at("projects").size()},{"sources",result.at("sources")}};
}

void watch_registered_model_usage(const fs::path& home, const Json& sources, std::stop_token stop) {
    using Clock = std::chrono::steady_clock;
    auto due = Clock::time_point{}; std::string previous_registry;
    while (!stop.stop_requested()) {
        try {
            const auto registry = read_text(home/"registry/state.json",kMaximumStateBytes,true);
            if (registry != previous_registry || Clock::now() >= due) {
                previous_registry = registry; due = Clock::now()+std::chrono::minutes(5);
                (void)collect_registered_model_usage(home,sources,nullptr,stop);
                if (!stop.stop_requested()) {
                    ensure_private_directory(home/"usage-health"); StateStore health(home/"usage-health",true);
                    health.recover_pending(); health.save({{"error",""},{"at_ms",now_ms()}});
                }
            }
        } catch (const std::exception& error) {
            if (!stop.stop_requested()) try {
                const auto* known = dynamic_cast<const Error*>(&error);
                ensure_private_directory(home/"usage-health"); StateStore health(home/"usage-health",true);
                health.recover_pending(); health.save({{"error",known ? known->code : "usage-collection-unavailable"},{"at_ms",now_ms()}});
            } catch (...) { return; } // Stop collection if even its health destination cannot be safely written.
            // Keep the previous complete snapshot; readers expose its age. A bad
            // registry or history source never blocks retrieval or rewrites state.
            due = Clock::now()+std::chrono::minutes(5);
        }
        for (unsigned i=0;i<50 && !stop.stop_requested();++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
} // namespace shin::observer
