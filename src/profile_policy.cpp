#include "profile_policy.h"
#include "context_internal.h"

namespace shin::context {
namespace {

struct PolicyField {
    const char* name;
    const char* alias;
    Json value;
};

std::vector<PolicyField> policy_fields(const Profile& p) {
    return {
        {"rootFiles", "root_files", p.root_files},
        {"optionalRootFiles", "optional_root_files", p.optional_root_files},
        {"sourceExtensions", "source_extensions", p.source_extensions},
        {"sourceFactRoots", "source_fact_roots", p.source_fact_roots},
        {"sourceFactExtensions", "source_fact_extensions", p.source_fact_extensions},
        {"sourceFactExtractors", "source_fact_extractors", p.source_fact_extractors},
        {"sourceFactAttributeKeys", "source_fact_attribute_keys", p.source_fact_attribute_keys},
        {"documentIncludePrefixes", "document_include_prefixes", p.document_include_prefixes},
        {"documentExcludePrefixes", "document_exclude_prefixes", p.document_exclude_prefixes},
        {"jsonKeyIncludePrefixes", "json_key_include_prefixes", p.json_key_include_prefixes},
        {"secretScanExcludePrefixes", "secret_scan_exclude_prefixes", p.secret_scan_exclude_prefixes},
        {"agentStartPaths", "agent_start_paths", p.agent_start_paths},
        {"compoundQueryPreferredStems", "compound_query_preferred_stems", p.compound_query_preferred_stems},
        {"compoundQueryPreferredStemSuffixes", "compound_query_preferred_stem_suffixes", p.compound_query_preferred_stem_suffixes},
        {"compoundQueryScopeStemSuffixes", "compound_query_scope_stem_suffixes", p.compound_query_scope_stem_suffixes},
        {"markdownMaxHeadingLevel", "markdown_max_heading_level", p.markdown_max_heading_level},
        {"markdownChunkLines", "markdown_chunk_lines", p.markdown_chunk_lines},
        {"markdownChunkMinLines", "markdown_chunk_min_lines", p.markdown_chunk_min_lines},
        {"markdownChunkChars", "markdown_chunk_chars", p.markdown_chunk_chars},
        {"sourceFactMaxFiles", "source_fact_max_files", p.source_fact_max_files},
        {"sourceFactMaxFileBytes", "source_fact_max_file_bytes", p.source_fact_max_file_bytes},
        {"jsonKeyMaxDepth", "json_key_max_depth", p.json_key_max_depth},
        {"jsonKeyMaxSectionsPerFile", "json_key_max_sections_per_file", p.json_key_max_sections_per_file},
        {"jsonlMaxRowsPerFile", "jsonl_max_rows_per_file", p.jsonl_max_rows_per_file},
        {"jsonKeySections", "json_key_sections", p.json_key_sections},
        {"jsonPathReferences", "json_path_references", p.json_path_references},
        {"jsonSchemaTitle", "json_schema_title", p.json_schema_title},
        {"jsonSummaryKeys", "json_summary_keys", p.json_summary_keys},
        {"sortTopics", "sort_topics", p.sort_topics},
        {"bootstrapOnly", "bootstrap_only", p.bootstrap_only},
        {"compoundQueryFirstTermScope", "compound_query_first_term_scope", p.compound_query_first_term_scope},
        {"defaultTopic", "default_topic", p.default_topic},
        {"evalResourcePolicy", "", p.eval_resource_policy}
    };
}

bool same_type(const Json& input, const Json& value) {
    if (value.is_array()) {
        return input.is_array() && std::all_of(input.begin(), input.end(),
            [](const Json& item) { return item.is_string(); });
    }
    if (value.is_number_integer()) {
        return input.is_number_integer() && input >= std::numeric_limits<int>::min() &&
               input <= std::numeric_limits<int>::max();
    }
    return input.type() == value.type();
}

Json parse_policy_source(const std::string& text) {
    std::vector<std::set<std::string>> objects;
    const auto callback = [&](int, Json::parse_event_t event, Json& value) {
        if (event == Json::parse_event_t::object_start) objects.emplace_back();
        if (event == Json::parse_event_t::object_end) objects.pop_back();
        if (event == Json::parse_event_t::key &&
            !objects.back().insert(value.get<std::string>()).second) {
            throw CliError("ambiguous-profile-policy", "configuration",
                           "Policy inspection rejects duplicate JSON keys.", 2, false,
                           "Keep exactly one value for every profile key.");
        }
        return true;
    };
    return Json::parse(text, callback);
}

} // namespace

Json inspect_profile_policy(const fs::path& root, const fs::path& profile_dir,
                            const std::string& profile_id,
                            const std::optional<std::string>& expected_hash) {
    RuntimeOptions options;
    options.root = fs::absolute(root).lexically_normal();
    options.profile_dir = fs::absolute(profile_dir).lexically_normal();
    options.profile = make_profile(profile_id, options.profile_dir);
    const auto& profile = options.profile;
    const auto text = read_file(profile.config_source);
    if (fnv1a64(text) != profile.config_hash ||
        (expected_hash && *expected_hash != profile.config_hash)) {
        throw CliError("profile-policy-revision-conflict", "configuration",
                       "Profile bytes no longer match the expected revision.", 2, false,
                       "Inspect the current profile and review the new revision before retrying.");
    }
    const Json source = parse_policy_source(text);
    Json result = make_result_document("shin.profile-policy.v1", "profile-policy", true);
    result["profile"] = profile.id;
    result["revision"] = {{"profile_config_hash", profile.config_hash},
                          {"core_version", kProductVersion}, {"generator_version", kGeneratorVersion}};
    result["binding"] = {{"root", options.root.generic_string()},
                         {"profile_config", profile.config_source}};
    result["capabilities"] = {{"read_only", true}, {"organization_constraints", false},
                              {"task_overlays", false}, {"activation", false}};
    result["index_state"] = "not-assessed";
    result["readiness_assessed"] = false;
    result["fields"] = Json::object();
    result["issues"] = Json::array();
    auto issue = [&](const std::string& code, const std::string& field) {
        result["issues"].push_back({{"severity", "error"}, {"code", code}, {"field", field}});
        result["ok"] = false;
    };
    for (const auto& field : policy_fields(profile)) {
        const bool canonical = source.contains(field.name);
        const bool alias = *field.alias != '\0' && source.contains(field.alias);
        const std::string key = canonical ? field.name : alias ? field.alias : "";
        if (canonical && alias) issue("conflicting-field-aliases", field.name);
        std::string origin = key.empty() ? "core-default" : "profile";
        if (!key.empty() && !same_type(source.at(key), field.value)) {
            issue("invalid-field-type", field.name);
            origin = "invalid-profile-input";
        } else if (std::string_view(field.name) == "agentStartPaths" &&
                   (key.empty() || source.at(key).empty())) {
            origin = "derived-root-files";
        } else if (std::string_view(field.name) == "sourceExtensions" &&
                   !key.empty() && source.at(key).empty()) {
            origin = "core-default-empty-input";
        }
        result["fields"][field.name] = {{"value", field.value}, {"origin", origin},
                                        {"input_key", key.empty() ? Json(nullptr) : Json(key)}};
    }
    // Complex rules remain owned by their existing parsers and byte-bound profile.
    result["rule_sets"] = {{"topicRules", profile.topic_rules.size()},
                            {"aliases", profile.aliases.size()}, {"evalCases", profile.eval_cases.size()},
                            {"packEvalCases", profile.pack_eval_cases.size()}};
    result["projection_scope"] = "scalar-and-path-settings-with-rule-counts";
    for (const auto& keys : std::vector<std::vector<std::string>>{
             {"topicRules", "topic_rules"}, {"aliases", "queryAliases", "query_aliases"},
             {"evalCases", "eval_cases"}, {"packEvalCases", "pack_eval_cases"},
             {"sourceLabel", "source_label"}}) {
        if (std::count_if(keys.begin(), keys.end(), [&](const auto& key) { return source.contains(key); }) > 1)
            issue("conflicting-field-aliases", keys.front());
        const auto* value = find_json_field_any(source, keys);
        if (value != nullptr && keys.front() != "sourceLabel" &&
            (!value->is_array() || !std::all_of(value->begin(), value->end(),
                 [](const Json& item) { return item.is_object(); }))) {
            issue("invalid-rule-set-type", keys.front());
        }
    }
    std::set<std::string> supported = {
        "schemaVersion", "document", "status", "summary", "id", "sourceLabel", "source_label",
        "topicRules", "topic_rules", "aliases", "queryAliases", "query_aliases",
        "evalCases", "eval_cases", "packEvalCases", "pack_eval_cases"
    };
    for (const auto& field : policy_fields(profile)) {
        supported.insert(field.name);
        if (*field.alias != '\0') supported.insert(field.alias);
    }
    for (auto field = source.begin(); field != source.end(); ++field) {
        if (!supported.contains(field.key())) issue("unsupported-profile-field", field.key());
    }
    const auto validation = validate_profile(options, true);
    for (const auto& item : validation.issues) {
        result["issues"].push_back({{"severity", item.severity}, {"code", item.id},
                                     {"path", item.path}, {"summary", item.summary}});
    }
    if (validation.counts.errors != 0 || validation.counts.warnings != 0) result["ok"] = false;
    result["profile_validation"] = {{"strict", true}, {"errors", validation.counts.errors},
                                    {"warnings", validation.counts.warnings}};
    return result;
}

int run_profile_policy(const RuntimeOptions& options, const std::vector<std::string>& args) {
    std::optional<std::string> expected;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] != "--expect-profile-hash")
            throw_usage_error("unsupported-option", "profile-policy only accepts --expect-profile-hash HASH.");
        if (expected) throw_usage_error("duplicate-option", "--expect-profile-hash may appear only once.");
        ++i;
        if (i == args.size() || args[i].empty() || args[i].starts_with("--"))
            throw_usage_error("missing-option-value", "--expect-profile-hash requires a hash.");
        expected = args[i];
    }
    const auto result = inspect_profile_policy(options.root, options.profile_dir, options.profile.id, expected);
    emit_json_document(std::cout, result);
    return result.at("ok").get<bool>() ? 0 : 1;
}

} // namespace shin::context
