#include "context_internal.h"

namespace shin::context {
namespace {

struct AdoptionIssue {
    std::string severity;
    std::string code;
    std::string path;
    std::string message;
};

struct AdoptionDocument {
    std::string document;
    std::string status;
    std::string summary;
    std::string project_id;
    std::string context_epoch;
    std::string root_policy;
    std::string minimum_version;
    std::string generator_identity;
    std::string profile_id;
    std::string profile_registry;
    std::string profile_kind;
    bool generic_bootstrap_allowed = true;
    std::string index_ownership;
    std::string index_key;
    bool consumer_writes_allowed = true;
    bool require_current = false;
    bool profile_check_strict = false;
    bool check = false;
    bool doctor_strict = false;
    bool eval_strict = false;
    bool pack_eval_strict = false;
    std::string agent_entry_path;
    bool context_epoch_reference_required = false;
    bool marker_reference_required = false;
    bool profile_reference_required = false;
    std::size_t extension_count = 0;
};

struct AdoptionCommandOptions {
    std::string marker;
    bool strict = false;
};

void add_issue(std::vector<AdoptionIssue>& issues,
               std::string code,
               std::string path,
               std::string message,
               std::string severity = "error") {
    issues.push_back({std::move(severity), std::move(code), std::move(path), std::move(message)});
}

void validate_allowed_fields(const Json& object,
                             const std::set<std::string>& allowed,
                             const std::string& path,
                             std::vector<AdoptionIssue>& issues) {
    if (!object.is_object()) {
        return;
    }
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (!allowed.contains(field.key())) {
            add_issue(issues, "unknown-adoption-field", path + "." + field.key(),
                      "The adoption document contains an unknown field.");
        }
    }
}

const Json* required_object(const Json& parent,
                            const std::string& key,
                            const std::string& path,
                            std::vector<AdoptionIssue>& issues) {
    const Json* value = find_json_field(parent, key);
    if (value == nullptr) {
        add_issue(issues, "missing-adoption-field", path + "." + key,
                  "The adoption document is missing a required object.");
        return nullptr;
    }
    if (!value->is_object()) {
        add_issue(issues, "invalid-adoption-field-type", path + "." + key,
                  "The adoption field must be an object.");
        return nullptr;
    }
    return value;
}

std::string required_string(const Json& parent,
                            const std::string& key,
                            const std::string& path,
                            std::vector<AdoptionIssue>& issues) {
    const Json* value = find_json_field(parent, key);
    if (value == nullptr) {
        add_issue(issues, "missing-adoption-field", path + "." + key,
                  "The adoption document is missing a required string.");
        return {};
    }
    if (!value->is_string() || trim(value->get<std::string>()).empty()) {
        add_issue(issues, "invalid-adoption-field-type", path + "." + key,
                  "The adoption field must be a non-empty string.");
        return {};
    }
    return value->get<std::string>();
}

bool required_boolean(const Json& parent,
                      const std::string& key,
                      const std::string& path,
                      std::vector<AdoptionIssue>& issues) {
    const Json* value = find_json_field(parent, key);
    if (value == nullptr) {
        add_issue(issues, "missing-adoption-field", path + "." + key,
                  "The adoption document is missing a required boolean.");
        return false;
    }
    if (!value->is_boolean()) {
        add_issue(issues, "invalid-adoption-field-type", path + "." + key,
                  "The adoption field must be a boolean.");
        return false;
    }
    return value->get<bool>();
}

bool adoption_identifier_valid(const std::string& value) {
    return portable_identifier_valid(value);
}

bool adoption_extension_identifier_valid(const std::string& value) {
    if (!adoption_identifier_valid(value) || value.find('.') == std::string::npos) {
        return false;
    }
    std::size_t begin = 0;
    std::size_t components = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('.', begin);
        const std::string component = value.substr(begin, end - begin);
        if (!adoption_identifier_valid(component)) {
            return false;
        }
        ++components;
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return components >= 2;
}

std::optional<std::array<unsigned int, 3>> parse_release_version(const std::string& value) {
    std::array<unsigned int, 3> result{};
    std::size_t begin = 0;
    for (std::size_t index = 0; index < result.size(); ++index) {
        const std::size_t end = index + 1 == result.size() ? value.size() : value.find('.', begin);
        if (end == std::string::npos || end == begin) {
            return std::nullopt;
        }
        const std::string_view component(value.data() + static_cast<std::ptrdiff_t>(begin), end - begin);
        if (component.size() > 1 && component.front() == '0') {
            return std::nullopt;
        }
        unsigned int parsed_value = 0;
        const auto parsed = std::from_chars(component.data(), component.data() + component.size(), parsed_value);
        if (parsed.ec != std::errc{} || parsed.ptr != component.data() + component.size()) {
            return std::nullopt;
        }
        result[index] = parsed_value;
        begin = end + 1;
    }
    if (begin != value.size() + 1) {
        return std::nullopt;
    }
    return result;
}

struct RunningVersion {
    std::array<unsigned int, 3> release;
    bool prerelease = false;
};

std::optional<RunningVersion> parse_running_version(const std::string& value) {
    const std::size_t prerelease_begin = value.find('-');
    const std::string release = value.substr(0, prerelease_begin);
    const auto release_version = parse_release_version(release);
    if (!release_version.has_value()) {
        return std::nullopt;
    }
    if (prerelease_begin == std::string::npos) {
        return RunningVersion{*release_version, false};
    }

    constexpr std::string_view development_prefix = "-dev.";
    constexpr std::string_view candidate_prefix = "-rc.";
    const std::string_view prerelease(value.data() + static_cast<std::ptrdiff_t>(prerelease_begin),
                                      value.size() - prerelease_begin);
    const std::size_t prefix_size = prerelease.starts_with(development_prefix) ? development_prefix.size() :
                                    prerelease.starts_with(candidate_prefix) ? candidate_prefix.size() : 0;
    if (prefix_size == 0 || prerelease.size() == prefix_size) {
        return std::nullopt;
    }
    const std::string_view serial = prerelease.substr(prefix_size);
    if ((serial.size() > 1 && serial.front() == '0') ||
        !std::all_of(serial.begin(), serial.end(), [](unsigned char character) {
            return is_ascii_digit(character);
        })) {
        return std::nullopt;
    }
    return RunningVersion{*release_version, true};
}

bool running_version_at_least(const std::string& current, const std::string& minimum) {
    const auto current_version = parse_running_version(current);
    const auto minimum_version = parse_release_version(minimum);
    if (!current_version.has_value() || !minimum_version.has_value()) {
        return false;
    }
    if (current_version->release != *minimum_version) {
        return current_version->release > *minimum_version;
    }
    return !current_version->prerelease;
}

void require_exact_value(const std::string& actual,
                         const std::string& expected,
                         const std::string& path,
                         const std::string& code,
                         std::vector<AdoptionIssue>& issues) {
    if (!actual.empty() && actual != expected) {
        add_issue(issues, code, path, "Expected " + expected + ", got " + actual + ".");
    }
}

void require_true(bool actual,
                  const std::string& path,
                  const std::string& code,
                  std::vector<AdoptionIssue>& issues) {
    if (!actual) {
        add_issue(issues, code, path, "Permanent adoption requires this policy to be true.");
    }
}

void require_false(bool actual,
                   const std::string& path,
                   const std::string& code,
                   std::vector<AdoptionIssue>& issues) {
    if (actual) {
        add_issue(issues, code, path, "Permanent adoption requires this policy to be false.");
    }
}

void validate_identifier(const std::string& value,
                         const std::string& path,
                         std::vector<AdoptionIssue>& issues) {
    if (!value.empty() && !adoption_identifier_valid(value)) {
        add_issue(issues, "invalid-adoption-identifier", path,
                  "Identifiers must use lowercase ASCII letters, digits, dot, underscore, or hyphen.");
    }
}

AdoptionDocument parse_adoption_document(const Json& root,
                                         std::vector<AdoptionIssue>& issues) {
    AdoptionDocument adoption;
    if (!root.is_object()) {
        add_issue(issues, "invalid-adoption-document", "$", "The adoption document must be a JSON object.");
        return adoption;
    }

    validate_allowed_fields(root,
                            {"schemaVersion", "document", "status", "summary", "project", "engine",
                             "profile", "index", "qualityGates", "agent", "extensions"},
                            "$", issues);
    const std::string schema = required_string(root, "schemaVersion", "$", issues);
    adoption.document = required_string(root, "document", "$", issues);
    adoption.status = required_string(root, "status", "$", issues);
    adoption.summary = required_string(root, "summary", "$", issues);
    require_exact_value(schema, "shin.project-adoption.v1", "$.schemaVersion",
                        "unsupported-adoption-schema", issues);
    require_exact_value(adoption.status, "active", "$.status", "inactive-adoption-marker", issues);

    if (const Json* project = required_object(root, "project", "$", issues)) {
        validate_allowed_fields(*project, {"id", "contextEpoch", "rootPolicy"}, "$.project", issues);
        adoption.project_id = required_string(*project, "id", "$.project", issues);
        adoption.context_epoch = required_string(*project, "contextEpoch", "$.project", issues);
        adoption.root_policy = required_string(*project, "rootPolicy", "$.project", issues);
        validate_identifier(adoption.project_id, "$.project.id", issues);
        validate_identifier(adoption.context_epoch, "$.project.contextEpoch", issues);
        require_exact_value(adoption.root_policy, "cli-root", "$.project.rootPolicy",
                            "unsupported-adoption-root-policy", issues);
    }

    if (const Json* engine = required_object(root, "engine", "$", issues)) {
        validate_allowed_fields(*engine, {"minimumVersion", "generatorIdentity"}, "$.engine", issues);
        adoption.minimum_version = required_string(*engine, "minimumVersion", "$.engine", issues);
        adoption.generator_identity = required_string(*engine, "generatorIdentity", "$.engine", issues);
        if (!adoption.minimum_version.empty() && !parse_release_version(adoption.minimum_version).has_value()) {
            add_issue(issues, "invalid-minimum-version", "$.engine.minimumVersion",
                      "minimumVersion must be a canonical three-component release version.");
        }
        if (!adoption.minimum_version.empty() &&
            !running_version_at_least(std::string(kProductVersion), adoption.minimum_version)) {
            add_issue(issues, "engine-version-too-old", "$.engine.minimumVersion",
                      "The running Shin version does not satisfy the adoption minimum.");
        }
        require_exact_value(adoption.generator_identity, kGeneratorVersion, "$.engine.generatorIdentity",
                            "generator-identity-mismatch", issues);
    }

    if (const Json* profile = required_object(root, "profile", "$", issues)) {
        validate_allowed_fields(*profile, {"id", "registry", "kind", "genericBootstrapAllowed"},
                                "$.profile", issues);
        adoption.profile_id = required_string(*profile, "id", "$.profile", issues);
        adoption.profile_registry = required_string(*profile, "registry", "$.profile", issues);
        adoption.profile_kind = required_string(*profile, "kind", "$.profile", issues);
        adoption.generic_bootstrap_allowed =
            required_boolean(*profile, "genericBootstrapAllowed", "$.profile", issues);
        validate_identifier(adoption.profile_id, "$.profile.id", issues);
        require_exact_value(adoption.profile_registry, "explicit-profile-dir", "$.profile.registry",
                            "unsupported-profile-registry", issues);
        require_exact_value(adoption.profile_kind, "permanent", "$.profile.kind",
                            "unsupported-adoption-profile-kind", issues);
        require_false(adoption.generic_bootstrap_allowed, "$.profile.genericBootstrapAllowed",
                      "generic-bootstrap-enabled-for-permanent-adoption", issues);
    }

    if (const Json* index = required_object(root, "index", "$", issues)) {
        validate_allowed_fields(*index, {"ownership", "key", "consumerWritesAllowed", "requireCurrent"},
                                "$.index", issues);
        adoption.index_ownership = required_string(*index, "ownership", "$.index", issues);
        adoption.index_key = required_string(*index, "key", "$.index", issues);
        adoption.consumer_writes_allowed =
            required_boolean(*index, "consumerWritesAllowed", "$.index", issues);
        adoption.require_current = required_boolean(*index, "requireCurrent", "$.index", issues);
        validate_identifier(adoption.index_key, "$.index.key", issues);
        require_exact_value(adoption.index_ownership, "tool-external", "$.index.ownership",
                            "unsupported-index-ownership", issues);
        require_false(adoption.consumer_writes_allowed, "$.index.consumerWritesAllowed",
                      "consumer-index-writes-enabled", issues);
        require_true(adoption.require_current, "$.index.requireCurrent",
                     "current-index-not-required", issues);
    }

    if (const Json* gates = required_object(root, "qualityGates", "$", issues)) {
        validate_allowed_fields(*gates,
                                {"profileCheckStrict", "check", "doctorStrict", "evalStrict",
                                 "packEvalStrict"},
                                "$.qualityGates", issues);
        adoption.profile_check_strict =
            required_boolean(*gates, "profileCheckStrict", "$.qualityGates", issues);
        adoption.check = required_boolean(*gates, "check", "$.qualityGates", issues);
        adoption.doctor_strict = required_boolean(*gates, "doctorStrict", "$.qualityGates", issues);
        adoption.eval_strict = required_boolean(*gates, "evalStrict", "$.qualityGates", issues);
        adoption.pack_eval_strict = required_boolean(*gates, "packEvalStrict", "$.qualityGates", issues);
        require_true(adoption.profile_check_strict, "$.qualityGates.profileCheckStrict",
                     "profile-check-gate-disabled", issues);
        require_true(adoption.check, "$.qualityGates.check", "index-check-gate-disabled", issues);
        require_true(adoption.doctor_strict, "$.qualityGates.doctorStrict",
                     "doctor-gate-disabled", issues);
        require_true(adoption.eval_strict, "$.qualityGates.evalStrict", "eval-gate-disabled", issues);
        require_true(adoption.pack_eval_strict, "$.qualityGates.packEvalStrict",
                     "pack-eval-gate-disabled", issues);
    }

    if (const Json* agent = required_object(root, "agent", "$", issues)) {
        validate_allowed_fields(*agent,
                                {"entryPath", "contextEpochReferenceRequired",
                                 "markerReferenceRequired", "profileReferenceRequired"},
                                "$.agent", issues);
        adoption.agent_entry_path = required_string(*agent, "entryPath", "$.agent", issues);
        adoption.context_epoch_reference_required =
            required_boolean(*agent, "contextEpochReferenceRequired", "$.agent", issues);
        adoption.marker_reference_required =
            required_boolean(*agent, "markerReferenceRequired", "$.agent", issues);
        adoption.profile_reference_required =
            required_boolean(*agent, "profileReferenceRequired", "$.agent", issues);
        if (!adoption.agent_entry_path.empty() &&
            !profile_relative_path_valid(adoption.agent_entry_path)) {
            add_issue(issues, "invalid-agent-entry-path", "$.agent.entryPath",
                      "entryPath must be a canonical repository-relative file path.");
        }
        require_true(adoption.context_epoch_reference_required,
                     "$.agent.contextEpochReferenceRequired", "agent-epoch-reference-disabled", issues);
        require_true(adoption.marker_reference_required,
                     "$.agent.markerReferenceRequired", "agent-marker-reference-disabled", issues);
        require_true(adoption.profile_reference_required,
                     "$.agent.profileReferenceRequired", "agent-profile-reference-disabled", issues);
    }

    if (const Json* extensions = find_json_field(root, "extensions")) {
        if (!extensions->is_object()) {
            add_issue(issues, "invalid-adoption-field-type", "$.extensions",
                      "extensions must be an object keyed by a namespaced identifier.");
        } else {
            adoption.extension_count = extensions->size();
            for (auto extension = extensions->begin(); extension != extensions->end(); ++extension) {
                if (!adoption_extension_identifier_valid(extension.key())) {
                    add_issue(issues, "invalid-extension-identifier", "$.extensions." + extension.key(),
                              "Extension keys must be lowercase namespaced identifiers.");
                }
                if (!extension->is_object()) {
                    add_issue(issues, "invalid-extension-value", "$.extensions." + extension.key(),
                              "Each extension value must be an object; Core does not interpret its fields.");
                }
            }
        }
    }
    return adoption;
}

AdoptionCommandOptions parse_adoption_command_options(const std::vector<std::string>& args) {
    AdoptionCommandOptions options;
    bool strict_seen = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--marker") {
            if (!options.marker.empty()) {
                throw_usage_error("duplicate-option", "adoption-check accepts --marker only once.");
            }
            if (index + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--marker requires a file path.");
            }
            options.marker = args[++index];
            continue;
        }
        if (args[index] == "--strict") {
            if (strict_seen) {
                throw_usage_error("duplicate-option", "adoption-check accepts --strict only once.");
            }
            strict_seen = true;
            options.strict = true;
            continue;
        }
        throw_usage_error("unsupported-option", "Unsupported adoption-check option: " + args[index]);
    }
    if (options.marker.empty()) {
        throw_usage_error("missing-option-value", "adoption-check requires --marker FILE.");
    }
    return options;
}

