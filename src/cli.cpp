#include "context_internal.h"
#include "generated_row_decoder.h"
#include "generated_io.h"
#include "profile_policy.h"

namespace shin::context {

int run_discover(const RuntimeOptions& options) {
    const DiscoveryResult result = discover_project(options.root);
    std::cout << "{\n"
              << "  \"schema_version\": \"shin.discovery.v1\",\n"
              << "  \"tool\": \"shin-context\",\n"
              << "  \"command\": \"discover\",\n"
              << "  \"ok\": true,\n"
              << "  \"root\": " << json_string(options.root.generic_string()) << ",\n"
              << "  \"has_git\": " << (result.has_git ? "true" : "false") << ",\n"
              << "  \"has_docs_dir\": " << (result.has_docs_dir ? "true" : "false") << ",\n"
              << "  \"has_readme\": " << (result.has_readme ? "true" : "false") << ",\n"
              << "  \"has_agents\": " << (result.has_agents ? "true" : "false") << ",\n"
              << "  \"has_dotnet_solution\": " << (result.has_dotnet_solution ? "true" : "false") << ",\n"
              << "  \"has_retool_source_roots\": " << (result.has_retool_source_roots ? "true" : "false") << ",\n"
              << "  \"build_systems\": " << json_string_array(result.build_systems) << ",\n"
              << "  \"project_kinds\": " << json_string_array(result.project_kinds) << ",\n"
              << "  \"signals\": " << json_string_array(result.signals) << ",\n"
              << "  \"recommended_profiles\": " << json_string_array(result.recommended_profiles) << ",\n"
              << "  \"recommended_extractors\": " << json_string_array(result.recommended_extractors) << ",\n"
              << "  \"bootstrap\": {\"state\":" << json_string(result.bootstrap_state)
              << ",\"indexable\":" << (result.indexable ? "true" : "false")
              << ",\"source_only_supported\":" << (result.source_only_supported ? "true" : "false")
              << ",\"project_mutation_required\":" << (result.project_mutation_required ? "true" : "false")
              << ",\"primary_profile\":"
              << (result.primary_profile.empty() ? "null" : json_string(result.primary_profile))
              << ",\"profile_mode\":\"bootstrap\""
              << ",\"next_steps\":" << json_string_array(result.next_steps) << "},\n"
              << "  \"warnings\": " << json_string_array(result.warnings) << ",\n"
              << "  \"counts\": {"
              << "\"markdown_files\":" << result.markdown_files
              << ",\"json_files\":" << result.json_files
              << ",\"yaml_files\":" << result.yaml_files
              << ",\"pom_files\":" << result.pom_files
              << ",\"gradle_files\":" << result.gradle_files
              << ",\"package_json_files\":" << result.package_json_files
              << ",\"dotnet_solution_files\":" << result.dotnet_solution_files
              << ",\"csproj_files\":" << result.csproj_files
              << ",\"java_files\":" << result.java_files
              << ",\"kotlin_files\":" << result.kotlin_files
              << ",\"csharp_files\":" << result.csharp_files
              << ",\"cpp_files\":" << result.cpp_files
              << ",\"ts_files\":" << result.ts_files
              << ",\"rsx_files\":" << result.rsx_files
              << ",\"retool_workflow_files\":" << result.retool_workflow_files
              << ",\"retool_resource_files\":" << result.retool_resource_files
              << ",\"docs_files\":" << result.docs_files
              << ",\"indexable_docs_files\":" << result.indexable_docs_files
              << ",\"generated_kb_files\":" << result.generated_kb_files
              << ",\"profile_files\":" << result.profile_files
              << ",\"max_depth_scanned\":" << result.max_depth_scanned
              << "}\n"
              << "}\n";
    return 0;
}

static int run_help() {
    Json output = make_result_document("shin.help.v1", "help", true);
    output["global_options"] = {
        "--profile ID", "--profile-dir PATH", "--root PATH", "--out-dir PATH",
        "--from-index", "--source-scan", "--help", "--version"
    };
    output["commands"] = Json::array({
        {{"name", "help"}, {"arguments", ""}},
        {{"name", "version"}, {"arguments", ""}},
        {{"name", "discover"}, {"arguments", ""}},
        {{"name", "build"}, {"arguments", ""}},
        {{"name", "check"}, {"arguments", ""}},
        {{"name", "search"}, {"arguments", "QUERY [--limit N] [--require-current]"}},
        {{"name", "context"}, {"arguments", "QUERY [--limit N] [--per-doc N] [--require-current]"}},
        {{"name", "brief"}, {"arguments", "QUERY [--limit N] [--per-doc N] [--projection full|compact] [--require-current]"}},
        {{"name", "inspect"}, {"arguments", "PATH [--require-current]"}},
        {{"name", "read"}, {"arguments", "--input FILE|-"}},
        {{"name", "pack"}, {"arguments", "--input FILE [--jobs N] [--projection full|compact] [--telemetry] [--require-current] [--strict]"}},
        {{"name", "pack-diff"}, {"arguments", "--from OUT_DIR --to OUT_DIR --input FILE [--jobs N] [--limit N] [--require-current] [--strict]"}},
        {{"name", "pack-bench"}, {"arguments", "--input FILE [--jobs N] [--projection full|compact] [--iterations N] [--warmup N] [--require-current] [--strict]"}},
        {{"name", "pack-eval"}, {"arguments", "[--failed-only] [--require-current] [--strict]"}},
        {{"name", "batch"}, {"arguments", "--input FILE [--jobs N] [--require-current]"}},
        {{"name", "diagnose"}, {"arguments", "QUERY --expected-path PATH [--limit N] [--per-doc N] [--require-current] [--strict]"}},
        {{"name", "worktree-diff"}, {"arguments", "[--limit N] [--strict]"}},
        {{"name", "impact"}, {"arguments", "[--limit N] [--depth N] [--max-evidence-bytes N] [--receipts BUNDLE --task-id ID --thread-id ID [--trusted-producer ID] [--observed-at UNIX_SECONDS]] [--require-current] [--strict]"}},
        {{"name", "receipt-scope"}, {"arguments", "--input REQUEST|- --task-id ID --thread-id ID [--observed-at UNIX_SECONDS] [--require-current]"}},
        {{"name", "receipt-import"}, {"arguments", "--input REQUEST|- --task-id ID --thread-id ID [--observed-at UNIX_SECONDS] [--require-current]"}},
        {{"name", "receipt-check"}, {"arguments", "--input BUNDLE --task-id ID --thread-id ID [--observed-at UNIX_SECONDS] [--trusted-producer ID] [--require-current] [--strict]"}},
        {{"name", "checkpoint"}, {"arguments", "--input FILE|- --task-id ID --thread-id ID [--observed-at UNIX_SECONDS] [--jobs N] [--require-current]"}},
        {{"name", "resume"}, {"arguments", "--checkpoint FILE --task-id ID --thread-id ID [--task TEXT] [--observed-at UNIX_SECONDS] [--trusted-producer ID] [--jobs N] [--require-current]"}},
        {{"name", "snapshot-diff"}, {"arguments", "--from OUT_DIR --to OUT_DIR [--limit N] [--strict]"}},
        {{"name", "federated-brief"}, {"arguments", "--config FILE QUERY [--limit N] [--per-doc N] [--require-current]"}},
        {{"name", "federated-eval"}, {"arguments", "--config FILE --suite FILE [--strict] [--require-current]"}},
        {{"name", "suggest-profile"}, {"arguments", "--config FILE QUERY [--from-root PATH] [--limit N]"}},
        {{"name", "index-list"}, {"arguments", "--cache-dir PATH [--strict]"}},
        {{"name", "index-prune"}, {"arguments", "--cache-dir PATH [--keep-key KEY] [--prune-unreferenced] [--apply] [--strict]"}},
        {{"name", "bench"}, {"arguments", "QUERY [--iterations N] [--build-iterations N] [--warmup N]"}},
        {{"name", "eval"}, {"arguments", "[--failed-only] [--strict] [--require-current] [--limit N] [--per-doc N]"}},
        {{"name", "adoption-plan"}, {"arguments", "[--marker FILE] [--agent-entry FILE]"}},
        {{"name", "adoption-check"}, {"arguments", "--marker FILE [--strict]"}},
        {{"name", "profile-check"}, {"arguments", "[--strict]"}},
        {{"name", "profile-policy"}, {"arguments", "[--expect-profile-hash HASH]"}},
        {{"name", "validate"}, {"arguments", "[--marker FILE] [--require-current] [--summary] [--strict]"}},
        {{"name", "ready"}, {"arguments", "[--marker FILE] [--routing-config FILE --task QUERY] [--limit N] [--strict] [--verbose]"}},
        {{"name", "doctor"}, {"arguments", "[--strict]"}},
        {{"name", "vacuum"}, {"arguments", "[--apply] [--strict]"}}
    });
    emit_json_document(std::cout, output);
    return 0;
}

static int run_version() {
    Json output = make_result_document("shin.version.v1", "version", true);
    output["version"] = kProductVersion;
    output["generator_version"] = kGeneratorVersion;
    output["stability"] = kProductVersion.find('-') == std::string_view::npos ? "stable" : "development";
    output["generated_json_decoder"] = {
        {"library", "simdjson"},
        {"version", generated_row_decoder_version()},
        {"api", "dom"},
        {"implementation", generated_row_decoder_implementation()},
        {"scope", "generated-jsonl-only"}
    };
    emit_json_document(std::cout, output);
    return 0;
}

static bool is_known_command(const std::string& command) {
    static const std::set<std::string> commands = {
        "help", "version", "discover", "build", "check", "search", "context", "brief", "inspect", "read", "pack", "pack-diff", "pack-bench", "pack-eval", "batch", "diagnose", "worktree-diff",
        "snapshot-diff", "checkpoint", "resume", "impact", "federated-brief",
        "federated-eval", "suggest-profile",
        "index-list", "index-prune", "bench", "eval", "adoption-plan", "adoption-check",
        "profile-check", "profile-policy", "validate", "ready", "doctor", "vacuum", "receipt-scope", "receipt-import", "receipt-check"
    };
    return commands.find(command) != commands.end();
}

static void require_no_command_args(const std::vector<std::string>& args, const std::string& command) {
    if (args.size() != 1) {
        throw_usage_error("unexpected-arguments", command + " does not accept command arguments.");
    }
}

int run(int argc, char** argv) {
    if (argc < 2) {
        throw_usage_error("missing-command", "A command is required.");
    }
    RuntimeOptions options;
    std::string profile_id = "shin";
    fs::path profile_dir = "profiles";
    bool profile_set = false;
    bool profile_dir_set = false;
    bool root_set = false;
    bool out_root_set = false;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    std::vector<std::string> command_args;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--profile") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--profile requires a value.");
            }
            profile_id = args[++i];
            profile_set = true;
            continue;
        }
        if (args[i] == "--profile-dir") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--profile-dir requires a value.");
            }
            profile_dir = fs::path(args[++i]);
            profile_dir_set = true;
            continue;
        }
        if (args[i] == "--root") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--root requires a value.");
            }
            options.root = fs::absolute(args[++i]);
            root_set = true;
            continue;
        }
        if (args[i] == "--out-dir") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--out-dir requires a value.");
            }
            options.out_root = fs::absolute(args[++i]);
            out_root_set = true;
            continue;
        }
        if (args[i] == "--from-index") {
            options.force_index = true;
            continue;
        }
        if (args[i] == "--source-scan") {
            options.force_source_scan = true;
            continue;
        }
        command_args.push_back(args[i]);
    }
    if (options.force_index && options.force_source_scan) {
        throw_usage_error("conflicting-options", "--from-index and --source-scan are mutually exclusive.");
    }
    args = command_args;
    if (!out_root_set) {
        options.out_root = options.root;
    }
    if (args.empty()) {
        throw_usage_error("missing-command", "A command is required.");
    }
    if (args[0] == "--help") {
        args[0] = "help";
    }
    if (args[0] == "--version") {
        args[0] = "version";
    }
    if (args[0] == "kb") {
        if (args.size() < 2) {
            throw_usage_error("missing-command", "kb requires build or check.");
        }
        args.erase(args.begin());
        if (args[0] != "build" && args[0] != "check") {
            throw_usage_error("unsupported-command", "kb only supports build or check.");
        }
    }

    const std::string& command = args[0];
    if (!is_known_command(command)) {
        throw_usage_error("unknown-command", "Unknown command: " + command + ".");
    }
    if (command == "help") {
        require_no_command_args(args, command);
        return run_help();
    }
    if (command == "version") {
        require_no_command_args(args, command);
        return run_version();
    }
    if (command == "discover") {
        require_no_command_args(args, command);
        return run_discover(options);
    }
    if (command == "snapshot-diff") {
        if (profile_set || profile_dir_set || root_set || out_root_set ||
            options.force_index || options.force_source_scan) {
            throw_usage_error("unsupported-option",
                              "snapshot-diff takes both generated output locations from --from and --to.");
        }
        std::vector<std::string> diff_args(args.begin() + 1, args.end());
        return run_snapshot_diff(diff_args);
    }
    if (command == "federated-brief" || command == "federated-eval") {
        if (profile_set || profile_dir_set || root_set || out_root_set ||
            options.force_index || options.force_source_scan) {
            throw_usage_error("unsupported-option",
                              command + " takes profile, root, and index locations only from --config.");
        }
        std::vector<std::string> federated_args(args.begin() + 1, args.end());
        return command == "federated-brief" ? run_federated_brief(federated_args)
                                             : run_federated_eval(federated_args);
    }
    if (command == "suggest-profile") {
        if (profile_set || profile_dir_set || root_set || out_root_set ||
            options.force_index || options.force_source_scan) {
            throw_usage_error(
                "unsupported-option",
                "suggest-profile takes profile and root candidates only from --config.");
        }
        std::vector<std::string> suggest_args(args.begin() + 1, args.end());
        return run_suggest_profile(suggest_args);
    }
    if (command == "index-list" || command == "index-prune") {
        if (profile_set || profile_dir_set || root_set || out_root_set ||
            options.force_index || options.force_source_scan) {
            throw_usage_error(
                "unsupported-option",
                command + " takes its dedicated cache root only from --cache-dir.");
        }
        std::vector<std::string> lifecycle_args(args.begin() + 1, args.end());
        return command == "index-list" ? run_index_list(lifecycle_args)
                                         : run_index_prune(lifecycle_args);
    }
    if (command == "adoption-check") {
        if (profile_set || out_root_set || options.force_index || options.force_source_scan) {
            throw_usage_error("unsupported-option",
                              "adoption-check reads its profile id from the marker and does not accept "
                              "--profile, --out-dir, --from-index, or --source-scan.");
        }
        std::vector<std::string> adoption_args(args.begin() + 1, args.end());
        return run_adoption_check(options.root, profile_dir, adoption_args);
    }
    if (command == "adoption-plan") {
        if (out_root_set || options.force_index || options.force_source_scan) {
            throw_usage_error("unsupported-option",
                              "adoption-plan does not accept --out-dir, --from-index, or --source-scan.");
        }
        std::vector<std::string> adoption_plan_args(args.begin() + 1, args.end());
        const std::optional<std::string> candidate_profile =
            profile_set ? std::optional<std::string>(profile_id) : std::nullopt;
        return run_adoption_plan(options.root, profile_dir, candidate_profile, adoption_plan_args);
    }
    if (command == "pack-diff" && out_root_set) {
        throw_usage_error("unsupported-option",
                          "pack-diff takes generated output locations from --from and --to.");
    }
    if ((options.force_index || options.force_source_scan) &&
        command != "search" && command != "context" && command != "brief") {
        throw_usage_error("unsupported-option",
                          "--from-index and --source-scan are only valid for search, context, or brief.");
    }
    options.profile_dir = fs::absolute(profile_dir).lexically_normal();
    options.profile = make_profile(profile_id, profile_dir);

    if (command == "profile-policy") {
        return run_profile_policy(options, {args.begin() + 1, args.end()});
    }

    std::unique_ptr<shin::generated_io::IndexLock> index_lock;
    const bool vacuum_apply = command == "vacuum" &&
                              std::find(args.begin() + 1, args.end(), "--apply") != args.end();
    const bool reads_generated_index =
        command == "check" || command == "bench" ||
        command == "doctor" || command == "vacuum" || command == "validate" ||
        command == "worktree-diff";
    if (reads_generated_index && !vacuum_apply && !(options.force_source_scan &&
                                   (command == "search" || command == "context" || command == "brief"))) {
        index_lock = lock_index_for_read(options.out_root);
    }

    if (command == "build") {
        require_no_command_args(args, command);
        return run_build(options);
    }
    if (command == "check") {
        require_no_command_args(args, command);
        return run_check(options);
    }
    if (command == "search") {
        std::vector<std::string> search_args(args.begin() + 1, args.end());
        return run_search(options, search_args);
    }
    if (command == "context") {
        std::vector<std::string> context_args(args.begin() + 1, args.end());
        return run_context(options, context_args);
    }
    if (command == "brief") {
        std::vector<std::string> brief_args(args.begin() + 1, args.end());
        return run_brief(options, brief_args);
    }
    if (command == "inspect") {
        return run_inspect(options, {args.begin() + 1, args.end()});
    }
    if (command == "read") {
        return run_read(options, {args.begin() + 1, args.end()});
    }
    if (command == "pack") {
        std::vector<std::string> pack_args(args.begin() + 1, args.end());
        return run_pack(options, pack_args);
    }
    if (command == "pack-diff") {
        std::vector<std::string> pack_diff_args(args.begin() + 1, args.end());
        return run_pack_diff(options, pack_diff_args);
    }
    if (command == "pack-bench") {
        std::vector<std::string> pack_bench_args(args.begin() + 1, args.end());
        return run_pack_bench(options, pack_bench_args);
    }
    if (command == "pack-eval") {
        std::vector<std::string> pack_eval_args(args.begin() + 1, args.end());
        return run_pack_eval(options, pack_eval_args);
    }
    if (command == "batch") {
        std::vector<std::string> batch_args(args.begin() + 1, args.end());
        return run_batch(options, batch_args);
    }
    if (command == "diagnose") {
        std::vector<std::string> diagnose_args(args.begin() + 1, args.end());
        return run_diagnose(options, diagnose_args);
    }
    if (command == "worktree-diff") {
        std::vector<std::string> worktree_diff_args(args.begin() + 1, args.end());
        return run_worktree_diff(options, worktree_diff_args);
    }
    if (command == "impact") {
        return run_impact(options, {args.begin() + 1, args.end()});
    }
    if (command == "receipt-scope" || command == "receipt-import" || command == "receipt-check") {
        return run_execution_evidence(options, {args.begin() + 1, args.end()}, command);
    }
    if (command == "checkpoint" || command == "resume") {
        const std::vector<std::string> session_args(args.begin() + 1, args.end());
        return command == "checkpoint" ? run_checkpoint(options, session_args)
                                         : run_resume(options, session_args);
    }
    if (command == "bench") {
        std::vector<std::string> bench_args(args.begin() + 1, args.end());
        return run_bench(options, bench_args);
    }
    if (command == "eval") {
        std::vector<std::string> eval_args(args.begin() + 1, args.end());
        return run_eval(options, eval_args);
    }
    if (command == "profile-check") {
        std::vector<std::string> profile_check_args(args.begin() + 1, args.end());
        return run_profile_check(options, profile_check_args);
    }
    if (command == "validate") {
        std::vector<std::string> validate_args(args.begin() + 1, args.end());
        return run_validate(options, profile_dir, validate_args);
    }
    if (command == "ready") {
        std::vector<std::string> ready_args(args.begin() + 1, args.end());
        return run_ready(options, profile_dir, ready_args);
    }
    if (command == "doctor") {
        std::vector<std::string> doctor_args(args.begin() + 1, args.end());
        return run_doctor(options, doctor_args);
    }
    if (command == "vacuum") {
        std::vector<std::string> vacuum_args(args.begin() + 1, args.end());
        return run_vacuum(options, vacuum_args);
    }
    throw CliError("command-dispatch-failed", "internal", "Command dispatch failed.", 1, false,
                   "Report the command and arguments as a Shin Context Engine defect.");
}

