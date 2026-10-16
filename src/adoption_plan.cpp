#include "context_internal.h"

namespace shin::context {
namespace {

constexpr std::string_view kCanonicalMarker = "docs/machine/shin-project-adoption-v1.json";
constexpr std::string_view kCanonicalAgentEntry = "AGENTS.md";

struct AdoptionPlanOptions {
    std::string marker;
    std::string agent_entry;
    bool marker_explicit = false;
    bool agent_entry_explicit = false;
};

struct RelativeFileProbe {
    std::string status;
    bool present = false;
    bool regular = false;
};

AdoptionPlanOptions parse_adoption_plan_options(const std::vector<std::string>& args) {
    AdoptionPlanOptions options;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto read_path = [&](std::string& target, bool& seen, const std::string& option) {
            if (seen) {
                throw_usage_error("duplicate-option", "adoption-plan accepts " + option + " only once.");
            }
            if (index + 1 >= args.size()) {
                throw_usage_error("missing-option-value", option + " requires a file path.");
            }
            target = args[++index];
            seen = true;
        };
        if (arg == "--marker") {
            read_path(options.marker, options.marker_explicit, arg);
            continue;
        }
        if (arg == "--agent-entry") {
            read_path(options.agent_entry, options.agent_entry_explicit, arg);
            continue;
        }
        throw_usage_error("unsupported-option", "Unsupported adoption-plan option: " + arg);
    }
    return options;
}

RelativeFileProbe probe_relative_file(const fs::path& root,
                                      const std::string& path,
                                      std::string_view kind) {
    if (!profile_relative_path_valid(path)) {
        throw_usage_error("invalid-" + std::string(kind) + "-path",
                          "adoption-plan requires a canonical repository-relative " +
                              std::string(kind) + " path.");
    }
    if (profile_path_contains_symlink(root, path)) {
        throw CliError("unsafe-" + std::string(kind), "configuration",
                       "adoption-plan refuses a " + std::string(kind) +
                           " path that traverses a symlink: " + path,
                       1, false, "Use a regular non-symlink file below --root.");
    }

    std::error_code error;
    const fs::file_status status = fs::symlink_status(root / path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return {"missing", false, false};
    }
    if (error) {
        throw CliError(std::string(kind) + "-probe-failed", "configuration",
                       "adoption-plan could not inspect the selected " + std::string(kind) +
                           " path: " + path + " (" + error.message() + ")",
                       1, false, "Use a canonical path below --root with traversable regular ancestors.");
    }
    if (!fs::exists(status)) {
        return {"missing", false, false};
    }
    if (fs::is_symlink(status) || !fs::is_regular_file(status)) {
        throw CliError("unsafe-" + std::string(kind), "configuration",
                       "adoption-plan requires the selected " + std::string(kind) +
                           " to be a regular non-symlink file: " + path,
                       1, false, "Use a regular non-symlink file below --root.");
    }
    return {"regular", true, true};
}

std::pair<std::string, RelativeFileProbe> select_marker(const fs::path& root,
                                                        const AdoptionPlanOptions& options) {
    if (options.marker_explicit) {
        return {options.marker, probe_relative_file(root, options.marker, "marker")};
    }
    static const std::array<std::string_view, 4> candidates = {
        kCanonicalMarker,
        "docs/machine/shin-context-engine-adoption-v1.json",
        "docs/shin-context-engine-adoption.json",
        "docs/architecture/shin-context-engine-adoption.json"
    };
    for (const std::string_view candidate : candidates) {
        RelativeFileProbe probe = probe_relative_file(root, std::string(candidate), "marker");
        if (probe.present) {
            return {std::string(candidate), std::move(probe)};
        }
    }
    return {std::string(kCanonicalMarker), {"missing", false, false}};
}

Json profile_check_counts_json(const ProfileCheckCounts& counts) {
    return {
        {"root_files", counts.root_files},
        {"optional_root_files", counts.optional_root_files},
        {"source_extensions", counts.source_extensions},
        {"source_fact_roots", counts.source_fact_roots},
        {"source_fact_extensions", counts.source_fact_extensions},
        {"source_fact_extractors", counts.source_fact_extractors},
        {"document_include_prefixes", counts.document_include_prefixes},
        {"document_exclude_prefixes", counts.document_exclude_prefixes},
        {"json_key_include_prefixes", counts.json_key_include_prefixes},
        {"secret_scan_exclude_prefixes", counts.secret_scan_exclude_prefixes},
        {"agent_start_paths", counts.agent_start_paths},
        {"topic_rules", counts.topic_rules},
        {"aliases", counts.aliases},
        {"eval_cases", counts.eval_cases},
        {"pack_eval_cases", counts.pack_eval_cases},
        {"errors", counts.errors},
        {"warnings", counts.warnings},
        {"infos", counts.infos}
    };
}

bool issue_resolved_by_marker_creation(const DoctorIssue& issue, const std::string& marker) {
    static const std::set<std::string> missing_path_issues = {
        "missing-root-file",
        "missing-agent-start-path",
        "missing-eval-must-path",
        "missing-eval-must-top-path",
        "missing-eval-rank-path"
    };
    return issue.path == marker && missing_path_issues.contains(issue.id);
}

Json profile_check_json(const ProfileCheckResult& result, const std::string& marker) {
    Json issues = Json::array();
    for (const DoctorIssue& issue : result.issues) {
        issues.push_back({
            {"severity", issue.severity},
            {"code", issue.id},
            {"path", issue.path},
            {"summary", issue.summary},
            {"resolved_by_marker_creation_candidate",
             issue_resolved_by_marker_creation(issue, marker)}
        });
    }
    return {
        {"strict", result.strict},
        {"ok", result.counts.errors == 0 && result.counts.warnings == 0},
        {"counts", profile_check_counts_json(result.counts)},
        {"issues", std::move(issues)}
    };
}

bool profile_ready_after_marker_creation(const ProfileCheckResult& result,
                                         const std::string& marker) {
    return std::none_of(result.issues.begin(), result.issues.end(), [&](const DoctorIssue& issue) {
        if (issue.severity != "error" && issue.severity != "warning") {
            return false;
        }
        return !issue_resolved_by_marker_creation(issue, marker);
    });
}

void add_gap(Json& gaps,
             std::string code,
             std::string stage,
             std::string path,
             std::string summary) {
    gaps.push_back({
        {"code", std::move(code)},
        {"stage", std::move(stage)},
        {"path", std::move(path)},
        {"summary", std::move(summary)}
    });
}

Json command_json(std::string id, std::string stage, std::vector<std::string> argv) {
    return {
        {"id", std::move(id)},
        {"stage", std::move(stage)},
        {"argv", std::move(argv)},
        {"writes_consumer", false}
    };
}

std::vector<std::string> profile_command_prefix(const fs::path& root,
                                                const fs::path& profile_dir,
                                                const std::string& profile_id) {
    return {
        "shin-context",
        "--profile", profile_id,
        "--profile-dir", fs::absolute(profile_dir).lexically_normal().generic_string(),
        "--root", root.lexically_normal().generic_string()
    };
}

void append_profile_command(Json& commands,
                            const fs::path& root,
                            const fs::path& profile_dir,
                            const std::string& profile_id,
                            const std::string& id,
                            const std::string& stage,
                            bool use_out_dir,
                            std::vector<std::string> tail) {
    std::vector<std::string> argv = profile_command_prefix(root, profile_dir, profile_id);
    if (use_out_dir) {
        argv.insert(argv.end(), {"--out-dir", "<EXTERNAL_OUTPUT_BASE>/<INDEX_KEY>"});
    }
    argv.insert(argv.end(), std::make_move_iterator(tail.begin()),
                std::make_move_iterator(tail.end()));
    commands.push_back(command_json(id, stage, std::move(argv)));
}

void append_bootstrap_commands(Json& commands,
                               const fs::path& root,
                               const fs::path& profile_dir,
                               const std::string& profile_id) {
    append_profile_command(commands, root, profile_dir, profile_id,
                           "bootstrap-profile-check", "bootstrap", false,
                           {"profile-check", "--strict"});
    append_profile_command(commands, root, profile_dir, profile_id,
                           "bootstrap-build", "bootstrap", true, {"build"});
    append_profile_command(commands, root, profile_dir, profile_id,
                           "bootstrap-check", "bootstrap", true, {"check"});
    append_profile_command(commands, root, profile_dir, profile_id,
                           "bootstrap-doctor", "bootstrap", true, {"doctor"});
    append_profile_command(commands, root, profile_dir, profile_id,
                           "bootstrap-brief", "bootstrap", true,
                           {"brief", "<TASK_QUERY>", "--require-current"});
}

void append_permanent_commands(Json& commands,
                               const fs::path& root,
                               const fs::path& profile_dir,
                               const std::string& profile_id) {
    append_profile_command(commands, root, profile_dir, profile_id,
                           "permanent-profile-check", "promotion", false,
                           {"profile-check", "--strict"});
    append_profile_command(commands, root, profile_dir, profile_id,
                           "permanent-build", "promotion", true, {"build"});
    append_profile_command(commands, root, profile_dir, profile_id,
                           "permanent-check", "promotion", true, {"check"});
    append_profile_command(commands, root, profile_dir, profile_id,
                           "permanent-doctor", "promotion", true,
                           {"doctor", "--strict"});
    append_profile_command(commands, root, profile_dir, profile_id,
                           "permanent-eval", "promotion", true,
                           {"eval", "--strict"});
    append_profile_command(commands, root, profile_dir, profile_id,
                           "permanent-pack-eval", "promotion", true,
                           {"pack-eval", "--strict"});
}

Json discovery_json(const DiscoveryResult& discovery) {
    return {
        {"state", discovery.bootstrap_state},
        {"indexable", discovery.indexable},
        {"source_only_supported", discovery.source_only_supported},
        {"project_mutation_required", discovery.project_mutation_required},
        {"primary_profile",
         discovery.primary_profile.empty() ? Json(nullptr) : Json(discovery.primary_profile)},
        {"recommended_profiles", discovery.recommended_profiles},
        {"project_kinds", discovery.project_kinds},
        {"build_systems", discovery.build_systems},
        {"signals", discovery.signals},
        {"warnings", discovery.warnings}
    };
}

} // namespace

