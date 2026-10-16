#include "context_internal.h"
#include "validation_generation.h"

namespace shin::context {
namespace {

constexpr std::size_t kValidationGateDiagnosticBytes = 6U * 1024U;
constexpr std::size_t kValidationSummaryMaxBytes = 64U * 1024U;

struct ValidateOptions {
    bool strict = false;
    bool require_current = false;
    bool summary = false;
    std::optional<std::string> marker;
};

struct ValidationGate {
    std::string id;
    std::string domain;
    bool attempted = true;
    bool ok = false;
    int exit_code = 1;
    double elapsed_ms = 0.0;
    Json result = nullptr;
    Json error = nullptr;
};

class ScopedCoutCapture {
public:
    explicit ScopedCoutCapture(std::ostream& destination)
        : previous_(std::cout.rdbuf(destination.rdbuf())) {}

    ~ScopedCoutCapture() { std::cout.rdbuf(previous_); }

    ScopedCoutCapture(const ScopedCoutCapture&) = delete;
    ScopedCoutCapture& operator=(const ScopedCoutCapture&) = delete;

private:
    std::streambuf* previous_;
};

ValidateOptions parse_validate_options(const std::vector<std::string>& args) {
    ValidateOptions options;
    bool strict_seen = false;
    bool current_seen = false;
    bool summary_seen = false;
    bool marker_seen = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--strict") {
            if (strict_seen) {
                throw_usage_error("duplicate-option", "validate accepts --strict only once.");
            }
            options.strict = true;
            strict_seen = true;
        } else if (args[index] == "--require-current") {
            if (current_seen) {
                throw_usage_error("duplicate-option",
                                  "validate accepts --require-current only once.");
            }
            options.require_current = true;
            current_seen = true;
        } else if (args[index] == "--summary") {
            if (summary_seen) {
                throw_usage_error("duplicate-option",
                                  "validate accepts --summary only once.");
            }
            options.summary = true;
            summary_seen = true;
        } else if (args[index] == "--marker") {
            if (marker_seen || index + 1 >= args.size()) {
                throw_usage_error(marker_seen ? "duplicate-option" : "missing-option-value",
                                  marker_seen ? "validate accepts --marker only once."
                                              : "--marker requires a repository-relative path.");
            }
            options.marker = args[++index];
            marker_seen = true;
        } else {
            throw_usage_error(
                "unsupported-option",
                "validate only accepts --strict, --require-current, "
                "--summary, and --marker.");
        }
    }
    return options;
}

Json validation_error_json(const CliError& error) {
    Json result = {
        {"code", error.code()},
        {"category", error.category()},
        {"message", error.what()},
        {"retryable", error.retryable()},
        {"remediation", error.remediation()}
    };
    if (!error.diagnostic().is_null()) result["diagnostic"] = error.diagnostic();
    return result;
}

template <typename Invoke>
ValidationGate execute_validation_gate(std::string id,
                                       std::string domain,
                                       Invoke&& invoke) {
    ValidationGate gate;
    gate.id = std::move(id);
    gate.domain = std::move(domain);
    std::ostringstream captured;
    const auto started = SteadyClock::now();
    try {
        {
            ScopedCoutCapture capture(captured);
            gate.exit_code = invoke();
        }
        const std::string output = captured.str();
        if (!try_parse_json_text(output, gate.result) || !gate.result.is_object()) {
            throw CliError("validation-gate-output-invalid", "internal",
                           "Validation gate " + gate.id +
                               " did not emit one JSON result document.",
                           1, false,
                           "Report the gate and profile as a Shin Context Engine defect.");
        }
        const auto ok = gate.result.find("ok");
        gate.ok = gate.exit_code == 0 && ok != gate.result.end() &&
                  ok->is_boolean() && ok->get<bool>();
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const CliError& error) {
        gate.exit_code = error.exit_code();
        gate.error = validation_error_json(error);
    } catch (const std::exception& error) {
        gate.exit_code = 1;
        gate.error = {
            {"code", "validation-gate-execution-failed"},
            {"category", "runtime"},
            {"message", error.what()},
            {"retryable", false},
            {"remediation", "Inspect this gate result and run the gate directly if needed."}
        };
    }
    gate.elapsed_ms = elapsed_ms_since(started);
    return gate;
}

