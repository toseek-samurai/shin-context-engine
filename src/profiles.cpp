#include "context_internal.h"

namespace shin::context {

bool profile_relative_path_valid(const std::string& value, bool allow_dot) {
    if (value.empty() || value.front() == '/' || value.find('\\') != std::string::npos) {
        return false;
    }
    if (allow_dot && value == ".") {
        return true;
    }
    if (value.back() == '/') {
        return false;
    }
    const fs::path path(value);
    if (path.is_absolute()) {
        return false;
    }
    for (const fs::path& component : path) {
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

bool profile_path_contains_symlink(const fs::path& root, const std::string& relative_path) {
    if (!profile_relative_path_valid(relative_path, true)) {
        return false;
    }
    fs::path current = root;
    for (const fs::path& component : fs::path(relative_path)) {
        if (component == ".") {
            continue;
        }
        current /= component;
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (ec || !fs::exists(status)) {
            return false;
        }
        if (fs::is_symlink(status)) {
            return true;
        }
    }
    return false;
}

std::string normalize_repository_file_argument(const fs::path& root,
                                               const std::string& value,
                                               const std::string& option) {
    if (value.empty()) {
        throw_usage_error("missing-option-value", option + " requires a file path.");
    }
    if (value.find('\\') != std::string::npos) {
        throw_usage_error("invalid-repository-file-path",
                          option + " does not accept backslash path separators.",
                          "Use a path below --root with portable '/' separators.");
    }

    std::error_code error;
    const fs::path absolute_root = fs::absolute(root, error).lexically_normal();
    if (error) {
        throw CliError("repository-root-resolution-failed", "configuration",
                       "Cannot resolve --root while normalizing " + option + ": " +
                           error.message(),
                       2, false, "Pass an existing readable directory through --root.");
    }
    const fs::path raw(value);
    const fs::path absolute_candidate =
        (raw.is_absolute() ? raw : absolute_root / raw).lexically_normal();
    const fs::path lexical_relative = absolute_candidate.lexically_relative(absolute_root);
    const std::string relative = lexical_relative.generic_string();
    if (!profile_relative_path_valid(relative)) {
        throw_usage_error(
            "repository-file-outside-root",
            option + " must resolve to a file below --root.",
            "Pass " + option + " <relative-file> where <relative-file> is below " +
                absolute_root.generic_string() + ".");
    }

    return relative;
}

static bool regular_file_without_symlink(const fs::path& root, const std::string& relative_path) {
    fs::path current = root.lexically_normal();
    for (const fs::path& component : fs::path(relative_path)) {
        current /= component;
        std::error_code component_ec;
        const fs::file_status component_status = fs::symlink_status(current, component_ec);
        if (component_ec) {
            const auto condition = component_ec.default_error_condition();
            if (condition == std::errc::no_such_file_or_directory ||
                condition == std::errc::not_a_directory) return false;
            throw_source_observation_error("enumerate-source-root", component_ec, false);
        }
        if (fs::is_symlink(component_status)) return false;
        if (!fs::exists(component_status)) return false;
    }
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(root / relative_path, ec);
    if (ec) {
        const auto condition = ec.default_error_condition();
        if (condition == std::errc::no_such_file_or_directory ||
            condition == std::errc::not_a_directory) return false;
        throw_source_observation_error("enumerate-source-root", ec, false);
    }
    return fs::is_regular_file(status);
}

static std::vector<PackEvalCase> extract_pack_eval_cases(
    const Json& root,
    std::vector<std::string>& configuration_errors) {
    std::vector<PackEvalCase> cases;
    const Json* array = find_json_field_any(root, {"packEvalCases", "pack_eval_cases"});
    if (array == nullptr) {
        return cases;
    }
    if (!array->is_array()) {
        configuration_errors.push_back("packEvalCases must be an array.");
        return cases;
    }

    static const std::set<std::string> allowed_fields = {
        "id", "name", "request",
        "mustPaths", "must_paths", "mustNotPaths", "must_not_paths",
        "expectedComplete", "expected_complete",
        "requireSerialParallelIdentity", "require_serial_parallel_identity",
        "maxEvidenceBytes", "max_evidence_bytes", "maxRecords", "max_records",
        "maxCompactBytes", "max_compact_bytes"
    };

    for (std::size_t case_index = 0; case_index < array->size(); ++case_index) {
        const Json& object = (*array)[case_index];
        const std::string location = "packEvalCases[" + std::to_string(case_index) + "]";
        if (!object.is_object()) {
            configuration_errors.push_back(location + " must be an object.");
            continue;
        }
        for (auto field = object.begin(); field != object.end(); ++field) {
            if (!allowed_fields.contains(field.key())) {
                configuration_errors.push_back(location + " has unknown field: " + field.key());
            }
        }

        auto field_any = [&](const std::vector<std::string>& keys,
                             const std::string& label) -> const Json* {
            const Json* selected = nullptr;
            for (const std::string& key : keys) {
                const auto value = object.find(key);
                if (value == object.end()) {
                    continue;
                }
                if (selected != nullptr) {
                    configuration_errors.push_back(
                        location + "." + label + " must use only one supported field alias.");
                }
                selected = &*value;
            }
            return selected;
        };
        auto string_array = [&](const std::vector<std::string>& keys,
                                const std::string& label) {
            std::vector<std::string> values;
            const Json* value = field_any(keys, label);
            if (value == nullptr) {
                return values;
            }
            if (!value->is_array() || !std::all_of(
                    value->begin(), value->end(),
                    [](const Json& item) { return item.is_string(); })) {
                configuration_errors.push_back(
                    location + "." + label + " must be an array of strings.");
                return values;
            }
            for (const Json& item : *value) {
                values.push_back(item.get<std::string>());
            }
            return values;
        };
        auto boolean_value = [&](const std::vector<std::string>& keys,
                                 const std::string& label,
                                 bool fallback) {
            const Json* value = field_any(keys, label);
            if (value == nullptr) {
                return fallback;
            }
            if (!value->is_boolean()) {
                configuration_errors.push_back(location + "." + label + " must be a boolean.");
                return fallback;
            }
            return value->get<bool>();
        };
        auto integer_value = [&](const std::vector<std::string>& keys,
                                 const std::string& label,
                                 int fallback) {
            const Json* value = field_any(keys, label);
            if (value == nullptr) {
                return fallback;
            }
            if (!value->is_number_integer()) {
                configuration_errors.push_back(location + "." + label + " must be an integer.");
                return fallback;
            }
            try {
                const long long parsed = value->get<long long>();
                if (parsed < std::numeric_limits<int>::min() ||
                    parsed > std::numeric_limits<int>::max()) {
                    configuration_errors.push_back(
                        location + "." + label + " is outside the supported integer range.");
                    return fallback;
                }
                return static_cast<int>(parsed);
            } catch (const std::exception&) {
                configuration_errors.push_back(
                    location + "." + label + " is outside the supported integer range.");
                return fallback;
            }
        };

        PackEvalCase item;
        const Json* id = field_any({"id", "name"}, "id");
        if (id != nullptr && id->is_string()) {
            item.id = id->get<std::string>();
        } else {
            configuration_errors.push_back(location + ".id must be a non-empty string.");
        }
        const Json* request = field_any({"request"}, "request");
        if (request != nullptr && request->is_object()) {
            item.request_document = *request;
        } else {
            configuration_errors.push_back(location + ".request must be an object.");
        }
        item.must_paths = string_array({"mustPaths", "must_paths"}, "mustPaths");
        item.must_not_paths = string_array({"mustNotPaths", "must_not_paths"}, "mustNotPaths");
        item.expected_complete = boolean_value(
            {"expectedComplete", "expected_complete"}, "expectedComplete", true);
        item.require_serial_parallel_identity = boolean_value(
            {"requireSerialParallelIdentity", "require_serial_parallel_identity"},
            "requireSerialParallelIdentity", true);
        item.max_evidence_bytes = integer_value(
            {"maxEvidenceBytes", "max_evidence_bytes"}, "maxEvidenceBytes", -1);
        item.max_records = integer_value(
            {"maxRecords", "max_records"}, "maxRecords", -1);
        item.max_compact_bytes = integer_value(
            {"maxCompactBytes", "max_compact_bytes"}, "maxCompactBytes", -1);
        if (!item.id.empty() && item.request_document.is_object()) {
            cases.push_back(std::move(item));
        }
    }
    return cases;
}

static Profile load_profile_file(const fs::path& path) {
    const std::string text = read_file(path);
    const Json root = parse_json_text(text, path.string());
    Profile profile;
    profile.id = json_string_value(root, "id");
    profile.source_label = json_string_any(root, {"sourceLabel", "source_label"});
    profile.root_files = json_string_array_any(root, {"rootFiles", "root_files"});
    profile.optional_root_files =
        json_string_array_any(root, {"optionalRootFiles", "optional_root_files"});
    std::vector<std::string> source_extensions =
        json_string_array_any(root, {"sourceExtensions", "source_extensions"});
    if (!source_extensions.empty()) {
        profile.source_extensions = source_extensions;
    }
    profile.source_fact_roots = json_string_array_any(root, {"sourceFactRoots", "source_fact_roots"});
    profile.source_fact_extensions =
        json_string_array_any(root, {"sourceFactExtensions", "source_fact_extensions"});
    if (root.contains("sourceFactExtractors") || root.contains("source_fact_extractors")) {
        profile.source_fact_extractors =
            json_string_array_any(root, {"sourceFactExtractors", "source_fact_extractors"});
    }
    const bool has_attribute_keys = root.contains("sourceFactAttributeKeys");
    const bool has_attribute_keys_alias = root.contains("source_fact_attribute_keys");
    if (has_attribute_keys && has_attribute_keys_alias) {
        profile.source_fact_config_errors.push_back(
            "sourceFactAttributeKeys must use only one supported field alias.");
    }
    if (has_attribute_keys || has_attribute_keys_alias) {
        const Json& attribute_keys = has_attribute_keys
                                         ? root.at("sourceFactAttributeKeys")
                                         : root.at("source_fact_attribute_keys");
        if (!attribute_keys.is_array() ||
            !std::all_of(attribute_keys.begin(), attribute_keys.end(),
                         [](const Json& item) { return item.is_string(); })) {
            profile.source_fact_config_errors.push_back(
                "sourceFactAttributeKeys must be an array of strings.");
        } else {
            for (const Json& key : attribute_keys) {
                profile.source_fact_attribute_keys.push_back(key.get<std::string>());
            }
        }
    }
    profile.document_include_prefixes =
        json_string_array_any(root, {"documentIncludePrefixes", "document_include_prefixes"});
    profile.document_exclude_prefixes =
        json_string_array_any(root, {"documentExcludePrefixes", "document_exclude_prefixes"});
    profile.json_key_include_prefixes =
        json_string_array_any(root, {"jsonKeyIncludePrefixes", "json_key_include_prefixes"});
    profile.secret_scan_exclude_prefixes =
        json_string_array_any(root, {"secretScanExcludePrefixes", "secret_scan_exclude_prefixes"});
    profile.agent_start_paths = json_string_array_any(root, {"agentStartPaths", "agent_start_paths"});
    profile.topic_rules = extract_topic_rules(root);
    profile.aliases = extract_alias_rules(root);
    profile.eval_cases = extract_eval_cases(root, profile.eval_case_config_errors);
    if (root.contains("evalResourcePolicy")) {
        const Json& policy = root.at("evalResourcePolicy");
        if (!policy.is_string() || (policy != "strict" && policy != "advisory")) {
            profile.eval_case_config_errors.push_back(
                "evalResourcePolicy must be strict or advisory.");
        } else {
            profile.eval_resource_policy = policy.get<std::string>();
        }
    }
    profile.pack_eval_cases = extract_pack_eval_cases(
        root, profile.pack_eval_case_config_errors);
    profile.compound_query_preferred_stems = json_string_array_any(
        root, {"compoundQueryPreferredStems", "compound_query_preferred_stems"});
    profile.compound_query_preferred_stem_suffixes = json_string_array_any(
        root, {"compoundQueryPreferredStemSuffixes", "compound_query_preferred_stem_suffixes"});
    profile.compound_query_scope_stem_suffixes = json_string_array_any(
        root, {"compoundQueryScopeStemSuffixes", "compound_query_scope_stem_suffixes"});
    profile.markdown_max_heading_level =
        json_int_any(root, {"markdownMaxHeadingLevel", "markdown_max_heading_level"},
                     profile.markdown_max_heading_level);
    profile.markdown_chunk_lines =
        json_int_any(root, {"markdownChunkLines", "markdown_chunk_lines"}, profile.markdown_chunk_lines);
    profile.markdown_chunk_min_lines =
        json_int_any(root, {"markdownChunkMinLines", "markdown_chunk_min_lines"},
                     profile.markdown_chunk_min_lines);
    profile.markdown_chunk_chars =
        json_int_any(root, {"markdownChunkChars", "markdown_chunk_chars"}, profile.markdown_chunk_chars);
    profile.source_fact_max_files =
        json_int_any(root, {"sourceFactMaxFiles", "source_fact_max_files"}, profile.source_fact_max_files);
    profile.source_fact_max_file_bytes =
        json_int_any(root, {"sourceFactMaxFileBytes", "source_fact_max_file_bytes"},
                     profile.source_fact_max_file_bytes);
    profile.json_key_max_depth =
        json_int_any(root, {"jsonKeyMaxDepth", "json_key_max_depth"}, profile.json_key_max_depth);
    profile.json_key_max_sections_per_file =
        json_int_any(root, {"jsonKeyMaxSectionsPerFile", "json_key_max_sections_per_file"},
                     profile.json_key_max_sections_per_file);
    profile.jsonl_max_rows_per_file =
        json_int_any(root, {"jsonlMaxRowsPerFile", "jsonl_max_rows_per_file"},
                     profile.jsonl_max_rows_per_file);
    profile.json_key_sections =
        json_bool_any(root, {"jsonKeySections", "json_key_sections"}, profile.json_key_sections);
    profile.json_path_references =
        json_bool_any(root, {"jsonPathReferences", "json_path_references"}, profile.json_path_references);
    profile.json_schema_title =
        json_bool_any(root, {"jsonSchemaTitle", "json_schema_title"}, profile.json_schema_title);
    profile.json_summary_keys =
        json_bool_any(root, {"jsonSummaryKeys", "json_summary_keys"}, profile.json_summary_keys);
    profile.sort_topics = json_bool_any(root, {"sortTopics", "sort_topics"}, profile.sort_topics);
    profile.bootstrap_only =
        json_bool_any(root, {"bootstrapOnly", "bootstrap_only"}, profile.bootstrap_only);
    profile.compound_query_first_term_scope =
        json_bool_any(root, {"compoundQueryFirstTermScope", "compound_query_first_term_scope"},
                      profile.compound_query_first_term_scope);
    profile.default_topic = json_string_any(root, {"defaultTopic", "default_topic"});
    profile.config_source = path.generic_string();
    profile.config_hash = fnv1a64(text);

    if (profile.id.empty()) {
        throw CliError("invalid-profile", "configuration", "Profile config is missing id: " + path.string(),
                       2, false, "Repair the profile JSON and run profile-check --strict.");
    }
    if (profile.source_label.empty()) {
        throw CliError("invalid-profile", "configuration",
                       "Profile config is missing sourceLabel: " + path.string(), 2, false,
                       "Repair the profile JSON and run profile-check --strict.");
    }
    if (profile.agent_start_paths.empty()) {
        profile.agent_start_paths = profile.root_files;
    }
    return profile;
}

Profile make_profile(const std::string& id, const fs::path& profile_dir) {
    if (!portable_identifier_valid(id)) {
        throw CliError("invalid-profile-id", "configuration",
                       "Profile id must be a lowercase portable identifier: " + id,
                       2, false, "Select a profile id containing lowercase ASCII letters, digits, '-', '_', or '.'.");
    }
    const fs::path config_path = profile_dir / (id + ".json");
    if (!fs::exists(config_path)) {
        throw CliError("unknown-profile", "configuration", "Unknown profile: " + id, 2, false,
                       "Add " + config_path.generic_string() + " or select an existing profile id.");
    }
    Profile profile = load_profile_file(config_path);
    if (profile.id != id) {
        throw CliError("profile-id-mismatch", "configuration",
                       "Profile id mismatch: requested " + id + ", config has " + profile.id, 2, false,
                       "Make the profile file name and id field identical.");
    }
    return profile;
}

std::vector<std::string> detect_topics(const Profile& profile,
                                       const std::string& path,
                                       const std::string& title,
                                       const std::string& content) {
    std::string searchable = to_lower(path + "\n" + title + "\n" + content);
    std::vector<std::string> topics;
    for (const TopicRule& rule : profile.topic_rules) {
        for (const std::string& keyword : rule.keywords) {
            if (searchable.find(to_lower(keyword)) != std::string::npos) {
                topics.push_back(rule.id);
                break;
            }
        }
    }
    if (topics.empty() && !profile.default_topic.empty()) {
        topics.push_back(profile.default_topic);
    }
    if (profile.sort_topics) {
        std::sort(topics.begin(), topics.end());
    }
    return topics;
}

static std::string extract_title(const Profile& profile,
                          const std::string& path,
                          const std::string& kind,
                          const std::vector<std::string>& lines,
                          const Json* json_doc) {
    if (kind == "markdown") {
        for (const std::string& line : lines) {
            if (markdown_heading_level(line) == 1) {
                return trim(line.substr(2));
            }
        }
    } else if (profile.json_schema_title && json_doc != nullptr) {
        std::string value = json_string_value(*json_doc, "document");
        if (!value.empty()) {
            return value;
        }
        value = json_string_value(*json_doc, "schemaVersion");
        if (!value.empty()) {
            return value;
        }
    }
    return title_from_path(path);
}

static std::string extract_summary(const Profile& profile,
                            const std::string& kind,
                            const std::vector<std::string>& lines,
                            const Json* json_doc) {
    if (kind == "json" && profile.json_summary_keys && json_doc != nullptr) {
        for (const char* key : {"summary", "purpose", "goal", "status", "decision", "scope", "conclusion"}) {
            std::string value = json_string_value(*json_doc, key);
            if (!value.empty()) {
                return truncate_text(redact_sensitive_text(compact_whitespace(value)), 280);
            }
        }
    }
    for (const std::string& line : lines) {
        std::string compact = compact_whitespace(line);
        if (compact.empty() || starts_with(compact, "#") || starts_with(compact, "```")) {
            continue;
        }
        if (kind == "json" && (compact == "{" || compact == "}" || compact == "[" || compact == "]")) {
            continue;
        }
        return truncate_text(redact_sensitive_text(compact), 280);
    }
    return "";
}

static bool path_matches_profile_prefix(const std::string& path, const std::string& raw_prefix) {
    std::string prefix = raw_prefix;
    while (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }
    return !prefix.empty() &&
           (path == prefix || (starts_with(path, prefix) && path.size() > prefix.size() &&
                               path[prefix.size()] == '/'));
}

bool path_matches_any_profile_prefix(const std::string& path, const std::vector<std::string>& prefixes) {
    return std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string& prefix) {
        return path_matches_profile_prefix(path, prefix);
    });
}

bool document_path_allowed(const Profile& profile, const std::string& path) {
    const bool included = profile.document_include_prefixes.empty() ||
                          path_matches_any_profile_prefix(path, profile.document_include_prefixes);
    return included && !path_matches_any_profile_prefix(path, profile.document_exclude_prefixes);
}

bool shin_internal_control_path(const std::string& path) {
    const std::string normalized = fs::path(path).lexically_normal().generic_string();
    return starts_with(to_lower(normalized), "docs/kb/.shin-context");
}

static bool is_source_doc_path(const Profile& profile, const std::string& path) {
    if (shin_internal_control_path(path) || !document_path_allowed(profile, path)) {
        return false;
    }
    for (const std::string& root_file : profile.root_files) {
        if (path == root_file) {
            return true;
        }
    }
    if (!starts_with(path, "docs/")) {
        return false;
    }
    if (starts_with(path, "docs/kb/generated/")) {
        return false;
    }
    return extension_allowed(profile.source_extensions, path);
}

std::vector<std::string> enumerate_source_paths(const Profile& profile, const fs::path& root) {
    std::vector<std::string> paths;
    // Explicit entries may alias the same file (hard links or case-insensitive
    // spellings). Compare observed file identities once per entry, rather than
    // re-statting every preceding pair on each freshness/validation pass.
    std::set<std::pair<std::uint64_t, std::uint64_t>> explicit_files;
    auto append_unique_file = [&](const std::string& candidate) {
        const auto identity = source_file_metadata(root / candidate, "enumerate-source-entry");
        if (explicit_files.emplace(identity.device_id, identity.file_id).second)
            paths.push_back(candidate);
    };
    for (const std::string& root_file : profile.root_files) {
        if (profile_relative_path_valid(root_file) && !shin_internal_control_path(root_file) &&
            document_path_allowed(profile, root_file) &&
            regular_file_without_symlink(root, root_file)) {
            append_unique_file(root_file);
        }
    }
    for (const std::string& root_file : profile.optional_root_files) {
        if (profile_relative_path_valid(root_file) && !shin_internal_control_path(root_file) &&
            document_path_allowed(profile, root_file) &&
            regular_file_without_symlink(root, root_file)) {
            append_unique_file(root_file);
        }
    }
    const fs::path docs_root = root.lexically_normal() / "docs";
    std::error_code docs_ec;
    const fs::file_status docs_status = fs::symlink_status(docs_root, docs_ec);
    if (docs_ec) {
        const auto condition = docs_ec.default_error_condition();
        if (condition != std::errc::no_such_file_or_directory &&
            condition != std::errc::not_a_directory) {
            throw_source_observation_error("enumerate-source-root", docs_ec, false);
        }
    } else if (fs::is_directory(docs_status) && !fs::is_symlink(docs_status)) {
        std::error_code iterator_ec;
        fs::recursive_directory_iterator it(docs_root, fs::directory_options::none, iterator_ec), end;
        if (iterator_ec) {
            throw_source_observation_error("enumerate-source-root", iterator_ec);
        }
        while (it != end) {
            std::error_code entry_ec;
            const bool symlink = it->is_symlink(entry_ec);
            if (entry_ec) {
                throw_source_observation_error("enumerate-source-entry", entry_ec);
            }
            if (symlink) {
                it.disable_recursion_pending();
            } else {
                const fs::path rel_path = it->path().lexically_relative(root.lexically_normal());
                const std::string rel = rel_path.generic_string();
                if (!profile_relative_path_valid(rel)) {
                    throw_source_observation_io_error("enumerate-source-entry");
                }
                const bool directory = it->is_directory(entry_ec);
                if (entry_ec) {
                    throw_source_observation_error("enumerate-source-entry", entry_ec);
                }
                if (directory && (shin_internal_control_path(rel) || rel == "docs/kb/generated")) {
                    it.disable_recursion_pending();
                } else if (!directory) {
                    const bool regular = it->is_regular_file(entry_ec);
                    if (entry_ec) {
                        throw_source_observation_error("enumerate-source-entry", entry_ec);
                    }
                    if (regular && is_source_doc_path(profile, rel)) paths.push_back(rel);
                }
            }
            it.increment(iterator_ec);
            if (iterator_ec) throw_source_observation_error("enumerate-source-entry", iterator_ec);
        }
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

static SourceDoc read_source_doc(const Profile& profile, const fs::path& root, const std::string& path) {
    SourceDoc doc{};
    doc.path = path;
    const std::string ext = extension_of(path);
    if (ext == ".md" || ext == ".mdc") {
        doc.kind = "markdown";
    } else if (ext == ".json") {
        doc.kind = "json";
    } else if (ext == ".jsonl") {
        doc.kind = "jsonl";
    } else if (ext == ".yaml" || ext == ".yml") {
        doc.kind = "yaml";
    } else {
        doc.kind = "text";
    }
    doc.content = read_file(root / path);
    doc.lines = split_lines(doc.content);
    doc.line_count = static_cast<int>(doc.lines.size());
    doc.byte_count = static_cast<int>(doc.content.size());
    doc.content_hash = fnv1a64(doc.content);
    doc.id = slugify(path);
    Json json_doc;
    const Json* json_doc_ptr = nullptr;
    if (doc.kind == "json" && try_parse_json_text(doc.content, json_doc)) {
        json_doc_ptr = &json_doc;
    }
    doc.title = extract_title(profile, path, doc.kind, doc.lines, json_doc_ptr);
    doc.summary = extract_summary(profile, doc.kind, doc.lines, json_doc_ptr);
    doc.doc_type = detect_doc_type(path, doc.kind, doc.title, doc.content);
    doc.area = detect_area(path);
    doc.status = detect_status(path, doc.content, json_doc_ptr);
    if (doc.kind == "markdown") {
        for (const std::string& line : doc.lines) {
            if (markdown_heading_level(line) > 0) {
                ++doc.heading_count;
            }
        }
    }
    doc.topics = detect_topics(profile, path, doc.title, doc.content);
    if ((doc.doc_type == "adr" || doc.doc_type == "decision-index") &&
        std::find(doc.topics.begin(), doc.topics.end(), "decisions") == doc.topics.end()) {
        doc.topics.push_back("decisions");
    }
    if (doc.doc_type == "fact-store" &&
        std::find(doc.topics.begin(), doc.topics.end(), "facts") == doc.topics.end()) {
        doc.topics.push_back("facts");
    }
    if (profile.sort_topics) {
        std::sort(doc.topics.begin(), doc.topics.end());
    }
    const EvidenceProvenance provenance = infer_document_evidence_provenance(doc);
    doc.evidence_kind = provenance.evidence_kind;
    doc.verification_level = provenance.verification_level;
    return doc;
}

std::vector<SourceDoc> read_sources(const Profile& profile, const fs::path& root) {
    std::vector<std::string> paths = enumerate_source_paths(profile, root);
    std::vector<SourceDoc> docs(paths.size());
    parallel_for_indices(paths.size(), [&](std::size_t index) {
        docs[index] = read_source_doc(profile, root, paths[index]);
    });
    return docs;
}

} // namespace shin::context