std::optional<std::string> find_existing_adoption_marker(const fs::path& root) {
    const auto [path, probe] = select_marker(root, AdoptionPlanOptions{});
    return probe.present ? std::optional<std::string>(path) : std::nullopt;
}

int run_adoption_plan(const fs::path& root,
                      const fs::path& profile_dir,
                      const std::optional<std::string>& candidate_profile_id,
                      const std::vector<std::string>& args) {
    AdoptionPlanOptions options = parse_adoption_plan_options(args);
    if (options.marker_explicit) {
        options.marker = normalize_repository_file_argument(root, options.marker, "--marker");
    }
    const DiscoveryResult discovery = discover_project(root);
    const auto [marker_path, marker_probe] = select_marker(root, options);
    const std::string agent_entry = options.agent_entry_explicit
                                        ? options.agent_entry
                                        : std::string(kCanonicalAgentEntry);
    const RelativeFileProbe agent_probe = probe_relative_file(root, agent_entry, "agent-entry");

    Json gaps = Json::array();
    Json commands = Json::array();
    Json next_actions = Json::array();
    Json bootstrap = {
        {"profile", discovery.primary_profile.empty() ? Json(nullptr) : Json(discovery.primary_profile)},
        {"available", false},
        {"valid", false},
        {"profile_check", nullptr}
    };

    bool bootstrap_ready = false;
    if (discovery.indexable && !discovery.primary_profile.empty()) {
        try {
            Profile profile = make_profile(discovery.primary_profile, profile_dir);
            RuntimeOptions profile_options;
            profile_options.root = root;
            profile_options.out_root = root;
            profile_options.profile = profile;
            const ProfileCheckResult profile_check = validate_profile(profile_options, true);
            const bool valid = profile.bootstrap_only && profile_check.counts.errors == 0 &&
                               profile_check.counts.warnings == 0;
            bootstrap = {
                {"profile", profile.id},
                {"available", true},
                {"bootstrap_only", profile.bootstrap_only},
                {"valid", valid},
                {"profile_check", profile_check_json(profile_check, marker_path)}
            };
            bootstrap_ready = valid;
            append_bootstrap_commands(commands, root, profile_dir, profile.id);
            if (!profile.bootstrap_only) {
                add_gap(gaps, "discovery-profile-not-bootstrap", "bootstrap", profile.id,
                        "The discovery-selected profile must be bootstrapOnly.");
            }
            if (!valid) {
                add_gap(gaps, "bootstrap-profile-invalid", "bootstrap", profile.id,
                        "The discovery-selected bootstrap profile does not pass strict profile validation.");
            }
        } catch (const std::exception& error) {
            add_gap(gaps, "bootstrap-profile-unavailable", "bootstrap", discovery.primary_profile,
                    error.what());
        }
    }

    Json candidate = nullptr;
    bool marker_authoring_ready = false;
    if (candidate_profile_id.has_value()) {
        const std::string& profile_id = *candidate_profile_id;
        if (!portable_identifier_valid(profile_id)) {
            throw_usage_error("invalid-profile-id",
                              "adoption-plan requires a lowercase portable profile identifier.");
        }
        try {
            Profile profile = make_profile(profile_id, profile_dir);
            RuntimeOptions profile_options;
            profile_options.root = root;
            profile_options.out_root = root;
            profile_options.profile = profile;
            const ProfileCheckResult profile_check = validate_profile(profile_options, true);
            const bool marker_registered = std::find(profile.agent_start_paths.begin(),
                                                     profile.agent_start_paths.end(), marker_path) !=
                                           profile.agent_start_paths.end();
            const bool agent_registered = std::find(profile.agent_start_paths.begin(),
                                                    profile.agent_start_paths.end(), agent_entry) !=
                                          profile.agent_start_paths.end();
            const bool permanent = !profile.bootstrap_only;
            const bool has_eval = !profile.eval_cases.empty();
            const bool has_pack_eval = !profile.pack_eval_cases.empty();
            const bool ready_after_marker = profile_ready_after_marker_creation(profile_check, marker_path);
            marker_authoring_ready = !marker_probe.present && permanent && has_eval && has_pack_eval &&
                                     marker_registered && agent_registered && agent_probe.regular &&
                                     ready_after_marker;

            candidate = {
                {"id", profile.id},
                {"available", true},
                {"bootstrap_only", profile.bootstrap_only},
                {"eval_cases", profile.eval_cases.size()},
                {"pack_eval_cases", profile.pack_eval_cases.size()},
                {"marker_registered", marker_registered},
                {"agent_entry_registered", agent_registered},
                {"strict_profile_valid_now",
                 profile_check.counts.errors == 0 && profile_check.counts.warnings == 0},
                {"strict_profile_valid_after_marker_creation", ready_after_marker},
                {"ready_for_marker_authoring", marker_authoring_ready},
                {"profile_check", profile_check_json(profile_check, marker_path)}
            };
            append_permanent_commands(commands, root, profile_dir, profile.id);

            if (!permanent) {
                add_gap(gaps, "bootstrap-profile-not-permanent", "promotion", profile.id,
                        "A bootstrapOnly profile cannot become permanent adoption authority.");
            }
            if (!has_eval) {
                add_gap(gaps, "permanent-profile-eval-missing", "promotion", profile.id,
                        "Permanent adoption requires at least one reviewed evalCases entry.");
            }
            if (!has_pack_eval) {
                add_gap(gaps, "permanent-profile-pack-eval-missing", "promotion", profile.id,
                        "Permanent adoption requires at least one reviewed packEvalCases entry.");
            }
            if (!marker_registered) {
                add_gap(gaps, "profile-adoption-marker-missing", "promotion", marker_path,
                        "The permanent profile must register the selected marker in agentStartPaths.");
            }
            if (!agent_registered) {
                add_gap(gaps, "profile-agent-entry-missing", "promotion", agent_entry,
                        "The permanent profile must register the selected agent entry in agentStartPaths.");
            }
            if (!agent_probe.regular) {
                add_gap(gaps, "agent-entry-missing", "promotion", agent_entry,
                        "Create and review a regular agent entry before permanent adoption.");
            }
            if (!ready_after_marker) {
                add_gap(gaps, "permanent-profile-invalid", "promotion", profile.id,
                        "The candidate profile has strict validation issues not resolved by creating the marker.");
            }
        } catch (const CliError& error) {
            candidate = {
                {"id", profile_id},
                {"available", false},
                {"error", {{"code", error.code()}, {"message", error.what()}}}
            };
            add_gap(gaps, "candidate-profile-unavailable", "promotion", profile_id, error.what());
        } catch (const std::exception& error) {
            candidate = {
                {"id", profile_id},
                {"available", false},
                {"error", {{"code", "profile-load-failed"}, {"message", error.what()}}}
            };
            add_gap(gaps, "candidate-profile-unavailable", "promotion", profile_id, error.what());
        }
    }

    std::vector<std::string> adoption_argv = {
        "shin-context",
        "--profile-dir", fs::absolute(profile_dir).lexically_normal().generic_string(),
        "--root", root.lexically_normal().generic_string(),
        "adoption-check", "--marker", marker_path, "--strict"
    };
    commands.push_back(command_json("adoption-check", "adoption", std::move(adoption_argv)));

    std::string state;
    if (marker_probe.regular) {
        state = "adoption-check-ready";
        next_actions.push_back("run-adoption-check");
    } else if (!discovery.indexable) {
        state = "not-indexable";
        add_gap(gaps, "no-indexable-content", "bootstrap", ".",
                "Add source code or machine documentation before bootstrap.");
        next_actions.push_back("add-indexable-source-or-machine-docs");
    } else if (!candidate_profile_id.has_value()) {
        state = bootstrap_ready ? "bootstrap-ready" : "blocked";
        if (bootstrap_ready) {
            next_actions.push_back("run-bootstrap-route");
            next_actions.push_back("review-permanent-profile-policy");
        } else {
            next_actions.push_back("resolve-bootstrap-profile-gaps");
        }
    } else if (marker_authoring_ready) {
        state = "marker-authoring-ready";
        next_actions.push_back("review-and-create-adoption-marker");
        next_actions.push_back("run-adoption-check");
        next_actions.push_back("run-permanent-quality-gates");
    } else {
        state = "promotion-incomplete";
        next_actions.push_back("resolve-promotion-gaps");
    }

    Json output = make_result_document("shin.adoption-plan.v1", "adoption-plan", true);
    output["state"] = state;
    output["root"] = root.lexically_normal().generic_string();
    output["profile_registry"] = fs::absolute(profile_dir).lexically_normal().generic_string();
    output["deterministic"] = true;
    output["project_mutation_performed"] = false;
    output["discovery"] = discovery_json(discovery);
    output["marker"] = {
        {"path", marker_path},
        {"selection", options.marker_explicit
                          ? "explicit"
                          : (marker_probe.present ? "conventional-existing" : "canonical-proposal")},
        {"status", marker_probe.status},
        {"present", marker_probe.present},
        {"path_safe", true},
        {"safe_regular", marker_probe.regular}
    };
    output["agent"] = {
        {"entry_path", agent_entry},
        {"selection", options.agent_entry_explicit ? "explicit" : "canonical-proposal"},
        {"status", agent_probe.status},
        {"present", agent_probe.present},
        {"path_safe", true},
        {"safe_regular", agent_probe.regular}
    };
    output["bootstrap"] = std::move(bootstrap);
    output["candidate_profile"] = std::move(candidate);
    output["readiness"] = {
        {"bootstrap_ready", bootstrap_ready},
        {"marker_authoring_ready", marker_authoring_ready},
        {"adoption_check_ready", marker_probe.regular},
        {"adoption_validated", false}
    };
    output["external_output"] = {
        {"directory_template", "<EXTERNAL_OUTPUT_BASE>/<INDEX_KEY>"},
        {"bootstrap_key_source", "temporary-bootstrap-choice"},
        {"permanent_key_source", "marker.index.key"},
        {"consumer_writes_allowed", false}
    };
    output["review_requirements"] = {
        "document-and-source-roots",
        "bounded-file-and-structured-data-limits",
        "topic-alias-and-authority-policy",
        "agent-start-paths",
        "golden-eval-corpus",
        "representative-pack-eval-corpus",
        "location-independent-adoption-marker",
        "canonical-marker-index-key",
        "single-project-adoption-gate"
    };
    output["quality_gates"] = {
        "profile-check --strict",
        "build",
        "check",
        "doctor --strict",
        "eval --strict",
        "pack-eval --strict",
        "adoption-check --strict"
    };
    output["gaps"] = std::move(gaps);
    output["commands"] = std::move(commands);
    output["next_actions"] = std::move(next_actions);
    emit_json_document(std::cout, output);
    return 0;
}

} // namespace shin::context