std::string detect_command_name(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--profile" || arg == "--profile-dir" || arg == "--root" || arg == "--out-dir") {
            ++index;
            continue;
        }
        if (arg == "--from-index" || arg == "--source-scan") {
            continue;
        }
        if (arg == "--help") {
            return "help";
        }
        if (arg == "--version") {
            return "version";
        }
        if (arg == "kb") {
            return index + 1 < argc ? argv[index + 1] : "kb";
        }
        return arg;
    }
    return "unknown";
}

static void emit_error_result(std::string_view command,
                       std::string_view code,
                       std::string_view category,
                       std::string_view message,
                       int exit_code,
                       bool retryable,
                       std::string_view remediation,
                       const Json& diagnostic) {
    Json output = make_result_document("shin.error.v1",
                                       command.empty() ? "unknown" : std::string(command), false);
    output["error"] = {
        {"code", std::string(code)},
        {"category", std::string(category)},
        {"message", std::string(message)},
        {"exit_code", exit_code},
        {"retryable", retryable},
        {"remediation", std::string(remediation)}
    };
    if (!diagnostic.is_null()) output["error"]["diagnostic"] = diagnostic;
    emit_json_document(std::cerr, output);
}

void emit_error_result_noexcept(std::string_view command,
                                std::string_view code,
                                std::string_view category,
                                std::string_view message,
                                int exit_code,
                                bool retryable,
                                std::string_view remediation,
                                const Json& diagnostic) noexcept {
    try {
        emit_error_result(command, code, category, message, exit_code, retryable, remediation, diagnostic);
    } catch (...) {
        std::fputs("{\"command\":\"unknown\",\"error\":{\"category\":\"internal\","
                   "\"code\":\"error-envelope-failed\",\"exit_code\":1,"
                   "\"message\":\"Failed to allocate the complete error envelope.\","
                   "\"remediation\":\"Reduce resource pressure and retry.\",\"retryable\":true},"
                   "\"ok\":false,\"schema_version\":\"shin.error.v1\",\"tool\":\"shin-context\"}\n",
                   stderr);
    }
}

} // namespace shin::context
