#include "shin/observer/local_client_setup.h"
#include "shin/observer/local_package.h"

namespace shin::observer {
Json local_client_setup(const fs::path& executable, const fs::path& root,
                        const fs::path& home, const std::string& client) {
    if (client != "generic" && client != "codex" && client != "opencode" && client != "rider")
        throw Error("invalid-arguments", "Choose --client codex, opencode, rider or generic.");
    if (!fs::is_directory(root))
        throw Error("project-root-unavailable", "Choose an existing project folder.");
    const auto project = fs::canonical(root).string();
    const auto data_home = fs::absolute(home).lexically_normal().string();
    const auto adapter = package_binary(executable,
#ifdef _WIN32
        "shin-mcp.exe");
#else
        "shin-mcp");
#endif
    if (!fs::is_regular_file(adapter))
        throw Error("mcp-unavailable", "shin-mcp is missing beside shin-ce. Use a complete native kit.");
    const Json args = Json::array({"--root", project, "--home", data_home});
    const Json server{{"command", adapter.string()}, {"args", args}};
    std::string configuration;
    if (client == "codex") {
        // JSON string escapes are valid TOML basic-string escapes for paths.
        configuration = "[mcp_servers.shin]\ncommand = " + Json(adapter.string()).dump() +
                        "\nargs = " + args.dump() + "\n";
    } else if (client == "opencode") {
        Json command = Json::array({adapter.string()});
        for (const auto& arg : args) command.push_back(arg);
        configuration = Json{{"mcp", {{"shin", {{"type", "local"}, {"command", command}, {"enabled", true}}}}}}.dump(2) + "\n";
    } else configuration = Json{{"mcpServers", {{"shin", server}}}}.dump(2) + "\n";
    return {{"schema_version", "shin.client-setup.v1"}, {"ok", true}, {"client", client},
        {"root", project}, {"home", data_home}, {"server", server}, {"configuration", configuration},
        {"client_modified", false}, {"readiness_assessed", false}};
}
}
