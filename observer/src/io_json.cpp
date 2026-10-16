#include "shin/observer/io.h"
#include <unordered_set>
#include <vector>

namespace shin::observer {
Json parse_json(const std::string& text) {
    if (text.size() > kMaximumStateBytes) throw Error("json-too-large", "JSON input exceeds the bounded limit.");
    const auto invalid = [] { throw Error("invalid-json", "JSON input is malformed or exceeds the nesting limit."); };
    std::vector<std::unordered_set<std::string>> object_keys;
    const Json::parser_callback_t callback = [&](int depth, Json::parse_event_t event, Json& value) {
        if ((event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) && depth >= 32) invalid();
        if (event == Json::parse_event_t::object_start) object_keys.emplace_back();
        else if (event == Json::parse_event_t::key) {
            if (object_keys.empty() || !object_keys.back().insert(value.get<std::string>()).second) invalid();
        } else if (event == Json::parse_event_t::object_end) {
            if (object_keys.empty()) invalid();
            object_keys.pop_back();
        }
        return true;
    };
    try { return Json::parse(text, callback, true, false); }
    catch (const Error&) { throw; }
    catch (const std::exception&) { throw Error("invalid-json", "JSON input is malformed or exceeds the nesting limit."); }
}
}
