#include "context_internal.h"
#include "execution_evidence.h"
#include "execution_io.h"
#include "execution_report.h"

namespace shin::context {
namespace {

struct CommandOptions {
    SessionCommandOptions session;
    bool strict = false;
};

CommandOptions command_options(const std::vector<std::string>& args, const std::string& command) {
    std::vector<std::string> session_args;
    bool strict = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& argument = args[index];
        if (argument == "--strict") {
            if (strict) throw_usage_error("duplicate-option", "--strict may appear only once.");
            if (command != "receipt-check") throw_usage_error("unsupported-option", "Only receipt-check accepts --strict.");
            strict = true;
        } else {
            if (argument == "--jobs" || argument == "--task") throw_usage_error("unsupported-option", "Execution evidence does not accept jobs or task wording.");
            session_args.push_back(argument);
            if (argument != "--require-current" && index + 1 < args.size()) session_args.push_back(args[++index]);
        }
    }
    return {parse_session_options(session_args, command == "receipt-check", "--input"), strict};
}

Json read_inputs_file(const fs::path& path, const fs::path& root) {
    SessionCommandOptions input;
    input.input = path;
    return validate_execution_inputs(read_session_input(input, root));
}

bool fixed_scope_matches(const Json& captured, const Json& current) {
    for (const char* field : {"root", "profile", "profile_config_hash", "task_id", "thread_id"}) {
        if (captured.at(field) != current.at(field)) return false;
    }
    return true;
}

Json import_receipt(const RuntimeOptions& options, const Json& request, const Json& scope,
                    const SessionCommandOptions& command) {
    require_session_fields(request, {"schema_version", "format", "report_path", "inputs_path", "receipt_id", "producer",
                                     "started_at", "observed_at", "ttl_seconds", "command_fingerprint", "complete"});
    if (session_string(request, "schema_version", 128) != "shin.receipt-import-request.v1") {
        throw_usage_error("unsupported-receipt-import", "Expected shin.receipt-import-request.v1.");
    }
    if (!request.contains("complete") || request.at("complete") != true) {
        throw_usage_error("incomplete-execution-report", "The producer must explicitly declare report completion.");
    }
    const std::string format = session_string(request, "format", 32);
    const fs::path report_path = session_string(request, "report_path", 4096);
    const Json inputs = read_inputs_file(session_string(request, "inputs_path", 4096), options.root);
    if (!fixed_scope_matches(inputs.at("binding"), scope)) {
        throw_usage_error("execution-scope-mismatch", "Captured execution inputs belong to a different project, profile or task.");
    }
    const auto observed = session_integer(request, "observed_at", 0, command.observed_at);
    const auto started = session_integer(request, "started_at", inputs.at("captured_at").get<std::int64_t>(), observed);
    Json receipt = {{"schema_version", "shin.execution-receipt.v1"},
                    {"receipt_id", session_string(request, "receipt_id", 128)}, {"kind", "test"},
                    {"producer", session_string(request, "producer", 128)}, {"started_at", started},
                    {"observed_at", observed}, {"ttl_seconds", session_integer(request, "ttl_seconds", 1, kMaxSessionTtl)},
                    {"command_fingerprint", session_string(request, "command_fingerprint", 80)},
                    {"scope", inputs.at("binding")}, {"inputs", inputs}};
    require_session_digest(receipt.at("command_fingerprint").get<std::string>());
    const std::string content = read_execution_report_file(report_path, kMaxExecutionReportBytes);
    const ExecutionReportSummary parsed = parse_execution_report(format, content);
    receipt["result"] = parsed.result;
    receipt["report"] = {{"format", parsed.format}, {"content_hash", fnv1a64(content)}, {"bytes", content.size()},
                         {"payload_retained", false}, {"summary", execution_report_summary_json(parsed)}};
    const Json normalized = parse_execution_receipts(Json{{"receipts", Json::array({receipt})}});
    SessionCommandOptions assessment = command;
    assessment.trusted_producers.insert(receipt.at("producer").get<std::string>());
    const auto verified = assess_execution_receipts(options, normalized, scope, assessment, true);
    if (!verified.receipts.at(0).at("scope_current").get<bool>()) {
        throw_usage_error("execution-inputs-changed", "Execution inputs changed after their pre-run capture.");
    }
    // The internal applicability check is not a trust grant in the emitted bundle.
    Json bundle = make_result_document("shin.receipt-bundle.v1", "receipt-import", true);
    bundle["receipts"] = normalized;
    return bundle;
}

}  // namespace

int run_execution_evidence(const RuntimeOptions& input_options, const std::vector<std::string>& args,
                           const std::string& command_name) {
    if (input_options.force_source_scan) throw_usage_error("unsupported-option", "Execution evidence requires a published snapshot.");
    CommandOptions command = command_options(args, command_name);
    const RuntimeOptions options = canonical_session_options(input_options);
    const Json input = read_session_input(command.session, options.root);
    RetrievalSession session(options, RetrievalSessionOpenMode::verified_snapshot);
    const WorktreeInventory inventory = observe_worktree(options);
    const Json comparison = compare_worktree(options, session.manifest(), inventory, 0);
    const bool complete = worktree_observation_complete(inventory);
    const bool current = comparison.at("current").get<bool>() && complete && assess_index_freshness(options, true).current;
    if (command.session.require_current && !current) {
        throw_index_error("execution-index-not-current", "Execution evidence requires a current index for --require-current.");
    }
    if (!complete) throw_index_error("execution-observation-incomplete", "Execution evidence requires a complete source inventory.");
    Json scope = session_binding(options, command.session);
    scope["worktree_id"] = comparison.at("worktree").at("worktree_id");
    scope["snapshot_id"] = session.freshness().snapshot_id;
    command.session.observed_at = session_observed_at(command.session);
    Json output;
    if (command_name == "receipt-scope") {
        output = capture_execution_inputs(options, input, scope, command.session.observed_at);
    } else if (command_name == "receipt-import") {
        output = import_receipt(options, input, scope, command.session);
    } else {
        const auto assessed = assess_execution_receipts(options, parse_execution_bundle(input), scope, command.session, complete);
        const Json& receipts = assessed.receipts;
        const bool passed = !receipts.empty() && std::all_of(receipts.begin(), receipts.end(), [](const Json& receipt) {
            return receipt.at("passed_current").get<bool>();
        });
        output = make_result_document("shin.receipt-assessment.v1", "receipt-check", !command.strict || passed);
        output["receipts"] = receipts;
        output["binding"] = scope;
        output["index_current"] = current;
        output["observed_at"] = assessed.observed_at;
        output["next_actions"] = execution_receipt_actions(receipts);
        output["strict"] = command.strict;
        output["require_current"] = command.session.require_current;
        output["policy"] = {{"read_only", true}, {"actions_executed", false}, {"source_authority_upgraded", false}};
    }
    require_worktree_stable(options, inventory);
    if (command_name == "receipt-scope") {
        output["captured_at"] = session_observed_at(command.session);
        output.erase("inputs_id");
        output["inputs_id"] = fnv1a64(output.dump());
    }
    if (output.dump().size() > kMaxSessionInputBytes) {
        throw_usage_error("execution-output-too-large", "Execution evidence exceeds the 256 KiB artifact budget.");
    }
    emit_session_document(output);
    return output.contains("ok") && output.at("ok") == false ? 1 : 0;
}

}  // namespace shin::context