Json validation_gate_json(const ValidationGate& gate) {
    return {
        {"id", gate.id},
        {"domain", gate.domain},
        {"attempted", gate.attempted},
        {"elapsed_ms", gate.attempted ? Json(gate.elapsed_ms) : Json(nullptr)},
        {"status", !gate.attempted ? "skipped" : (gate.ok ? "passed" : "failed")},
        {"ok", gate.ok},
        {"exit_code", gate.attempted ? Json(gate.exit_code) : Json(nullptr)},
        {"result", gate.result},
        {"error", gate.error}
    };
}

Json validation_gate_detail_command(const ValidationGate& gate,
                                    const RuntimeOptions& options,
                                    const ValidateOptions& validate_options) {
    Json argv = Json::array({"shin-context"});
    if (gate.id == "adoption-check") {
        argv.push_back("--profile-dir");
        argv.push_back(fs::absolute(options.profile_dir).lexically_normal().generic_string());
        argv.push_back("--root");
        argv.push_back(fs::absolute(options.root).lexically_normal().generic_string());
        argv.push_back("adoption-check");
        if (validate_options.marker.has_value()) {
            argv.push_back("--marker");
            argv.push_back(*validate_options.marker);
        }
    } else {
        for (const std::string& argument :
             std::vector<std::string>{"--profile", options.profile.id, "--profile-dir",
                                      fs::absolute(options.profile_dir)
                                          .lexically_normal()
                                          .generic_string(),
                                      "--root",
                                      fs::absolute(options.root).lexically_normal().generic_string(),
                                      "--out-dir",
                                      fs::absolute(options.out_root)
                                          .lexically_normal()
                                          .generic_string(),
                                      gate.id}) {
            argv.push_back(argument);
        }
    }
    const bool supports_strict = gate.id == "profile-check" || gate.id == "doctor" ||
                                 gate.id == "vacuum" || gate.id == "eval" ||
                                 gate.id == "pack-eval" || gate.id == "adoption-check";
    if (validate_options.strict && supports_strict) {
        argv.push_back("--strict");
    }
    if (validate_options.require_current && !validate_options.strict &&
        (gate.id == "eval" || gate.id == "pack-eval")) {
        argv.push_back("--require-current");
    }
    return {{"command", gate.id},
            {"argv", std::move(argv)},
            {"automatic_execution", false},
            {"mutation", {{"consumer", false}, {"index", false}}}};
}

Json validation_gate_summary_json(const ValidationGate& gate,
                                  const RuntimeOptions& options,
                                  const ValidateOptions& validate_options) {
    Json output = {
        {"id", gate.id},
        {"domain", gate.domain},
        {"attempted", gate.attempted},
        {"elapsed_ms", gate.attempted ? Json(gate.elapsed_ms) : Json(nullptr)},
        {"status", !gate.attempted ? "skipped" : (gate.ok ? "passed" : "failed")},
        {"ok", gate.ok},
        {"exit_code", gate.attempted ? Json(gate.exit_code) : Json(nullptr)}
    };
    if (gate.attempted && !gate.ok) {
        if (gate.result.is_object()) {
            Json compact = Json::object();
            for (const char* field :
                 {"schema_version", "tool", "command", "ok", "profile", "root", "out_root",
                  "projection", "snapshot_id", "strict", "require_current", "summary",
                  "query_summary", "index_freshness", "failure_limit", "failure_total",
                  "truncated_failures", "suite_failure_reasons", "failure_budget"}) {
                if (gate.result.contains(field)) {
                    compact[field] = gate.result.at(field);
                }
            }
            constexpr std::size_t kFailureDetailLimit = 20;
            std::vector<std::pair<std::string, const Json*>> detail_sources;
            std::size_t eligible_details = 0;
            std::size_t retained_details = 0;
            for (const char* field : {"failures", "cases", "issues", "actions"}) {
                const Json* values = find_json_field(gate.result, field);
                if (values == nullptr || !values->is_array()) {
                    continue;
                }
                eligible_details += values->size();
                compact[field] = Json::array();
                compact[std::string(field) + "_total"] = values->size();
                compact[std::string(field) + "_truncated"] = values->size();
                detail_sources.emplace_back(field, values);
            }
            auto update_budget = [&](Json& document, std::size_t returned,
                                     bool metadata_omitted = false) {
                document["diagnostic_budget"] = {
                    {"policy", "whole-record-byte-budget-v1"},
                    {"max_bytes", kValidationGateDiagnosticBytes},
                    {"eligible_records", eligible_details},
                    {"returned_records", returned},
                    {"omitted_records", eligible_details - returned},
                    {"metadata_omitted", metadata_omitted},
                    {"truncated", metadata_omitted || eligible_details != returned}};
            };
            compact["detail_command"] =
                validation_gate_detail_command(gate, options, validate_options);
            update_budget(compact, 0);

            for (const auto& [field, values] : detail_sources) {
                for (std::size_t index = 0; index < values->size() && index < kFailureDetailLimit;
                     ++index) {
                    Json candidate = compact;
                    candidate[field].push_back((*values)[index]);
                    candidate[field + "_truncated"] =
                        values->size() - candidate[field].size();
                    update_budget(candidate, retained_details + 1);
                    if (candidate.dump().size() <= kValidationGateDiagnosticBytes) {
                        compact = std::move(candidate);
                        ++retained_details;
                    }
                }
            }
            update_budget(compact, retained_details);
            if (compact.dump().size() > kValidationGateDiagnosticBytes) {
                compact = {
                    {"schema_version", json_string_value(gate.result, "schema_version")},
                    {"tool", json_string_value(gate.result, "tool")},
                    {"command", json_string_value(gate.result, "command")},
                    {"ok", false},
                    {"details_omitted", true},
                    {"detail_command",
                     validation_gate_detail_command(gate, options, validate_options)}};
                update_budget(compact, 0, true);
            }
            output["result"] = std::move(compact);
        } else {
            output["result"] = gate.result;
        }
        output["error"] = gate.error;
    }
    return output;
}

