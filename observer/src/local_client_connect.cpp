#include "shin/observer/local_client_setup.h"
#include "shin/observer/observer.h"
#include "client_config.h"
#include <set>

namespace shin::observer {
namespace {
fs::path location(const char* variable, const fs::path& fallback, char** environment) {
    const std::string key = std::string(variable) + "=";
    const char* value = nullptr;
    for (auto** entry = environment; entry && *entry; ++entry)
        if (std::string_view(*entry).starts_with(key)) { value = *entry + key.size(); break; }
    if (!value || !*value) return fallback;
    fs::path path(value);
    if (!path.is_absolute()) throw Error("client-config-path", "A client configuration environment override must be an absolute path.");
    return path.lexically_normal();
}
bool present(const fs::path& path) { return fs::symlink_status(path).type() != fs::file_type::not_found; }
Json arguments(const Json& server, const std::string& client) {
    if (!server.is_object()) return Json::array();
    if (client != "opencode") return server.value("args", Json::array());
    auto result = server.value("command", Json::array());
    if (!result.is_array() || result.empty()) return Json::array();
    result.erase(result.begin()); return result;
}
void create_parents(const fs::path& path) {
    if (present(path)) { directory_identity(path); return; }
    create_parents(path.parent_path()); ensure_private_directory(path);
}
std::string merged(const std::string& source, const Json& server, const std::string& client) {
    try {
        if (client == "codex") return client_config::merge_toml(source, server);
        if (client == "hermes") return client_config::merge_yaml(source, server);
        return client_config::merge_json(source, server, client == "opencode");
    } catch (const Error&) { throw; }
    catch (...) { throw Error("client-config-invalid", "Client settings could not be parsed or safely updated. The original file was preserved."); }
}
}
std::vector<fs::path> local_client_live_roots(const fs::path& executable, const fs::path& home,
                                            const std::string& client, const fs::path& config) {
    const auto refuse = [] { throw Error("client-access-unavailable", "The selected agent's MCP access changed or cannot be verified. Review its connection settings."); };
    directory_identity(config.parent_path());
    const auto server = client_config::read_server(read_user_config(config, kMaximumStateBytes), client);
    if (!server.is_object()) refuse();
    std::string command;
    if (client == "opencode") {
        if (server.value("type", "") != "local" || !server.contains("command") || !server.at("command").is_array() ||
            server.at("command").empty() || !server.at("command").front().is_string()) refuse();
        command = server.at("command").front().get<std::string>();
    } else {
        if (!server.contains("command") || !server.at("command").is_string() || server.contains("url")) refuse();
        command = server.at("command").get<std::string>();
        for (const auto* key : {"type", "transport"}) if (server.contains(key) && server.at(key) != "stdio") refuse();
    }
    std::error_code error;
    if (!fs::path(command).is_absolute() || fs::weakly_canonical(command, error) != executable || error) refuse();
    const auto args = arguments(server, client);
    if (!args.is_array() || args.size() > 38) refuse();
    std::set<std::string> seen;
    std::set<fs::path> unique;
    std::vector<fs::path> roots;
    for (std::size_t i = 0; i < args.size(); i += 2) {
        if (i + 1 >= args.size() || !args[i].is_string() || !args[i+1].is_string()) refuse();
        const auto flag = args[i].get<std::string>(), value = args[i+1].get<std::string>();
        if (flag == "--root") {
            const fs::path path(value);
            if (!path.is_absolute() || path.lexically_normal() != path || !unique.insert(path).second) refuse();
            roots.push_back(path);
        } else {
            if (!seen.insert(flag).second) refuse();
            if (flag == "--home") { if (fs::path(value).lexically_normal() != home) refuse(); }
            else if (flag == "--client-config") { if (fs::path(value) != config) refuse(); }
            else if (flag == "--client-format") { if (value != client) refuse(); }
            else refuse();
        }
    }
    if (roots.empty() || roots.size() > 16 || !seen.contains("--home") ||
        !seen.contains("--client-config") || !seen.contains("--client-format")) refuse();
    if (server.contains("enabled")) {
        if (!server.at("enabled").is_boolean()) refuse();
        if (!server.at("enabled").get<bool>()) return {};
    }
    return roots;
}
Json local_client_connections(const fs::path& executable, const fs::path& root,
                              const fs::path& home, Json targets) {
    for (auto& target : targets.at("targets")) {
        target["access_state"] = "not-configured";
        if (target.value("config", "").empty()) continue;
        try {
            const auto plan = local_client_connect(executable, root, home, target.at("client"),
                                                   fs::path(target.at("config").get<std::string>()), "");
            for (const auto* key : {"access_state", "access_configured", "live_catalog", "restart_required"}) target[key] = plan.at(key);
        } catch (const Error& error) {
            target["access_state"] = "review-required"; target["access_error"] = error.code;
        } catch (...) { target["access_state"] = "review-required"; target["access_error"] = "client-config-invalid"; }
    }
    return targets;
}
Json local_client_targets(const fs::path& user_home, char** environment) {
    Json targets = Json::array();
    std::set<fs::path> seen;
    auto add = [&](const std::string& client, const std::string& label, const fs::path& path) {
        if (!seen.insert(path).second) return;
        targets.push_back({{"client", client}, {"label", label}, {"config", path.string()},
            {"detected", present(path.parent_path())}, {"config_exists", present(path)}});
    };
    auto unavailable = [&](const std::string& client, const std::string& label) {
        targets.push_back({{"client", client}, {"label", label}, {"config", ""}, {"detected", true},
            {"config_exists", false}, {"choose_config_required", true}});
    };
    try { add("codex", "Codex", location("CODEX_HOME", user_home / ".codex", environment) / "config.toml"); }
    catch (const std::exception&) { unavailable("codex", "Codex"); }
    try {
    const auto opencode = location("XDG_CONFIG_HOME", user_home / ".config", environment) / "opencode";
    const auto opencode_file = present(opencode / "opencode.jsonc") ? opencode / "opencode.jsonc" : opencode / "opencode.json";
    add("opencode", "OpenCode", location("OPENCODE_CONFIG", opencode_file, environment));
    } catch (const std::exception&) { unavailable("opencode", "OpenCode"); }
    try { add("command-code", "Command Code", user_home / ".commandcode/mcp.json"); }
    catch (const std::exception&) { unavailable("command-code", "Command Code"); }
    const auto hermes = user_home / ".hermes";
    try { add("hermes", "Hermes", location("HERMES_HOME", hermes, environment) / "config.yaml"); }
    catch (const std::exception&) { unavailable("hermes", "Hermes"); }
    const auto profiles = hermes / "profiles";
    if (fs::is_directory(profiles) && !fs::is_symlink(profiles)) {
        std::vector<fs::path> configs;
        for (const auto& entry : fs::directory_iterator(profiles)) {
            if (configs.size() >= 64) throw Error("client-config-limit", "Too many Hermes profiles; select a configuration file explicitly.");
            if (!entry.is_symlink() && entry.is_directory() && fs::is_regular_file(entry.path() / "config.yaml"))
                configs.push_back(entry.path() / "config.yaml");
        }
        std::sort(configs.begin(), configs.end());
        for (const auto& path : configs) add("hermes", "Hermes — " + path.parent_path().filename().string(), path);
    }
    return {{"ok", true}, {"schema_version", "shin.client-targets.v1"}, {"targets", targets}, {"client_modified", false}};
}
Json local_client_connect(const fs::path& executable, const fs::path& root,
                          const fs::path& home, const std::string& client,
                          const fs::path& config, const std::string& revision) {
    if (client != "codex" && client != "opencode" && client != "command-code" && client != "hermes")
        throw Error("invalid-arguments", "Choose codex, opencode, command-code or hermes.");
    if (!config.is_absolute() || config.filename().empty() || config.lexically_normal() != config)
        throw Error("client-config-path", "Choose an absolute, normalized configuration file path.");
    auto setup = local_client_setup(executable, root, home, "generic");
    for (const auto& arg : Json::array({"--client-config", config.string(), "--client-format", client})) setup["server"]["args"].push_back(arg);
    const bool exists = present(config);
    const auto original = exists ? read_user_config(config, kMaximumStateBytes) : std::string();
    const auto content = merged(original, setup.at("server"), client);
    const auto before = client_config::read_server(original, client);
    const auto before_args = arguments(before, client);
    bool root_present = false, live_catalog = false;
    for (std::size_t i = 0; i + 1 < before_args.size(); i += 2) {
        if (before_args[i] == "--root" && before_args[i+1] == setup.at("root")) root_present = true;
        if (before_args[i] == "--client-config" && before_args[i+1] == config.string()) live_catalog = true;
    }
    const bool configured = root_present && before.value("enabled", true);
    if (content.size() > kMaximumStateBytes) throw Error("client-config-limit", "Updated settings exceed the supported file size; no settings were changed.");
    const auto hash = digest(Json::array({client, config.string(), setup.at("server"), exists, original, content}).dump());
    Json result{{"ok", true}, {"schema_version", "shin.client-connect.v1"}, {"client", client},
        {"config", config.string()}, {"root", setup.at("root")}, {"revision", hash},
        {"changed", content != original}, {"client_modified", false}, {"restart_required", !live_catalog},
        {"live_catalog", live_catalog}, {"access_configured", configured},
        {"access_state", configured ? (live_catalog ? "configured" : "reconnect-required") : "not-configured"},
        {"status", content == original ? "already-configured" : "ready-to-connect"}};
    if (revision.empty()) return result;
    if (revision != hash) throw Error("client-config-changed", "Settings changed after review. Refresh the agent list and try again; no settings were overwritten.");
    if (content == original) return result;
    create_parents(config.parent_path());
    const auto identity = directory_identity(config.parent_path());
    const auto backup_dir = config.parent_path() / ".shin-backups";
    if (!present(backup_dir)) ensure_private_directory(backup_dir);
    const auto lock_dir = backup_dir / digest(config.filename().string()).substr(0, 24);
    if (!present(lock_dir)) ensure_private_directory(lock_dir);
    StateStore lock(lock_dir, true);
    if (present(config) != exists || (exists && read_user_config(config, kMaximumStateBytes) != original))
        throw Error("client-config-changed", "Settings changed during connection. Retry without discarding the other changes.");
    if (exists) {
        const auto backup = lock_dir / (digest(original) + ".backup");
        write_private_archive(backup, original);
        replace_user_config(config, original, content, identity);
        result["backup"] = backup.string();
    } else write_user_archive(config, content);
    result["client_modified"] = true;
    result["status"] = "configured";
    result["access_configured"] = true;
    result["access_state"] = live_catalog ? "configured" : "reconnect-required";
    return result;
}
}
