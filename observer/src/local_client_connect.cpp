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
#ifdef _WIN32
    (void)executable; (void)root; (void)home; (void)client; (void)config; (void)revision;
    throw Error("client-config-platform", "Automatic client setup is currently available on macOS and Linux.");
#else
    if (client != "codex" && client != "opencode" && client != "command-code" && client != "hermes")
        throw Error("invalid-arguments", "Choose codex, opencode, command-code or hermes.");
    if (!config.is_absolute() || config.filename().empty() || config.lexically_normal() != config)
        throw Error("client-config-path", "Choose an absolute, normalized configuration file path.");
    const auto setup = local_client_setup(executable, root, home, "generic");
    const bool exists = present(config);
    const auto original = exists ? read_text(config, kMaximumStateBytes, false) : std::string();
    const auto content = merged(original, setup.at("server"), client);
    if (content.size() > kMaximumStateBytes) throw Error("client-config-limit", "Updated settings exceed the supported file size; no settings were changed.");
    const auto hash = digest(Json::array({client, config.string(), setup.at("server"), exists, original, content}).dump());
    Json result{{"ok", true}, {"schema_version", "shin.client-connect.v1"}, {"client", client},
        {"config", config.string()}, {"root", setup.at("root")}, {"revision", hash},
        {"changed", content != original}, {"client_modified", false}, {"restart_required", true},
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
    if (present(config) != exists || (exists && read_text(config, kMaximumStateBytes, false) != original))
        throw Error("client-config-changed", "Settings changed during connection. Retry without discarding the other changes.");
    if (exists) {
        const auto backup = lock_dir / (digest(original) + ".backup");
        write_private_archive(backup, original);
        replace_user_config(config, original, content, identity);
        result["backup"] = backup.string();
    } else write_user_archive(config, content);
    result["client_modified"] = true;
    result["status"] = "configured";
    return result;
#endif
}
}
