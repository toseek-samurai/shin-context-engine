#include "client_config.h"
#include "shin/observer/local_installation.h"
#include <set>

namespace shin::observer::client_config {
Json read_server(const std::string& source, const std::string& client) {
    try {
        if (client == "codex") return read_toml_server(source);
        if (client == "hermes") return read_yaml_server(source);
        if (client == "opencode" || client == "command-code") return read_json_server(source, client == "opencode");
        throw Error("invalid-arguments", "Unsupported client settings format.");
    } catch (const Error&) { throw; }
    catch (...) { throw Error("client-config-invalid", "Cannot safely read the selected agent's MCP settings."); }
}
Json merge_server(const Json& before, const Json& desired, bool opencode) {
    if (before.is_null()) {
        if (!opencode) return desired;
        Json command = Json::array({desired.at("command")});
        for (const auto& arg : desired.at("args")) command.push_back(arg);
        return {{"type", "local"}, {"command", command}, {"enabled", true}};
    }
    auto conflict = [] { throw Error("client-config-conflict", "The existing shin entry uses a different executable, data home or argument format. It was preserved."); };
    if (!before.is_object()) conflict();
    Json args;
    std::string command;
    if (opencode) {
        if (before.value("type", "") != "local" || !before.contains("command") ||
            !before.at("command").is_array() || before.at("command").empty()) conflict();
        args = before.at("command");
        if (!args.front().is_string()) conflict();
        command = args.front().get<std::string>(); args.erase(args.begin());
    } else {
        if (!before.contains("command") || !before.at("command").is_string() ||
            !before.contains("args") || !before.at("args").is_array() || before.contains("url")) conflict();
        for (const auto* key : {"type", "transport"})
            if (before.contains(key) && (!before.at(key).is_string() || before.at(key) != "stdio")) conflict();
        command = before.at("command"); args = before.at("args");
    }
    std::error_code error;
    if (!fs::path(command).is_absolute() || fs::weakly_canonical(command, error) !=
        fs::weakly_canonical(desired.at("command").get<std::string>()) || error) conflict();
    bool home_seen = false, root_seen = false, config_seen = false, format_seen = false;
    std::size_t roots = 0;
    std::set<fs::path> unique_roots;
    const auto root = desired.at("args").at(1).get<std::string>();
    for (std::size_t i = 0; i < args.size(); i += 2) {
        if (i + 1 >= args.size() || !args[i].is_string() || !args[i + 1].is_string()) conflict();
        const auto flag = args[i].get<std::string>(), value = args[i + 1].get<std::string>();
        if (value.empty()) conflict();
        if (flag == "--home") {
            if (home_seen || fs::path(value).lexically_normal() != fs::path(desired.at("args").at(3).get<std::string>()).lexically_normal()) conflict();
            home_seen = true;
        } else if (flag == "--root") {
            if (!fs::path(value).is_absolute() || fs::path(value).lexically_normal() != fs::path(value) ||
                !unique_roots.insert(fs::path(value)).second) conflict();
            ++roots;
            if (value == root) root_seen = true;
        } else if (flag == "--client-config" || flag == "--client-format") {
            bool& found = flag == "--client-config" ? config_seen : format_seen;
            if (found || desired.at("args").size() != 8 ||
                value != desired.at("args").at(flag == "--client-config" ? 5 : 7).get<std::string>()) conflict();
            found = true;
        } else conflict();
    }
    // An installed adapter pins its home. Older explicit-home entries are also
    // accepted. An unpinned legacy adapter needs an operator's route review.
    if (!roots || roots > 16 || (roots == 16 && !root_seen) || config_seen != format_seen) conflict();
    if (!home_seen) {
        const auto installed = installed_local_home(fs::path(command));
        if (!installed || installed->lexically_normal() != fs::path(desired.at("args").at(3).get<std::string>()).lexically_normal()) conflict();
    }
    if (!root_seen) { args.push_back("--root"); args.push_back(root); }
    if (desired.at("args").size() == 8) {
        if (!home_seen) { args.push_back("--home"); args.push_back(desired.at("args").at(3)); }
        if (!config_seen) for (std::size_t i = 4; i < 8; ++i) args.push_back(desired.at("args").at(i));
    }
    Json after = before;
    if (opencode) { args.insert(args.begin(), command); after["command"] = args; }
    else after["args"] = args;
    if (after.contains("enabled")) {
        if (!after.at("enabled").is_boolean()) conflict();
        after["enabled"] = true;
    }
    return after;
}
}
