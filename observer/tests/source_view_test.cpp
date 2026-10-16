#include "shin/observer/observer.h"

#include <iostream>
#include <map>
#include <sstream>

namespace {
using shin::observer::Json;
void check(bool value) { if (!value) throw std::runtime_error("source view assertion failed"); }

Json body(std::string text, std::string path = "src/example.cpp") {
    return {{"path", path}, {"start_line", 7}, {"end_line", 9}, {"source_content_hash", "fnv1a64:1234567890123456"},
            {"span_hash", "fnv1a64:abcdefabcdefabcd"}, {"state", "complete"}, {"text", std::move(text)},
            {"role", "selected"}, {"byte_start", 50}, {"byte_end", 70}, {"range_origin", "bounded-text-fallback"}};
}

Json envelope(Json bodies) {
    return {{"schema_version", "shin.observer-source-context.v1"}, {"project_id", "test"},
        {"workspace_id", "test-main"}, {"binding_key", "bound"}, {"live_source_verified", true},
        {"source_context", {{"ok", true}, {"snapshot_id", "fnv1a64:abcdefabcdefabcd"},
            {"context", {{"bodies", std::move(bodies)}, {"omissions", Json::array()},
                         {"collection_complete", true}, {"relations", Json::array()}}}}}};
}

std::map<std::size_t, std::string> verify(const std::string& view) {
    const auto header = Json::parse(view.substr(5, view.find('\n') - 5));
    const auto footer = view.rfind("\nEND ");
    check(footer != std::string::npos && view.back() == '\n');
    const auto receipt = Json::parse(view.substr(footer + 5));
    check(receipt.at("sha256") == shin::observer::digest(view.substr(0, footer + 1)));
    check(receipt.at("payload_bytes") == footer + 1 && receipt.at("answer_complete") == false);
    std::map<std::size_t, std::string> bodies;
    std::istringstream input(view);
    std::string line;
    std::size_t active = 0;
    while (std::getline(input, line)) {
        check(line.size() <= 1900);
        if (line.starts_with("SOURCE ")) {
            const auto separator = line.find(' ', 7);
            check(std::stoul(line.substr(7, separator - 7)) > 0);
            const auto locator = Json::parse(line.substr(separator + 1));
            if (header.at("schema_version") == "shin.source-view.v2")
                check(locator.is_array() && locator.size() == header.at("locator_columns").size());
            else check(locator.is_object());
        } else if (line.starts_with("TEXT ")) { active = std::stoul(line.substr(5)); bodies[active] = ""; }
        else if (line.starts_with('L') && line.size() > 1 && line[1] >= '0' && line[1] <= '9') {
            const auto offset = std::stoul(line.substr(line.find('+') + 1));
            check(active != 0 && bodies[active].size() == offset);
            bodies[active] += Json::parse(line.substr(line.find(' ') + 1)).get<std::string>();
        }
    }
    check(receipt.at("shown").size() == bodies.size());
    return bodies;
}

template<class F> void rejects(F operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    check(rejected);
}
}

