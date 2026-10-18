#include "client_config.h"
#include <toml++/toml.hpp>
#include <sstream>

namespace shin::observer::client_config {
namespace {
std::size_t offset(const std::string& text, toml::source_position position) {
    unsigned line = 1, column = 1;
    std::size_t i = text.starts_with("\xef\xbb\xbf") ? 3 : 0;
    for (; i < text.size(); ++i) {
        if (line == position.line && column == position.column) return i;
        if (text[i] == '\n') { ++line; column = 1; }
        else if ((static_cast<unsigned char>(text[i]) & 0xc0) != 0x80) ++column;
    }
    if (line == position.line && column == position.column) return i;
    throw Error("client-config-invalid", "Cannot locate a TOML setting safely.");
}
Json node_json(const toml::node& node) {
    std::ostringstream stream; stream << toml::json_formatter(node); return Json::parse(stream.str());
}
}
std::string merge_toml(const std::string& source, const Json& desired) {
    auto parsed = toml::parse(source);
    auto expected = parsed;
    const auto* container = parsed["mcp_servers"].as_table();
    if (parsed.contains("mcp_servers") && !container) throw Error("client-config-invalid", "mcp_servers must be a TOML table.");
    const auto* previous = container ? container->get("shin") : nullptr;
    const auto before = previous ? node_json(*previous) : Json();
    const auto after = merge_server(before, desired, false);
    if (before == after) return source;
    std::string result = source;
    if (previous) {
        const auto* server = previous->as_table();
        if (!server) throw Error("client-config-invalid", "The shin entry must be a TOML table.");
        std::vector<std::pair<toml::source_region, std::string>> edits;
        if (before.at("args") != after.at("args")) {
            edits.emplace_back(server->get("args")->source(), after.at("args").dump());
            toml::array args; for (const auto& a : after.at("args")) args.push_back(a.get<std::string>());
            expected["mcp_servers"]["shin"].as_table()->insert_or_assign("args", std::move(args));
        }
        if (before.contains("enabled") && before.at("enabled") != after.at("enabled")) {
            edits.emplace_back(server->get("enabled")->source(), "true");
            expected["mcp_servers"]["shin"].as_table()->insert_or_assign("enabled", true);
        }
        std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.first.begin > b.first.begin; });
        for (const auto& [span, value] : edits) {
            const auto begin = offset(source, span.begin), end = offset(source, span.end);
            result.replace(begin, end - begin, value);
        }
    } else {
        const auto command = Json(desired.at("command")).dump(), args = desired.at("args").dump();
        if (container && container->is_inline()) {
            const auto at = offset(source, container->source().begin) + 1;
            result.insert(at, " shin = { command = " + command + ", args = " + args + " }" + (container->empty() ? " " : ", "));
        } else result += "\n[mcp_servers.shin]\ncommand = " + command + "\nargs = " + args + "\n";
        if (!expected.contains("mcp_servers")) expected.insert("mcp_servers", toml::table{});
        toml::array arguments; for (const auto& arg : desired.at("args")) arguments.push_back(arg.get<std::string>());
        expected["mcp_servers"].as_table()->insert("shin", toml::table{{"command", desired.at("command").get<std::string>()}, {"args", std::move(arguments)}});
    }
    if (toml::parse(result) != expected) throw Error("client-config-invalid", "TOML validation failed; the original file was preserved.");
    return result;
}
}
