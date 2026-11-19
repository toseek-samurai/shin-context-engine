#include "shin/observer/observer.h"
#include "../src/source_view.h"
#if SHIN_OBSERVER_SOURCE_DISCOVERY
#include "../src/local_context_delivery.h"
#endif

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

#if SHIN_OBSERVER_SOURCE_DISCOVERY
void context_delivery() {
    using shin::observer::LocalContextDelivery;
    shin::observer::Config config;
    config.binding_key = "bound";
    config.binding = {{"project_id", "test"}, {"workspace_id", "test-main"}};
    const auto native = envelope(Json::array({body(std::string(9000, 'x'))})).at("source_context");
    const Json scope = {{"source_roots", Json::array({"src"})}};
    Json response = {{"ok", true}, {"command", "query"},
        {"phase_ms", {{"retrieval", 3.0}, {"final_verification", 2.0}}}};
    LocalContextDelivery delivery(response, config);
    delivery.query({{"snapshot_id", native.at("snapshot_id")}, {"evidence", Json::array()}}, native, scope);
    delivery.summarize(100);
    // A selected native body can be omitted by the view budget. The summary
    // describes actual delivery, while the footer remains independently usable.
    check(verify(response.at("source_context")).empty());
    const auto& summary = response.at("context_result");
    check(summary.at("source_fragments_shown") == 0 && summary.at("evidence") == "empty");
    check(summary.at("next_action") == "use-authorized-direct-sources");
    check(summary.at("performance").at("retrieval_ms") == 3 && summary.at("performance").at("verification_ms") == 2);
    check(response.at("snapshot_id") == native.at("snapshot_id") && response.at("source_scope") == scope);
    check(!response.at("answer_complete").get<bool>() && !summary.at("answer_assessed").get<bool>());

    auto visible = native;
    visible["context"]["bodies"][0]["text"] = "a visible source fragment\n";
    delivery.query({{"evidence", Json::array({{{"path", "docs/contract.md"}}})}}, visible, scope);
    response["source_coverage"] = {{"state", "partial"}};
    response["background_readiness"] = {{"qualified_resources", {{"resource_check", {{"ok", false}}}}}};
    delivery.summarize();
    check(verify(response.at("source_context")).size() == 1);
    check(response.at("context_result").at("source_fragments_shown") == 1);
    check(response.at("context_result").at("document_records") == 1);
    check(response.at("context_result").at("evidence") == "partial");
    check(response.at("context_result").at("search_checks") == "warning");

    // A retained partial reply has no native projection. Unknown stays distinct
    // from a measured empty view, even if its notice contains footer-like text.
    Json retained = {{"ok", true}, {"command", "query"},
        {"document_context", {{"evidence", Json::array()}}}, {"source_coverage", {{"state", "partial"}}},
        {"source_context", "Retained notice\nEND {\"shown\":[1]}"}};
    LocalContextDelivery(retained, config).summarize();
    check(retained.at("context_result").at("source_fragments_shown").is_null());
    check(retained.at("context_result").at("evidence") == "partial");
}
#endif
}

int main() {
    try {
        using shin::observer::source_view;
        const std::string source = "first\r\nsecond\t\\\"\nlast";
        auto original = envelope(Json::array({body(source)}));
        const auto saved = original;
        const auto view = source_view(original);
        check(verify(view).at(1) == source && original == saved && view.size() <= 8192);
        const auto projection = shin::observer::project_source_view(original);
        check(projection.text == view && projection.receipt == Json::parse(view.substr(view.rfind("\nEND ") + 5)));
#if SHIN_OBSERVER_SOURCE_DISCOVERY
        context_delivery();
#endif
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
        const std::string visible = "safe before\r\n     \r\nsafe after\r\n";
        redacted["source_context"]["context"]["bodies"][0]["text"] = visible;
        redacted["source_context"]["context"]["collection_complete"] = false;
        const auto partial = shin::observer::project_source_view(redacted);
        check(verify(partial.text).at(1) == visible);
        check(partial.receipt.at("shown") == Json::array({1}) &&
            partial.receipt.at("redacted") == Json::array({1}) &&
            partial.receipt.at("all_native_bodies_shown") == false);

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
