#include "shin/observer/observer.h"

#include <algorithm>

namespace shin::observer {
namespace {
void require_view(bool condition) {
    if (!condition) throw Error("source-view-invalid", "Cannot render a complete bounded source view.");
}

std::string inventory_view(const Json& result, std::size_t maximum_bytes) {
    const auto& native = result.at("source_inspection");
    require_view(native.at("ok") == true && native.at("records").is_array());
    Json view = {{"schema_version", "shin.source-inventory-view.v1"},
        {"project", result.at("project_id")}, {"workspace", result.at("workspace_id")},
        {"binding", result.at("binding_key")}, {"snapshot_id", native.at("snapshot_id")},
        {"path", native.at("path")}, {"live_source_verified", true}, {"answer_complete", false},
        {"scope", "indexed-records-and-resolved-declared-links; not compiler semantics"},
        {"native_coverage", native.at("coverage")},
        {"native_relation_coverage", {{"total", native.at("relations").at("total")},
            {"omitted", native.at("relations").at("omitted")}, {"complete", native.at("relations").at("complete")}}},
        {"records", Json::array()}, {"relations", native.at("relations").at("items")},
        {"view", {{"max_bytes", maximum_bytes}, {"omitted_records", 0}, {"omitted_relations", 0}}}};
    for (const auto& record : native.at("records")) {
        Json row;
        for (const char* key : {"id", "record_type", "start_line", "end_line", "evidence_kind", "verification_level"})
            if (record.contains(key)) row[key] = record.at(key);
        row["name"] = record.contains("symbol_name") ? record.at("symbol_name") : record.at("title");
        view["records"].push_back(std::move(row));
    }
    const auto rendered = [&] { return view.dump(2) + "\n"; };
    while (rendered().size() > maximum_bytes && !view.at("relations").empty()) {
        view["relations"].erase(view.at("relations").size() - 1);
        view["view"]["omitted_relations"] = view["view"]["omitted_relations"].get<std::size_t>() + 1;
    }
    while (rendered().size() > maximum_bytes && !view.at("records").empty()) {
        view["records"].erase(view.at("records").size() - 1);
        view["view"]["omitted_records"] = view["view"]["omitted_records"].get<std::size_t>() + 1;
    }
    const auto output = rendered();
    require_view(output.size() <= maximum_bytes);
    std::size_t begin = 0;
    while (begin < output.size()) {
        const auto end = output.find('\n', begin);
        require_view(end != std::string::npos && end - begin <= 1900);
        begin = end + 1;
    }
    return output;
}

std::string lines(const Json& body) {
    const auto text = body.at("text").get<std::string>();
    std::string result;
    std::size_t begin = 0, line = body.at("start_line").get<std::size_t>();
    while (begin < text.size()) {
        const auto newline = text.find('\n', begin);
        const auto line_end = newline == std::string::npos ? text.size() : newline + 1;
        auto end = std::min(begin + 256, line_end);
        // Segment long physical lines at UTF8 boundaries; quoted segments retain
        // original line endings, tabs and byte offsets without terminal controls.
        while (end < line_end && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) --end;
        require_view(end > begin);
        result += "L" + std::to_string(line) + "+" + std::to_string(begin) + " " +
                  Json(text.substr(begin, end - begin)).dump() + "\n";
        begin = end;
        if (begin == line_end && newline != std::string::npos) ++line;
    }
    return result;
}
}

std::string source_view(const Json& result, std::size_t maximum_bytes) {
    require_view(maximum_bytes >= 2048 && maximum_bytes <= 32768 &&
                 result.at("live_source_verified") == true);
    const auto schema = result.at("schema_version").get<std::string>();
    if (schema == "shin.observer-source-inspection.v1") return inventory_view(result, maximum_bytes);
    const bool context = schema == "shin.observer-source-context.v1";
    require_view(context || schema == "shin.observer-source-read.v1");
    const auto& native = result.at(context ? "source_context" : "source_read");
    const auto& evidence = native.at(context ? "context" : "evidence");
    const auto& bodies = evidence.at("bodies");
    require_view(bodies.is_array() && bodies.size() <= 12 && native.at("ok") == true);
    const auto upstream = evidence.at("omissions").size();
    const bool complete = context ? evidence.at("collection_complete").get<bool>() : native.at("complete").get<bool>();
    Json header = {{"schema_version", context ? "shin.source-view.v2" : "shin.source-view.v1"}, {"project", result.at("project_id")},
        {"workspace", result.at("workspace_id")}, {"binding", result.at("binding_key")},
        {"snapshot_id", native.at("snapshot_id")}, {"source_observed", true}, {"answer_complete", false},
        {"native_collection_complete", complete}, {"upstream_omissions", upstream},
        {"bodies", bodies.size()}, {"max_bytes", maximum_bytes}};
    const std::vector<std::string> locator_columns = {"path", "start_line", "end_line", "byte_start", "byte_end",
        "range_origin", "source_content_hash", "span_hash", "state", "role", "reason", "source_path", "source_line", "target_symbol"};
    if (context) {
        header["audit_relations"] = evidence.at("relations").size();
        header["body_selection"] = "one-dependency-then-ranked-fragments-v1";
        header["locator_columns"] = locator_columns;
    }
    std::string output = "SHIN " + header.dump() + "\n";
    output += "Paths belong to this Observer binding. SOURCE gives locators; TEXT gives JSON-quoted source segments.\n";
    if (context) output += "Locator arrays follow locator_columns; null means an absent field.\n";
    output += "L<number>+<body-byte-offset> labels physical source lines. Join decoded JSON segments for exact bytes; fragments may start/end inside a line.\n";
    output += "Not execution/answer proof. For omitted target_symbol, query that symbol; otherwise inspect/read the returned path.\n";
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        const auto& body = bodies[i];
        Json locator = context ? Json::array() : Json::object();
        for (const auto& key : locator_columns) {
            if (context) locator.push_back(body.contains(key) ? body.at(key) : Json(nullptr));
            else if (body.contains(key)) locator[key] = body.at(key);
        }
        output += "SOURCE " + std::to_string(i + 1) + " " + locator.dump() + "\n";
    }
    for (const auto& omission : evidence.at("omissions")) output += "OMISSION " + omission.dump() + "\n";
    // Bound the receipt from its schema and maximum inventories, not a fixed
    // allowance that can unnecessarily displace an otherwise fitting body.
    Json all_numbers = Json::array();
    for (std::size_t i = 0; i < bodies.size(); ++i) all_numbers.push_back(i + 1);
    const Json largest_receipt = {{"sha256", std::string(64, '0')}, {"payload_bytes", maximum_bytes},
        {"shown", all_numbers}, {"omitted_for_view_budget", Json::array()}, {"redacted", Json::array()},
        {"all_native_bodies_shown", false}, {"answer_complete", false}};
    const auto receipt_reserve = largest_receipt.dump().size() + 5;
    require_view(output.size() + receipt_reserve <= maximum_bytes);
    Json shown = Json::array(), omitted = Json::array(), redacted = Json::array();
    std::vector<std::size_t> order;
    // Reserve an opportunity for one complete declared dependency before
    // similar ranked excerpts consume the view. Source-read keeps request order.
    if (context) {
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            if (bodies[i].value("role", "") == "related" && bodies[i].at("state") == "complete" &&
                bodies[i].at("text").is_string()) { order.push_back(i); break; }
        }
    }
    for (std::size_t i = 0; i < bodies.size(); ++i)
        if (order.empty() || order.front() != i) order.push_back(i);
    for (const auto i : order) {
        const auto& body = bodies[i];
        if (body.at("state") != "complete" || !body.at("text").is_string()) {
            redacted.push_back(i + 1);
            continue;
        }
        const auto block = "TEXT " + std::to_string(i + 1) + "\n" + lines(body);
        if (output.size() + block.size() + receipt_reserve > maximum_bytes) {
            omitted.push_back(i + 1);
            continue;
        }
        output += block;
        shown.push_back(i + 1);
    }
    std::sort(shown.begin(), shown.end());
    std::sort(omitted.begin(), omitted.end());
    std::sort(redacted.begin(), redacted.end());
    const Json receipt = {{"sha256", digest(output)}, {"payload_bytes", output.size()}, {"shown", shown},
        {"omitted_for_view_budget", omitted}, {"redacted", redacted},
        {"all_native_bodies_shown", omitted.empty() && redacted.empty() && upstream == 0},
        {"answer_complete", false}};
    output += "END " + receipt.dump() + "\n";
    require_view(output.size() <= maximum_bytes);
    std::size_t begin = 0;
    while (begin < output.size()) {
        const auto end = output.find('\n', begin);
        require_view(end != std::string::npos && end - begin <= 1900);
        begin = end + 1;
    }
    return output;
}
}
