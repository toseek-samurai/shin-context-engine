#include "context_internal.h"
#include "generated_io.h"

namespace shin::context {
namespace {

constexpr std::size_t kMaxReadyTaskBytes = 16U * 1024U;
constexpr int kDefaultReadyDetailLimit = 20;
constexpr int kMaxReadyDetailLimit = 100;

struct ReadyOptions {
    std::optional<std::string> marker;
    std::optional<fs::path> routing_config;
    std::optional<std::string> task;
    int detail_limit = kDefaultReadyDetailLimit;
    bool strict = false;
    bool verbose = false;
};

struct ReadyStep {
    std::string id;
    bool attempted = true;
    bool ok = false;
    int exit_code = 1;
    Json result = nullptr;
    Json error = nullptr;
};

class ScopedReadyOutputCapture {
public:
    explicit ScopedReadyOutputCapture(std::ostream& destination)
        : previous_(std::cout.rdbuf(destination.rdbuf())) {}

    ~ScopedReadyOutputCapture() { std::cout.rdbuf(previous_); }

    ScopedReadyOutputCapture(const ScopedReadyOutputCapture&) = delete;
    ScopedReadyOutputCapture& operator=(const ScopedReadyOutputCapture&) = delete;

private:
    std::streambuf* previous_;
};

ReadyOptions parse_ready_options(const std::vector<std::string>& args) {
    ReadyOptions options;
    bool marker_seen = false;
    bool routing_seen = false;
    bool task_seen = false;
    bool limit_seen = false;
    bool strict_seen = false;
    bool verbose_seen = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& argument = args[index];
        auto read_value = [&](const std::string& option) -> const std::string& {
            if (index + 1 >= args.size()) {
                throw_usage_error("missing-option-value", option + " requires a value.");
            }
            return args[++index];
        };
        if (argument == "--marker") {
            if (marker_seen) {
                throw_usage_error("duplicate-option", "ready accepts --marker only once.");
            }
            options.marker = read_value(argument);
            marker_seen = true;
        } else if (argument == "--routing-config") {
            if (routing_seen) {
                throw_usage_error("duplicate-option",
                                  "ready accepts --routing-config only once.");
            }
            options.routing_config = fs::path(read_value(argument));
            routing_seen = true;
        } else if (argument == "--task") {
            if (task_seen) {
                throw_usage_error("duplicate-option", "ready accepts --task only once.");
            }
            options.task = trim(read_value(argument));
            task_seen = true;
        } else if (argument == "--limit") {
            if (limit_seen) {
                throw_usage_error("duplicate-option", "ready accepts --limit only once.");
            }
            options.detail_limit = parse_cli_integer(read_value(argument), "--limit", 1);
            if (options.detail_limit > kMaxReadyDetailLimit) {
                throw_usage_error("invalid-option-value",
                                  "ready --limit must be between 1 and 100.");
            }
            limit_seen = true;
        } else if (argument == "--strict") {
            if (strict_seen) {
                throw_usage_error("duplicate-option", "ready accepts --strict only once.");
            }
            options.strict = true;
            strict_seen = true;
        } else if (argument == "--verbose") {
            if (verbose_seen) {
                throw_usage_error("duplicate-option", "ready accepts --verbose only once.");
            }
            options.verbose = true;
            verbose_seen = true;
        } else {
            throw_usage_error("unsupported-option",
                              "Unsupported ready option: " + argument + ".");
        }
    }
    if (options.routing_config.has_value() != options.task.has_value()) {
        throw_usage_error(
            "incomplete-routing-request",
            "ready requires --routing-config and --task together.",
            "Omit both for one known route, or provide both for explicit route verification.");
    }
    if (options.task.has_value() &&
        (options.task->empty() || options.task->size() > kMaxReadyTaskBytes)) {
        throw_usage_error("invalid-ready-task",
                          "ready --task must contain 1-16384 bytes.");
    }
    return options;
}

Json ready_error_json(const CliError& error) {
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
ReadyStep execute_ready_step(std::string id, Invoke&& invoke) {
    ReadyStep step;
    step.id = std::move(id);
    std::ostringstream captured;
    try {
        {
            ScopedReadyOutputCapture capture(captured);
            step.exit_code = invoke();
        }
        const std::string output = captured.str();
        if (!try_parse_json_text(output, step.result) || !step.result.is_object()) {
            throw CliError(
                "ready-step-output-invalid", "internal",
                "Readiness step " + step.id + " did not emit one JSON result document.",
                1, false, "Report the step and invocation as a Shin defect.");
        }
        const auto ok = step.result.find("ok");
        step.ok = step.exit_code == 0 && ok != step.result.end() &&
                  ok->is_boolean() && ok->get<bool>();
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const CliError& error) {
        step.exit_code = error.exit_code();
        step.error = ready_error_json(error);
    } catch (const std::exception& error) {
        step.exit_code = 1;
        step.error = {
            {"code", "ready-step-execution-failed"},
            {"category", "runtime"},
            {"message", error.what()},
            {"retryable", false},
            {"remediation", "Run the failed readiness step directly for complete diagnostics."}
        };
    }
    return step;
}

ReadyStep skipped_ready_step(std::string id) {
    ReadyStep step;
    step.id = std::move(id);
    step.attempted = false;
    step.exit_code = 0;
    return step;
}

Json compact_success_result(const ReadyStep& step) {
    if (!step.result.is_object()) {
        return nullptr;
    }
    if (step.id == "adoption-check") {
        return {
            {"schema_version", step.result.value("schema_version", "")},
            {"project", step.result.value("project", Json(nullptr))},
            {"profile", step.result.value("profile", Json(nullptr))},
            {"index", step.result.value("index", Json(nullptr))}
        };
    }
    if (step.id == "worktree-diff") {
        return {
            {"schema_version", step.result.value("schema_version", "")},
            {"current", step.result.value("current", false)},
            {"comparison", step.result.value("comparison", Json(nullptr))},
            {"snapshot", step.result.value("snapshot", Json(nullptr))},
            {"summary", step.result.value("summary", Json(nullptr))}
        };
    }
    return step.result;
}

Json compact_failure_result(const ReadyStep& step) {
    if (!step.result.is_object()) {
        return nullptr;
    }
    if (step.id == "adoption-check" || step.id == "worktree-diff") {
        return compact_success_result(step);
    }
    if (step.id == "validate") {
        Json failed_gates = Json::array();
        const Json* gates = find_json_field(step.result, "gates");
        if (gates != nullptr && gates->is_array()) {
            for (const Json& gate : *gates) {
                if (gate.is_object() && gate.value("status", "") == "failed") {
                    Json failed_gate = {
                        {"id", gate.value("id", "unknown")},
                        {"domain", gate.value("domain", "unknown")},
                        {"exit_code", gate.value("exit_code", 1)}
                    };
                    if (gate.contains("result")) {
                        failed_gate["diagnostic"] = gate.at("result");
                    }
                    if (gate.contains("error")) {
                        failed_gate["error"] = gate.at("error");
                    }
                    failed_gates.push_back(std::move(failed_gate));
                }
            }
        }
        return {
            {"schema_version", step.result.value("schema_version", "")},
            {"profile", step.result.value("profile", Json(nullptr))},
            {"summary", step.result.value("summary", Json(nullptr))},
            {"failed_gates", std::move(failed_gates)},
            {"resource_check", step.result.value("resource_check", Json(nullptr))},
            {"warnings", step.result.value("warnings", Json::array())},
            {"warning_budget", step.result.value("warning_budget", Json(nullptr))}
        };
    }
    return step.result;
}

std::string ready_step_diagnostic_id(const ReadyStep& step) {
    std::string reason = "failed";
    if (step.error.is_object()) {
        reason = step.error.value("code", reason);
    } else if (step.id == "worktree-diff") {
        reason = "worktree-not-current";
    } else if (step.id == "validate") {
        reason = "validation-failed";
    }
    return "ready:" + step.id + ":" + reason;
}

Json ready_step_json(const ReadyStep& step, bool verbose) {
    Json output = {
        {"id", step.id},
        {"attempted", step.attempted},
        {"status", !step.attempted ? "skipped" : (step.ok ? "passed" : "failed")},
        {"ok", step.ok},
        {"exit_code", step.attempted ? Json(step.exit_code) : Json(nullptr)},
        {"error", step.error}
    };
    output["result"] = verbose ? step.result
                               : (step.ok ? compact_success_result(step)
                                          : compact_failure_result(step));
    return output;
}

Json ready_next_command(const RuntimeOptions& options,
                        const fs::path& profile_dir,
                        const ReadyOptions& ready_options,
                        const std::string& next_action) {
    std::vector<std::string> argv = {
        "shin-context",
        "--profile", options.profile.id,
        "--profile-dir", fs::absolute(profile_dir).lexically_normal().generic_string(),
        "--root", fs::absolute(options.root).lexically_normal().generic_string(),
        "--out-dir", fs::absolute(options.out_root).lexically_normal().generic_string()
    };
    bool index_mutation = false;
    std::string command;
    if (next_action == "rebuild-index") {
        command = "build";
        argv.push_back("build");
        index_mutation = true;
    } else if (next_action == "inspect-validation-failures") {
        command = "validate";
        argv.insert(argv.end(), {"validate", "--require-current"});
        if (ready_options.marker.has_value()) {
            argv.insert(argv.end(), {"--marker", *ready_options.marker});
        }
        if (ready_options.strict) {
            argv.push_back("--strict");
        }
    } else {
        return nullptr;
    }
    return {
        {"command", command},
        {"argv", argv},
        {"automatic_execution", false},
        {"mutation", {{"consumer", false}, {"index", index_mutation}}}
    };
}

fs::path canonical_existing_directory(const fs::path& path,
                                      const std::string& label) {
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(path, error);
    if (error || !fs::is_directory(canonical, error)) {
        throw CliError("ready-route-path-invalid", "configuration",
                       label + " is not an existing directory: " + path.string() + ".",
                       2, false, "Repair the explicit ready invocation.");
    }
    return canonical;
}

bool routing_step_aligned(const ReadyStep& step,
                          const RuntimeOptions& options,
                          const fs::path& profile_dir,
                          const std::optional<std::string>& marker,
                          Json& alignment) {
    alignment = {
        {"required", true},
        {"state", step.result.is_object() ? step.result.value("state", "invalid")
                                           : "failed"},
        {"profile", false},
        {"profile_dir", false},
        {"root", false},
        {"marker", false}
    };
    if (!step.ok || !step.result.is_object() ||
        step.result.value("state", "") != "current-route") {
        return false;
    }
    const Json* recommendation = find_json_field(step.result, "recommendation");
    if (recommendation == nullptr || !recommendation->is_object()) {
        return false;
    }
    alignment["profile"] =
        json_string_value(*recommendation, "profile") == options.profile.id;
    alignment["root"] = canonical_existing_directory(
                            json_string_value(*recommendation, "root"),
                            "Recommended root") ==
                        canonical_existing_directory(options.root, "Current root");
    alignment["profile_dir"] = canonical_existing_directory(
                                   json_string_value(*recommendation, "profile_dir"),
                                   "Recommended profile registry") ==
                               canonical_existing_directory(profile_dir,
                                                            "Current profile registry");
    const Json* recommended_marker = find_json_field(*recommendation, "marker");
    if (recommended_marker == nullptr || recommended_marker->is_null()) {
        alignment["marker"] = true;
    } else {
        alignment["marker"] = marker.has_value() && recommended_marker->is_string() &&
                              recommended_marker->get<std::string>() == *marker;
    }
    return alignment["profile"].get<bool>() &&
           alignment["profile_dir"].get<bool>() &&
           alignment["root"].get<bool>() &&
           alignment["marker"].get<bool>();
}

bool adoption_step_aligned(ReadyStep& step,
                           const RuntimeOptions& options,
                           Json& alignment) {
    alignment = {
        {"required", true},
        {"profile", false},
        {"index_key", false}
    };
    if (!step.ok || !step.result.is_object()) {
        return false;
    }
    const Json* profile = find_json_field(step.result, "profile");
    const Json* index = find_json_field(step.result, "index");
    if (profile == nullptr || !profile->is_object() ||
        index == nullptr || !index->is_object()) {
        step.ok = false;
        step.exit_code = 1;
        step.error = {
            {"code", "ready-adoption-output-invalid"},
            {"category", "internal"},
            {"message", "adoption-check omitted profile or index identity."},
            {"retryable", false},
            {"remediation", "Report the marker and invocation as a Shin defect."}
        };
        return false;
    }
    alignment["profile"] = json_string_value(*profile, "id") == options.profile.id;
    alignment["index_key"] =
        json_string_value(*index, "key") == options.out_root.filename().string();
    const bool aligned = alignment["profile"].get<bool>() &&
                         alignment["index_key"].get<bool>();
    if (!aligned) {
        step.ok = false;
        step.exit_code = 1;
        step.error = {
            {"code", "ready-adoption-route-mismatch"},
            {"category", "configuration"},
            {"message", "The selected profile or output directory does not match the adoption marker."},
            {"retryable", false},
            {"remediation", "Use marker.profile.id and <EXTERNAL_OUTPUT_BASE>/<marker.index.key>."}
        };
    }
    return aligned;
}

}  // namespace

int run_ready(const RuntimeOptions& options,
              const fs::path& profile_dir,
              const std::vector<std::string>& args) {
    ReadyOptions ready_options = parse_ready_options(args);
    if (ready_options.marker.has_value()) {
        ready_options.marker = normalize_repository_file_argument(
            options.root, *ready_options.marker, "--marker");
    }

    std::vector<ReadyStep> steps;
    steps.reserve(4);
    Json routing_alignment = {
        {"required", false}, {"state", "not-requested"},
        {"profile", true}, {"profile_dir", true}, {"root", true}, {"marker", true}
    };
    bool route_aligned = true;
    if (ready_options.routing_config.has_value()) {
        if (!ready_options.task.has_value()) {
            throw CliError("ready-routing-task-missing", "internal",
                           "Validated ready routing options have no task.", 1, false,
                           "Report this as a Shin Context Engine defect.");
        }
        const std::vector<std::string> routing_args = {
            "--config", ready_options.routing_config->string(),
            "--from-root", options.root.string(),
            "--limit", "3", *ready_options.task
        };
        steps.push_back(execute_ready_step(
            "suggest-profile", [&]() { return run_suggest_profile(routing_args); }));
        route_aligned = routing_step_aligned(
            steps.back(), options, profile_dir, ready_options.marker, routing_alignment);
    } else {
        steps.push_back(skipped_ready_step("suggest-profile"));
    }

    Json adoption_alignment = {
        {"required", ready_options.marker.has_value()},
        {"profile", !ready_options.marker.has_value()},
        {"index_key", !ready_options.marker.has_value()}
    };
    bool adoption_aligned = !ready_options.marker.has_value();
    if (!route_aligned) {
        steps.push_back(skipped_ready_step("adoption-check"));
        steps.push_back(skipped_ready_step("worktree-diff"));
        steps.push_back(skipped_ready_step("validate"));
    } else {
        if (ready_options.marker.has_value()) {
            std::vector<std::string> adoption_args = {"--marker", *ready_options.marker};
            if (ready_options.strict) {
                adoption_args.push_back("--strict");
            }
            steps.push_back(execute_ready_step(
                "adoption-check",
                [&]() { return run_adoption_check(options.root, profile_dir, adoption_args); }));
            adoption_aligned = adoption_step_aligned(
                steps.back(), options, adoption_alignment);
        } else {
            steps.push_back(skipped_ready_step("adoption-check"));
        }
        if (!adoption_aligned) {
            steps.push_back(skipped_ready_step("worktree-diff"));
            steps.push_back(skipped_ready_step("validate"));
        } else {
            std::unique_ptr<shin::generated_io::IndexLock> index_lock;
            std::vector<std::string> worktree_args = {
                "--limit", std::to_string(ready_options.detail_limit)
            };
            if (ready_options.strict) {
                worktree_args.push_back("--strict");
            }
            steps.push_back(execute_ready_step(
                "worktree-diff",
                [&]() {
                    index_lock = lock_index_for_read(options.out_root);
                    return run_worktree_diff(options, worktree_args);
                }));

            std::vector<std::string> validate_args = {"--require-current"};
            if (!ready_options.verbose) {
                validate_args.insert(validate_args.begin(), "--summary");
            }
            if (ready_options.strict) {
                validate_args.push_back("--strict");
            }
            if (index_lock) {
                steps.push_back(execute_ready_step(
                    "validate",
                    [&]() { return run_validate(options, profile_dir, validate_args); }));
            } else {
                steps.push_back(skipped_ready_step("validate"));
            }
        }
    }

    int attempted = 0;
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    bool worktree_current = false;
    bool validation_passed = false;
    bool retrieval_validation_passed = false;
    Json step_results = Json::array();
    Json diagnostic_ids = Json::array();
    for (const ReadyStep& step : steps) {
        if (!step.attempted) {
            ++skipped;
        } else {
            ++attempted;
            if (step.ok) {
                ++passed;
            } else {
                ++failed;
            }
        }
        if (step.id == "worktree-diff" && step.ok) {
            worktree_current = step.result.value("current", false);
        }
        if (step.id == "validate") {
            validation_passed = step.ok;
            retrieval_validation_passed = step.error.is_null() && step.result.is_object() &&
                step.result.value("retrieval_ready", Json(false)) == true;
        }
        if (step.attempted && !step.ok) {
            diagnostic_ids.push_back(ready_step_diagnostic_id(step));
        }
        step_results.push_back(ready_step_json(step, ready_options.verbose));
    }

    const bool ready = route_aligned && adoption_aligned &&
                       worktree_current && validation_passed && failed == 0;
    int readiness_requirements_failed = 0;
    if (!route_aligned || !adoption_aligned) {
        ++readiness_requirements_failed;
    } else {
        if (!worktree_current) {
            ++readiness_requirements_failed;
        }
        if (!validation_passed) {
            ++readiness_requirements_failed;
        }
    }
    std::string next_action = "brief-or-pack";
    if (!route_aligned) {
        next_action = "resolve-profile-route";
    } else if (!adoption_aligned) {
        next_action = "repair-adoption-invocation";
    } else if (!worktree_current) {
        next_action = "rebuild-index";
    } else if (!validation_passed) {
        next_action = "inspect-validation-failures";
    }

    Json output = make_result_document("shin.agent-readiness.v1", "ready", ready);
    output["ready"] = ready;
    output["retrieval_ready"] = route_aligned && adoption_aligned && worktree_current && retrieval_validation_passed;
    output["profile"] = options.profile.id;
    output["root"] = options.root.generic_string();
    output["out_root"] = options.out_root.generic_string();
    output["marker"] = ready_options.marker.has_value()
                           ? Json(*ready_options.marker)
                           : Json(nullptr);
    output["strict"] = ready_options.strict;
    output["require_current"] = true;
    output["policy"] = {
        {"profile_switch", false},
        {"build", false},
        {"snapshot_mutation", false},
        {"consumer_mutation", false},
        {"fail_fast", false},
        {"prerequisite_gated", true}
    };
    output["alignment"] = {
        {"routing", routing_alignment},
        {"adoption", adoption_alignment}
    };
    output["summary"] = {
        {"attempted", attempted},
        {"passed", passed},
        {"failed", failed},
        {"skipped", skipped},
        {"readiness_requirements_failed", readiness_requirements_failed},
        {"worktree_current", worktree_current},
        {"validation_passed", validation_passed}
    };
    output["next_action"] = next_action;
    output["next_command"] = ready_next_command(
        options, profile_dir, ready_options, next_action);
    output["diagnostic_ids"] = std::move(diagnostic_ids);
    if (ready_options.verbose) {
        output["verbose"] = true;
    }
    output["steps"] = std::move(step_results);
    emit_json_document(std::cout, output);
    return ready ? 0 : 1;
}

}  // namespace shin::context