Json adoption_issue_json(const AdoptionIssue& issue) {
    return {
        {"severity", issue.severity},
        {"code", issue.code},
        {"path", issue.path},
        {"message", issue.message}
    };
}

} // namespace

int run_adoption_check(const fs::path& root,
                       const fs::path& profile_dir,
                       const std::vector<std::string>& args) {
    AdoptionCommandOptions command = parse_adoption_command_options(args);
    if (!fs::exists(root) || !fs::is_directory(root)) {
        throw CliError("missing-root", "configuration", "Project root does not exist: " + root.string(),
                       2, false, "Pass an existing project directory through --root.");
    }
    command.marker = normalize_repository_file_argument(root, command.marker, "--marker");
    if (profile_path_contains_symlink(root, command.marker)) {
        throw CliError("unsafe-adoption-marker", "configuration",
                       "Adoption marker must not pass through a symlink: " + command.marker, 2, false,
                       "Store the marker as a regular file below the project root.");
    }
    const fs::path marker_path = root / command.marker;
    std::error_code marker_error;
    const fs::file_status marker_status = fs::symlink_status(marker_path, marker_error);
    if (marker_error || !fs::is_regular_file(marker_status)) {
        throw CliError("missing-adoption-marker", "configuration",
                       "Adoption marker is not a regular file: " + command.marker, 2, false,
                       "Create a shin.project-adoption.v1 marker below the project root.");
    }

    std::vector<AdoptionIssue> issues;
    const Json document = parse_json_text(read_file(marker_path), marker_path.string());
    const AdoptionDocument adoption = parse_adoption_document(document, issues);

    std::optional<Profile> profile;
    if (!adoption.profile_id.empty() && adoption_identifier_valid(adoption.profile_id)) {
        try {
            profile = make_profile(adoption.profile_id, profile_dir);
        } catch (const CliError& error) {
            add_issue(issues, error.code(), "$.profile.id", error.what());
        } catch (const std::exception& error) {
            add_issue(issues, "profile-load-failed", "$.profile.id", error.what());
        }
    }
    if (profile.has_value()) {
        if (profile->bootstrap_only) {
            add_issue(issues, "bootstrap-profile-adopted-as-permanent", "$.profile.id",
                      "A permanent adoption marker must select a non-bootstrap profile.");
        }
        if (adoption.eval_strict && profile->eval_cases.empty()) {
            add_issue(issues, "permanent-profile-eval-missing", "$.profile.id",
                      "evalStrict requires at least one profile eval case.");
        }
        if (adoption.pack_eval_strict && profile->pack_eval_cases.empty()) {
            add_issue(issues, "permanent-profile-pack-eval-missing", "$.profile.id",
                      "packEvalStrict requires at least one profile Pack eval case.");
        }
        if (std::find(profile->agent_start_paths.begin(), profile->agent_start_paths.end(),
                      command.marker) == profile->agent_start_paths.end()) {
            add_issue(issues, "profile-adoption-marker-missing", "$.profile.id",
                      "The permanent profile must register the adoption marker in agentStartPaths.");
        }
        if (!adoption.agent_entry_path.empty() &&
            std::find(profile->agent_start_paths.begin(), profile->agent_start_paths.end(),
                      adoption.agent_entry_path) == profile->agent_start_paths.end()) {
            add_issue(issues, "profile-agent-entry-missing", "$.profile.id",
                      "The permanent profile must register the declared agent entry in agentStartPaths.");
        }
    }

    if (!adoption.agent_entry_path.empty() && profile_relative_path_valid(adoption.agent_entry_path)) {
        if (profile_path_contains_symlink(root, adoption.agent_entry_path)) {
            add_issue(issues, "unsafe-agent-entry", "$.agent.entryPath",
                      "The agent entry path must not pass through a symlink.");
        } else {
            std::error_code agent_error;
            const fs::file_status agent_status =
                fs::symlink_status(root / adoption.agent_entry_path, agent_error);
            if (agent_error || !fs::is_regular_file(agent_status)) {
                add_issue(issues, "missing-agent-entry", "$.agent.entryPath",
                          "The declared agent entry is not a regular file below the project root.");
            } else {
                const std::string agent_text = read_file(root / adoption.agent_entry_path);
                if (adoption.context_epoch_reference_required && !adoption.context_epoch.empty() &&
                    agent_text.find(adoption.context_epoch) == std::string::npos) {
                    add_issue(issues, "agent-epoch-reference-missing", "$.agent.entryPath",
                              "The agent entry does not reference the adoption context epoch.");
                }
                if (adoption.marker_reference_required &&
                    agent_text.find(command.marker) == std::string::npos) {
                    add_issue(issues, "agent-marker-reference-missing", "$.agent.entryPath",
                              "The agent entry does not reference the adoption marker path.");
                }
                if (adoption.profile_reference_required && !adoption.profile_id.empty() &&
                    agent_text.find(adoption.profile_id) == std::string::npos) {
                    add_issue(issues, "agent-profile-reference-missing", "$.agent.entryPath",
                              "The agent entry does not reference the permanent profile id.");
                }
            }
        }
    }

    int errors = 0;
    int warnings = 0;
    Json issue_rows = Json::array();
    for (const AdoptionIssue& issue : issues) {
        issue_rows.push_back(adoption_issue_json(issue));
        if (issue.severity == "warning") {
            ++warnings;
        } else {
            ++errors;
        }
    }
    const bool ok = errors == 0 && (!command.strict || warnings == 0);
    Json output = make_result_document("shin.project-adoption-check.v1", "adoption-check", ok);
    output["strict"] = command.strict;
    output["root"] = root.generic_string();
    output["marker"] = command.marker;
    output["project"] = {
        {"id", adoption.project_id},
        {"context_epoch", adoption.context_epoch},
        {"root_policy", adoption.root_policy}
    };
    output["engine"] = {
        {"version", kProductVersion},
        {"minimum_version", adoption.minimum_version},
        {"generator_version", kGeneratorVersion},
        {"required_generator_identity", adoption.generator_identity}
    };
    output["profile"] = {
        {"id", adoption.profile_id},
        {"registry", adoption.profile_registry},
        {"kind", adoption.profile_kind},
        {"config", profile.has_value() ? Json(profile->config_source) : Json(nullptr)},
        {"bootstrap_only", profile.has_value() ? Json(profile->bootstrap_only) : Json(nullptr)},
        {"eval_cases", profile.has_value() ? Json(profile->eval_cases.size()) : Json(nullptr)},
        {"pack_eval_cases", profile.has_value() ? Json(profile->pack_eval_cases.size()) : Json(nullptr)},
        {"marker_registered", profile.has_value()
                                  ? Json(std::find(profile->agent_start_paths.begin(),
                                                   profile->agent_start_paths.end(), command.marker) !=
                                         profile->agent_start_paths.end())
                                  : Json(nullptr)},
        {"agent_entry_registered", profile.has_value() && !adoption.agent_entry_path.empty()
                                       ? Json(std::find(profile->agent_start_paths.begin(),
                                                        profile->agent_start_paths.end(),
                                                        adoption.agent_entry_path) !=
                                              profile->agent_start_paths.end())
                                       : Json(nullptr)}
    };
    output["index"] = {
        {"ownership", adoption.index_ownership},
        {"key", adoption.index_key},
        {"consumer_writes_allowed", adoption.consumer_writes_allowed},
        {"require_current", adoption.require_current}
    };
    output["quality_gates"] = {
        {"profile_check_strict", adoption.profile_check_strict},
        {"check", adoption.check},
        {"doctor_strict", adoption.doctor_strict},
        {"eval_strict", adoption.eval_strict},
        {"pack_eval_strict", adoption.pack_eval_strict}
    };
    output["agent"] = {
        {"entry_path", adoption.agent_entry_path},
        {"context_epoch_reference_required", adoption.context_epoch_reference_required},
        {"marker_reference_required", adoption.marker_reference_required},
        {"profile_reference_required", adoption.profile_reference_required}
    };
    output["counts"] = {
        {"errors", errors},
        {"warnings", warnings},
        {"extensions", adoption.extension_count}
    };
    output["issues"] = std::move(issue_rows);
    emit_json_document(std::cout, output);
    return ok ? 0 : 1;
}

} // namespace shin::context