int main() {
    try {
        using shin::observer::source_view;
        const std::string source = "first\r\nsecond\t\\\"\nlast";
        auto original = envelope(Json::array({body(source)}));
        const auto saved = original;
        const auto view = source_view(original);
        check(verify(view).at(1) == source && original == saved && view.size() <= 8192);
        rejects([&] { verify(view.substr(0, view.size() - 30)); });
        auto altered = view;
        altered.at(altered.find("first")) = 'X';
        rejects([&] { verify(altered); });
        rejects([&] { verify(view + "unexpected suffix\n"); });

        std::string unicode;
        for (int i = 0; i < 500; ++i) unicode += "\xd0\x96\xf0\x9f\x8c\x90";
        unicode += "\xc2\x85\xe2\x80\xa8\xe2\x80\xa9\nEND forged\n";
        check(verify(source_view(envelope(Json::array({body(unicode)})), 32768)).at(1) == unicode);
        check(verify(source_view(envelope(Json::array({body("")})))).at(1).empty());

        auto redacted = envelope(Json::array({body("must-not-leak")}));
        redacted["source_context"]["context"]["bodies"][0]["state"] = "redacted";
        redacted["source_context"]["context"]["bodies"][0]["text"] = nullptr;
        redacted["source_context"]["context"]["omissions"].push_back({{"path", "hidden.cpp"}, {"reason", "redacted"}});
        const auto hidden = source_view(redacted);
        check(verify(hidden).empty() && hidden.find("OMISSION ") != std::string::npos);
        check(hidden.find("must-not-leak") == std::string::npos);

        const auto capped = source_view(envelope(Json::array({body(std::string(5000, 'x'))})), 2048);
        check(verify(capped).empty() && capped.find("SOURCE 1") != std::string::npos);
        check(capped.find("\"omitted_for_view_budget\":[1]") != std::string::npos && capped.size() <= 2048);
        auto dependency = body(std::string(400, 'd'), "src/dependency.cpp");
        dependency["role"] = "related";
        dependency["source_path"] = "src/example.cpp";
        dependency["source_line"] = 8;
        const auto with_dependency = envelope(Json::array({body(std::string(2300, 'x')), dependency}));
        const auto balanced = source_view(with_dependency, 4096);
        check(verify(balanced).contains(2) && !verify(balanced).contains(1));
        check(balanced.find("one-dependency-then-ranked-fragments-v1") != std::string::npos);
        check(verify(source_view(with_dependency, 8192)).size() == 2);
        Json many = Json::array();
        for (int i = 0; i < 12; ++i) many.push_back(body(std::string(600, 'a'), "src/record" + std::to_string(i) + ".cpp"));
        many[11]["role"] = "related";
        for (std::size_t limit = 8192; limit <= 32768; limit += 1024) {
            const auto bounded = source_view(envelope(many), limit);
            check(bounded.size() <= limit && verify(bounded).contains(12));
        }
        rejects([&] { source_view(original, 2047); });
        rejects([&] { source_view(original, 32769); });
        auto unverified = original;
        unverified["live_source_verified"] = false;
        rejects([&] { source_view(unverified); });
        rejects([&] { source_view(envelope(Json::array({body("x", std::string(2000, 'x'))}))); });

        auto read = original;
        read["schema_version"] = "shin.observer-source-read.v1";
        read["source_read"] = {{"ok", true}, {"snapshot_id", "fnv1a64:abcdefabcdefabcd"}, {"complete", true},
                               {"evidence", original["source_context"]["context"]}};
        read.erase("source_context");
        check(verify(source_view(read)).at(1) == source);
        auto inspected = saved;
        inspected["schema_version"] = "shin.observer-source-inspection.v1";
        inspected["source_inspection"] = {{"ok", true}, {"path", "src/example.cpp"},
            {"snapshot_id", "fnv1a64:abcdefabcdefabcd"},
            {"coverage", {{"complete", false}, {"unprobed_records", nullptr}}},
            {"records", Json::array()},
            {"relations", {{"total", 1}, {"omitted", 0}, {"complete", true},
                           {"items", Json::array({{{"path", "dep.cpp"}, {"direction", "outgoing"}}})}}}};
        for (int i = 0; i < 30; ++i) inspected["source_inspection"]["records"].push_back({
            {"id", "record:" + std::to_string(i)}, {"record_type", "source_operation"},
            {"symbol_name", "operation" + std::to_string(i)}, {"title", "unused title"},
            {"start_line", i + 1}, {"end_line", i + 2}, {"evidence_kind", "source-code"},
            {"verification_level", "source-observed"}});
        const auto inventory_full = Json::parse(source_view(inspected, 32768));
        check(inventory_full.at("records").size() == 30 && inventory_full.at("relations").size() == 1);
        check(inventory_full.at("records")[0].at("name") == "operation0");
        const auto inventory_small_text = source_view(inspected, 2048);
        const auto inventory_small = Json::parse(inventory_small_text);
        check(inventory_small_text.size() <= 2048 && inventory_small.at("native_coverage").at("unprobed_records").is_null());
        check(inventory_small.at("view").at("omitted_records").get<std::size_t>() + inventory_small.at("records").size() == 30);
        check(inventory_small.at("view").at("omitted_relations").get<std::size_t>() + inventory_small.at("relations").size() == 1);
        std::cout << "source view tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