int gate_safety_findings(const ValidationGate& gate) {
    if (!gate.result.is_object()) {
        return 0;
    }
    const auto issues = gate.result.find("issues");
    if (issues == gate.result.end() || !issues->is_array()) {
        return 0;
    }
    int findings = 0;
    for (const Json& issue : *issues) {
        const std::string severity = json_string_value(issue, "severity");
        const std::string id = json_string_value(issue, "id");
        if (severity != "info" &&
            (starts_with(id, "unsafe-") || starts_with(id, "secret-like-"))) {
            ++findings;
        }
    }
    return findings;
}

}  // namespace

int run_validate(const RuntimeOptions& input_options,
                 const fs::path& profile_dir,
                 const std::vector<std::string>& args) {
    ValidateOptions validate_options = parse_validate_options(args);
    auto options = input_options;
    options.generation_admission.reset();
    options.validation_generation = std::make_shared<ValidationGeneration>(options);
    const auto initial_generations = generation_execution_count();
    if (validate_options.marker.has_value()) {
        validate_options.marker = normalize_repository_file_argument(
            options.root, *validate_options.marker, "--marker");
    }
    const std::vector<std::string> strict_args =
        validate_options.strict ? std::vector<std::string>{"--strict"}
                                : std::vector<std::string>{};
    std::vector<std::string> eval_args = strict_args;
    if (validate_options.require_current && !validate_options.strict) {
        eval_args.push_back("--require-current");
    }
    if (validate_options.summary) {
        eval_args.push_back("--failed-only");
    }

    std::vector<ValidationGate> gates;
    gates.reserve(7);
    gates.push_back(execute_validation_gate(
        "profile-check", "configuration",
        [&]() { return run_profile_check(options, strict_args); }));
    gates.push_back(execute_validation_gate(
        "check", "index-integrity", [&]() { return run_check(options); }));
    gates.push_back(execute_validation_gate(
        "doctor", "index-integrity",
        [&]() { return run_doctor(options, strict_args); }));
    gates.push_back(execute_validation_gate(
        "vacuum", "index-integrity",
        [&]() { return run_vacuum(options, strict_args); }));
    gates.push_back(execute_validation_gate(
        "eval", "retrieval-quality",
        [&]() { return run_eval(options, eval_args); }));
    gates.push_back(execute_validation_gate(
        "pack-eval", "retrieval-quality",
        [&]() { return run_pack_eval(options, eval_args); }));
    if (validate_options.marker.has_value()) {
        std::vector<std::string> adoption_args = {"--marker", *validate_options.marker};
        if (validate_options.strict) {
            adoption_args.push_back("--strict");
        }
        gates.push_back(execute_validation_gate(
            "adoption-check", "adoption",
            [&]() { return run_adoption_check(options.root, profile_dir, adoption_args); }));
    } else {
        ValidationGate skipped;
        skipped.id = "adoption-check";
        skipped.domain = "adoption";
        skipped.attempted = false;
        skipped.exit_code = 0;
        gates.push_back(std::move(skipped));
    }

    int attempted = 0;
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    std::map<std::string, int> failed_by_domain;
    int safety_findings = 0;
    int runtime_failures = 0;
    Json gate_results = Json::array();
    for (const ValidationGate& gate : gates) {
        if (!gate.attempted) {
            ++skipped;
        } else {
            ++attempted;
            if (gate.ok) {
                ++passed;
            } else {
                ++failed;
                ++failed_by_domain[gate.domain];
            }
        }
        safety_findings += gate_safety_findings(gate);
        if (gate.error.is_object()) {
            const std::string category = json_string_value(gate.error, "category");
            if (category == "runtime" || category == "internal" ||
                category == "resource" || category == "filesystem") {
                ++runtime_failures;
            }
        }
        gate_results.push_back(validate_options.summary
                                   ? validation_gate_summary_json(gate, options, validate_options)
                                   : validation_gate_json(gate));
    }

    const bool ok = failed == 0;
    Json output = make_result_document(validate_options.summary
                                           ? "shin.validation-summary.v2"
                                           : "shin.validation.v1",
                                       "validate", ok);
    if (validate_options.summary) {
        output["projection"] = "summary";
    }
    output["profile"] = options.profile.id;
    output["root"] = options.root.generic_string();
    output["out_root"] = options.out_root.generic_string();
    output["strict"] = validate_options.strict;
    output["require_current"] = validate_options.require_current;
    output["marker"] = validate_options.marker.has_value()
                           ? Json(*validate_options.marker)
                           : Json(nullptr);
    output["execution"] = {
        {"policy", "independent-complete-diagnostics-v1"},
        {"fail_fast", false},
        {"index_mutation", false}
    };
    output["preparation"] = options.validation_generation->metrics();
    output["preparation"]["generation_runs"] = generation_execution_count() - initial_generations;
    output["summary"] = {
        {"total", gates.size()},
        {"attempted", attempted},
        {"passed", passed},
        {"failed", failed},
        {"skipped", skipped},
        {"failed_by_domain", failed_by_domain},
        {"failure_classes", {
            {"safety_findings", safety_findings},
            {"configuration_failures", failed_by_domain["configuration"]},
            {"index_integrity_failures", failed_by_domain["index-integrity"]},
            {"retrieval_quality_failures", failed_by_domain["retrieval-quality"]},
            {"adoption_failures", failed_by_domain["adoption"]},
            {"runtime_failures", runtime_failures}
        }}
    };
    output["gates"] = std::move(gate_results);
    for (const auto& gate : gates) {
        if (gate.id != "eval" || !gate.result.is_object()) continue;
        for (const auto* field : {"quality_ok", "resource_check", "warnings", "warning_budget"}) {
            if (gate.result.contains(field)) output[field] = gate.result.at(field);
        }
    }
    if (validate_options.summary) {
        output["diagnostic_budget"] = {{"policy", "bounded-failed-gate-projection-v1"},
                                       {"max_bytes", kValidationSummaryMaxBytes},
                                       {"truncated", false}};
        if (output.dump(2).size() > kValidationSummaryMaxBytes) {
            output["diagnostic_budget"]["truncated"] = true;
            for (Json& gate : output["gates"]) {
                if (!gate.is_object() || gate.value("status", "") != "failed" ||
                    !gate.contains("result") || !gate["result"].is_object()) {
                    continue;
                }
                const Json& result = gate["result"];
                Json minimal = {{"schema_version", result.value("schema_version", "")},
                                {"command", result.value("command", gate.value("id", ""))},
                                {"summary", result.value("summary", Json(nullptr))},
                                {"detail_command", result.value("detail_command", Json(nullptr))},
                                {"diagnostics_truncated", true}};
                gate["result"] = std::move(minimal);
            }
        }
        if (output.dump(2).size() > kValidationSummaryMaxBytes) {
            for (Json& gate : output["gates"]) {
                if (!gate.is_object() || gate.value("status", "") != "failed") {
                    continue;
                }
                gate["result"] = {{"command", gate.value("id", "")},
                                  {"diagnostics_truncated", true}};
                if (gate.contains("error") && gate["error"].is_object()) {
                    gate["error"] = {
                        {"code", gate["error"].value("code", "validation-gate-failed")},
                        {"category", gate["error"].value("category", "runtime")}};
                }
            }
        }
    }
    emit_json_document(std::cout, output);
    return ok ? 0 : 1;
}

}  // namespace shin::context
