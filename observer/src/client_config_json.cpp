#include "client_config.h"
#include <map>
#include <set>
#include <vector>

namespace shin::observer::client_config {
namespace {
struct Token { std::size_t begin, end; char kind; };
struct Span { std::size_t begin, end; bool empty = false; };
struct Document {
    std::string clean;
    std::vector<Token> tokens;
    std::map<std::vector<std::string>, Span> spans;
    Json value;
    explicit Document(const std::string& source) : clean(source) {
        // Blanking comments/trailing commas retains every byte offset for edits.
        if (source.starts_with("\xef\xbb\xbf")) clean.replace(0, 3, "   ");
        for (std::size_t i = source.starts_with("\xef\xbb\xbf") ? 3 : 0; i < source.size();) {
            const auto begin = i;
            if (source[i] == '"') {
                ++i;
                while (i < source.size()) {
                    if (source[i] == '\\') { i = std::min(i + 2, source.size()); continue; }
                    if (source[i++] == '"') break;
                }
                tokens.push_back({begin, i, 's'});
            } else if (i + 1 < source.size() && source[i] == '/' && (source[i + 1] == '/' || source[i + 1] == '*')) {
                const bool line = source[i + 1] == '/'; i += 2;
                if (line) { while (i < source.size() && source[i] != '\n') ++i; }
                else {
                    const auto end = source.find("*/", i);
                    if (end == std::string::npos) throw Error("client-config-invalid", "An unterminated comment was found. Settings were preserved.");
                    i = end + 2;
                }
                for (auto n = begin; n < i; ++n) if (clean[n] != '\n' && clean[n] != '\r') clean[n] = ' ';
            } else if (std::string_view(" \t\r\n").find(source[i]) != std::string_view::npos) ++i;
            else if (std::string_view("{}[]:,").find(source[i]) != std::string_view::npos) {
                tokens.push_back({i, i + 1, source[i]}); ++i;
            } else {
                while (i < source.size() && std::string_view(" \t\r\n{}[]:,/").find(source[i]) == std::string_view::npos) ++i;
                if (i == begin) throw Error("client-config-invalid", "Invalid JSON settings. The file was preserved.");
                tokens.push_back({begin, i, 'v'});
            }
        }
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
            if (tokens[i].kind == ',' && (tokens[i + 1].kind == '}' || tokens[i + 1].kind == ']')) clean[tokens[i].begin] = ' ';
        int depth = 0;
        for (const auto& token : tokens) {
            if (token.kind == '{' || token.kind == '[') ++depth;
            if (depth > 64) throw Error("client-config-invalid", "JSON settings are nested too deeply.");
            if (token.kind == '}' || token.kind == ']') --depth;
        }
        value = Json::parse(clean);
        std::size_t cursor = 0;
        scan(cursor, {}, 0);
        if (!value.is_object()) throw Error("client-config-invalid", "Client settings must be an object.");
    }
    void scan(std::size_t& cursor, const std::vector<std::string>& path, unsigned depth) {
        if (depth > 64) throw Error("client-config-invalid", "Client settings are nested too deeply.");
        const auto first = tokens.at(cursor++);
        bool empty = false;
        if (first.kind == '{') {
            empty = tokens.at(cursor).kind == '}';
            std::set<std::string> keys;
            while (tokens.at(cursor).kind != '}') {
                const auto key = tokens.at(cursor++);
                const auto name = Json::parse(clean.substr(key.begin, key.end - key.begin)).get<std::string>();
                if (!keys.insert(name).second) throw Error("client-config-invalid", "Duplicate JSON keys require review. Settings were preserved.");
                ++cursor; // colon; the library has already validated the syntax
                auto child = path; child.push_back(name); scan(cursor, child, depth + 1);
                if (tokens.at(cursor).kind == ',') ++cursor;
            }
            ++cursor;
        } else if (first.kind == '[') {
            std::size_t index = 0;
            while (tokens.at(cursor).kind != ']') {
                auto child = path; child.push_back(std::to_string(index++)); scan(cursor, child, depth + 1);
                if (tokens.at(cursor).kind == ',') ++cursor;
            }
            ++cursor;
        }
        spans[path] = {first.begin, tokens.at(cursor - 1).end, empty};
    }
    std::string add(const std::string& source, const std::vector<std::string>& path,
                    const std::string& key, const Json& content) const {
        const auto span = spans.at(path);
        std::string inserted = "\n" + Json(key).dump() + ": " + content.dump(2) + (span.empty ? "\n" : ",\n");
        auto result = source; result.insert(span.begin + 1, inserted); return result;
    }
};
}
std::string merge_json(const std::string& source, const Json& desired, bool opencode) {
    const auto input = source.empty() ? std::string("{}\n") : source;
    Document document(input);
    const std::string container = opencode ? "mcp" : "mcpServers";
    auto expected = document.value;
    if (!expected.contains(container)) expected[container] = Json::object();
    if (!expected[container].is_object()) throw Error("client-config-invalid", "The MCP settings must be an object.");
    const auto before = expected[container].value("shin", Json());
    const auto after = merge_server(before, desired, opencode);
    if (before == after) return source;
    expected[container]["shin"] = after;
    std::string result;
    if (!document.value.contains(container)) result = document.add(input, {}, container, expected[container]);
    else if (!document.value.at(container).contains("shin")) result = document.add(input, {container}, "shin", after);
    else {
        result = input;
        // Replace only changed leaf values. All other settings and comments stay.
        std::vector<std::pair<Span, std::string>> edits;
        for (const auto* key : {opencode ? "command" : "args", "enabled"}) {
            if (!before.contains(key) || before.at(key) == after.at(key)) continue;
            edits.emplace_back(document.spans.at({container, "shin", key}), after.at(key).dump());
        }
        std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.first.begin > b.first.begin; });
        for (const auto& [span, text] : edits) result.replace(span.begin, span.end - span.begin, text);
    }
    if (Document(result).value != expected) throw Error("client-config-invalid", "Settings validation failed; the original file was preserved.");
    return result;
}
}
