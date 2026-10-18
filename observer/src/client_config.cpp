#include "client_config.h"
#include "shin/observer/local_installation.h"

namespace shin::observer::client_config {
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
    bool home_seen = false, root_seen = false;
    std::size_t roots = 0;
    const auto root = desired.at("args").at(1).get<std::string>();
    for (std::size_t i = 0; i < args.size(); i += 2) {
        if (i + 1 >= args.size() || !args[i].is_string() || !args[i + 1].is_string()) conflict();
        const auto flag = args[i].get<std::string>(), value = args[i + 1].get<std::string>();
        if (value.empty()) conflict();
        if (flag == "--home") {
            if (home_seen || fs::path(value).lexically_normal() != fs::path(desired.at("args").at(3).get<std::string>()).lexically_normal()) conflict();
            home_seen = true;
        } else if (flag == "--root") {
            ++roots;
            if (value == root) root_seen = true;
        } else conflict();
    }
    // An installed adapter pins its home. Older explicit-home entries are also
    // accepted. An unpinned legacy adapter needs an operator's route review.
    if (!roots) conflict();
    if (!home_seen) {
        const auto installed = installed_local_home(fs::path(command));
        if (!installed || installed->lexically_normal() != fs::path(desired.at("args").at(3).get<std::string>()).lexically_normal()) conflict();
    }
    if (!root_seen) { args.push_back("--root"); args.push_back(root); }
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
