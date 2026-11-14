#pragma once
#include "context_internal.h"

namespace shin::observer {

// A delivery summary, never a model-answer or whole-repository coverage verdict.
inline void add_context_result(Json& result, double measured_retrieval_ms = 0) {
    if (result.value("command", std::string{}) != "query" || !result.contains("document_context")) return;
    const auto& document = result.at("document_context");
    const auto documents = document.value("evidence", Json::array()).size();
    std::size_t shown = 0;
    bool known = false;
    const auto text = result.value("source_context", std::string{});
    const auto end = text.rfind("\nEND ");
    if (end != std::string::npos) {
        const auto receipt = Json::parse(text.substr(end + 5), nullptr, false);
        if (receipt.is_object() && receipt.contains("shown") && receipt.at("shown").is_array()) {
            shown = receipt.at("shown").size(); known = true;
        }
    }
    const bool partial = result.value("source_coverage", Json::object()).value("state", std::string{}) == "partial";
    const bool found = documents > 0 || shown > 0;
    const auto evidence = partial ? "partial" : found ? "found" : known ? "empty" : "not-assessed";
    const auto phases = result.value("phase_ms", Json::object());
    const auto peak = shin::context::process_peak_rss_bytes();
    // Both call sites deliver this only after successful native admission and
    // selected-source verification; bootstrap envelopes need not contain flags.
    result["context_result"] = {{"availability", result.value("ok", false) ? "ready" : "unavailable"},
        {"evidence", evidence}, {"document_records", documents}, {"source_fragments_shown", known ? Json(shown) : Json(nullptr)},
        {"answer_assessed", false}, {"next_action", found ? "inspect-returned-sources" : "use-authorized-direct-sources"},
        {"performance", {{"retrieval_ms", phases.value("retrieval", measured_retrieval_ms)},
            {"verification_ms", phases.value("final_verification", 0.0)},
            {"process_peak_rss_bytes", peak > 0 ? Json(peak) : Json(nullptr)},
            {"memory_scope", "reader-process-lifetime-peak-not-query-allocation"}}}};
    const auto readiness = result.value("background_readiness", result.value("readiness", Json::object()));
    const bool resource_warning = readiness.value("qualified_resources", Json::object())
        .value("resource_check", Json::object()).value("ok", true) == false;
    if (!result.value("quality_warning", Json()).is_null() || resource_warning) {
        result["context_result"]["search_checks"] = "warning";
        result["context_result"]["diagnostic_owner"] = "shin";
        result["context_result"]["notice"] = "Safe context remains available. Shin owns search diagnosis; review a project profile for actual owner/contract changes. Continue product work without document rewrites or search-counter tuning.";
    }
}
} // namespace shin::observer
