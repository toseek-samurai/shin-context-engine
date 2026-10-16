#include "context_internal.h"
#include "execution_evidence.h"
#include "execution_report.h"

namespace shin::context {
namespace {

std::string receipt_enum(const Json& receipt, const std::string& field,
                         const std::set<std::string>& allowed) {
    const std::string value = session_string(receipt, field, 128);
    if (!allowed.contains(value)) throw_usage_error("invalid-session-enum", "Unsupported execution receipt enum.");
    return value;
}

void validate_report(const Json& report, const std::string& result) {
    require_session_fields(report, {"format", "content_hash", "bytes", "payload_retained", "summary"});
    receipt_enum(report, "format", {"junit", "playwright"});
    require_session_digest(session_string(report, "content_hash", 80));
    session_integer(report, "bytes", 0, 4U * 1024U * 1024U);
    if (!report.contains("payload_retained") || report.at("payload_retained") != false) {
        throw_usage_error("invalid-receipt-report", "Receipts cannot retain report payloads.");
    }
    if (!report.contains("summary")) throw_usage_error("invalid-receipt-report", "Report summary is required.");
    const Json& summary = report.at("summary");
    require_session_fields(summary, {"total", "passed", "failed", "skipped", "flaky", "interrupted", "expected_failures", "global_errors"});
    std::map<std::string, std::int64_t> counts;
    for (const char* field : {"total", "passed", "failed", "skipped", "flaky", "interrupted", "expected_failures", "global_errors"}) {
        counts[field] = session_integer(summary, field, 0, 100000);
    }
    if (counts["total"] != counts["passed"] + counts["failed"] + counts["skipped"] + counts["flaky"] + counts["interrupted"] ||
        counts["expected_failures"] > counts["passed"]) {
        throw_usage_error("invalid-receipt-report", "Report outcome counts are inconsistent.");
    }
    ExecutionReportSummary normalized;
    normalized.total = static_cast<std::uint64_t>(counts["total"]);
    normalized.failed = static_cast<std::uint64_t>(counts["failed"]);
    normalized.interrupted = static_cast<std::uint64_t>(counts["interrupted"]);
    normalized.global_errors = static_cast<std::uint64_t>(counts["global_errors"]);
    normalized.skipped = static_cast<std::uint64_t>(counts["skipped"]);
    normalized.flaky = static_cast<std::uint64_t>(counts["flaky"]);
    const std::string expected = execution_report_outcome(normalized);
    if (result != expected) throw_usage_error("invalid-receipt-report", "Receipt result contradicts its normalized report.");
}

Json scope_differences(const Json& expected, const Json& actual) {
    Json result = Json::array();
    for (auto field = expected.begin(); field != expected.end(); ++field) {
        if (!actual.contains(field.key()) || actual.at(field.key()) != field.value()) result.push_back(field.key());
    }
    return result;
}

std::string next_action_for(bool trusted, const std::string& age, bool scope_current,
                            const std::string& result) {
    if (!trusted) return "review-untrusted-producer";
    if (age == "future") return "review-receipt-clock";
    if (age == "expired") return "refresh-expired-receipt";
    if (!scope_current) return "rerun-changed-inputs";
    if (result == "fail") return "review-failed-check";
    if (result != "pass") return "obtain-complete-execution-evidence";
    return "reuse-scoped-result";
}

}  // namespace

Json parse_execution_receipts(const Json& input) {
    const auto values = input.find("receipts");
    if (values == input.end() || !values->is_array() || values->size() > 32) {
        throw_usage_error("invalid-session-array", "Execution receipts require an array of at most 32 items.");
    }
    Json result = Json::array();
    std::set<std::string> ids;
    for (const Json& receipt : *values) {
        require_session_fields(receipt, {"schema_version", "receipt_id", "kind", "producer", "observed_at", "ttl_seconds",
                                         "result", "scope", "command_fingerprint", "started_at", "inputs", "report"});
        if (session_string(receipt, "schema_version", 128) != "shin.execution-receipt.v1") {
            throw_usage_error("unsupported-execution-receipt", "Expected shin.execution-receipt.v1.");
        }
        if (!ids.insert(session_string(receipt, "receipt_id", 128)).second) {
            throw_usage_error("duplicate-session-id", "Execution receipt ids must be unique.");
        }
        receipt_enum(receipt, "kind", {"test", "build", "browser", "service", "migration", "deployment", "load-test"});
        const std::string outcome = receipt_enum(receipt, "result", {"pass", "fail", "unknown", "skipped"});
        session_string(receipt, "producer", 128);
        const auto observed_at = session_integer(receipt, "observed_at", 0, kMaxSessionTimestamp);
        session_integer(receipt, "ttl_seconds", 1, kMaxSessionTtl);
        require_session_digest(session_string(receipt, "command_fingerprint", 80));
        if (!receipt.contains("scope")) throw_usage_error("invalid-receipt-scope", "Execution receipt requires a scope.");
        parse_session_binding(receipt.at("scope"), true);
        if (receipt.contains("started_at")) session_integer(receipt, "started_at", 0, observed_at);
        if (receipt.contains("inputs")) {
            const Json inputs = validate_execution_inputs(receipt.at("inputs"));
            const auto started_at = session_integer(receipt, "started_at", 0, observed_at);
            if (inputs.at("binding") != receipt.at("scope") || inputs.at("captured_at").get<std::int64_t>() > started_at) {
                throw_usage_error("invalid-receipt-input-binding", "Receipt inputs must be captured before execution with the same binding.");
            }
        }
        if (receipt.contains("report")) {
            if (!receipt.contains("inputs") || receipt.at("kind") != "test") {
                throw_usage_error("invalid-receipt-report", "Imported test reports require captured inputs.");
            }
            validate_report(receipt.at("report"), outcome);
        }
        result.push_back(receipt);
    }
    return result;
}

Json read_execution_bundle(const fs::path& path, const fs::path& root) {
    SessionCommandOptions input;
    input.input = path;
    return parse_execution_bundle(read_session_input(input, root));
}

Json parse_execution_bundle(const Json& document) {
    require_session_fields(document, {"schema_version", "tool", "command", "ok", "receipts"});
    if (session_string(document, "schema_version", 128) != "shin.receipt-bundle.v1") {
        throw_usage_error("unsupported-receipt-bundle", "Expected shin.receipt-bundle.v1.");
    }
    if ((document.contains("tool") && document.at("tool") != "shin-context") ||
        (document.contains("command") && document.at("command") != "receipt-import") ||
        (document.contains("ok") && document.at("ok") != true)) {
        throw_usage_error("invalid-receipt-bundle", "Receipt bundle envelope is inconsistent.");
    }
    return parse_execution_receipts(document);
}

ExecutionReceiptAssessment assess_execution_receipts(const RuntimeOptions& options, const Json& receipts,
                               const Json& current_scope, const SessionEvaluationOptions& command,
                               bool observation_complete) {
    Json result = Json::array();
    std::map<std::string, Json> input_cache;
    for (const Json& receipt : receipts) {
        if (!receipt.contains("inputs")) continue;
        const Json differences = scope_differences(current_scope, receipt.at("scope"));
        const bool fixed_binding = std::all_of(differences.begin(), differences.end(), [](const Json& field) {
            return field == "worktree_id" || field == "snapshot_id";
        });
        if (fixed_binding) {
            const std::string key = receipt.at("inputs").dump();
            if (!input_cache.contains(key)) input_cache.emplace(key, assess_execution_inputs(options, receipt.at("inputs")));
        }
    }
    const std::int64_t now = session_observed_at(command);
    for (const Json& receipt : receipts) {
        const std::string age = session_age(receipt.at("observed_at").get<std::int64_t>(),
                                            receipt.at("ttl_seconds").get<std::int64_t>(), now);
        const Json all_differences = scope_differences(current_scope, receipt.at("scope"));
        Json differences = all_differences;
        Json inputs = {{"coverage", "profile"}, {"current", true}, {"changed_selectors", Json::array()}, {"complete", true}};
        bool narrowed = false;
        bool fixed_binding = true;
        for (const Json& field : all_differences) {
            if (field != "worktree_id" && field != "snapshot_id") fixed_binding = false;
        }
        // Never read paths from a receipt bound to another project, profile or task.
        if (receipt.contains("inputs") && fixed_binding) {
            const std::string key = receipt.at("inputs").dump();
            inputs = input_cache.at(key);
            narrowed = inputs.at("coverage") == "declared-complete";
            if (narrowed) {
                differences = Json::array();
                for (const Json& field : all_differences) {
                    if (field != "worktree_id" && field != "snapshot_id") differences.push_back(field);
                }
            }
        } else if (receipt.contains("inputs")) {
            inputs["coverage"] = receipt.at("inputs").at("coverage");
            inputs["current"] = false;
            inputs["complete"] = false;
        }
        if (inputs.at("coverage") == "profile") inputs["current"] = all_differences.empty();
        const bool scope_current = differences.empty() && inputs.at("current").get<bool>();
        const bool exact_trust = command.trusted_receipt_documents.contains(receipt.dump());
        const bool trusted = exact_trust || command.trusted_producers.contains(receipt.at("producer").get<std::string>());
        const bool usable = age == "current" && scope_current && trusted && observation_complete && inputs.at("complete").get<bool>();
        const std::string outcome = receipt.at("result").get<std::string>();
        const bool executed = usable && (outcome == "pass" || outcome == "fail");
        Json item = {
            {"receipt_id", receipt.at("receipt_id")}, {"producer", receipt.at("producer")}, {"kind", receipt.at("kind")},
            {"result", outcome}, {"observed_at", receipt.at("observed_at")}, {"ttl_seconds", receipt.at("ttl_seconds")},
            {"age", age}, {"scope_current", scope_current}, {"scope_differences", differences}, {"all_scope_differences", all_differences},
            {"input_coverage", inputs.at("coverage")}, {"inputs_current", inputs.at("current")},
            {"changed_selectors", inputs.at("changed_selectors")}, {"narrowed_scope_used", narrowed},
            {"producer_trusted", trusted},
            {"trust_source", exact_trust ? "explicit-host-receipt-policy" : "explicit-cli-policy"},
            {"verification_level", executed ? "executed" : "producer-asserted"}, {"usable", usable},
            {"passed_current", usable && outcome == "pass"}, {"command_fingerprint", receipt.at("command_fingerprint")},
            {"next_action", next_action_for(trusted, age, scope_current && observation_complete, outcome)},
            {"command_independently_verified", false}, {"continuous_execution_attestation", false}
        };
        if (receipt.contains("report")) item["report"] = receipt.at("report");
        result.push_back(std::move(item));
    }
    return {std::move(result), now};
}

Json execution_receipt_actions(const Json& receipts) {
    Json actions = Json::array();
    for (const Json& receipt : receipts) {
        if (receipt.at("passed_current").get<bool>()) continue;
        actions.push_back({{"code", receipt.at("next_action")}, {"receipt_id", receipt.at("receipt_id")},
                           {"source", "shin-receipt-policy"}, {"automatic_execution", false},
                           {"text", "Review the scoped receipt and explicitly rerun or reconfirm its check as required."}});
    }
    return actions;
}

}  // namespace shin::context
