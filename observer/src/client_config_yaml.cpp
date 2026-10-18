#include "client_config.h"
#include <yaml.h>
#include <map>
#include <set>
#include <vector>

namespace shin::observer::client_config {
namespace {
struct Span { std::size_t begin, end, end_column; bool flow, empty; };
class Document {
    yaml_parser_t parser{};
    yaml_event_t event{};
    bool live = false;
    std::vector<std::size_t> offsets;
    void next() {
        if (live) yaml_event_delete(&event);
        live = false;
        if (!yaml_parser_parse(&parser, &event)) throw Error("client-config-invalid", "Invalid YAML settings. The file was preserved.");
        live = true;
    }
    Json node(const std::vector<std::string>& path, unsigned depth) {
        if (depth > 64) throw Error("client-config-invalid", "YAML settings are nested too deeply.");
        const auto type = event.type;
        const auto begin = event.start_mark.index;
        Json value;
        bool flow = true;
        if (type == YAML_SCALAR_EVENT) {
            if (event.data.scalar.anchor) throw Error("client-config-unsupported", "YAML anchors require manual review. Settings were preserved.");
            const std::string text(reinterpret_cast<char*>(event.data.scalar.value), event.data.scalar.length);
            value = text;
            const std::string tag = event.data.scalar.tag ? reinterpret_cast<char*>(event.data.scalar.tag) : "";
            if (!tag.empty() && tag != YAML_STR_TAG && tag != YAML_BOOL_TAG && tag != YAML_INT_TAG && tag != YAML_FLOAT_TAG && tag != YAML_NULL_TAG)
                throw Error("client-config-unsupported", "Custom YAML tags require manual review. Settings were preserved.");
            if ((event.data.scalar.style == YAML_PLAIN_SCALAR_STYLE || (!tag.empty() && tag != YAML_STR_TAG)) && tag != YAML_STR_TAG) {
                if (text == "true" || text == "True" || text == "TRUE" || text == "yes" || text == "Yes" || text == "YES" || text == "on" || text == "On" || text == "ON") value = true;
                else if (text == "false" || text == "False" || text == "FALSE" || text == "no" || text == "No" || text == "NO" || text == "off" || text == "Off" || text == "OFF") value = false;
                else if (text.empty() || text == "null" || text == "Null" || text == "NULL" || text == "~") value = nullptr;
                else if (!text.empty() && std::string_view("0123456789+-. ").find(text.front()) != std::string_view::npos) {
                    const auto numeric = Json::parse(text, nullptr, false);
                    if (numeric.is_number()) value = numeric;
                    else if (text.starts_with("0x") || text.starts_with("0o") || text == ".inf" || text == ".nan" || (text.front() >= '0' && text.front() <= '9')) value = Json{{"yaml-number", text}};
                }
            }
        } else if (type == YAML_MAPPING_START_EVENT) {
            if (event.data.mapping_start.anchor) throw Error("client-config-unsupported", "YAML anchors require manual review. Settings were preserved.");
            if (event.data.mapping_start.tag && std::string(reinterpret_cast<char*>(event.data.mapping_start.tag)) != YAML_MAP_TAG)
                throw Error("client-config-unsupported", "Custom YAML tags require manual review. Settings were preserved.");
            flow = event.data.mapping_start.style == YAML_FLOW_MAPPING_STYLE;
            value = Json::object(); next();
            while (event.type != YAML_MAPPING_END_EVENT) {
                if (event.type != YAML_SCALAR_EVENT) throw Error("client-config-unsupported", "YAML settings need scalar keys.");
                const std::string key(reinterpret_cast<char*>(event.data.scalar.value), event.data.scalar.length);
                if (key == "<<" || value.contains(key)) throw Error("client-config-unsupported", "Duplicate or merged YAML keys require manual review.");
                next(); auto child = path; child.push_back(key); value[key] = node(child, depth + 1);
            }
        } else if (type == YAML_SEQUENCE_START_EVENT) {
            if (event.data.sequence_start.anchor) throw Error("client-config-unsupported", "YAML anchors require manual review. Settings were preserved.");
            if (event.data.sequence_start.tag && std::string(reinterpret_cast<char*>(event.data.sequence_start.tag)) != YAML_SEQ_TAG)
                throw Error("client-config-unsupported", "Custom YAML tags require manual review. Settings were preserved.");
            flow = event.data.sequence_start.style == YAML_FLOW_SEQUENCE_STYLE;
            value = Json::array(); next();
            while (event.type != YAML_SEQUENCE_END_EVENT) {
                auto child = path; child.push_back(std::to_string(value.size())); value.push_back(node(child, depth + 1));
            }
        } else throw Error("client-config-unsupported", "YAML aliases or complex nodes require manual review. Settings were preserved.");
        spans[path] = {offsets.at(begin), offsets.at(event.end_mark.index), event.end_mark.column, flow, value.empty()};
        next(); return value;
    }
public:
    Json value;
    std::map<std::vector<std::string>, Span> spans;
    explicit Document(const std::string& source) {
        for (std::size_t i = 0; i < source.size(); ++i)
            if ((static_cast<unsigned char>(source[i]) & 0xc0) != 0x80) offsets.push_back(i);
        offsets.push_back(source.size());
        if (!yaml_parser_initialize(&parser)) throw Error("client-config-invalid", "Cannot initialize the YAML parser.");
        yaml_parser_set_input_string(&parser, reinterpret_cast<const unsigned char*>(source.data()), source.size());
        try {
            next(); if (event.type != YAML_STREAM_START_EVENT) throw Error("client-config-invalid", "Invalid YAML stream.");
            next();
            if (event.type == YAML_STREAM_END_EVENT) { value = Json::object(); return; }
            if (event.type != YAML_DOCUMENT_START_EVENT) throw Error("client-config-invalid", "Invalid YAML document.");
            next(); value = node({}, 0);
            if (event.type != YAML_DOCUMENT_END_EVENT) throw Error("client-config-invalid", "Invalid YAML document end.");
            next(); if (event.type != YAML_STREAM_END_EVENT) throw Error("client-config-unsupported", "Multiple YAML documents require manual review.");
        } catch (...) { if (live) yaml_event_delete(&event); yaml_parser_delete(&parser); throw; }
    }
    ~Document() { if (live) yaml_event_delete(&event); yaml_parser_delete(&parser); }
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    std::string add(std::string source, const std::vector<std::string>& path, const std::string& key, const Json& content) const {
        const auto span = spans.at(path);
        if (span.flow) source.insert(span.begin + 1, Json(key).dump() + ": " + content.dump() + (span.empty ? "" : ", "));
        else {
            const auto start = source.rfind('\n', span.begin);
            const auto indent = span.begin - (start == std::string::npos ? 0 : start + 1);
            source.insert(span.begin, key + ": " + content.dump() + "\n" + std::string(indent, ' '));
        }
        return source;
    }
};
}
std::string merge_yaml(const std::string& source, const Json& desired) {
    const auto input = source.empty() ? std::string("{}\n") : source;
    Document document(input);
    if (!document.value.is_object()) throw Error("client-config-invalid", "Hermes settings must be a YAML mapping.");
    auto expected = document.value;
    if (!expected.contains("mcp_servers")) expected["mcp_servers"] = Json::object();
    if (!expected["mcp_servers"].is_object()) throw Error("client-config-invalid", "mcp_servers must be a YAML mapping.");
    const auto before = expected["mcp_servers"].value("shin", Json());
    const auto after = merge_server(before, desired, false);
    if (before == after) return source;
    expected["mcp_servers"]["shin"] = after;
    std::string result;
    if (!document.value.contains("mcp_servers")) result = document.add(input, {}, "mcp_servers", expected["mcp_servers"]);
    else if (!document.value.at("mcp_servers").contains("shin")) result = document.add(input, {"mcp_servers"}, "shin", after);
    else {
        result = input;
        std::vector<std::pair<Span, std::string>> edits;
        for (const auto* key : {"args", "enabled"}) {
            if (!before.contains(key) || before.at(key) == after.at(key)) continue;
            const auto span = document.spans.at({"mcp_servers", "shin", key});
            edits.emplace_back(span, after.at(key).dump() + (span.flow ? "" : "\n" + std::string(span.end_column, ' ')));
        }
        std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.first.begin > b.first.begin; });
        for (const auto& [span, text] : edits) result.replace(span.begin, span.end - span.begin, text);
    }
    if (Document(result).value != expected) throw Error("client-config-invalid", "YAML validation failed; the original file was preserved.");
    return result;
}
}
