#include "local_context_delivery.h"
#include "source_view.h"
#include "context_internal.h"

namespace shin::observer {
Json LocalContextDelivery::wrap(const Json& source, const char* schema, const char* key) const {
    return {{"schema_version", schema}, {"binding_key", config_.binding_key},
        {"project_id", config_.binding.at("project_id")}, {"workspace_id", config_.binding.at("workspace_id")},
        {"live_source_verified", true}, {key, source}};
}

void LocalContextDelivery::query(Json documents, const Json& source, const Json& scope) {
    auto projection = project_source_view(wrap(source, "shin.observer-source-context.v1", "source_context"));
    shown_ = projection.receipt.at("shown").size();
    response_["source_context"] = std::move(projection.text);
    response_["source_scope"] = scope;
    response_["snapshot_id"] = source.at("snapshot_id");
    response_["document_context"] = std::move(documents);
    response_["answer_complete"] = false;
    response_["query_persisted"] = false;
}

void LocalContextDelivery::followup(const Json& source) {
    if (response_.at("command") == "inspect") {
        response_["source_inventory"] = parse_json(source_view(wrap(source,
            "shin.observer-source-inspection.v1", "source_inspection")));
    } else {
        response_["source_context"] = source_view(wrap(source, "shin.observer-source-read.v1", "source_read"), 32768);
        response_["native_read_complete"] = source.at("complete");
    }
    response_["snapshot_id"] = source.at("snapshot_id");
    response_["answer_complete"] = false;
    response_["query_persisted"] = false;
}

// A delivery summary, never a model-answer or whole-repository coverage verdict.
void LocalContextDelivery::summarize(double measured_retrieval_ms) const {
    auto& result = response_;
    if (result.value("command", std::string{}) != "query" || !result.contains("document_context")) return;
    const auto documents = result.at("document_context").value("evidence", Json::array()).size();
    const auto shown = shown_.value_or(0);
    const bool partial = result.value("source_coverage", Json::object()).value("state", std::string{}) == "partial";
    const bool found = documents > 0 || shown > 0;
    const auto evidence = partial ? "partial" : found ? "found" : shown_ ? "empty" : "not-assessed";
    const auto phases = result.value("phase_ms", Json::object());
    const auto peak = shin::context::process_peak_rss_bytes();
    result["context_result"] = {{"availability", result.value("ok", false) ? "ready" : "unavailable"},
        {"evidence", evidence}, {"document_records", documents}, {"source_fragments_shown", shown_ ? Json(shown) : Json(nullptr)},
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
        result["context_result"]["notice"] = "Verified context is available. Inspect the returned evidence and omissions. Report a reproducible search miss to Shin with the original query and expected location.";
    }
}
} // namespace shin::observer
